/**
 * @file app_dp_parser.c
 * @brief afp pawswiff DP parser.
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
#include "app_battery.h"

demo_dp_t g_cmd = {0};
demo_dp_t g_rsp = {0};
UINT32_T g_sn = 0;

STATIC VOID_T app_dp_set_value(UINT8_T *buf, UINT32_T value)
{
    buf[0] = (UINT8_T)((value >> 24) & 0xFF);
    buf[1] = (UINT8_T)((value >> 16) & 0xFF);
    buf[2] = (UINT8_T)((value >> 8) & 0xFF);
    buf[3] = (UINT8_T)(value & 0xFF);
}

STATIC UINT32_T app_dp_get_value(UINT8_T *buf)
{
    return ((UINT32_T)buf[0] << 24) |
           ((UINT32_T)buf[1] << 16) |
           ((UINT32_T)buf[2] << 8) |
           ((UINT32_T)buf[3]);
}

STATIC VOID_T app_dp_reset_state_for_mode(VOID_T)
{
    UINT32_T timeout_ms = app_motor_get_mode_timeout_ms();

    if (app_motor_get_mode() == GAME_MODE_ALTERNATING && timeout_ms == 0) {
        app_state_enter_sleep();
    } else {
        app_state_reset_work_cycle_for(timeout_ms);
    }
}

OPERATE_RET app_dp_parser(UINT8_T *buf, UINT32_T size)
{
    if (buf == NULL || size < 4 || size > SIZEOF(g_cmd)) {
        return OPRT_INVALID_PARM;
    }

    memset(&g_cmd, 0, SIZEOF(g_cmd));
    memcpy(&g_cmd, buf, size);
    tal_util_reverse_byte(&g_cmd.dp_data_len, SIZEOF(UINT16_T));

    TAL_PR_HEXDUMP_INFO("dp_cmd", (VOID_T *)&g_cmd, g_cmd.dp_data_len + 4);

    switch (g_cmd.dp_id) {
    case DP_ID_SWITCH:
        app_state_set_app_power(g_cmd.dp_data[0] != 0);
        app_led_update();
        break;
    case DP_ID_MODE:
        app_motor_set_mode((game_mode_t)g_cmd.dp_data[0]);
        app_dp_reset_state_for_mode();
        break;
    case DP_ID_ALT_LASER_TIME: {
        UINT32_T seconds;

        if (g_cmd.dp_data_len < DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        seconds = app_dp_get_value(g_cmd.dp_data);
        if (seconds > 180) {
            seconds = 180;
        }
        app_dp_set_value(g_cmd.dp_data, seconds);
        app_motor_set_alt_laser_time((UINT16_T)seconds);
        app_dp_reset_state_for_mode();
        break;
    }
    case DP_ID_ALT_BUG_TIME: {
        UINT32_T seconds;

        if (g_cmd.dp_data_len < DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        seconds = app_dp_get_value(g_cmd.dp_data);
        if (seconds > 180) {
            seconds = 180;
        }
        app_dp_set_value(g_cmd.dp_data, seconds);
        app_motor_set_alt_bug_time((UINT16_T)seconds);
        app_dp_reset_state_for_mode();
        break;
    }
    case DP_ID_STEPLESS_CONTROL: {
        UINT32_T percent;

        if (g_cmd.dp_data_len < DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        percent = app_dp_get_value(g_cmd.dp_data);
        if (percent > 100) {
            percent = 100;
        }
        app_dp_set_value(g_cmd.dp_data, percent);
        app_motor_set_stepless_percent((UINT8_T)percent);
        break;
    }
    default:
        TAL_PR_DEBUG("[dp] unsupported id=%d", g_cmd.dp_id);
        break;
    }

    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);
    return OPRT_OK;
}

OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T *buf, UINT32_T size)
{
    memset(&g_rsp, 0, SIZEOF(g_rsp));
    g_rsp.dp_id = dp_id;

    switch (dp_id) {
    case DP_ID_SWITCH:
        g_rsp.dp_type = DT_BOOL;
        g_rsp.dp_data_len = DT_BOOL_LEN;
        memcpy(g_rsp.dp_data, buf, DT_BOOL_LEN);
        break;
    case DP_ID_MODE:
    case DP_ID_WORK_STATE:
        g_rsp.dp_type = DT_ENUM;
        g_rsp.dp_data_len = DT_ENUM_LEN;
        memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        break;
    case DP_ID_BATTERY:
    case DP_ID_ALT_LASER_TIME:
    case DP_ID_ALT_BUG_TIME:
    case DP_ID_STEPLESS_CONTROL:
        g_rsp.dp_type = DT_VALUE;
        g_rsp.dp_data_len = DT_VALUE_LEN;
        memcpy(g_rsp.dp_data, buf, DT_VALUE_LEN);
        break;
    default:
        return OPRT_INVALID_PARM;
    }

    UINT16_T rsp_len = g_rsp.dp_data_len + 4;
    tal_util_reverse_byte(&g_rsp.dp_data_len, SIZEOF(UINT16_T));
    return tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE, DP_SEND_FOR_CLOUD_PANEL,
                                 DP_SEND_WITHOUT_RESPONSE, (VOID_T *)&g_rsp, rsp_len);
}

VOID_T app_dp_report_all(VOID_T)
{
    UINT8_T bool_buf[DT_BOOL_LEN] = {0};
    UINT8_T enum_buf[DT_ENUM_LEN] = {0};
    UINT8_T value_buf[DT_VALUE_LEN] = {0};

    bool_buf[0] = app_state_is_app_power_on() ? 1 : 0;
    app_dp_report(DP_ID_SWITCH, bool_buf, DT_BOOL_LEN);

    enum_buf[0] = (UINT8_T)app_motor_get_report_mode();
    app_dp_report(DP_ID_MODE, enum_buf, DT_ENUM_LEN);

    enum_buf[0] = app_state_get_dp_enum();
    app_dp_report(DP_ID_WORK_STATE, enum_buf, DT_ENUM_LEN);

    app_dp_set_value(value_buf, app_battery_get_percent());
    app_dp_report(DP_ID_BATTERY, value_buf, DT_VALUE_LEN);

    app_dp_set_value(value_buf, app_motor_get_alt_laser_time());
    app_dp_report(DP_ID_ALT_LASER_TIME, value_buf, DT_VALUE_LEN);

    app_dp_set_value(value_buf, app_motor_get_alt_bug_time());
    app_dp_report(DP_ID_ALT_BUG_TIME, value_buf, DT_VALUE_LEN);

    app_dp_set_value(value_buf, app_motor_get_stepless_percent());
    app_dp_report(DP_ID_STEPLESS_CONTROL, value_buf, DT_VALUE_LEN);
}
