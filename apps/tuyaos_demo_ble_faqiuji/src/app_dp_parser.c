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

STATIC VOID_T app_audio_stop_current(VOID_T)
{
    if (faqiuji_audio_is_recording()) {
        faqiuji_audio_record_stop();
    } else if (faqiuji_audio_is_playing()) {
        faqiuji_audio_stop();
    }
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
        case DP_ID_SOUND_MODE:
            app_audio_stop_current();
            faqiuji_audio_play_start(app_sound_mode_to_file(g_cmd.dp_data, g_cmd.dp_data_len));
            break;
        case DP_ID_SOUND:
            if (app_dp_is_true(g_cmd.dp_data, g_cmd.dp_data_len)) {
                app_audio_stop_current();
                faqiuji_audio_record_start(FAQIUJI_AUDIO_USER_RECORDING);
            } else if (faqiuji_audio_is_recording()) {
                faqiuji_audio_record_stop();
            }
            break;
        case DP_ID_PLAY:
            if (app_dp_is_true(g_cmd.dp_data, g_cmd.dp_data_len)) {
                app_audio_stop_current();
                faqiuji_audio_play_start(FAQIUJI_AUDIO_USER_RECORDING);
            } else if (faqiuji_audio_is_playing()) {
                faqiuji_audio_stop();
            }
            break;
        default:
            break;
    }

    /* 对 rw DP 回复确认上报 */
    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);

    return OPRT_OK;
}

OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T* buf, UINT32_T size)
{
    UINT8_T dp_type = g_cmd.dp_type;
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
