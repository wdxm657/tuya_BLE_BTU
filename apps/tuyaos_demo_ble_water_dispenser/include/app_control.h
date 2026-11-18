/** @file app_control.h
 * @brief 应用模块接口
 */
#ifndef __APP_CONTROL_H__
#define __APP_CONTROL_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

VOID_T app_control_init(VOID_T);
VOID_T app_control_boot(VOID_T);

#ifdef __cplusplus
}
#endif
#endif
