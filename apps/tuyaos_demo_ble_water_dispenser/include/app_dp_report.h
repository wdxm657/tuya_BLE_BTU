/** @file app_dp_report.h
 * @brief 应用模块接口
 */
#ifndef __APP_DP_REPORT_H__
#define __APP_DP_REPORT_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

VOID_T app_dp_report_init(VOID_T);
VOID_T app_dp_report_on_connected(VOID_T);
VOID_T app_dp_report_on_disconnected(VOID_T);

#ifdef __cplusplus
}
#endif
#endif
