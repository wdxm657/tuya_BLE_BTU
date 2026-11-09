/**
 * @file app_dp_parser.c
 * @brief This is app_dp_parser file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */


#include "string.h"

#include "tal_log.h"
#include "tal_util.h"

#include "tuya_ble_api.h"
#include "tuya_ble_mutli_tsf_protocol.h"

#include "app_dp_parser.h"
#include "app_state.h"
#include "app_motor.h"
#include "app_led.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/


/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/


/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
demo_dp_t g_cmd = {0};
demo_dp_t g_rsp = {0};
UINT32_T  g_sn  = 0;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/




OPERATE_RET app_dp_parser(UINT8_T* buf, UINT32_T size)
{
    memcpy(&g_cmd, buf, size);
    tal_util_reverse_byte(&g_cmd.dp_data_len, SIZEOF(UINT16_T));
    memcpy(&g_rsp, &g_cmd, size);

    TAL_PR_HEXDUMP_INFO("dp_cmd", (VOID_T*)&g_cmd, (g_cmd.dp_data_len + 4));

    switch (g_cmd.dp_id) {
        case DP_ID_SWITCH: {
            /* 蓝牙开关控制电源（与物理按键短按相同） */
            BOOL_T sw_on = (g_cmd.dp_data[0] != 0);
            TAL_PR_INFO("DP_ID_SWITCH: %s", sw_on ? "ON" : "OFF");
            app_state_set_power(sw_on);
        } break;

        case DP_ID_MODE: {
            /* 游戏模式设置 */
            game_mode_t mode = (game_mode_t)g_cmd.dp_data[0];
            TAL_PR_INFO("DP_ID_MODE: %d", mode);
            if (mode <= GAME_MODE_INTERACTIVE) {
                app_motor_set_mode(mode);
            }
        } break;

        case DP_ID_MOVEMENT_LEVEL: {
            TAL_PR_INFO("DP_ID_MOVEMENT_LEVEL: %d", g_cmd.dp_data[0]);
            /* 产品定义中未使用，保留扩展 */
        } break;

        default: {
            TAL_PR_DEBUG("invalid dp_id: %d", g_cmd.dp_id);
        } break;
    }

    /* 任何有效的 APP 指令都复位待机定时器 */
    app_state_reset_standby_timer();

    /* 对 rw DP 回复确认上报 */
    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);

    return OPRT_OK;
}

OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T* buf, UINT32_T size)
{
    memset(&g_rsp, 0, SIZEOF(demo_dp_t));

    g_rsp.dp_id = dp_id;

    switch (dp_id) {
        /* bool: 开关 */
        case DP_ID_SWITCH: {
            g_rsp.dp_type = DT_BOOL;
            g_rsp.dp_data_len = DT_BOOL_LEN;
            memcpy(g_rsp.dp_data, buf, DT_BOOL_LEN);
        } break;

        /* enum: 工作模式 active/simple/mild */
        case DP_ID_MODE: {
            g_rsp.dp_type = DT_ENUM;
            g_rsp.dp_data_len = DT_ENUM_LEN;
            memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        } break;

        /* value: 电池电量 0~100% */
        case DP_ID_BATTERY: {
            g_rsp.dp_type = DT_VALUE;
            g_rsp.dp_data_len = DT_VALUE_LEN;
            memcpy(g_rsp.dp_data, buf, DT_VALUE_LEN);
        } break;

        /* bitmap: 故障告警 */
        case DP_ID_FAULT: {
            g_rsp.dp_type = DT_BITMAP;
            g_rsp.dp_data_len = DT_BITMAP_MAX;
            memcpy(g_rsp.dp_data, buf, DT_BITMAP_MAX);
        } break;

        /* enum: 行进速度 level_1~level_5 */
        case DP_ID_MOVEMENT_LEVEL: {
            g_rsp.dp_type = DT_ENUM;
            g_rsp.dp_data_len = DT_ENUM_LEN;
            memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        } break;

        /* enum: 工作状态 work/standby/charging/charge_done */
        case DP_ID_WORK_STATE: {
            g_rsp.dp_type = DT_ENUM;
            g_rsp.dp_data_len = DT_ENUM_LEN;
            memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        } break;

        default: {
        } break;
    }

    UINT16_T rsp_len = g_rsp.dp_data_len + 4;

    tal_util_reverse_byte(&g_rsp.dp_data_len, SIZEOF(UINT16_T));

    TAL_PR_HEXDUMP_INFO("dp_rsp", (VOID_T*)&g_rsp, rsp_len);

    return tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE, DP_SEND_FOR_CLOUD_PANEL, DP_SEND_WITHOUT_RESPONSE, (VOID_T*)&g_rsp, rsp_len);
}

