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

#include "board.h"
#include "tal_flash.h"
#include "tal_log.h"
#include "tal_util.h"
#include "tkl_system.h"

#include "tuya_ble_api.h"
#include "tuya_ble_mutli_tsf_protocol.h"

#include "app_dp_parser.h"
#include "faqiuji_audio.h"
#include "faqiuji_mcu_protocol.h"

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
STATIC UINT8_T sg_launch_mode = 0U;
STATIC BOOL_T sg_switch_enabled = FALSE;
STATIC UINT16_T sg_auto_tease_count = 20U;
STATIC UINT16_T sg_standby_tease_min = 20U;
STATIC UINT8_T sg_motor_speed = 1U;
STATIC UINT8_T sg_sound_mode = 0U;
STATIC UINT8_T sg_volume = 100U;
STATIC UINT32_T sg_record_start_ms;
STATIC BOOL_T sg_sleep_audio_active;
STATIC UINT8_T sg_sleep_audio_phase;
STATIC UINT32_T sg_sleep_phase_start_ms;
STATIC UINT8_T sg_comfort_sound_file = FAQIUJI_AUDIO_FACTORY_SOUND_1;
STATIC UINT8_T sg_battery_percent;
STATIC UINT8_T sg_radar_state;
STATIC UINT8_T sg_work_state;
STATIC UINT8_T sg_fault;
STATIC BOOL_T sg_battery_valid;
STATIC BOOL_T sg_radar_valid;
STATIC BOOL_T sg_work_valid;
STATIC BOOL_T sg_fault_valid;

#define FAQIUJI_DP_FLASH_MAGIC       (0x46414A51UL)
#define FAQIUJI_DP_FLASH_VERSION     (2U)
#define FAQIUJI_DP_FLASH_ERASE_SIZE  (0x1000U)

#pragma pack(1)
typedef struct {
    UINT32_T magic;
    UINT8_T version;
    UINT8_T switch_enabled;
    UINT8_T launch_mode;
    UINT8_T sound_mode;
    UINT16_T auto_tease_count;
    UINT16_T standby_tease_min;
    UINT8_T volume;
    /* Append fields to preserve the version 1 layout. */
    UINT8_T motor_speed;
} faqiuji_dp_flash_data_t;
#pragma pack()

STATIC VOID_T app_dp_set_defaults(VOID_T)
{
    sg_switch_enabled = FALSE;
    sg_launch_mode = 0U;
    sg_auto_tease_count = 20U;
    sg_standby_tease_min = 20U;
    sg_motor_speed = 1U;
    sg_sound_mode = 0U;
    sg_volume = 100U;
    sg_comfort_sound_file = FAQIUJI_AUDIO_FACTORY_SOUND_1;
}

OPERATE_RET app_dp_save_persistent(VOID_T)
{
    faqiuji_dp_flash_data_t data = {0};
    OPERATE_RET ret;

    data.magic = FAQIUJI_DP_FLASH_MAGIC;
    data.version = FAQIUJI_DP_FLASH_VERSION;
    data.switch_enabled = sg_switch_enabled ? 1U : 0U;
    data.launch_mode = sg_launch_mode;
    data.sound_mode = sg_sound_mode;
    data.auto_tease_count = sg_auto_tease_count;
    data.standby_tease_min = sg_standby_tease_min;
    data.motor_speed = sg_motor_speed;
    data.volume = sg_volume;

    ret = tal_flash_erase(USER_FLASH_ADDR_FAQIUJI,
                          FAQIUJI_DP_FLASH_ERASE_SIZE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("FAQIUJI flash erase failed: %d", ret);
        return ret;
    }
    ret = tal_flash_write(USER_FLASH_ADDR_FAQIUJI,
                          (CONST UCHAR_T *)&data, SIZEOF(data));
    TAL_PR_INFO("FAQIUJI config save ret=%d switch=%u mode=%u count=%u standby=%u sound=%u volume=%u speed=%u",
                ret, data.switch_enabled, data.launch_mode,
                data.auto_tease_count, data.standby_tease_min,
                data.sound_mode, data.volume, data.motor_speed);
    return ret;
}

OPERATE_RET app_dp_load_persistent(VOID_T)
{
    faqiuji_dp_flash_data_t data = {0};
    OPERATE_RET ret;

    app_dp_set_defaults();
    ret = tal_flash_read(USER_FLASH_ADDR_FAQIUJI,
                         (UCHAR_T *)&data, SIZEOF(data));
    if (ret != OPRT_OK ||
        data.magic != FAQIUJI_DP_FLASH_MAGIC ||
        (data.version != 1U && data.version != FAQIUJI_DP_FLASH_VERSION) ||
        data.switch_enabled > 1U ||
        data.launch_mode > 2U ||
        data.sound_mode > 1U ||
        data.auto_tease_count < 10U ||
        data.auto_tease_count > 90U ||
        data.standby_tease_min < 10U ||
        data.standby_tease_min > 30U ||
        data.volume < 1U ||
        data.volume > 100U ||
        (data.version == FAQIUJI_DP_FLASH_VERSION &&
         (data.motor_speed < 1U || data.motor_speed > 100U))) {
        TAL_PR_INFO("FAQIUJI config not found, use defaults");
        return OPRT_OK;
    }

    sg_switch_enabled = data.switch_enabled ? TRUE : FALSE;
    sg_launch_mode = data.launch_mode;
    sg_sound_mode = data.sound_mode;
    sg_auto_tease_count = data.auto_tease_count;
    sg_standby_tease_min = data.standby_tease_min;
    sg_motor_speed = data.version == 1U ? 1U : data.motor_speed;
    sg_volume = data.volume;
    sg_comfort_sound_file = sg_sound_mode ?
                            FAQIUJI_AUDIO_FACTORY_SOUND_2 :
                            FAQIUJI_AUDIO_FACTORY_SOUND_1;
    faqiuji_audio_set_volume(sg_volume);
    TAL_PR_INFO("FAQIUJI config restored: switch=%u mode=%u count=%u standby=%u sound=%u volume=%u speed=%u",
                sg_switch_enabled, sg_launch_mode, sg_auto_tease_count,
                sg_standby_tease_min, sg_sound_mode, sg_volume, sg_motor_speed);
    return OPRT_OK;
}

OPERATE_RET app_dp_sync_to_mcu(VOID_T)
{
    OPERATE_RET ret;

    if (!faqiuji_mcu_is_ping_ok()) {
        return OPRT_OK;
    }
    ret = faqiuji_mcu_config_sync(sg_switch_enabled, sg_launch_mode,
                                  sg_auto_tease_count,
                                  sg_standby_tease_min,
                                  sg_motor_speed);
    if (ret == OPRT_OK) {
        TAL_PR_INFO("FAQIUJI config sync sent to MCU");
    }
    return ret;
}

OPERATE_RET app_dp_reset_to_defaults(VOID_T)
{
    OPERATE_RET ret;
    OPERATE_RET sync_ret;

    sg_sleep_audio_active = FALSE;
    sg_sleep_audio_phase = 0U;
    sg_sleep_phase_start_ms = 0U;
    sg_record_start_ms = 0U;
    if (faqiuji_audio_is_recording()) {
        ret = faqiuji_audio_record_stop();
        if (ret != OPRT_OK) {
            TAL_PR_ERR("FACTORY RESET: stop recording failed=%d", ret);
        }
    }
    (VOID_T)faqiuji_audio_stop();
    (VOID_T)faqiuji_audio_take_play_finished();
    app_dp_set_defaults();
    (VOID_T)faqiuji_audio_set_volume(sg_volume);
    sg_work_state = 1U;
    sg_work_valid = FALSE;
    sg_fault_valid = FALSE;

    /* One CONFIG_SYNC also turns the PUYA motor off. */
    sync_ret = app_dp_sync_to_mcu();
    ret = app_dp_save_persistent();
    TAL_PR_INFO("FACTORY RESET: defaults saved=%d mcu_sync=%d",
                ret, sync_ret);
    return ret != OPRT_OK ? ret : sync_ret;
}

#define FAQIUJI_SLEEP_AUDIO_PHASE_MS 600000UL
#define FAQIUJI_AUDIO_SLEEP_OWNER     0U
#define FAQIUJI_AUDIO_SLEEP_COMFORT   1U

STATIC BOOL_T app_dp_is_true(CONST UINT8_T *data, UINT16_T len)
{
    return (len > 0 && (data[0] == 1 || data[0] == '1' ||
                        data[0] == 't' || data[0] == 'T'));
}

STATIC UINT8_T app_sound_mode_to_file(CONST UINT8_T *data, UINT16_T len)
{
    if (len == 1 && data[0] == 1) return FAQIUJI_AUDIO_FACTORY_SOUND_2;
    if (len >= 7 && memcmp(data, "sound_2", 7) == 0) return FAQIUJI_AUDIO_FACTORY_SOUND_2;
    return FAQIUJI_AUDIO_FACTORY_SOUND_1;
}

STATIC OPERATE_RET app_sleep_audio_start(VOID_T)
{
    UINT8_T file_id = (sg_sleep_audio_phase == FAQIUJI_AUDIO_SLEEP_OWNER) ?
                      FAQIUJI_AUDIO_USER_FILE_ID : sg_comfort_sound_file;
    OPERATE_RET ret = faqiuji_audio_play_start(file_id);

    if (ret != OPRT_OK && sg_sleep_audio_phase == FAQIUJI_AUDIO_SLEEP_OWNER) {
        sg_sleep_audio_phase = FAQIUJI_AUDIO_SLEEP_COMFORT;
        ret = faqiuji_audio_play_start(sg_comfort_sound_file);
    }
    return ret;
}

STATIC UINT8_T app_mode_to_launch_mode(CONST UINT8_T *data, UINT16_T len)
{
    if (len == 1U && data[0] == 0U) {
        return 0U;
    }
    if (len == 1U && data[0] == 1U) {
        return 1U;
    }
    if (len == 1U && data[0] == 2U) {
        return 2U;
    }
    if (len >= 6U && memcmp(data, "active", 6U) == 0) {
        return 0U;
    }
    if (len >= 6U && memcmp(data, "simple", 6U) == 0) {
        return 1U;
    }
    if (len >= 4U && memcmp(data, "mild", 4U) == 0) {
        return 2U;
    }
    return 0xFFU;
}

STATIC VOID_T app_audio_stop_current(VOID_T)
{
    if (faqiuji_audio_is_playing()) {
        faqiuji_audio_stop();
    }
    if (faqiuji_audio_is_recording()) {
        faqiuji_audio_record_stop();
    }
}

STATIC UINT8_T app_dp_type(UINT8_T dp_id)
{
    if (dp_id == DP_ID_MODE) return DT_ENUM;
    if (dp_id == DP_ID_SOUND_MODE) return DT_ENUM;
    if (dp_id == DP_ID_WORK_STATE) return DT_ENUM;
    if (dp_id == DP_ID_RADAR_SENSE) return DT_ENUM;
    if (dp_id == DP_ID_FAULT) return DT_BITMAP;
    if (dp_id == DP_ID_BATTERY) return DT_VALUE;
    if (dp_id == DP_ID_VOLUME) return DT_VALUE;
    if (dp_id == DP_ID_MOTOR_SPEED) return DT_VALUE;
    if (dp_id == DP_ID_AUTO_TEASE_TIME ||
        dp_id == DP_ID_STANDBY_TEASE_TIME) return DT_VALUE;
    return DT_BOOL;
}

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/




OPERATE_RET app_dp_parser(UINT8_T* buf, UINT32_T size)
{
    if (buf == NULL || size < 4 || size > sizeof(g_cmd)) {
        return OPRT_INVALID_PARM;
    }
    memcpy(&g_cmd, buf, size);
    tal_util_reverse_byte(&g_cmd.dp_data_len, SIZEOF(UINT16_T));
    if (g_cmd.dp_data_len > size - 4 || g_cmd.dp_data_len > sizeof(g_cmd.dp_data)) {
        return OPRT_INVALID_PARM;
    }
    memcpy(&g_rsp, &g_cmd, size);

    // TAL_PR_HEXDUMP_INFO("dp_cmd", (VOID_T*)&g_cmd, (g_cmd.dp_data_len + 4));
    // TAL_PR_INFO("DP RX: id=%u type=%u len=%u first=0x%02x",
    //             g_cmd.dp_id, g_cmd.dp_type, g_cmd.dp_data_len,
    //             g_cmd.dp_data_len ? g_cmd.dp_data[0] : 0U);

    switch (g_cmd.dp_id) {
        case DP_ID_SWITCH:
            if (g_cmd.dp_data_len != DT_BOOL_LEN) {
                TAL_PR_ERR("DP SWITCH: invalid data length=%u",
                           g_cmd.dp_data_len);
                return OPRT_INVALID_PARM;
            }
            {
                BOOL_T enabled = app_dp_is_true(g_cmd.dp_data,
                                                g_cmd.dp_data_len);
                sg_switch_enabled = enabled;
                (VOID_T)app_dp_save_persistent();
                OPERATE_RET ret = faqiuji_mcu_is_ping_ok() ?
                                  faqiuji_mcu_control_set(enabled) :
                                  OPRT_OK;
                TAL_PR_INFO("DP SWITCH: enabled=%d mcu_control_ret=%d",
                            enabled, ret);
                if (ret != OPRT_OK) {
                    return ret;
                }
            }
            break;
        case DP_ID_MODE: {
            UINT8_T launch_mode = app_mode_to_launch_mode(g_cmd.dp_data,
                                                            g_cmd.dp_data_len);
            OPERATE_RET ret;
            if (launch_mode > 2U) {
                TAL_PR_ERR("DP MODE: unsupported value");
                return OPRT_INVALID_PARM;
            }
            sg_launch_mode = launch_mode;
            (VOID_T)app_dp_save_persistent();
            TAL_PR_INFO("DP MODE: configure launch mode=%u", launch_mode);
            ret = faqiuji_mcu_is_ping_ok() ?
                  faqiuji_mcu_config_set(sg_launch_mode,
                                         sg_auto_tease_count,
                                         sg_standby_tease_min) :
                  OPRT_OK;
            TAL_PR_INFO("DP MODE: mcu_config_ret=%d", ret);
            if (ret != OPRT_OK) {
                return ret;
            }
            break;
        }
        case DP_ID_AUTO_TEASE_TIME: {
            UINT32_T count;
            OPERATE_RET ret;
            if (g_cmd.dp_data_len != DT_VALUE_LEN) return OPRT_INVALID_PARM;
            count = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                    ((UINT32_T)g_cmd.dp_data[1] << 16) |
                    ((UINT32_T)g_cmd.dp_data[2] << 8) |
                    g_cmd.dp_data[3];
            if (count < 10U) count = 10U;
            if (count > 90U) count = 90U;
            sg_auto_tease_count = (UINT16_T)count;
            (VOID_T)app_dp_save_persistent();
            TAL_PR_INFO("DP AUTO_TEASE_TIME: count=%u", sg_auto_tease_count);
            ret = faqiuji_mcu_is_ping_ok() ?
                  faqiuji_mcu_config_set(sg_launch_mode,
                                         sg_auto_tease_count,
                                         sg_standby_tease_min) :
                  OPRT_OK;
            TAL_PR_INFO("DP AUTO_TEASE_TIME: mcu_config_ret=%d", ret);
            if (ret != OPRT_OK) {
                return ret;
            }
            break;
        }
        case DP_ID_STANDBY_TEASE_TIME: {
            UINT32_T minutes;
            OPERATE_RET ret;
            if (g_cmd.dp_data_len != DT_VALUE_LEN) return OPRT_INVALID_PARM;
            minutes = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                      ((UINT32_T)g_cmd.dp_data[1] << 16) |
                      ((UINT32_T)g_cmd.dp_data[2] << 8) |
                      g_cmd.dp_data[3];
            if (minutes < 10U) minutes = 10U;
            if (minutes > 30U) minutes = 30U;
            sg_standby_tease_min = (UINT16_T)minutes;
            (VOID_T)app_dp_save_persistent();
            TAL_PR_INFO("DP STANDBY_TEASE_TIME: minutes=%u",
                        sg_standby_tease_min);
            ret = faqiuji_mcu_is_ping_ok() ?
                  faqiuji_mcu_config_set(sg_launch_mode,
                                         sg_auto_tease_count,
                                         sg_standby_tease_min) :
                  OPRT_OK;
            TAL_PR_INFO("DP STANDBY_TEASE_TIME: mcu_config_ret=%d", ret);
            if (ret != OPRT_OK) {
                return ret;
            }
            break;
        }
        case DP_ID_SOUND_MODE:
            TAL_PR_INFO("DP SOUND_MODE: len=%u type=%u first=%u",
                        g_cmd.dp_data_len, g_cmd.dp_type,
                        g_cmd.dp_data_len ? g_cmd.dp_data[0] : 0);
            app_audio_stop_current();
            {
                UINT8_T file_id = app_sound_mode_to_file(g_cmd.dp_data, g_cmd.dp_data_len);
                sg_comfort_sound_file = file_id;
                sg_sound_mode = (file_id == FAQIUJI_AUDIO_FACTORY_SOUND_2) ? 1U : 0U;
                (VOID_T)app_dp_save_persistent();
                OPERATE_RET audio_ret = faqiuji_audio_preview_start(file_id);
                TAL_PR_INFO("DP SOUND_MODE: start file=%u ret=%d", file_id, audio_ret);
            }
            break;
        case DP_ID_SOUND:
            if (g_cmd.dp_data_len != DT_BOOL_LEN) {
                TAL_PR_ERR("DP SOUND: invalid data length=%u",
                           g_cmd.dp_data_len);
                return OPRT_INVALID_PARM;
            }
            {
                BOOL_T recording = app_dp_is_true(g_cmd.dp_data,
                                                  g_cmd.dp_data_len);
                OPERATE_RET ret = OPRT_OK;
                TAL_PR_INFO("DP SOUND: recording=%d", recording);
                if (recording) {
                    if (faqiuji_audio_is_playing()) {
                        faqiuji_audio_stop();
                    }
                    if (!faqiuji_audio_is_recording()) {
                        ret = faqiuji_audio_record_start(
                            FAQIUJI_AUDIO_USER_FILE_ID);
                        if (ret != OPRT_OK) {
                            g_cmd.dp_data[0] = 0;
                        } else {
                            sg_record_start_ms = tkl_system_get_millisecond();
                        }
                    }
                } else if (faqiuji_audio_is_recording()) {
                    ret = faqiuji_audio_record_stop();
                    if (ret != OPRT_OK) {
                        g_cmd.dp_data[0] = 1;
                    }
                }
                TAL_PR_INFO("DP SOUND: record_ret=%d final=%u", ret,
                            g_cmd.dp_data[0]);
            }
            break;
        case DP_ID_PLAY:
            TAL_PR_INFO("DP PLAY: len=%u type=%u value=%u recording=%d playing=%d",
                        g_cmd.dp_data_len, g_cmd.dp_type,
                        g_cmd.dp_data_len ? g_cmd.dp_data[0] : 0,
                        faqiuji_audio_is_recording(),
                        faqiuji_audio_is_playing());
            if (g_cmd.dp_data_len != DT_BOOL_LEN) {
                TAL_PR_ERR("DP PLAY: invalid data length=%u", g_cmd.dp_data_len);
                return OPRT_INVALID_PARM;
            }
            if (app_dp_is_true(g_cmd.dp_data, g_cmd.dp_data_len)) {
                if (faqiuji_audio_is_recording()) {
                    OPERATE_RET stop_ret = faqiuji_audio_record_stop();
                    TAL_PR_INFO("DP PLAY: stop recording ret=%d", stop_ret);
                }
                app_audio_stop_current();
                {
                    OPERATE_RET play_ret =
                        faqiuji_audio_play_start(FAQIUJI_AUDIO_USER_FILE_ID);
                    TAL_PR_INFO("DP PLAY: start user recording file=%u ret=%d",
                                FAQIUJI_AUDIO_USER_FILE_ID, play_ret);
                    if (play_ret != OPRT_OK) {
                        g_cmd.dp_data[0] = 0;
                    }
                }
            } else if (faqiuji_audio_is_playing()) {
                OPERATE_RET stop_ret = faqiuji_audio_stop();
                TAL_PR_INFO("DP PLAY: stop playback ret=%d", stop_ret);
                /* The command already reports false; consume the internal
                 * event so it is not reported a second time. */
                faqiuji_audio_take_play_finished();
            } else {
                TAL_PR_INFO("DP PLAY: already stopped");
            }
            break;
        case DP_ID_VOLUME: {
            UINT32_T volume;
            UINT8_T volume_value;
            if (g_cmd.dp_data_len != DT_VALUE_LEN) {
                TAL_PR_ERR("DP VOLUME: invalid data length=%u",
                           g_cmd.dp_data_len);
                return OPRT_INVALID_PARM;
            }
            volume = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                     ((UINT32_T)g_cmd.dp_data[1] << 16) |
                     ((UINT32_T)g_cmd.dp_data[2] << 8) |
            g_cmd.dp_data[3];
            if (volume < 1) volume = 1;
            if (volume > 100) volume = 100;
            volume_value = (UINT8_T)volume;
            sg_volume = volume_value;
            (VOID_T)app_dp_save_persistent();
            faqiuji_audio_set_volume(volume_value);
            TAL_PR_INFO("DP VOLUME: volume=%u", volume_value);
            break;
        }
        case DP_ID_MOTOR_SPEED: {
            UINT32_T motor_speed;
            OPERATE_RET ret;

            if (g_cmd.dp_type != DT_VALUE ||
                g_cmd.dp_data_len != DT_VALUE_LEN) {
                TAL_PR_ERR("DP MOTOR_SPEED: invalid type=%u length=%u",
                           g_cmd.dp_type, g_cmd.dp_data_len);
                return OPRT_INVALID_PARM;
            }
            motor_speed = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                          ((UINT32_T)g_cmd.dp_data[1] << 16) |
                          ((UINT32_T)g_cmd.dp_data[2] << 8) |
                          g_cmd.dp_data[3];
            if (motor_speed < 1U || motor_speed > 100U) {
                TAL_PR_ERR("DP MOTOR_SPEED: out of range=%lu",
                           (unsigned long)motor_speed);
                return OPRT_INVALID_PARM;
            }
            if (sg_motor_speed != (UINT8_T)motor_speed) {
                sg_motor_speed = (UINT8_T)motor_speed;
                (VOID_T)app_dp_save_persistent();
            }
            ret = faqiuji_mcu_is_ping_ok() ?
                  faqiuji_mcu_duty_set(sg_motor_speed) : OPRT_OK;
            TAL_PR_INFO("DP MOTOR_SPEED: speed=%u mcu_duty_ret=%d",
                        sg_motor_speed, ret);
            if (ret != OPRT_OK) {
                return ret;
            }
            break;
        }
        default:
            TAL_PR_WARN("DP UNKNOWN: id=%u type=%u len=%u",
                        g_cmd.dp_id, g_cmd.dp_type, g_cmd.dp_data_len);
            break;
    }

    /* 对 rw DP 回复确认上报 */
    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);

    return OPRT_OK;
}

OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T* buf, UINT32_T size)
{
    UINT8_T dp_type = app_dp_type(dp_id);
    if (buf == NULL || size > sizeof(g_rsp.dp_data)) {
        return OPRT_INVALID_PARM;
    }
    /* Keep MCU state even when the phone is disconnected. */
    switch (dp_id) {
        case DP_ID_BATTERY:
            if (size != DT_VALUE_LEN || buf[0] != 0U ||
                buf[1] != 0U || buf[2] != 0U || buf[3] > 100U) {
                return OPRT_INVALID_PARM;
            }
            sg_battery_percent = buf[3];
            sg_battery_valid = TRUE;
            break;
        case DP_ID_RADAR_SENSE:
            if (size != DT_ENUM_LEN || buf[0] > 1U) {
                return OPRT_INVALID_PARM;
            }
            sg_radar_state = buf[0];
            sg_radar_valid = TRUE;
            break;
        case DP_ID_WORK_STATE:
            if (size != DT_ENUM_LEN || buf[0] > 3U) {
                return OPRT_INVALID_PARM;
            }
            sg_work_state = buf[0];
            sg_work_valid = TRUE;
            break;
        case DP_ID_FAULT:
            if (size != 1U || buf[0] > 3U) {
                return OPRT_INVALID_PARM;
            }
            sg_fault = buf[0];
            sg_fault_valid = TRUE;
            break;
        case DP_ID_SWITCH:
            if (size != DT_BOOL_LEN || buf[0] > 1U) {
                return OPRT_INVALID_PARM;
            }
            sg_switch_enabled = buf[0] ? TRUE : FALSE;
            break;
        default:
            break;
    }
    memset(&g_rsp, 0, SIZEOF(demo_dp_t));

    g_rsp.dp_id = dp_id;
    g_rsp.dp_type = dp_type;
    g_rsp.dp_data_len = (UINT16_T)size;
    memcpy(g_rsp.dp_data, buf, size);
    UINT16_T rsp_len = (UINT16_T)(size + 4);

    tal_util_reverse_byte(&g_rsp.dp_data_len, SIZEOF(UINT16_T));

    // TAL_PR_HEXDUMP_INFO("dp_rsp", (VOID_T*)&g_rsp, rsp_len);

    return tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE, DP_SEND_FOR_CLOUD_PANEL, DP_SEND_WITHOUT_RESPONSE, (VOID_T*)&g_rsp, rsp_len);
}

VOID_T app_dp_process_audio_events(VOID_T)
{
    UINT8_T value = 0;
    UINT32_T now = tkl_system_get_millisecond();

    if (faqiuji_audio_is_recording() &&
        (now - sg_record_start_ms) >= 10000UL) {
        if (faqiuji_audio_record_stop() == OPRT_OK) {
            app_dp_report(DP_ID_SOUND, &value, DT_BOOL_LEN);
        }
    }

    if (faqiuji_audio_take_play_finished()) {
        if (!sg_sleep_audio_active) {
            app_dp_report(DP_ID_PLAY, &value, DT_BOOL_LEN);
        }
    }

    if (!sg_sleep_audio_active) {
        return;
    }
    if ((now - sg_sleep_phase_start_ms) < FAQIUJI_SLEEP_AUDIO_PHASE_MS) {
        return;
    }

    sg_sleep_phase_start_ms = now;
    if (sg_sleep_audio_phase == FAQIUJI_AUDIO_SLEEP_OWNER) {
        faqiuji_audio_stop();
        sg_sleep_audio_phase = FAQIUJI_AUDIO_SLEEP_COMFORT;
        (void)app_sleep_audio_start();
    } else {
        faqiuji_audio_stop();
        (void)app_sleep_audio_start();
    }
}

VOID_T app_dp_set_work_state(UINT8_T work_state)
{
    if (work_state == 1U) {
        if (!sg_sleep_audio_active) {
            sg_sleep_audio_active = TRUE;
            sg_sleep_audio_phase = FAQIUJI_AUDIO_SLEEP_OWNER;
            sg_sleep_phase_start_ms = tkl_system_get_millisecond();
            (void)app_sleep_audio_start();
        }
    } else if (work_state == 0U || work_state == 2U ||
               work_state == 3U) {
        if (sg_sleep_audio_active) {
            sg_sleep_audio_active = FALSE;
            if (faqiuji_audio_is_playing()) {
                (void)faqiuji_audio_stop();
            }
        }
    }
}

VOID_T app_dp_report_all(VOID_T)
{
    CONST struct {
        UINT8_T id;
        UINT32_T value;
        BOOL_T valid;
    } items[] = {
        {DP_ID_SWITCH, sg_switch_enabled ? 1U : 0U, TRUE},
        {DP_ID_MODE, sg_launch_mode, TRUE},
        {DP_ID_SOUND_MODE, sg_sound_mode, TRUE},
        {DP_ID_SOUND, faqiuji_audio_is_recording() ? 1U : 0U, TRUE},
        {DP_ID_PLAY, (faqiuji_audio_is_playing() && !sg_sleep_audio_active) ?
                     1U : 0U, TRUE},
        {DP_ID_VOLUME, sg_volume, TRUE},
        {DP_ID_BATTERY, sg_battery_percent, sg_battery_valid},
        {DP_ID_FAULT, sg_fault, sg_fault_valid},
        {DP_ID_WORK_STATE, sg_work_state, sg_work_valid},
        {DP_ID_AUTO_TEASE_TIME, sg_auto_tease_count, TRUE},
        {DP_ID_STANDBY_TEASE_TIME, sg_standby_tease_min, TRUE},
        {DP_ID_RADAR_SENSE, sg_radar_state, sg_radar_valid},
        {DP_ID_MOTOR_SPEED, sg_motor_speed, TRUE},
    };
    UINT8_T payload[(sizeof(items) / sizeof(items[0])) * (4U + DT_VALUE_LEN)];
    UINT16_T length = 0U;
    UINT16_T i;
    OPERATE_RET ret;

    /* Send one KLV list to avoid filling the BLE event queue. */
    for (i = 0U; i < sizeof(items) / sizeof(items[0]); ++i) {
        UINT8_T type;
        if (!items[i].valid) {
            continue;
        }
        type = app_dp_type(items[i].id);
        payload[length++] = items[i].id;
        payload[length++] = type;
        payload[length++] = 0U;
        payload[length++] = type == DT_VALUE ? DT_VALUE_LEN : 1U;
        if (type == DT_VALUE) {
            payload[length++] = (UINT8_T)(items[i].value >> 24U);
            payload[length++] = (UINT8_T)(items[i].value >> 16U);
            payload[length++] = (UINT8_T)(items[i].value >> 8U);
        }
        payload[length++] = (UINT8_T)items[i].value;
    }
    ret = tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE,
                                DP_SEND_FOR_CLOUD_PANEL,
                                DP_SEND_WITHOUT_RESPONSE, payload, length);
    TAL_PR_INFO("DP report all: len=%u ret=%d", length, ret);
}
