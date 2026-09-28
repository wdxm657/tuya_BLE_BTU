/**
 * @file app_dp_parser.h
 * @brief afp pawswiff DP parser.
 */

#ifndef __APP_DP_PARSER_H__
#define __APP_DP_PARSER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP_ID_SWITCH           1   /* bool rw: App on/off */
#define DP_ID_MODE             4   /* enum rw: active/simple/mild */
#define DP_ID_BATTERY         18   /* value ro: 0~100 */
#define DP_ID_WORK_STATE      26   /* enum ro: standby/charging/charge_done */
#define DP_ID_ALT_LASER_TIME  28   /* value rw: alternating laser chase seconds */
#define DP_ID_ALT_BUG_TIME    29   /* value rw: alternating bug hunt seconds */
#define DP_ID_STEPLESS_CONTROL 30  /* value rw: speed 0~100 maps to PWM duty 40~80 */

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
VOID_T app_dp_report_all(VOID_T);
OPERATE_RET app_dp_load_persistent(VOID_T);
OPERATE_RET app_dp_save_battery_percent(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
