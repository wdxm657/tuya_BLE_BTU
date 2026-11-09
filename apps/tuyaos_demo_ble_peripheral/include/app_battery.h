/**
 * @file app_battery.h
 * @brief 电池电量监测模块 — 连续 ADC 采样 + 定时检测 + 临界低电保护
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 架构：
 *   1.初始化时开启 AD_BAT_SWITCH（持续使能分压电路）
 *   2.2 秒定时器周期性读取电压、更新缓存、检测阈值
 *   3.外部模块通过 Get 接口获取缓存的电压/百分比
 *   4.临界低电量时自动进入深度睡眠保护电池
 */

#ifndef __APP_BATTERY_H__
#define __APP_BATTERY_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/* ---------- 电池电压范围 ---------- */
#define BATTERY_VOLT_FULL_MV        4200    /**< 满电电压 mV */
#define BATTERY_VOLT_EMPTY_MV       3000    /**< 亏电电压 mV */

/* ---------- ADC 参数 ---------- */
#define BATTERY_ADC_CH              6       /**< 电池 ADC 通道号 (PB6) */
#define BATTERY_DIVIDER_RATIO       2       /**< 硬件分压比 */

/* ---------- 采样滤波 ---------- */
#define BATTERY_ADC_SAMPLES         8       /**< 每次读取的 ADC 次数（均值滤波） */
#define BATTERY_SAMPLE_INTERVAL_MS  1       /**< 两次采样间隔 ms */

/* ---------- 监测周期 ---------- */
#define BATTERY_MONITOR_INTERVAL_MS 1000   /**< 电池监测定时器周期 ms（1 秒） */

/* ---------- 阈值 ---------- */
#define BATTERY_LOW_THRESHOLD       20      /**< 低电量阈值 % */
#define BATTERY_CRITICAL_THRESHOLD  5       /**< 临界低电阈值 %（触发深度睡眠保护） */

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief 初始化电池 ADC + 开启 AD_BAT_SWITCH + 启动监测定时器
 * @return OPRT_OK 成功
 */
OPERATE_RET app_battery_init(VOID_T);

/**
 * @brief 读取一次电池电压（AD_BAT_SWITCH 已常开，直接 ADC 采样）
 * @param[out] vol_mv 电池电压 mV
 * @return OPRT_OK 成功
 */
OPERATE_RET app_battery_read_voltage(INT32_T *vol_mv);

/**
 * @brief 获取缓存的电量百分比（由监测定时器持续更新）
 * @return UINT8_T 0~100%
 */
UINT8_T app_battery_get_percent(VOID_T);

/**
 * @brief 获取缓存的电压 mV
 * @return INT32_T mV
 */
INT32_T app_battery_get_voltage(VOID_T);

/**
 * @brief 检查是否低电量（低于 BATTERY_LOW_THRESHOLD %）
 * @return TRUE=低电量
 */
BOOL_T app_battery_is_low(VOID_T);

/**
 * @brief 检查是否临界低电（低于 BATTERY_CRITICAL_THRESHOLD %）
 * @return TRUE=临界，需立即进入深度睡眠
 */
BOOL_T app_battery_is_critical(VOID_T);

/**
 * @brief 注册临界低电回调（由 tuya_sdk_callback 注册关机逻辑）
 * @param[in] cb 回调函数
 */
VOID_T app_battery_register_critical_cb(VOID_T (*cb)(VOID_T));

#ifdef __cplusplus
}
#endif

#endif /* __APP_BATTERY_H__ */
