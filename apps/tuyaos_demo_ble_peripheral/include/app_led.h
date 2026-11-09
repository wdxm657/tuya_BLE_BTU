/**
 * @file app_led.h
 * @brief LED 指示灯模块 — 根据设备状态切换 灭/蓝闪/绿闪/红闪/红常亮
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 产品定义指示灯行为：
 *   熄灭：关机（app上操作），或电池已充满电
 *   蓝色灯闪烁：蓝牙未连接
 *   绿色灯闪烁：工作状态
 *   红色闪烁：电量不足，请尽快为设备充电
 *   红色常亮：设备正在充电中
 */

#ifndef __APP_LED_H__
#define __APP_LED_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** LED 显示模式 */
typedef enum {
    LED_MODE_OFF,           /**< 熄灭：关机/待机 */
    LED_MODE_BLUE_BLINK,    /**< 配网中：蓝灯闪烁 */
    LED_MODE_GREEN_BLINK,   /**< 工作状态：绿灯闪烁 */
    LED_MODE_RED_BLINK,     /**< 电量不足：红灯闪烁 */
    LED_MODE_RED_SOLID,     /**< 充电中：红灯常亮 */
} led_mode_t;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief 初始化 LED 模块（GPIO+定时器）
 */
VOID_T app_led_init(VOID_T);

/**
 * @brief 设置 LED 显示模式
 * @param[in] mode LED 模式 @ref led_mode_t
 */
VOID_T app_led_set_mode(led_mode_t mode);

/**
 * @brief 根据当前设备状态统一刷新 LED 模式（自动判定优先级）
 *
 * 优先级（高→低）：
 *   充电中 → 红灯常亮
 *   低电量（工作中）→ 红灯闪烁
 *   工作正常 → 绿灯闪烁
 *   待机/满电/关机 → 熄灭
 */
VOID_T app_led_update(VOID_T);

/**
 * @brief 获取当前 LED 模式
 * @return led_mode_t
 */
led_mode_t app_led_get_mode(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_LED_H__ */
