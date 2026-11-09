/**
 * @file app_dp_parser.h
 * @brief This is app_dp_parser file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __APP_DP_PARSER_H__
#define __APP_DP_PARSER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
/** DP ID 定义 — 来自 afp FIFA football toy 产品配置 */
/* rw: 可读写, ro: 只读 */
#define  DP_ID_SWITCH            1   /* bool, rw — 开关 */
#define  DP_ID_MODE              4   /* enum, rw — 工作模式: active/simple/mild */
#define  DP_ID_BATTERY          18   /* value, ro — 电池电量 0~100% */
#define  DP_ID_FAULT            20   /* bitmap, ro — 故障告警: bit0=battery_low, bit1=device_stuck */
#define  DP_ID_MOVEMENT_LEVEL   22   /* enum, rw — 行进速度: level_1~level_5 */
#define  DP_ID_WORK_STATE       26   /* enum, ro — 工作状态: work/standby/charging/charge_done */

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/
#pragma pack(1)
typedef struct {
    UINT8_T  dp_id;
    UINT8_T  dp_type;
    UINT16_T dp_data_len;
    UINT8_T  dp_data[600];
} demo_dp_t;
#pragma pack()

/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
extern demo_dp_t g_cmd;
extern demo_dp_t g_rsp;
extern UINT32_T  g_sn;

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
OPERATE_RET app_dp_parser(UINT8_T* buf, UINT32_T size);

/**
 * @brief app_dp_report
 *
 * @param[in] dp_id: dp_id
 * @param[in] buf: buf
 * @param[in] size: size
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T* buf, UINT32_T size);


#ifdef __cplusplus
}
#endif

#endif /* __APP_DP_PARSER_H__ */

