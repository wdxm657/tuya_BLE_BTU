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
/** 恢复出厂重启后的30秒配网窗口。 */
VOID_T app_power_manager_start_pairing(VOID_T);
BOOL_T app_power_manager_is_pairing(VOID_T);
/** 配网期间消费短按松开事件，结束配网并请求超低功耗。 */
BOOL_T app_power_manager_pairing_short_press(VOID_T);
/** 成功绑定后清除待配网标志，结束配网窗口。 */
VOID_T app_power_manager_on_bound(VOID_T);

#ifdef __cplusplus
}
#endif
#endif
