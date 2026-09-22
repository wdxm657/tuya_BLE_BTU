#include "string.h"
#include "board.h"
#include "tal_gpio.h"
#include "tal_spi.h"
#include "tkl_spi.h"
#include "tal_log.h"
#include "faqiuji_ext_flash.h"

#define FLASH_CMD_WREN 0x06
#define FLASH_CMD_RDSR 0x05
#define FLASH_CMD_READ 0x03
#define FLASH_CMD_PP   0x02
#define FLASH_CMD_SE   0x20
#define FLASH_CMD_RDID 0x9F
#define FLASH_SPI      TUYA_SPI_NUM_0
#define FLASH_TEST_ADDR (FAQIUJI_FLASH_SIZE - FAQIUJI_FLASH_SECTOR)
#define FLASH_FIXED_TEST_ADDR_0 0x000000UL
#define FLASH_FIXED_TEST_ADDR_1 0x00000CUL

STATIC BOOL_T sg_flash_ready = FALSE;

STATIC OPERATE_RET flash_xfer(CONST UINT8_T *tx, UINT32_T tx_len, UINT8_T *rx, UINT32_T rx_len)
{
    return tal_spi_xfer_with_length(FLASH_SPI, (VOID_T *)tx, tx_len, rx, rx_len);
}

STATIC OPERATE_RET flash_cmd(UINT8_T cmd)
{
    return tal_spi_send(FLASH_SPI, &cmd, 1);
}

STATIC OPERATE_RET flash_read_status(UINT8_T *status)
{
    UINT8_T tx = FLASH_CMD_RDSR;

    if (status == NULL) return OPRT_INVALID_PARM;
    return flash_xfer(&tx, 1, status, 1);
}

STATIC OPERATE_RET flash_wait_ready(VOID_T)
{
    UINT8_T status = 0;
    UINT32_T guard = 50000;
    while (guard--) {
        if (flash_read_status(&status) != OPRT_OK) return OPRT_COM_ERROR;
        if ((status & 1) == 0) return OPRT_OK;
    }
    return OPRT_TIMEOUT;
}

STATIC OPERATE_RET flash_write_enable(VOID_T)
{
    UINT8_T status = 0;
    OPERATE_RET ret = flash_wait_ready();
    if (ret != OPRT_OK) return ret;

    ret = flash_cmd(FLASH_CMD_WREN);
    if (ret != OPRT_OK) return ret;

    ret = flash_read_status(&status);
    if (ret != OPRT_OK) return ret;
    // TAL_PR_DEBUG("GT25Q16 WREN status: 0x%02x", status);
    if ((status & 0x02) == 0) {
        TAL_PR_ERR("GT25Q16 WREN rejected, status: 0x%02x", status);
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

STATIC BOOL_T flash_jedec_id_valid(CONST UINT8_T *id)
{
    return id != NULL &&
           !(id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) &&
           !(id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF);
}

STATIC OPERATE_RET flash_check_fixed_data(UINT32_T addr, CONST UINT8_T *expect, UINT32_T len)
{
    UINT8_T actual[4] = {0};
    OPERATE_RET ret;

    if (expect == NULL || len == 0 || len > sizeof(actual)) return OPRT_INVALID_PARM;

    ret = faqiuji_ext_flash_read(addr, actual, len);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("GT25Q16 fixed self test read addr 0x%06x err: %d", addr, ret);
        return ret;
    }

    if (memcmp(expect, actual, len) != 0) {
        TAL_PR_HEXDUMP_INFO("GT25Q16 fixed self test expect", expect, len);
        TAL_PR_HEXDUMP_INFO("GT25Q16 fixed self test actual", actual, len);
        return OPRT_COM_ERROR;
    }

    TAL_PR_INFO("GT25Q16 fixed self test pass addr: 0x%06x", addr);
    return OPRT_OK;
}

STATIC OPERATE_RET flash_power_on_self_test(VOID_T)
{
    CONST UINT8_T fixed_data_0[] = {0x1D, 0xFB, 0x65, 0xF9};
    CONST UINT8_T fixed_data_1[] = {0x36, 0x04, 0x89, 0xFF};
    CONST UINT8_T test_pattern[] = {
        0x46, 0x41, 0x51, 0x49, 0x55, 0x4A, 0x49, 0x21,
        0xA5, 0x5A, 0xC3, 0x3C, 0x12, 0x34, 0x56, 0x78,
    };
    UINT8_T read_buf[sizeof(test_pattern)] = {0};
    OPERATE_RET ret;

    ret = flash_check_fixed_data(FLASH_FIXED_TEST_ADDR_0, fixed_data_0, sizeof(fixed_data_0));
    if (ret != OPRT_OK) return ret;

    ret = flash_check_fixed_data(FLASH_FIXED_TEST_ADDR_1, fixed_data_1, sizeof(fixed_data_1));
    if (ret != OPRT_OK) return ret;

    TAL_PR_INFO("GT25Q16 self test addr: 0x%06x", FLASH_TEST_ADDR);

    ret = faqiuji_ext_flash_erase(FLASH_TEST_ADDR, FAQIUJI_FLASH_SECTOR);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("GT25Q16 self test erase err: %d", ret);
        return ret;
    }

    ret = faqiuji_ext_flash_write(FLASH_TEST_ADDR, test_pattern, sizeof(test_pattern));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("GT25Q16 self test write err: %d", ret);
        return ret;
    }

    ret = faqiuji_ext_flash_read(FLASH_TEST_ADDR, read_buf, sizeof(read_buf));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("GT25Q16 self test read err: %d", ret);
        return ret;
    }

    if (memcmp(test_pattern, read_buf, sizeof(test_pattern)) != 0) {
        TAL_PR_HEXDUMP_INFO("GT25Q16 self test expect", test_pattern, sizeof(test_pattern));
        TAL_PR_HEXDUMP_INFO("GT25Q16 self test actual", read_buf, sizeof(read_buf));
        return OPRT_COM_ERROR;
    }

    TAL_PR_INFO("GT25Q16 self test pass");
    return OPRT_OK;
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

    sg_flash_ready = FALSE;
    TAL_PR_INFO("GT25Q16 INIT");
    ret = tkl_spi_ioctl(FLASH_SPI, 0, &group);
    if (ret != OPRT_OK && ret != OPRT_INIT_MORE_THAN_ONCE) {
        TAL_PR_ERR("GT25Q16 spi io group err: %d", ret);
        return ret;
    }
    TAL_PR_INFO("GT25Q16 INIT tkl_spi_ioctl done");
    ret = tal_spi_init(FLASH_SPI, &cfg);
    if (ret != OPRT_OK && ret != OPRT_INIT_MORE_THAN_ONCE) {
        TAL_PR_ERR("GT25Q16 spi init err: %d", ret);
        return ret;
    }
    TAL_PR_INFO("GT25Q16 INIT tal_spi_init done");
    ret = flash_xfer(&tx, 1, id, sizeof(id));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("GT25Q16 read id err: %d", ret);
        return ret;
    }

    TAL_PR_INFO("GT25Q16 JEDEC ID: %02x %02x %02x", id[0], id[1], id[2]);
    if (!flash_jedec_id_valid(id)) {
        TAL_PR_ERR("GT25Q16 invalid JEDEC ID");
        return OPRT_COM_ERROR;
    }

    sg_flash_ready = TRUE;
    // ret = flash_power_on_self_test();
    if (ret != OPRT_OK) {
        sg_flash_ready = FALSE;
        return ret;
    }

    TAL_PR_INFO("GT25Q16 INIT DONE");
    return OPRT_OK;
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
    UINT8_T status = 0;
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
        ret = tal_spi_send(FLASH_SPI, tx, (UINT16_T)(part + 4));
        if (ret != OPRT_OK) return ret;
        ret = flash_read_status(&status);
        if (ret != OPRT_OK) return ret;
        // TAL_PR_DEBUG("GT25Q16 page program status: 0x%02x", status);
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
        ret = tal_spi_send(FLASH_SPI, tx, sizeof(tx));
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
    if (file_id != FAQIUJI_AUDIO_USER_FILE_ID) return OPRT_INVALID_PARM;
    return faqiuji_ext_flash_erase(FAQIUJI_AUDIO_USER_BASE, FAQIUJI_AUDIO_SLOT_SIZE);
}

OPERATE_RET faqiuji_ext_flash_audio_read(UINT8_T file_id, UINT32_T offset, UINT8_T *buf, UINT32_T len)
{
    UINT32_T base;
    UINT32_T size;

    if (file_id == FAQIUJI_AUDIO_FACTORY_SOUND_1) {
        base = FAQIUJI_AUDIO_FACTORY_1_BASE;
        size = FAQIUJI_AUDIO_FACTORY_1_SIZE;
    } else if (file_id == FAQIUJI_AUDIO_FACTORY_SOUND_2) {
        base = FAQIUJI_AUDIO_FACTORY_2_BASE;
        size = FAQIUJI_AUDIO_FACTORY_2_SIZE;
    } else if (file_id == FAQIUJI_AUDIO_USER_FILE_ID) {
        base = FAQIUJI_AUDIO_USER_BASE;
        size = FAQIUJI_AUDIO_SLOT_SIZE;
    } else {
        return OPRT_INVALID_PARM;
    }

    if (offset > size || len > size - offset) return OPRT_INVALID_PARM;
    return faqiuji_ext_flash_read(base + offset, buf, len);
}

OPERATE_RET faqiuji_ext_flash_audio_write(UINT8_T file_id, UINT32_T offset, CONST UINT8_T *buf, UINT32_T len)
{
    if (file_id != FAQIUJI_AUDIO_USER_FILE_ID ||
        offset > FAQIUJI_AUDIO_SLOT_SIZE || len > FAQIUJI_AUDIO_SLOT_SIZE - offset) {
        return OPRT_INVALID_PARM;
    }
    return faqiuji_ext_flash_write(FAQIUJI_AUDIO_USER_BASE + offset, buf, len);
}
