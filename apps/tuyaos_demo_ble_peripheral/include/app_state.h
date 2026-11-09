/**
 * @file app_state.h
 * @brief 设备状态机 — 管理 work/standby/charging/charge_done 优先级及转换
 * @version 1.2
 * @date 2026-06-16
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 电源架构（统一开关机）：
 *   物理按键短按和蓝牙 DP_SWITCH 控制同一个电源状态，不分软件/硬件开关。
 *   开机 → 设备可进入工作模式（电机可运动）
 *   关机 → 低功耗模式，所有功能关闭，需按键唤醒
 *
 * USB插入时：强制关机；拔出后需再次开机。
 */

#ifndef __APP_STATE_H__
#define __APP_STATE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 待机超时时间 ms (5 分钟) */
#define STANDBY_TIMEOUT_MS      (5 * 60 * 1000UL)

/** 设备状态枚举 — 优先级从高到低：charging > work > standby > charge_done
 *  注：与 DP_ID_WORK_STATE 枚举值一致 */
typedef enum {
    DEV_STATE_WORK        = 0,    /**< 工作中（可进行电机运动） */
    DEV_STATE_STANDBY     = 1,    /**< 待机（开机但5分钟无操作/超时，指示灯熄灭，BLE可接收指令） */
    DEV_STATE_CHARGING    = 2,    /**< 充电中（最高优先级，电机不工作，红灯常亮，强制关机） */
    DEV_STATE_CHARGE_DONE = 3,    /**< 满电（指示灯熄灭，仅满电推送） */
} dev_state_t;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief 初始化状态机（定时器、默认状态）
 */
VOID_T app_state_init(VOID_T);

/**
 * @brief 获取当前设备状态
 * @return dev_state_t
 */
dev_state_t app_state_get(VOID_T);

/**
 * @brief 设置充电状态
 * @param[in] charging TRUE=正在充电, FALSE=未充电
 *
 * 充电插入时：强制关机。
 * 充电拔出时：不自动开机，需按键/蓝牙再次开机。
 */
VOID_T app_state_set_charging(BOOL_T charging);

/**
 * @brief 获取是否正在充电
 * @return TRUE=充电中
 */
BOOL_T app_state_is_charging(VOID_T);

/**
 * @brief 设置满电状态
 * @param[in] done TRUE=满电
 */
VOID_T app_state_set_charge_done(BOOL_T done);

/**
 * @brief 切换电源开关（由物理按键短按调用）
 *        控制设备进入/退出低功耗模式
 * @return TRUE=开机, FALSE=关机
 */
BOOL_T app_state_toggle_power(VOID_T);

/**
 * @brief 设置电源开关（由蓝牙 DP_SWITCH / 临界低电自动关机调用）
 * @param[in] on TRUE=开机, FALSE=关机
 * @return TRUE=开机, FALSE=关机
 */
BOOL_T app_state_set_power(BOOL_T on);

/**
 * @brief 设备是否开机
 * @return TRUE=开机
 */
BOOL_T app_state_is_powered_on(VOID_T);

/**
 * @brief 待机定时器复位（由触摸/按键/DP操作触发）
 */
VOID_T app_state_reset_standby_timer(VOID_T);

/**
 * @brief 状态机周期性处理（由主循环调用）
 *  - 待机超时检查
 *  - 充电/满电自动转换
 */
VOID_T app_state_process(VOID_T);

/**
 * @brief 获取设备状态对应 DP_WORK_STATE 的枚举值
 * @return UINT8_T 枚举值
 */
UINT8_T app_state_get_dp_enum(VOID_T);

/**
 * @brief 注册设备状态变更回调（供 LED 模块等订阅）
 * @param[in] cb 回调: void cb(dev_state_t old_state, dev_state_t new_state)
 */
VOID_T app_state_register_change_cb(VOID_T (*cb)(dev_state_t, dev_state_t));

/**
 * @brief 注册电源开关回调（供电机/主循环订阅：启停电机 + BLE广播等）
 * @param[in] on_cb 开机回调
 * @param[in] off_cb 关机回调
 */
VOID_T app_state_register_power_cb(VOID_T (*on_cb)(VOID_T), VOID_T (*off_cb)(VOID_T));

#ifdef __cplusplus
}
#endif

#endif /* __APP_STATE_H__ */
