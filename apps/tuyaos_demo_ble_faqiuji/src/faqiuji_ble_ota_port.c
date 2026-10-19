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
#if defined(FAQIUJI) && (FAQIUJI == 1)
#include "faqiuji_mcu_protocol.h"
#endif

#if defined(TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE) && (TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE == 1)

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
#if defined(FAQIUJI) && (FAQIUJI == 1)
STATIC UINT32_T sg_puya_inactive_base = 0U;
STATIC UINT32_T sg_puya_slot_size = 0U;
STATIC UINT32_T sg_puya_version = 0x00010000UL;
STATIC UINT32_T sg_puya_target_version = 0U;
STATIC BOOL_T sg_puya_ota_started = FALSE;
#endif

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
#if defined(FAQIUJI) && (FAQIUJI == 1)
    FAQIUJI_MCU_FRAME_T response = {0};
    OPERATE_RET ret;

    (VOID_T)image_size;
    (VOID_T)type;
    (VOID_T)path;
    sg_puya_ota_started = FALSE;

    ret = faqiuji_mcu_send_wait_response(FAQIUJI_MCU_CMD_OTA_INFO,
                                         NULL, 0U, &response, 800U);
    if (ret != OPRT_OK || response.len < 24U || response.payload[0] != 0U ||
        response.payload[19] != 0U) {
        TAL_PR_ERR("PUYA OTA_INFO failed ret=%d len=%u status=%u",
                   ret, response.len, response.len ? response.payload[0] : 0xFFU);
        return OPRT_COM_ERROR;
    }

    sg_puya_inactive_base = ((UINT32_T)response.payload[6]) |
                            ((UINT32_T)response.payload[7] << 8U) |
                            ((UINT32_T)response.payload[8] << 16U) |
                            ((UINT32_T)response.payload[9] << 24U);
    sg_puya_slot_size = ((UINT32_T)response.payload[10]) |
                        ((UINT32_T)response.payload[11] << 8U) |
                        ((UINT32_T)response.payload[12] << 16U) |
                        ((UINT32_T)response.payload[13] << 24U);
    if ((sg_puya_inactive_base != 0x08004000UL &&
         sg_puya_inactive_base != 0x08010000UL) ||
        sg_puya_slot_size == 0U || sg_puya_slot_size > 0x0000C000UL) {
        TAL_PR_ERR("PUYA OTA_INFO invalid base=0x%08x size=%u",
                   sg_puya_inactive_base, sg_puya_slot_size);
        return OPRT_COM_ERROR;
    }
    tuya_ble_attach_ota_info[0].version = ((UINT32_T)response.payload[14]) |
                                          ((UINT32_T)response.payload[15] << 8U) |
                                          ((UINT32_T)response.payload[16] << 16U) |
                                          ((UINT32_T)response.payload[17] << 24U);
    sg_puya_version = tuya_ble_attach_ota_info[0].version;
    TAL_PR_INFO("PUYA OTA ready: inactive=0x%08x size=%u version=0x%08x",
                sg_puya_inactive_base, sg_puya_slot_size,
                tuya_ble_attach_ota_info[0].version);
    return OPRT_OK;
#else
    (VOID_T)image_size;
    (VOID_T)type;
    (VOID_T)path;
    return OPRT_OK;
#endif
}

OPERATE_RET tuya_ble_attach_ota_port_data_process(TUYA_OTA_DATA_T *pack, UINT32_T* remain_len)
{
#if defined(FAQIUJI) && (FAQIUJI == 1)
    FAQIUJI_MCU_FRAME_T response = {0};
    UINT8_T payload[4U + FAQIUJI_MCU_OTA_DATA_MAX];
    UINT32_T offset;
    UINT32_T chunk;
    UINT32_T image_crc;
    UINT32_T image_version;
    UINT32_T cursor;
    OPERATE_RET ret;
    typedef struct {
        UINT32_T *p_attach_ota_idx;
        UINT32_T attach_ota_version;
        UINT32_T *p_firmware_crc32;
        UINT8_T *p_firmware_md5;
    } FAQIUJI_OTA_PRIVATE_T;
    FAQIUJI_OTA_PRIVATE_T *private_data;

    (VOID_T)remain_len;
    if (pack == NULL || pack->data == NULL || pack->len == 0U ||
        pack->total_len == 0U || pack->offset > pack->total_len ||
        pack->len > pack->total_len - pack->offset ||
        pack->total_len > sg_puya_slot_size) {
        return OPRT_INVALID_PARM;
    }
    private_data = (FAQIUJI_OTA_PRIVATE_T *)pack->pri_data;
    if (private_data == NULL || private_data->p_firmware_crc32 == NULL) {
        return OPRT_INVALID_PARM;
    }

    image_crc = *private_data->p_firmware_crc32;
    image_version = private_data->attach_ota_version;
    sg_puya_target_version = image_version;
    if (!sg_puya_ota_started) {
        UINT8_T begin_payload[16];
        if (pack->offset != 0U) {
            return OPRT_COM_ERROR;
        }
        begin_payload[0] = (UINT8_T)sg_puya_inactive_base;
        begin_payload[1] = (UINT8_T)(sg_puya_inactive_base >> 8U);
        begin_payload[2] = (UINT8_T)(sg_puya_inactive_base >> 16U);
        begin_payload[3] = (UINT8_T)(sg_puya_inactive_base >> 24U);
        begin_payload[4] = (UINT8_T)pack->total_len;
        begin_payload[5] = (UINT8_T)(pack->total_len >> 8U);
        begin_payload[6] = (UINT8_T)(pack->total_len >> 16U);
        begin_payload[7] = (UINT8_T)(pack->total_len >> 24U);
        begin_payload[8] = (UINT8_T)image_crc;
        begin_payload[9] = (UINT8_T)(image_crc >> 8U);
        begin_payload[10] = (UINT8_T)(image_crc >> 16U);
        begin_payload[11] = (UINT8_T)(image_crc >> 24U);
        begin_payload[12] = (UINT8_T)image_version;
        begin_payload[13] = (UINT8_T)(image_version >> 8U);
        begin_payload[14] = (UINT8_T)(image_version >> 16U);
        begin_payload[15] = (UINT8_T)(image_version >> 24U);

        ret = faqiuji_mcu_send_wait_response(FAQIUJI_MCU_CMD_OTA_BEGIN,
                                             begin_payload,
                                             (UINT8_T)sizeof(begin_payload),
                                             &response, 1000U);
        if (ret != OPRT_OK || response.len < 5U || response.payload[0] != 0U) {
            TAL_PR_ERR("PUYA OTA_BEGIN failed ret=%d status=%u",
                       ret, response.len ? response.payload[0] : 0xFFU);
            return OPRT_COM_ERROR;
        }
        sg_puya_ota_started = TRUE;
    }

    offset = pack->offset;
    cursor = 0U;
    while (cursor < pack->len) {
        chunk = pack->len - cursor;
        if (chunk > FAQIUJI_MCU_OTA_DATA_MAX) {
            chunk = FAQIUJI_MCU_OTA_DATA_MAX;
        }
        payload[0] = (UINT8_T)offset;
        payload[1] = (UINT8_T)(offset >> 8U);
        payload[2] = (UINT8_T)(offset >> 16U);
        payload[3] = (UINT8_T)(offset >> 24U);
        memcpy(&payload[4], &pack->data[cursor], chunk);
        ret = faqiuji_mcu_send_wait_response(FAQIUJI_MCU_CMD_OTA_DATA,
                                             payload, (UINT8_T)(chunk + 4U),
                                             &response, 1000U);
        if (ret != OPRT_OK || response.len < 5U || response.payload[0] != 0U ||
            (((UINT32_T)response.payload[1]) |
             ((UINT32_T)response.payload[2] << 8U) |
             ((UINT32_T)response.payload[3] << 16U) |
             ((UINT32_T)response.payload[4] << 24U)) != offset + chunk) {
            TAL_PR_ERR("PUYA OTA_DATA failed ret=%d offset=%u chunk=%u",
                       ret, offset, chunk);
            return OPRT_COM_ERROR;
        }
        offset += chunk;
        cursor += chunk;
    }
    return OPRT_OK;
#else
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
#endif
}

OPERATE_RET tuya_ble_attach_ota_port_end_notify(BOOL_T reset)
{
#if defined(FAQIUJI) && (FAQIUJI == 1)
    FAQIUJI_MCU_FRAME_T response = {0};
    OPERATE_RET ret;

    if (!sg_puya_ota_started) {
        return reset ? OPRT_COM_ERROR : OPRT_OK;
    }
    if (!reset) {
        (VOID_T)faqiuji_mcu_send_wait_response(FAQIUJI_MCU_CMD_OTA_ABORT,
                                               NULL, 0U, &response, 500U);
        sg_puya_ota_started = FALSE;
        return OPRT_OK;
    }

    ret = faqiuji_mcu_send_wait_response(FAQIUJI_MCU_CMD_OTA_END,
                                         NULL, 0U, &response, 1500U);
    if (ret != OPRT_OK || response.len < 5U || response.payload[0] != 0U) {
        TAL_PR_ERR("PUYA OTA_END failed ret=%d status=%u",
                   ret, response.len ? response.payload[0] : 0xFFU);
        return OPRT_COM_ERROR;
    }
    sg_puya_version = sg_puya_target_version;
    ret = faqiuji_mcu_send_wait_response(FAQIUJI_MCU_CMD_OTA_REBOOT,
                                         NULL, 0U, &response, 500U);
    sg_puya_ota_started = FALSE;
    return (ret == OPRT_OK && response.len >= 5U && response.payload[0] == 0U) ?
           OPRT_OK : OPRT_COM_ERROR;
#else
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
#endif
}

OPERATE_RET tuya_ble_attach_ota_port_get_old_firmware_info(TUYA_OTA_FIRMWARE_INFO_T **info)
{
#if defined(FAQIUJI) && (FAQIUJI == 1)
    if (info == NULL) {
        return OPRT_INVALID_PARM;
    }
    tuya_ble_attach_ota_info[0].type = 1U;
    tuya_ble_attach_ota_info[0].version = sg_puya_version;
    tuya_ble_device_update_mcu_version(tuya_ble_attach_ota_info[0].version,
                                       0x00010000UL);
    tuya_ble_device_update_attach_version(&tuya_ble_attach_ota_info[0], 1U);
    *info = &tuya_ble_attach_ota_info[0].firmware_info;
    return OPRT_OK;
#else
    OPERATE_RET ret = OPRT_OK;

    tuya_ble_attach_ota_port_info_load();

    tuya_ble_device_update_mcu_version(tuya_ble_attach_ota_info[0].version, 0x00010000);
    tuya_ble_device_update_attach_version(&tuya_ble_attach_ota_info[1], TUYA_BLE_ATTACH_OTA_PORT_NUM-1);

    *info = &tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info;

    if (tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.len <= TUYA_BLE_ATTACH_OTA_PORT_SIZE) {
        sg_data_crc32 = tuya_ble_attach_ota_info[(*p_attach_ota_idx)].firmware_info.crc32;
    }

    return ret;
#endif
}

#endif // TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE

