/**
 * @file tuya_ble_attach_ota_port.c
 * @brief This is tuya_ble_attach_ota_port file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#include "board.h"

#include "tal_flash.h"
#include "tal_util.h"
#include "tuya_ble_api.h"
#include "tuya_ble_attach_ota_port.h"

#if defined(TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE) && (TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE == 1)

#if defined(FAQIUJI) && (FAQIUJI == 1)
#include "faqiuji_mcu_protocol.h"
#include "tkl_system.h"

#if TUYA_BLE_ATTACH_OTA_PORT_NUM < 2
#error "Channel 10 requires attachment index 1"
#endif

typedef struct {
    UINT32_T *p_attach_ota_idx;
    UINT32_T attach_ota_version;
    UINT32_T *p_firmware_crc32;
    UINT8_T *p_firmware_md5;
} FAQIUJI_OTA_PRIVATE_T;

TUYA_BLE_ATTACH_OTA_INFO_T tuya_ble_attach_ota_info[TUYA_BLE_ATTACH_OTA_PORT_NUM];
STATIC UINT32_T sg_port_idx = 1U;
UINT32_T *p_attach_ota_idx = &sg_port_idx;
STATIC struct {
    UINT32_T running_base, base, capacity, chunk, size, offset, crc, expected_crc;
    UINT32_T refresh_tick;
    UINT8_T active, begun, refresh, protocol;
} sg_puya;

STATIC UINT32_T puya_read32(CONST UINT8_T *p)
{
    return (UINT32_T)p[0] | ((UINT32_T)p[1] << 8) |
           ((UINT32_T)p[2] << 16) | ((UINT32_T)p[3] << 24);
}

STATIC VOID_T puya_write32(UINT8_T *p, UINT32_T value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

STATIC OPERATE_RET puya_command(UINT8_T cmd, CONST UINT8_T *data,
                               UINT8_T len, UINT32_T offset)
{
    FAQIUJI_MCU_FRAME_T reply;
    UINT32_T timeout = cmd == FAQIUJI_MCU_CMD_OTA_END ? 1000U : 150U;
    UINT32_T start = tkl_system_get_millisecond();
    OPERATE_RET ret = faqiuji_mcu_send_wait_response(cmd, data, len, &reply, timeout);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("PUYA OTA UART cmd=0x%02x failed ret=%d expected_offset=%u",
                   cmd, ret, offset);
        TAL_PR_ERR("PUYA OTA UART elapsed=%u ms timeout_per_attempt=%u ms",
                   tkl_system_get_millisecond() - start, timeout);
        return ret;
    }
    if (reply.len != 5U || reply.payload[0] != 0U ||
        puya_read32(reply.payload + 1) != offset) {
        TAL_PR_ERR("PUYA OTA UART cmd=0x%02x reply len=%u status=%u",
                   cmd, reply.len, reply.len ? reply.payload[0] : 0xFFU);
        if (reply.len == 5U) {
            TAL_PR_ERR("PUYA OTA UART offset=%u expected=%u",
                       puya_read32(reply.payload + 1), offset);
        }
        return OPRT_COM_ERROR;
    }
    if (cmd == FAQIUJI_MCU_CMD_OTA_END) {
        TAL_PR_INFO("PUYA OTA END acknowledged in %u ms",
                    tkl_system_get_millisecond() - start);
    }
    return OPRT_OK;
}

UINT32_T tuya_ble_attach_ota_port_info_save(VOID_T)
{
    /* PUYA owns image metadata; interrupted transfers restart at offset zero. */
    return OPRT_OK;
}

UINT32_T tuya_ble_attach_ota_port_info_load(VOID_T)
{
    FAQIUJI_MCU_FRAME_T reply;
    OPERATE_RET ret = faqiuji_mcu_send_wait_response(
        FAQIUJI_MCU_CMD_OTA_INFO, NULL, 0, &reply, 150U);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("PUYA OTA INFO failed ret=%d", ret);
        return ret;
    }
    if (reply.len != 24U || reply.payload[0] != 0U ||
        (reply.payload[1] != 1U &&
         reply.payload[1] != FAQIUJI_MCU_OTA_PROTOCOL_VERSION) ||
        reply.payload[19] != 0U ||
        reply.payload[18] == 0U) {
        TAL_PR_ERR("PUYA OTA INFO invalid len=%u", reply.len);
        return OPRT_COM_ERROR;
    }
    sg_puya.running_base = puya_read32(reply.payload + 2);
    sg_puya.protocol = reply.payload[1];
    sg_puya.base = puya_read32(reply.payload + 6);
    sg_puya.capacity = puya_read32(reply.payload + 10);
    sg_puya.chunk = reply.payload[18];
    if (sg_puya.chunk > FAQIUJI_MCU_OTA_DATA_MAX) {
        sg_puya.chunk = FAQIUJI_MCU_OTA_DATA_MAX;
    }
    tuya_ble_attach_ota_info[1].type = 10U;
    tuya_ble_attach_ota_info[1].version = puya_read32(reply.payload + 14);
    TAL_PR_INFO("PUYA firmware version: %u.%u.%u",
                (tuya_ble_attach_ota_info[1].version >> 16) & 0xFFU,
                (tuya_ble_attach_ota_info[1].version >> 8) & 0xFFU,
                tuya_ble_attach_ota_info[1].version & 0xFFU);
    TAL_PR_INFO("PUYA OTA running=0x%08x target=0x%08x capacity=%u chunk=%u",
                sg_puya.running_base, sg_puya.base, sg_puya.capacity, sg_puya.chunk);
    tuya_ble_device_update_attach_version(&tuya_ble_attach_ota_info[1], 1);
    return OPRT_OK;
}

/* Called from the application loop, after PUYA has had time to reboot. */
VOID_T tuya_ble_attach_ota_port_process(VOID_T)
{
    if (!sg_puya.active && sg_puya.refresh &&
        tkl_system_get_millisecond() - sg_puya.refresh_tick >= 1000U) {
        sg_puya.refresh_tick = tkl_system_get_millisecond();
        if (tuya_ble_attach_ota_port_info_load() == OPRT_OK) {
            sg_puya.refresh = 0U;
        } else {
            --sg_puya.refresh;
        }
    }
}

OPERATE_RET tuya_ble_attach_ota_port_get_old_firmware_info(
    TUYA_OTA_FIRMWARE_INFO_T **info)
{
    if (info == NULL) {
        return OPRT_INVALID_PARM;
    }
    memset(&tuya_ble_attach_ota_info[1].firmware_info, 0,
           sizeof(TUYA_OTA_FIRMWARE_INFO_T));
    *info = &tuya_ble_attach_ota_info[1].firmware_info;
    sg_puya.refresh_tick = tkl_system_get_millisecond();
    sg_puya.refresh = 5U;
    return OPRT_OK;
}

OPERATE_RET tuya_ble_attach_ota_port_start_notify(
    UINT_T image_size, TUYA_OTA_TYPE_E type, TUYA_OTA_PATH_E path)
{
    if (sg_puya.active || tuya_ble_attach_ota_port_info_load() != OPRT_OK) {
        return OPRT_COM_ERROR;
    }
    if (sg_puya.protocol != FAQIUJI_MCU_OTA_PROTOCOL_VERSION) {
        TAL_PR_ERR("PUYA OTA requires unified-image APP and IAP; protocol=%u",
                   sg_puya.protocol);
        return OPRT_NOT_SUPPORTED;
    }
    if (sg_puya.running_base != FAQIUJI_MCU_OTA_EXEC_BASE ||
        sg_puya.base != FAQIUJI_MCU_OTA_EXEC_BASE + sg_puya.capacity) {
        TAL_PR_ERR("PUYA OTA invalid run/download layout");
        return OPRT_INVALID_PARM;
    }
    sg_puya.active = 1U;
    sg_puya.begun = 0U;
    sg_puya.offset = 0U;
    sg_puya.crc = 0U;
    sg_puya.size = 0U;
    return OPRT_OK;
}

OPERATE_RET tuya_ble_attach_ota_port_data_process(
    TUYA_OTA_DATA_T *pack, UINT32_T *remain_len)
{
    FAQIUJI_OTA_PRIVATE_T *priv;
    UINT8_T buffer[FAQIUJI_MCU_MAX_PAYLOAD];
    UINT32_T pos = 0U, count, reset_vector;
    OPERATE_RET ret;
    if (pack == NULL || pack->data == NULL || pack->pri_data == NULL) {
        TAL_PR_ERR("PUYA OTA invalid data or private parameter");
        return OPRT_INVALID_PARM;
    }
    if (!sg_puya.active || pack->len == 0U ||
        pack->offset != sg_puya.offset || pack->total_len < 8U ||
        pack->total_len > sg_puya.capacity || pack->offset > pack->total_len ||
        pack->len > pack->total_len - pack->offset) {
        TAL_PR_ERR("PUYA OTA invalid packet active=%u offset=%u expected=%u",
                   sg_puya.active, pack->offset, sg_puya.offset);
        TAL_PR_ERR("PUYA OTA packet len=%u total=%u capacity=%u",
                   pack->len, pack->total_len, sg_puya.capacity);
        return OPRT_INVALID_PARM;
    }
    priv = pack->pri_data;
    if (priv->p_attach_ota_idx == NULL || *priv->p_attach_ota_idx != 1U ||
        priv->p_firmware_crc32 == NULL) {
        TAL_PR_ERR("PUYA OTA invalid channel index or firmware CRC pointer");
        return OPRT_INVALID_PARM;
    }
    if (!sg_puya.begun) {
        if (pack->offset != 0U || pack->len < 8U) {
            TAL_PR_ERR("PUYA OTA first packet needs vector table: offset=%u len=%u",
                       pack->offset, pack->len);
            return OPRT_INVALID_PARM;
        }
        reset_vector = puya_read32(pack->data + 4);
        if (!(reset_vector & 1U) ||
            (reset_vector & ~1U) < FAQIUJI_MCU_OTA_EXEC_BASE ||
            (reset_vector & ~1U) - FAQIUJI_MCU_OTA_EXEC_BASE >= pack->total_len) {
            TAL_PR_ERR("PUYA OTA invalid unified image: reset=0x%08x size=%u",
                       reset_vector, pack->total_len);
            TAL_PR_ERR("PUYA OTA image must be linked at 0x%08x",
                       (UINT32_T)FAQIUJI_MCU_OTA_EXEC_BASE);
            return OPRT_INVALID_PARM;
        }
        sg_puya.size = pack->total_len;
        sg_puya.expected_crc = *priv->p_firmware_crc32;
        puya_write32(buffer, sg_puya.base);
        puya_write32(buffer + 4, sg_puya.size);
        puya_write32(buffer + 8, sg_puya.expected_crc);
        puya_write32(buffer + 12, priv->attach_ota_version);
        /* BEGIN may succeed even if its ACK is lost; abort on any later error. */
        sg_puya.begun = 1U;
        ret = puya_command(FAQIUJI_MCU_CMD_OTA_BEGIN, buffer, 16U, 0U);
        if (ret != OPRT_OK) {
            return ret;
        }
    }
    if (pack->total_len != sg_puya.size) {
        TAL_PR_ERR("PUYA OTA total length changed: %u expected=%u",
                   pack->total_len, sg_puya.size);
        return OPRT_INVALID_PARM;
    }
    while (pos < pack->len) {
        count = pack->len - pos;
        if (count > sg_puya.chunk) {
            count = sg_puya.chunk;
        }
        puya_write32(buffer, sg_puya.offset);
        memcpy(buffer + 4, pack->data + pos, count);
        ret = puya_command(FAQIUJI_MCU_CMD_OTA_DATA, buffer, count + 4U,
                           sg_puya.offset + count);
        if (ret != OPRT_OK) {
            return ret;
        }
        sg_puya.offset += count;
        pos += count;
    }
    sg_puya.crc = tal_util_crc32(pack->data, pack->len, &sg_puya.crc);
    if (remain_len != NULL) {
        *remain_len = sg_puya.size - sg_puya.offset;
    }
    return OPRT_OK;
}

OPERATE_RET tuya_ble_attach_ota_port_end_notify(BOOL_T reset)
{
    OPERATE_RET ret;
    if (!reset) {
        if (sg_puya.begun) {
            /* ABORT's offset is irrelevant after a lost data ACK. */
            FAQIUJI_MCU_FRAME_T reply;
            ret = faqiuji_mcu_send_wait_response(
                FAQIUJI_MCU_CMD_OTA_ABORT, NULL, 0, &reply, 1000U);
            sg_puya.active = sg_puya.begun = 0U;
            return ret;
        }
        sg_puya.active = 0U;
        return OPRT_OK;
    }
    if (!sg_puya.active || !sg_puya.begun ||
        sg_puya.offset != sg_puya.size) {
        return 1;
    }
    if (sg_puya.crc != sg_puya.expected_crc) {
        return 2;
    }
    ret = puya_command(FAQIUJI_MCU_CMD_OTA_END, NULL, 0, sg_puya.size);
    if (ret == OPRT_OK) {
        ret = puya_command(FAQIUJI_MCU_CMD_OTA_REBOOT, NULL, 0, sg_puya.size);
    }
    if (ret == OPRT_OK) {
        sg_puya.active = sg_puya.begun = 0U;
    }
    return ret;
}

#else
/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
#define TUYA_BLE_ATTACH_OTA_PORT_INFO_ADDR          BOARD_FLASH_SDK_TEST_START_ADDR
#define TUYA_BLE_ATTACH_OTA_PORT_SIZE               (0x1000000)
#define TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE     TUYA_NV_ERASE_MIN_SIZE
#define TUYA_BLE_ATTACH_OTA_PORT_FILE_MD5_LEN       (16)

#define TUYA_BLE_ATTACH_OTA_PORT_MCU_FVER_NUM       0x00010000
#define TUYA_BLE_ATTACH_OTA_PORT_ATTACH_FVER_NUM    0x00010000

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/
typedef struct {
    UINT32_T* p_attach_ota_idx;
    UINT32_T  attach_ota_version;
    UINT32_T* p_firmware_crc32;
    UINT8_T*  p_firmware_md5;
} tuya_ble_attach_ota_pri_param_t;

/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
STATIC UINT32_T sg_firmware_len = 0;
STATIC UINT32_T sg_data_crc32 = 0;
STATIC tuya_ble_attach_ota_pri_param_t *tuya_ble_attach_ota_pri_param __attribute__((aligned(4))) = NULL;

TUYA_BLE_ATTACH_OTA_INFO_T tuya_ble_attach_ota_info[TUYA_BLE_ATTACH_OTA_PORT_NUM] = {0};
UINT32_T  sg_attach_ota_idx = 0;
UINT32_T* p_attach_ota_idx = &sg_attach_ota_idx;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/




UINT32_T tuya_ble_attach_ota_port_info_save(VOID_T)
{
    tal_flash_erase(TUYA_BLE_ATTACH_OTA_PORT_INFO_ADDR, TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE);
    tal_flash_write(TUYA_BLE_ATTACH_OTA_PORT_INFO_ADDR, (VOID_T*)&(*p_attach_ota_idx), SIZEOF(UINT32_T));
    tal_flash_write(TUYA_BLE_ATTACH_OTA_PORT_INFO_ADDR + 4, (VOID_T*)tuya_ble_attach_ota_info, SIZEOF(TUYA_BLE_ATTACH_OTA_INFO_T)*TUYA_BLE_ATTACH_OTA_PORT_NUM);
    return OPRT_OK;
}

UINT32_T tuya_ble_attach_ota_port_info_load(VOID_T)
{
    tal_flash_read(TUYA_BLE_ATTACH_OTA_PORT_INFO_ADDR, (VOID_T*)&(*p_attach_ota_idx), SIZEOF(UINT32_T));
    if ((*p_attach_ota_idx) < TUYA_BLE_ATTACH_OTA_PORT_NUM) {
        tal_flash_read(TUYA_BLE_ATTACH_OTA_PORT_INFO_ADDR + 4, (VOID_T*)tuya_ble_attach_ota_info, SIZEOF(TUYA_BLE_ATTACH_OTA_INFO_T)*TUYA_BLE_ATTACH_OTA_PORT_NUM);
    } else {
        (*p_attach_ota_idx) = 0;
        for (UINT32_T idx=0; idx<TUYA_BLE_ATTACH_OTA_PORT_NUM; idx++) {
            if (idx == 0) {
                tuya_ble_attach_ota_info[idx].type = 1;
                tuya_ble_attach_ota_info[idx].version = TUYA_BLE_ATTACH_OTA_PORT_MCU_FVER_NUM;
            } else {
                tuya_ble_attach_ota_info[idx].type = idx + 9;
                tuya_ble_attach_ota_info[idx].version = TUYA_BLE_ATTACH_OTA_PORT_ATTACH_FVER_NUM;
            }
            memset(&tuya_ble_attach_ota_info[idx].firmware_info, 0, sizeof(TUYA_OTA_FIRMWARE_INFO_T));
        }
        tuya_ble_attach_ota_port_info_save();
    }

//    TAL_PR_INFO("sg_attach_ota_idx-%d", (*p_attach_ota_idx));
//    for (UINT32_T idx=0; idx<TUYA_BLE_ATTACH_OTA_PORT_NUM; idx++) {
//        TAL_PR_INFO("idx-%d, type-%02d, version-%08x, len-%08x, crc32-%08x", idx, tuya_ble_attach_ota_info[idx].type, tuya_ble_attach_ota_info[idx].version,
//            tuya_ble_attach_ota_info[idx].firmware_info.len, tuya_ble_attach_ota_info[idx].firmware_info.crc32);
//    }

    return OPRT_OK;
}

OPERATE_RET tuya_ble_attach_ota_port_start_notify(UINT_T image_size, TUYA_OTA_TYPE_E type, TUYA_OTA_PATH_E path)
{
    return OPRT_OK;
}

OPERATE_RET tuya_ble_attach_ota_port_data_process(TUYA_OTA_DATA_T *pack, UINT32_T* remain_len)
{
    OPERATE_RET ret = OPRT_OK;

    tuya_ble_attach_ota_pri_param = pack->pri_data;

    if ((*p_attach_ota_idx) != (*tuya_ble_attach_ota_pri_param->p_attach_ota_idx)) {
        TAL_PR_ERR("tuya_ble_attach_ota_port_data_process type error");
        return OPRT_INVALID_PARM;
    }
    p_attach_ota_idx = tuya_ble_attach_ota_pri_param->p_attach_ota_idx;

    BOOL_T flag_4k = FALSE;
    if ((pack->offset == 0) || ((pack->offset + pack->len) >= (((pack->offset/TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE) + 1)*TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE))) {
        // Step1: Erase Flash
//        if (pack->offset == 0) {
//            tal_flash_erase(BOARD_FLASH_OTA_START_ADDR, TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE);
//        } else {
//            UINT32_T erase_addr = BOARD_FLASH_OTA_START_ADDR + (((pack->offset/TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE) + 1)*TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE);
//            tal_flash_erase(erase_addr, TUYA_BLE_ATTACH_OTA_PORT_ERASE_MIN_SIZE);
//        }

        flag_4k = TRUE;
    }

    sg_data_crc32 = tal_util_crc32(pack->data, pack->len, &sg_data_crc32);

    // Step2: Write Flash
//    tal_flash_write(BOARD_FLASH_OTA_START_ADDR + pack->offset, pack->data, pack->len);

    sg_firmware_len = pack->total_len;

    memcpy(tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.md5, tuya_ble_attach_ota_pri_param->p_firmware_md5, TUYA_BLE_ATTACH_OTA_PORT_FILE_MD5_LEN);
    tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.len = pack->offset + pack->len;
    tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.crc32 = sg_data_crc32;

    if (flag_4k) {
        tuya_ble_attach_ota_port_info_save();
    }

    return ret;
}

OPERATE_RET tuya_ble_attach_ota_port_end_notify(BOOL_T reset)
{
    OPERATE_RET ret = OPRT_OK;

    if (sg_firmware_len != tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.len) {
        return 1;
    }

    if (sg_data_crc32 != *(tuya_ble_attach_ota_pri_param->p_firmware_crc32)) {
        return 2;
    }

    sg_firmware_len = 0;
    sg_data_crc32 = 0;

    tuya_ble_attach_ota_info[(*p_attach_ota_idx)].version = tuya_ble_attach_ota_pri_param->attach_ota_version;

    (*p_attach_ota_idx)++;

    tuya_ble_attach_ota_port_info_save();

    return ret;
}

OPERATE_RET tuya_ble_attach_ota_port_get_old_firmware_info(TUYA_OTA_FIRMWARE_INFO_T **info)
{
    OPERATE_RET ret = OPRT_OK;

    tuya_ble_attach_ota_port_info_load();

    tuya_ble_device_update_mcu_version(tuya_ble_attach_ota_info[0].version, 0x00010000);
    tuya_ble_device_update_attach_version(&tuya_ble_attach_ota_info[1], TUYA_BLE_ATTACH_OTA_PORT_NUM-1);

    *info = &tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info;

    if (tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.len <= TUYA_BLE_ATTACH_OTA_PORT_SIZE) {
        sg_data_crc32 = tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.crc32;
    }

    return ret;
}

#endif /* FAQIUJI */
#endif // TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE

