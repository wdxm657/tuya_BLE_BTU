/**
 * @file tuya_sdk_callback.h
 * @brief This is tuya_sdk_callback file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __TUYA_SDK_CALLBACK_H__
#define __TUYA_SDK_CALLBACK_H__

#include "tuya_cloud_types.h"
#include "tal_uart.h"
#include "tal_bluetooth.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/


/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/


/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
extern TAL_UART_CFG_T tal_uart_cfg;
extern TAL_BLE_ADV_PARAMS_T tal_adv_param;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief
 *
 * @param[in] param1:
 * @param[in] param2:
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
UINT16_T tuya_app_get_conn_handle(VOID_T);


#ifdef __cplusplus
}
#endif

#endif /* __TUYA_SDK_CALLBACK_H__ */

