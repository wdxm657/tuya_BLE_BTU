/**
 * @file tuya_ble_attach_ota_port.h
 * @brief This is tuya_ble_attach_ota_port file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __TUYA_BLE_ATTACH_OTA_PORT_H__
#define __TUYA_BLE_ATTACH_OTA_PORT_H__

#include "tuya_cloud_types.h"
#include "tuya_ble_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE) && (TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE == 1)

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
#ifndef TUYA_BLE_ATTACH_OTA_PORT_NUM
#define TUYA_BLE_ATTACH_OTA_PORT_NUM                (11) // min 1
#endif

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/
typedef struct {
    UINT32_T type:8;
    UINT32_T version:24;
    TUYA_OTA_FIRMWARE_INFO_T firmware_info;
} TUYA_BLE_ATTACH_OTA_INFO_T;

/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
extern TUYA_BLE_ATTACH_OTA_INFO_T tuya_ble_attach_ota_info[];
extern UINT32_T* p_attach_ota_idx;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief tuya_ble_attach_ota_port_info_save
 *
 * @param[in] param: none
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
UINT32_T tuya_ble_attach_ota_port_info_save(VOID_T);

/**
 * @brief tuya_ble_attach_ota_port_info_load
 *
 * @param[in] param: none
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
UINT32_T tuya_ble_attach_ota_port_info_load(VOID_T);

#if defined(FAQIUJI) && (FAQIUJI == 1)
VOID_T tuya_ble_attach_ota_port_process(VOID_T);
#endif

/**
 * @brief tuya_ble_attach_ota_port_start_notify
 *
 * @param[in] image_size: image_size
 * @param[in] type: type
 * @param[in] path: path
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET tuya_ble_attach_ota_port_start_notify(UINT_T image_size, TUYA_OTA_TYPE_E type, TUYA_OTA_PATH_E path);

/**
 * @brief tuya_ble_attach_ota_port_data_process
 *
 * @param[in] *pack: *pack
 * @param[in] remain_len: remain_len
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET tuya_ble_attach_ota_port_data_process(TUYA_OTA_DATA_T *pack, UINT32_T* remain_len);

/**
 * @brief tuya_ble_attach_ota_port_end_notify
 *
 * @param[in] reset: reset
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET tuya_ble_attach_ota_port_end_notify(BOOL_T reset);

/**
 * @brief tuya_ble_attach_ota_port_get_old_firmware_info
 *
 * @param[in] **info: **info
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET tuya_ble_attach_ota_port_get_old_firmware_info(TUYA_OTA_FIRMWARE_INFO_T **info);

#endif // TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE


#ifdef __cplusplus
}
#endif

#endif /* __TUYA_BLE_ATTACH_OTA_PORT_H__ */

