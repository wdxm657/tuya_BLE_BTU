/**
 * @file app_dp_parser.h
 * @brief afp pawswiff DP parser.
 */

#ifndef __APP_DP_PARSER_H__
#define __APP_DP_PARSER_H__

#include "tuya_cloud_types.h"
#include "app_motor_tuning_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP_ID_SWITCH           1   /* bool rw: App on/off */
#define DP_ID_MODE             4   /* enum rw: ConstantSpeed/VariableSpeed */
#define DP_ID_BATTERY         18   /* value ro: 0~100 */
#define DP_ID_WORK_STATE      26   /* enum ro: work/standby/turnoff */
#define DP_ID_GAME_INTERVAL   28   /* value rw: 10~120 minutes */
#define DP_ID_STEPLESS_CONTROL 30  /* value rw: control 0~100 maps to PWM duty 40~80 */
#define DP_ID_GAME_REPEAT     35   /* enum rw: 1/2/3/4/5/unlimited (indices 0~5) */

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

/* Normal boot only: restore user choices before registering run callbacks. */
OPERATE_RET app_dp_load_persistent(VOID_T);
OPERATE_RET app_dp_clear_persistent(VOID_T);
/* Persist the current APP power choice after a local key action. */
OPERATE_RET app_dp_save_persistent(VOID_T);
OPERATE_RET app_dp_parser(UINT8_T *buf, UINT32_T size);
OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T *buf, UINT32_T size);
VOID_T app_dp_report_all(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
