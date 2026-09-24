#ifndef __APP_DP_PARSER_H__
#define __APP_DP_PARSER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP_ID_SWITCH       1
#define DP_ID_MODE         4
#define DP_ID_SOUND_MODE   14
#define DP_ID_SOUND        15
#define DP_ID_PLAY         16
#define DP_ID_VOLUME       30
#define DP_ID_BATTERY      18
#define DP_ID_FAULT        20
#define DP_ID_MOVEMENT_LEVEL 22
#define DP_ID_WORK_STATE    26
#define DP_ID_AUTO_TEASE_TIME 28
#define DP_ID_STANDBY_TEASE_TIME 29

#pragma pack(1)
typedef struct {
    UINT8_T  dp_id;
    UINT8_T  dp_type;
    UINT16_T dp_data_len;
    UINT8_T  dp_data[600];
} demo_dp_t;
#pragma pack()

extern demo_dp_t g_cmd;
extern demo_dp_t g_rsp;
extern UINT32_T  g_sn;

OPERATE_RET app_dp_parser(UINT8_T *buf, UINT32_T size);
OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T *buf, UINT32_T size);
VOID_T app_dp_process_audio_events(VOID_T);
VOID_T app_dp_set_work_state(UINT8_T work_state);

#ifdef __cplusplus
}
#endif

#endif
