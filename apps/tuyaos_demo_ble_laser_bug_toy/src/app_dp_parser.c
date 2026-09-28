/**
 * @file app_dp_parser.c
 * @brief afp pawswiff DP parser.
 */

#include "string.h"

#include "board.h"
#include "tal_flash.h"
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

#define APP_DP_FLASH_MAGIC       (0x4C425047UL)
#define APP_DP_FLASH_VERSION     (2U)
#define APP_DP_FLASH_ERASE_SIZE  (0x1000U)

#pragma pack(1)
typedef struct {
    UINT32_T magic;
    UINT8_T  version;
    UINT8_T  mode;
    UINT16_T alt_laser_time;
    UINT16_T alt_bug_time;
    UINT8_T  stepless_percent;
    UINT8_T  alt_bug_speed;
    UINT8_T  alt_laser_speed;
    UINT8_T  alt_game_rounds;
    UINT8_T  battery_percent;
} app_dp_flash_data_t;
#pragma pack()

STATIC OPERATE_RET app_dp_persistent_write(VOID_T)
{
    app_dp_flash_data_t data = {0};
    OPERATE_RET ret;

    data.magic = APP_DP_FLASH_MAGIC;
    data.version = APP_DP_FLASH_VERSION;
    data.mode = (UINT8_T)app_motor_get_report_mode();
    data.alt_laser_time = app_motor_get_alt_laser_time();
    data.alt_bug_time = app_motor_get_alt_bug_time();
    data.stepless_percent = app_motor_get_stepless_percent();
    data.alt_bug_speed = app_motor_get_alt_bug_speed();
    data.alt_laser_speed = app_motor_get_alt_laser_speed();
    data.alt_game_rounds = app_motor_get_alt_game_rounds();
    data.battery_percent = app_battery_get_percent();

    ret = tal_flash_erase(USER_FLASH_ADDR_LASER_BUG, APP_DP_FLASH_ERASE_SIZE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[dp] erase persistent data failed: %d", ret);
        return ret;
    }

    ret = tal_flash_write(USER_FLASH_ADDR_LASER_BUG,
                          (CONST UCHAR_T *)&data, SIZEOF(data));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[dp] write persistent data failed: %d", ret);
    }
    TAL_PR_INFO("[dp] persistent data writed: mode=%d, laser=%d, bug=%d, speed=%d, battery=%d",
                data.mode, data.alt_laser_time, data.alt_bug_time,
                data.stepless_percent, data.battery_percent);
    return ret;
}

OPERATE_RET app_dp_load_persistent(VOID_T)
{
    app_dp_flash_data_t data = {0};
    OPERATE_RET ret;

    ret = tal_flash_read(USER_FLASH_ADDR_LASER_BUG,
                         (UCHAR_T *)&data, SIZEOF(data));
    if (ret != OPRT_OK) {
        TAL_PR_WARN("[dp] read persistent data failed: %d", ret);
        return ret;
    }

    if (data.magic != APP_DP_FLASH_MAGIC ||
        data.version != APP_DP_FLASH_VERSION ||
        data.mode > GAME_MODE_ALTERNATING ||
        data.alt_laser_time > 180 ||
        data.alt_bug_time > 180 ||
        data.stepless_percent > 100 ||
        data.alt_bug_speed < 1 || data.alt_bug_speed > 100 ||
        data.alt_laser_speed < 1 || data.alt_laser_speed > 100 ||
        data.alt_game_rounds < 1 || data.alt_game_rounds > 5 ||
        data.battery_percent > 100) {
        TAL_PR_INFO("[dp] persistent data not found, use defaults");
        return OPRT_OK;
    }

    app_motor_set_mode((game_mode_t)data.mode);
    app_motor_set_alt_laser_time(data.alt_laser_time);
    app_motor_set_alt_bug_time(data.alt_bug_time);
    app_motor_set_stepless_percent(data.stepless_percent);
    app_motor_set_alt_bug_speed(data.alt_bug_speed);
    app_motor_set_alt_laser_speed(data.alt_laser_speed);
    app_motor_set_alt_game_rounds(data.alt_game_rounds);
    app_battery_set_percent(data.battery_percent);
    TAL_PR_INFO("[dp] persistent data restored: mode=%d, laser=%d, bug=%d, speed=%d, alt_bug_speed=%d, alt_laser_speed=%d, rounds=%d, battery=%d",
                data.mode, data.alt_laser_time, data.alt_bug_time,
                data.stepless_percent, data.alt_bug_speed,
                data.alt_laser_speed, data.alt_game_rounds,
                data.battery_percent);
    return OPRT_OK;
}

OPERATE_RET app_dp_save_battery_percent(VOID_T)
{
    return app_dp_persistent_write();
}

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
        app_dp_persistent_write();
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
        app_dp_persistent_write();
        app_dp_reset_state_for_mode();
        break;
    }
    case DP_ID_ALT_BUG_SPEED:
    case DP_ID_ALT_LASER_SPEED: {
        UINT32_T percent;

        if (g_cmd.dp_data_len < DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        percent = app_dp_get_value(g_cmd.dp_data);
        if (percent < 1) {
            percent = 1;
        }
        if (percent > 100) {
            percent = 100;
        }
        app_dp_set_value(g_cmd.dp_data, percent);
        if (g_cmd.dp_id == DP_ID_ALT_BUG_SPEED) {
            app_motor_set_alt_bug_speed((UINT8_T)percent);
        } else {
            app_motor_set_alt_laser_speed((UINT8_T)percent);
        }
        app_dp_persistent_write();
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
        app_dp_persistent_write();
        app_dp_reset_state_for_mode();
        break;
    }
    case DP_ID_STEPLESS_CONTROL:
    {
        UINT32_T percent;

        if (g_cmd.dp_data_len < DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        percent = app_dp_get_value(g_cmd.dp_data);
        if (percent < 1) {
            percent = 1;
        }
        if (percent > 100) {
            percent = 100;
        }
        app_dp_set_value(g_cmd.dp_data, percent);
        app_motor_set_stepless_percent((UINT8_T)percent);
        app_dp_persistent_write();
        break;
    }
    case DP_ID_ALT_GAME_ROUNDS: {
        UINT8_T enum_index;

        if (g_cmd.dp_data_len < DT_ENUM_LEN) {
            return OPRT_INVALID_PARM;
        }
        enum_index = g_cmd.dp_data[0];
        if (enum_index > 4) {
            enum_index = 4;
        }
        g_cmd.dp_data[0] = enum_index;
        app_motor_set_alt_game_rounds((UINT8_T)(enum_index + 1));
        app_dp_persistent_write();
        app_dp_reset_state_for_mode();
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
    case DP_ID_ALT_GAME_ROUNDS:
        g_rsp.dp_type = DT_ENUM;
        g_rsp.dp_data_len = DT_ENUM_LEN;
        memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        break;
    case DP_ID_BATTERY:
    case DP_ID_ALT_BUG_SPEED:
    case DP_ID_ALT_LASER_SPEED:
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

    app_dp_set_value(value_buf, app_motor_get_alt_bug_speed());
    app_dp_report(DP_ID_ALT_BUG_SPEED, value_buf, DT_VALUE_LEN);

    app_dp_set_value(value_buf, app_motor_get_alt_laser_speed());
    app_dp_report(DP_ID_ALT_LASER_SPEED, value_buf, DT_VALUE_LEN);

    enum_buf[0] = app_motor_get_alt_game_rounds() - 1;
    app_dp_report(DP_ID_ALT_GAME_ROUNDS, enum_buf, DT_ENUM_LEN);
}
