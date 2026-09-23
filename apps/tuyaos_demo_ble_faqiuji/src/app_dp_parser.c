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
STATIC UINT16_T sg_auto_tease_count = 20U;
STATIC UINT16_T sg_standby_tease_min = 20U;

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
    if (dp_id == DP_ID_VOLUME) return DT_VALUE;
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

    TAL_PR_HEXDUMP_INFO("dp_cmd", (VOID_T*)&g_cmd, (g_cmd.dp_data_len + 4));

    switch (g_cmd.dp_id) {
        case DP_ID_SWITCH:
            if (g_cmd.dp_data_len != DT_BOOL_LEN) {
                return OPRT_INVALID_PARM;
            }
            /* The switch business is reserved for a later implementation. */
            break;
        case DP_ID_MODE: {
            UINT8_T launch_mode = app_mode_to_launch_mode(g_cmd.dp_data,
                                                            g_cmd.dp_data_len);
            if (launch_mode > 2U) {
                TAL_PR_ERR("DP MODE: unsupported value");
                return OPRT_INVALID_PARM;
            }
            sg_launch_mode = launch_mode;
            TAL_PR_INFO("DP MODE: configure launch mode=%u", launch_mode);
            if (faqiuji_mcu_config_set(sg_launch_mode,
                                       sg_auto_tease_count,
                                       sg_standby_tease_min) != OPRT_OK) {
                return OPRT_COM_ERROR;
            }
            break;
        }
        case DP_ID_AUTO_TEASE_TIME: {
            UINT32_T count;
            if (g_cmd.dp_data_len != DT_VALUE_LEN) return OPRT_INVALID_PARM;
            count = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                    ((UINT32_T)g_cmd.dp_data[1] << 16) |
                    ((UINT32_T)g_cmd.dp_data[2] << 8) |
                    g_cmd.dp_data[3];
            if (count < 10U) count = 10U;
            if (count > 90U) count = 90U;
            sg_auto_tease_count = (UINT16_T)count;
            if (faqiuji_mcu_config_set(sg_launch_mode,
                                       sg_auto_tease_count,
                                       sg_standby_tease_min) != OPRT_OK) {
                return OPRT_COM_ERROR;
            }
            break;
        }
        case DP_ID_STANDBY_TEASE_TIME: {
            UINT32_T minutes;
            if (g_cmd.dp_data_len != DT_VALUE_LEN) return OPRT_INVALID_PARM;
            minutes = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                      ((UINT32_T)g_cmd.dp_data[1] << 16) |
                      ((UINT32_T)g_cmd.dp_data[2] << 8) |
                      g_cmd.dp_data[3];
            if (minutes < 10U) minutes = 10U;
            if (minutes > 30U) minutes = 30U;
            sg_standby_tease_min = (UINT16_T)minutes;
            if (faqiuji_mcu_config_set(sg_launch_mode,
                                       sg_auto_tease_count,
                                       sg_standby_tease_min) != OPRT_OK) {
                return OPRT_COM_ERROR;
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
                OPERATE_RET audio_ret = faqiuji_audio_preview_start(file_id);
                TAL_PR_INFO("DP SOUND_MODE: start file=%u ret=%d", file_id, audio_ret);
            }
            break;
        case DP_ID_SOUND:
            if (g_cmd.dp_data_len != DT_BOOL_LEN) {
                return OPRT_INVALID_PARM;
            }
            if (app_dp_is_true(g_cmd.dp_data, g_cmd.dp_data_len)) {
                if (faqiuji_audio_is_playing()) {
                    faqiuji_audio_stop();
                }
                if (!faqiuji_audio_is_recording()) {
                    if (faqiuji_audio_record_start(FAQIUJI_AUDIO_USER_FILE_ID) != OPRT_OK) {
                        g_cmd.dp_data[0] = 0;
                    }
                }
            } else if (faqiuji_audio_is_recording()) {
                if (faqiuji_audio_record_stop() != OPRT_OK) {
                    g_cmd.dp_data[0] = 1;
                }
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
            if (g_cmd.dp_data_len != DT_VALUE_LEN) return OPRT_INVALID_PARM;
            volume = ((UINT32_T)g_cmd.dp_data[0] << 24) |
                     ((UINT32_T)g_cmd.dp_data[1] << 16) |
                     ((UINT32_T)g_cmd.dp_data[2] << 8) |
                     g_cmd.dp_data[3];
            if (volume < 1) volume = 1;
            if (volume > 100) volume = 100;
            faqiuji_audio_set_volume((UINT8_T)volume);
            break;
        }
        default:
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
    memset(&g_rsp, 0, SIZEOF(demo_dp_t));

    g_rsp.dp_id = dp_id;
    g_rsp.dp_type = dp_type;
    g_rsp.dp_data_len = (UINT16_T)size;
    memcpy(g_rsp.dp_data, buf, size);
    UINT16_T rsp_len = (UINT16_T)(size + 4);

    tal_util_reverse_byte(&g_rsp.dp_data_len, SIZEOF(UINT16_T));

    TAL_PR_HEXDUMP_INFO("dp_rsp", (VOID_T*)&g_rsp, rsp_len);

    return tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE, DP_SEND_FOR_CLOUD_PANEL, DP_SEND_WITHOUT_RESPONSE, (VOID_T*)&g_rsp, rsp_len);
}

VOID_T app_dp_process_audio_events(VOID_T)
{
    UINT8_T value = 0;

    if (faqiuji_audio_take_play_finished()) {
        app_dp_report(DP_ID_PLAY, &value, DT_BOOL_LEN);
    }
}
