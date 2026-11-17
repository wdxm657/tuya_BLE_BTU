/** @file app_power_manager.h
 * @brief 应用模块接口
 */
#ifndef __APP_POWER_MANAGER_H__
#define __APP_POWER_MANAGER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册电源/USB/欠压及 sleep 回调，检查启动欠压。 */
VOID_T app_power_manager_init(VOID_T);
/** 主循环处理硬件唤醒、状态机和 retention 超时。 */
VOID_T app_power_manager_process(VOID_T);

#ifdef __cplusplus
}
#endif
#endif
