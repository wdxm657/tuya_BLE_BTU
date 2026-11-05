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

extern BOOL_T tkl_cpu_is_sleep(VOID_T);

demo_dp_t g_cmd = {0};
demo_dp_t g_rsp = {0};
UINT32_T g_sn = 0;

#define APP_DP_FLASH_MAGIC      0x43535452UL
#define APP_DP_FLASH_VERSION    2U
#define APP_DP_FLASH_ERASE_SIZE 0x1000U

typedef struct {
    UINT32_T magic;
    UINT8_T version;
    UINT8_T power;
    UINT8_T mode;
    UINT8_T stepless_percent;
    UINT32_T checksum;
    /* Keep the v1 prefix intact so existing user configurations can be migrated. */
    UINT8_T game_repeat;
    UINT8_T reserved;
    UINT16_T game_interval_min;
} app_dp_flash_data_t;

STATIC app_dp_flash_data_t s_saved_config;
STATIC BOOL_T s_saved_config_valid = FALSE;
STATIC BOOL_T s_config_write_failed = FALSE;

STATIC UINT32_T app_dp_config_checksum(CONST app_dp_flash_data_t *data)
{
    UINT32_T checksum = data->magic ^ ((UINT32_T)data->version << 24) ^
                        ((UINT32_T)data->power << 16) ^ ((UINT32_T)data->mode << 8) ^
                        data->stepless_percent ^ 0xA55A5AA5UL;
    if (data->version >= 2U) {
        checksum ^= ((UINT32_T)data->game_repeat << 24) ^
                    ((UINT32_T)data->game_interval_min << 8) ^ data->reserved;
    }
    return checksum;
}

OPERATE_RET app_dp_load_persistent(VOID_T)
{
    app_dp_flash_data_t data;
    OPERATE_RET ret;

    s_saved_config_valid = FALSE;
    s_config_write_failed = FALSE;
    ret = tal_flash_read(USER_FLASH_ADDR_CAT_STRING, (UCHAR_T *)&data, SIZEOF(data));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[dp] config read failed: %d", ret);
        return ret;
    }
    if (data.magic != APP_DP_FLASH_MAGIC ||
        (data.version != 1U && data.version != APP_DP_FLASH_VERSION) || data.power > 1 ||
        data.mode > WORK_MODE_VARIABLE || data.stepless_percent > 100 ||
        data.checksum != app_dp_config_checksum(&data)) {
        TAL_PR_INFO("[dp] no valid saved config, use defaults");
        return OPRT_OK;
    }

    if (data.version == 1U) {
        data.version = APP_DP_FLASH_VERSION;
        data.game_repeat = GAME_REPEAT_1;
        data.game_interval_min = GAME_INTERVAL_DEFAULT_MIN;
        data.reserved = 0;
        data.checksum = app_dp_config_checksum(&data);
    } else if (data.game_repeat > GAME_REPEAT_UNLIMITED ||
               data.game_interval_min < GAME_INTERVAL_MIN ||
               data.game_interval_min > GAME_INTERVAL_MAX || data.reserved != 0) {
        TAL_PR_WARN("[dp] invalid game settings, use defaults");
        return OPRT_OK;
    }

    app_state_set_game_repeat((game_repeat_t)data.game_repeat);
    app_state_set_game_interval(data.game_interval_min);
    app_motor_set_mode((work_mode_t)data.mode);
    app_motor_set_stepless_percent(data.stepless_percent);
    app_state_set_app_power(data.power != 0);
    s_saved_config = data;
    s_saved_config_valid = TRUE;
    TAL_PR_INFO("[dp] config restored: power=%d, mode=%d, speed=%d",
                data.power, data.mode, data.stepless_percent);
    TAL_PR_INFO("[dp] game settings: repeat=%d, interval=%dmin",
                data.game_repeat, data.game_interval_min);
    return OPRT_OK;
}

OPERATE_RET app_dp_clear_persistent(VOID_T)
{
    OPERATE_RET ret = tal_flash_erase(USER_FLASH_ADDR_CAT_STRING, APP_DP_FLASH_ERASE_SIZE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[dp] clear config failed: %d", ret);
        return ret;
    }

    memset(&s_saved_config, 0, SIZEOF(s_saved_config));
    s_saved_config_valid = FALSE;
    s_config_write_failed = FALSE;
    app_state_set_game_repeat(GAME_REPEAT_1);
    app_state_set_game_interval(GAME_INTERVAL_DEFAULT_MIN);
    TAL_PR_INFO("[dp] saved config cleared");
    return OPRT_OK;
}

STATIC OPERATE_RET app_dp_save_persistent(VOID_T)
{
    app_dp_flash_data_t data = {0};
    app_dp_flash_data_t verify;
    OPERATE_RET ret;

    data.magic = APP_DP_FLASH_MAGIC;
    data.version = APP_DP_FLASH_VERSION;
    /* Keep user choices separate from automatic runtime state changes. */
    data.power = s_saved_config_valid ? s_saved_config.power :
                 (app_state_is_app_power_on() ? 1 : 0);
    data.mode = s_saved_config_valid ? s_saved_config.mode :
                (UINT8_T)app_motor_get_mode();
    data.stepless_percent = s_saved_config_valid ? s_saved_config.stepless_percent :
                            app_motor_get_stepless_percent();
    data.game_repeat = (UINT8_T)app_state_get_game_repeat();
    data.game_interval_min = (UINT16_T)app_state_get_game_interval();
    switch (g_cmd.dp_id) {
    case DP_ID_SWITCH:
        data.power = app_state_is_app_power_on() ? 1 : 0;
        break;
    case DP_ID_MODE:
        data.mode = (UINT8_T)app_motor_get_mode();
        break;
    case DP_ID_STEPLESS_CONTROL:
        data.stepless_percent = app_motor_get_stepless_percent();
        break;
    case DP_ID_GAME_REPEAT:
    case DP_ID_GAME_INTERVAL:
        break;
    default:
        return OPRT_OK;
    }
    data.checksum = app_dp_config_checksum(&data);
    if (s_saved_config_valid && !s_config_write_failed &&
        memcmp(&data, &s_saved_config, SIZEOF(data)) == 0) {
        return OPRT_OK;
    }

    ret = tal_flash_erase(USER_FLASH_ADDR_CAT_STRING, APP_DP_FLASH_ERASE_SIZE);
    if (ret == OPRT_OK) {
        ret = tal_flash_write(USER_FLASH_ADDR_CAT_STRING,
                              (CONST UCHAR_T *)&data, SIZEOF(data));
    }
    if (ret == OPRT_OK) {
        ret = tal_flash_read(USER_FLASH_ADDR_CAT_STRING, (UCHAR_T *)&verify, SIZEOF(verify));
        if (ret == OPRT_OK && memcmp(&data, &verify, SIZEOF(data)) != 0) {
            ret = OPRT_COM_ERROR;
        }
    }
    if (ret != OPRT_OK) {
        s_config_write_failed = TRUE;
        TAL_PR_ERR("[dp] config save failed: %d", ret);
        return ret;
    }
    s_saved_config = data;
    s_saved_config_valid = TRUE;
    s_config_write_failed = FALSE;
    return OPRT_OK;
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

OPERATE_RET app_dp_parser(UINT8_T *buf, UINT32_T size)
{
    if (buf == NULL || size < 4 || size > SIZEOF(g_cmd)) {
        return OPRT_INVALID_PARM;
    }

    memset(&g_cmd, 0, SIZEOF(g_cmd));
    memcpy(&g_cmd, buf, size);
    tal_util_reverse_byte(&g_cmd.dp_data_len, SIZEOF(UINT16_T));

    if (g_cmd.dp_data_len != size - 4) {
        return OPRT_INVALID_PARM;
    }
    switch (g_cmd.dp_id) {
    case DP_ID_SWITCH:
        if (g_cmd.dp_type != DT_BOOL || g_cmd.dp_data_len != DT_BOOL_LEN ||
            g_cmd.dp_data[0] > 1) {
            return OPRT_INVALID_PARM;
        }
        break;
    case DP_ID_MODE:
        if (g_cmd.dp_type != DT_ENUM || g_cmd.dp_data_len != DT_ENUM_LEN ||
            g_cmd.dp_data[0] > WORK_MODE_VARIABLE) {
            return OPRT_INVALID_PARM;
        }
        break;
    case DP_ID_GAME_REPEAT:
        if (g_cmd.dp_type != DT_ENUM || g_cmd.dp_data_len != DT_ENUM_LEN ||
            g_cmd.dp_data[0] > GAME_REPEAT_UNLIMITED) {
            return OPRT_INVALID_PARM;
        }
        break;
    case DP_ID_GAME_INTERVAL: {
        UINT32_T minutes;

        if (g_cmd.dp_type != DT_VALUE || g_cmd.dp_data_len != DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        minutes = app_dp_get_value(g_cmd.dp_data);
        if (minutes < GAME_INTERVAL_MIN || minutes > GAME_INTERVAL_MAX) {
            return OPRT_INVALID_PARM;
        }
        break;
    }
    case DP_ID_STEPLESS_CONTROL:
        if (g_cmd.dp_type != DT_VALUE || g_cmd.dp_data_len != DT_VALUE_LEN) {
            return OPRT_INVALID_PARM;
        }
        break;
#if APP_ENABLE_MOTOR_TUNING_DPS
    case DP_ID_DEBUG_MOTOR_DUTY_MIN:
    case DP_ID_DEBUG_MOTOR_DUTY_MAX:
    case DP_ID_DEBUG_DUTY_BOOST_MAX:
        if (g_cmd.dp_type != DT_VALUE || g_cmd.dp_data_len != DT_VALUE_LEN ||
            app_dp_get_value(g_cmd.dp_data) > 100U) {
            return OPRT_INVALID_PARM;
        }
        if ((g_cmd.dp_id == DP_ID_DEBUG_MOTOR_DUTY_MIN &&
             app_dp_get_value(g_cmd.dp_data) > app_motor_get_tuning_param(MOTOR_TUNING_DUTY_MAX)) ||
            (g_cmd.dp_id == DP_ID_DEBUG_MOTOR_DUTY_MAX &&
             app_dp_get_value(g_cmd.dp_data) < app_motor_get_tuning_param(MOTOR_TUNING_DUTY_MIN))) {
            return OPRT_INVALID_PARM;
        }
        break;
#endif
    default:
        break;
    }

    // TAL_PR_HEXDUMP_INFO("dp_cmd", (VOID_T *)&g_cmd, g_cmd.dp_data_len + 4);

    switch (g_cmd.dp_id) {
    case DP_ID_SWITCH:
        app_state_set_app_power(g_cmd.dp_data[0] != 0);
        if (!g_cmd.dp_data[0])
        {
            // 测试APP关机进入休眠状态专用
            // tal_cpu_allow_sleep();
        }else{
            if (tkl_cpu_is_sleep())
            {
                app_state_force_retention_wakeup();
            }
        }
        
        app_led_update();
        break;
    case DP_ID_MODE:
        app_motor_set_mode((work_mode_t)g_cmd.dp_data[0]);
        app_state_reset_work_cycle();
        break;
    case DP_ID_GAME_REPEAT:
        app_state_set_game_repeat((game_repeat_t)g_cmd.dp_data[0]);
        break;
    case DP_ID_GAME_INTERVAL:
        app_state_set_game_interval(app_dp_get_value(g_cmd.dp_data));
        break;
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
        // app_state_reset_work_cycle();
        break;
    }
#if APP_ENABLE_MOTOR_TUNING_DPS
    case DP_ID_DEBUG_MOTOR_DUTY_MIN:
    case DP_ID_DEBUG_MOTOR_DUTY_MAX:
    case DP_ID_DEBUG_DUTY_BOOST_MAX: {
        app_motor_tuning_param_t param =
            g_cmd.dp_id == DP_ID_DEBUG_MOTOR_DUTY_MIN ? MOTOR_TUNING_DUTY_MIN :
            g_cmd.dp_id == DP_ID_DEBUG_MOTOR_DUTY_MAX ? MOTOR_TUNING_DUTY_MAX :
                                                        MOTOR_TUNING_DUTY_BOOST_MAX;
        if (!app_motor_set_tuning_param(param, (UINT8_T)app_dp_get_value(g_cmd.dp_data))) {
            return OPRT_INVALID_PARM;
        }
        break;
    }
#endif
    default:
        TAL_PR_DEBUG("[dp] unsupported id=%d", g_cmd.dp_id);
        break;
    }

    OPERATE_RET ret = app_dp_save_persistent();
    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);
    return ret;
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
    case DP_ID_GAME_REPEAT:
        g_rsp.dp_type = DT_ENUM;
        g_rsp.dp_data_len = DT_ENUM_LEN;
        memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        break;
    case DP_ID_BATTERY:
    case DP_ID_STEPLESS_CONTROL:
    case DP_ID_GAME_INTERVAL:
#if APP_ENABLE_MOTOR_TUNING_DPS
    case DP_ID_DEBUG_MOTOR_DUTY_MIN:
    case DP_ID_DEBUG_MOTOR_DUTY_MAX:
    case DP_ID_DEBUG_DUTY_BOOST_MAX:
#endif
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

    enum_buf[0] = (UINT8_T)app_motor_get_mode();
    app_dp_report(DP_ID_MODE, enum_buf, DT_ENUM_LEN);

    enum_buf[0] = app_state_get_dp_enum();
    app_dp_report(DP_ID_WORK_STATE, enum_buf, DT_ENUM_LEN);

    app_dp_set_value(value_buf, app_battery_get_percent());
    app_dp_report(DP_ID_BATTERY, value_buf, DT_VALUE_LEN);

    app_dp_set_value(value_buf, app_motor_get_stepless_percent());
    app_dp_report(DP_ID_STEPLESS_CONTROL, value_buf, DT_VALUE_LEN);

    enum_buf[0] = (UINT8_T)app_state_get_game_repeat();
    app_dp_report(DP_ID_GAME_REPEAT, enum_buf, DT_ENUM_LEN);

    app_dp_set_value(value_buf, app_state_get_game_interval());
    app_dp_report(DP_ID_GAME_INTERVAL, value_buf, DT_VALUE_LEN);

#if APP_ENABLE_MOTOR_TUNING_DPS
    app_dp_set_value(value_buf, app_motor_get_tuning_param(MOTOR_TUNING_DUTY_MIN));
    app_dp_report(DP_ID_DEBUG_MOTOR_DUTY_MIN, value_buf, DT_VALUE_LEN);
    app_dp_set_value(value_buf, app_motor_get_tuning_param(MOTOR_TUNING_DUTY_MAX));
    app_dp_report(DP_ID_DEBUG_MOTOR_DUTY_MAX, value_buf, DT_VALUE_LEN);
    app_dp_set_value(value_buf, app_motor_get_tuning_param(MOTOR_TUNING_DUTY_BOOST_MAX));
    app_dp_report(DP_ID_DEBUG_DUTY_BOOST_MAX, value_buf, DT_VALUE_LEN);
#endif
}
