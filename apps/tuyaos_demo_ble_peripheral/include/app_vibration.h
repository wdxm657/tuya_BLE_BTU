/**
 * @file app_vibration.h
 * @brief 振动传感器模块 — GPIO 中断检测，用于互动模式激活
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __APP_VIBRATION_H__
#define __APP_VIBRATION_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief 初始化振动传感器（GPIO 中断注册）
 */
VOID_T app_vibration_init(VOID_T);

/**
 * @brief 注册振动触发回调（由 tuya_sdk_callback 调用，传入交互触发函数）
 * @param[in] cb 回调函数指针
 */
VOID_T app_vibration_register_cb(VOID_T (*cb)(VOID_T));

/**
 * @brief 获取上次振动触发以来的去抖时间 ms（用于防抖判断）
 * @return 距离上次振动的毫秒数
 */
UINT32_T app_vibration_get_last_tick_ms(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_VIBRATION_H__ */
