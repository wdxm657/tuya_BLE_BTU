#include "string.h"
#include "board.h"
#include "tal_gpio.h"
#include "tal_spi.h"
#include "tal_log.h"
#include "faqiuji_ext_flash.h"

#define FLASH_CMD_WREN 0x06
#define FLASH_CMD_RDSR 0x05
#define FLASH_CMD_READ 0x03
#define FLASH_CMD_PP   0x02
#define FLASH_CMD_SE   0x20
#define FLASH_CMD_RDID 0x9F
#define FLASH_SPI      TUYA_SPI_NUM_0

STATIC BOOL_T sg_flash_ready = FALSE;

STATIC OPERATE_RET flash_xfer(CONST UINT8_T *tx, UINT32_T tx_len, UINT8_T *rx, UINT32_T rx_len)
{
    return tal_spi_xfer_with_length(FLASH_SPI, (VOID_T *)tx, tx_len, rx, rx_len);
}

STATIC OPERATE_RET flash_cmd(UINT8_T cmd)
{
    UINT8_T rx = 0;
    return tal_spi_xfer(FLASH_SPI, &cmd, &rx, 1);
}

STATIC OPERATE_RET flash_wait_ready(VOID_T)
{
    UINT8_T tx = FLASH_CMD_RDSR;
    UINT8_T status = 0;
    UINT32_T guard = 50000;
    while (guard--) {
        if (flash_xfer(&tx, 1, &status, 1) != OPRT_OK) return OPRT_COM_ERROR;
        if ((status & 1) == 0) return OPRT_OK;
    }
    return OPRT_TIMEOUT;
}

STATIC OPERATE_RET flash_write_enable(VOID_T)
{
    OPERATE_RET ret = flash_cmd(FLASH_CMD_WREN);
    return (ret == OPRT_OK) ? flash_wait_ready() : ret;
}

OPERATE_RET faqiuji_ext_flash_init(VOID_T)
{
    UINT8_T group = 1;
    UINT8_T tx = FLASH_CMD_RDID;
    UINT8_T id[3] = {0};
    TUYA_SPI_BASE_CFG_T cfg = {
        .mode = TUYA_SPI_MODE0,
        .type = TUYA_SPI_AUTO_TYPE,
        .databits = TUYA_SPI_DATA_BIT8,
        .freq_hz = 8000000,
    };
    OPERATE_RET ret;

    ret = tal_spi_control(FLASH_SPI, 0, &group);
    if (ret != OPRT_OK) return ret;
    ret = tal_spi_init(FLASH_SPI, &cfg);
    if (ret != OPRT_OK && ret != OPRT_INIT_MORE_THAN_ONCE) return ret;
    ret = flash_xfer(&tx, 1, id, sizeof(id));
    if (ret == OPRT_OK) {
        TAL_PR_INFO("GT25Q16 JEDEC ID: %02x %02x %02x", id[0], id[1], id[2]);
        sg_flash_ready = TRUE;
    }
    return ret;
}

OPERATE_RET faqiuji_ext_flash_read(UINT32_T addr, UINT8_T *buf, UINT32_T len)
{
    UINT8_T tx[4] = {FLASH_CMD_READ, (UINT8_T)(addr >> 16), (UINT8_T)(addr >> 8), (UINT8_T)addr};
    if (!sg_flash_ready || buf == NULL || len == 0 || addr + len > FAQIUJI_FLASH_SIZE) return OPRT_INVALID_PARM;
    return flash_xfer(tx, sizeof(tx), buf, len);
}

OPERATE_RET faqiuji_ext_flash_write(UINT32_T addr, CONST UINT8_T *buf, UINT32_T len)
{
    UINT8_T tx[FAQIUJI_FLASH_PAGE + 4];
    UINT32_T part;
    OPERATE_RET ret;
    if (!sg_flash_ready || buf == NULL || len == 0 || addr + len > FAQIUJI_FLASH_SIZE) return OPRT_INVALID_PARM;
    while (len) {
        part = FAQIUJI_FLASH_PAGE - (addr & (FAQIUJI_FLASH_PAGE - 1));
        if (part > len) part = len;
        tx[0] = FLASH_CMD_PP;
        tx[1] = (UINT8_T)(addr >> 16);
        tx[2] = (UINT8_T)(addr >> 8);
        tx[3] = (UINT8_T)addr;
        memcpy(&tx[4], buf, part);
        ret = flash_write_enable();
        if (ret != OPRT_OK) return ret;
        ret = tal_spi_xfer(FLASH_SPI, tx, tx, part + 4);
        if (ret != OPRT_OK) return ret;
        ret = flash_wait_ready();
        if (ret != OPRT_OK) return ret;
        addr += part;
        buf += part;
        len -= part;
    }
    return OPRT_OK;
}

OPERATE_RET faqiuji_ext_flash_erase(UINT32_T addr, UINT32_T len)
{
    UINT8_T tx[4];
    OPERATE_RET ret;
    if (!sg_flash_ready || len == 0 || addr + len > FAQIUJI_FLASH_SIZE ||
        (addr & (FAQIUJI_FLASH_SECTOR - 1)) != 0 || (len & (FAQIUJI_FLASH_SECTOR - 1)) != 0) {
        return OPRT_INVALID_PARM;
    }
    while (len) {
        ret = flash_write_enable();
        if (ret != OPRT_OK) return ret;
        tx[0] = FLASH_CMD_SE;
        tx[1] = (UINT8_T)(addr >> 16);
        tx[2] = (UINT8_T)(addr >> 8);
        tx[3] = (UINT8_T)addr;
        ret = tal_spi_xfer(FLASH_SPI, tx, tx, sizeof(tx));
        if (ret != OPRT_OK) return ret;
        ret = flash_wait_ready();
        if (ret != OPRT_OK) return ret;
        addr += FAQIUJI_FLASH_SECTOR;
        len -= FAQIUJI_FLASH_SECTOR;
    }
    return OPRT_OK;
}

OPERATE_RET faqiuji_ext_flash_audio_erase(UINT8_T file_id)
{
    if (file_id >= FAQIUJI_AUDIO_SLOT_COUNT) return OPRT_INVALID_PARM;
    return faqiuji_ext_flash_erase(FAQIUJI_AUDIO_BASE + file_id * FAQIUJI_AUDIO_SLOT_SIZE,
                                   FAQIUJI_AUDIO_SLOT_SIZE);
}

OPERATE_RET faqiuji_ext_flash_audio_read(UINT8_T file_id, UINT32_T offset, UINT8_T *buf, UINT32_T len)
{
    if (file_id >= FAQIUJI_AUDIO_SLOT_COUNT || offset + len > FAQIUJI_AUDIO_SLOT_SIZE) return OPRT_INVALID_PARM;
    return faqiuji_ext_flash_read(FAQIUJI_AUDIO_BASE + file_id * FAQIUJI_AUDIO_SLOT_SIZE + offset, buf, len);
}

OPERATE_RET faqiuji_ext_flash_audio_write(UINT8_T file_id, UINT32_T offset, CONST UINT8_T *buf, UINT32_T len)
{
    if (file_id >= FAQIUJI_AUDIO_SLOT_COUNT || offset + len > FAQIUJI_AUDIO_SLOT_SIZE) return OPRT_INVALID_PARM;
    return faqiuji_ext_flash_write(FAQIUJI_AUDIO_BASE + file_id * FAQIUJI_AUDIO_SLOT_SIZE + offset, buf, len);
}
