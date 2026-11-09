/**
 * @file app_charge.h
 * @brief 充电检测模块 — 双引脚联合判定充电/满电/未插入
 * @version 1.2
 * @date 2026-06-17
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 硬件连接：
 *   USB_DET (D3)      — 上拉输入，高电平=USB插入
 *   CHARGE_STATE (A1) — 上拉输入，低电平=充电中，高电平=满电/未充电
 *
 * 联合判定：
 *   USB_DET=LOW                 → 未插入USB
 *   USB_DET=HIGH, CHG_STATE=LOW → 充电中
 *   USB_DET=HIGH, CHG_STATE=HIGH→ 满电
 *
 * 检测方式：
 *   主：周期定时器(100ms)轮询双引脚电平
 *   辅：电池电压监测（当电压 > 4.0V 时辅助判断）
 */

#ifndef __APP_CHARGE_H__
#define __APP_CHARGE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 电压辅助检测阈值 mV（高于此值视为可能正在充电） */
#define CHARGE_VOLT_THRESHOLD_MV    4000

/** 满电判定电压 mV（稳定在此值以上视为满电） */
#define CHARGE_FULL_VOLT_MV         4150

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief 初始化充电检测（双 GPIO 输入 + 周期轮询定时器）
 *   - 设置 USB_DET + CHARGE_STATE 为上拉输入
 *   - 创建并启动 100ms 周期定时器轮询双引脚电平
 *   - 启动时读取一次引脚状态，避免错过初始状态
 */
VOID_T app_charge_init(VOID_T);

/**
 * @brief 获取当前充电器插入状态（通过 GPIO 读取）
 * @return TRUE=充电器已插入
 */
BOOL_T app_charge_is_detected(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CHARGE_H__ */
