/**
 * @file app_state.c
 * @brief 设备状态机实现 — 管理 work/standby/charging/charge_done 状态优先级及转换
 * @version 1.2
 * @date 2026-06-16
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 设备状态优先级（高→低）：charging > work > standby > charge_done
 *   - charging：充电中，电机不工作，红灯常亮，强制关机
 *   - work：正常工作，电机可运动
 *   - standby：5分钟无操作进入待机，指示灯熄灭，BLE可接收指令
 *   - charge_done：满电，仅推送通知
 *
 * 电源架构（统一开关机）：
 *   物理按键短按和蓝牙 DP_SWITCH 控制同一个电源状态，不分软件/硬件开关。
 *   USB插入时：强制关机；拔出后需再次开机。
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "board.h"

#include "app_state.h"

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* 当前设备状态 */
STATIC dev_state_t s_dev_state = DEV_STATE_WORK;

/* 电源开关 */
STATIC BOOL_T  s_powered = FALSE;

/* 充电标志 */
STATIC BOOL_T  s_charging = FALSE;

/* 满电标志 */
STATIC BOOL_T  s_charge_done = FALSE;

/* 待机定时器 */
STATIC TIMER_ID s_standby_timer_id = NULL;
/* 状态变更回调 */
STATIC VOID_T (*s_state_change_cb)(dev_state_t, dev_state_t) = NULL;

/* 电源回调（电机启停 + BLE 广播等） */
STATIC VOID_T (*s_power_on_cb)(VOID_T)  = NULL;
STATIC VOID_T (*s_power_off_cb)(VOID_T) = NULL;

/* 待机超时定时器回调 — 进入待机 */
STATIC VOID_T app_state_standby_timeout_handler(TIMER_ID timer_id, VOID_T *arg);

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 内部更新设备状态（含变更通知）
 * @param[in] new_state 新状态
 */
STATIC VOID_T app_state_set(dev_state_t new_state)
{
    if (new_state == s_dev_state) {
        return;
    }

    dev_state_t old_state = s_dev_state;
    s_dev_state = new_state;

    TAL_PR_INFO("[state] %d -> %d", old_state, new_state);

    /* 通知回调 */
    if (s_state_change_cb != NULL) {
        s_state_change_cb(old_state, new_state);
    }
}

/**
 * @brief 根据电源/充电状态重新计算设备状态
 *
 * 优先级：charging(最高) > charge_done > powered→work > 未开机→standby
 * 注意：charging 检查必须在 !s_powered 之前，因为充电插入会强制关机，
 *       但仍需将设备状态设为 CHARGING（而非 STANDBY），以正确显示红灯常亮。
 */
STATIC VOID_T app_state_recalculate(VOID_T)
{
    if (s_charging) {
        /* 充电优先级最高 */
        app_state_set(DEV_STATE_CHARGING);
        /* 充电时停止待机定时器 */
        if (s_standby_timer_id != NULL) {
            tal_sw_timer_stop(s_standby_timer_id);
        }
        return;
    }

    if (s_charge_done) {
        app_state_set(DEV_STATE_CHARGE_DONE);
        return;
    }

    if (!s_powered) {
        app_state_set(DEV_STATE_STANDBY);
        return;
    }

    /* 开机 → 工作状态 */
    app_state_set(DEV_STATE_WORK);
}

/**
 * @brief 待机超时回调
 */
STATIC VOID_T app_state_standby_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    if (!s_powered) {
        return;
    }
    /* 只有在 work 状态时才进入 standby */
    if (s_dev_state == DEV_STATE_WORK) {
        TAL_PR_INFO("[state] work timeout -> standby");
        app_state_set(DEV_STATE_STANDBY);
    }
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

VOID_T app_state_init(VOID_T)
{
    /* 创建待机定时器 */
    tal_sw_timer_create(app_state_standby_timeout_handler, NULL, &s_standby_timer_id);

    /* 默认状态：关机、未充电 */
    s_powered     = FALSE;
    s_charging    = FALSE;
    s_charge_done = FALSE;
    s_dev_state   = DEV_STATE_STANDBY;  /* 默认待机，等待用户交互 */

    /* 启动待机定时器 */
    tal_sw_timer_start(s_standby_timer_id, STANDBY_TIMEOUT_MS, TAL_TIMER_ONCE);

    TAL_PR_INFO("[state] initialized (powered=%d)", s_powered);
}

dev_state_t app_state_get(VOID_T)
{
    return s_dev_state;
}

VOID_T app_state_set_charging(BOOL_T charging)
{
    if (s_charging == charging) {
        return;
    }

    s_charging = charging;
    TAL_PR_INFO("[state] charging=%d", charging);

    if (charging) {
        s_charge_done = FALSE;  /* 开始充电时清除满电标志 */

        /* USB插入：强制关机 */
        if (s_powered) {
            TAL_PR_INFO("[state] usb plugged -> force power off");
            app_state_set_power(FALSE);
        }
    } else {
        /* 停止充电时，不自动开机（需按键/蓝牙再次开机） */
        if (s_standby_timer_id != NULL) {
            tal_sw_timer_start(s_standby_timer_id, STANDBY_TIMEOUT_MS, TAL_TIMER_ONCE);
        }
    }

    app_state_recalculate();
}

BOOL_T app_state_is_charging(VOID_T)
{
    return s_charging;
}

VOID_T app_state_set_charge_done(BOOL_T done)
{
    if (s_charge_done == done) {
        return;
    }

    s_charge_done = done;
    TAL_PR_INFO("[state] charge_done=%d", done);

    app_state_recalculate();
}

BOOL_T app_state_toggle_power(VOID_T)
{
    return app_state_set_power(!s_powered);
}

BOOL_T app_state_set_power(BOOL_T on)
{

    /* 充电中不允许开机 */
    TAL_PR_INFO("[state] s_charging=%s", s_charging ? "1" : "0");
    if (on && s_charging) {
        TAL_PR_WARN("[state] can't power on while charging");
        return FALSE;
    }

    s_powered = on;
    TAL_PR_INFO("[state] power=%s", s_powered ? "ON" : "OFF");

    if (s_powered) {
        /* 开机：重新计算设备状态 */
        app_state_recalculate();
        /* 启动待机定时器 */
        if (s_standby_timer_id != NULL) {
            tal_sw_timer_start(s_standby_timer_id, STANDBY_TIMEOUT_MS, TAL_TIMER_ONCE);
        }
        /* 回调：启动电机 + 恢复 BLE 广播等 */
        if (s_power_on_cb != NULL) {
            s_power_on_cb();
        }
    } else {
        /* 关机：进入低功耗模式，所有功能关闭 */
        app_state_set(DEV_STATE_STANDBY);
        if (s_standby_timer_id != NULL) {
            tal_sw_timer_stop(s_standby_timer_id);
        }
        /* 回调：停止电机、关闭 LED、进入低功耗 */
        if (s_power_off_cb != NULL) {
            s_power_off_cb();
        }
    }

    return s_powered;
}

BOOL_T app_state_is_powered_on(VOID_T)
{
    return s_powered;
}

VOID_T app_state_reset_standby_timer(VOID_T)
{
    if (!s_powered) {
        return;
    }

    /* 如果当前是 standby，恢复 work */
    if (s_dev_state == DEV_STATE_STANDBY) {
        TAL_PR_INFO("[state] touch wakeup -> work");
        app_state_set(DEV_STATE_WORK);
    }

    /* 重启待机定时器 */
    if (s_standby_timer_id != NULL) {
        tal_sw_timer_stop(s_standby_timer_id);
        tal_sw_timer_start(s_standby_timer_id, STANDBY_TIMEOUT_MS, TAL_TIMER_ONCE);
    }
}

VOID_T app_state_process(VOID_T)
{
    /* 周期性处理（目前暂无需要轮询的逻辑，充电检测由外部驱动） */
    /* 本函数保留作为扩展点 */
}

UINT8_T app_state_get_dp_enum(VOID_T)
{
    return (UINT8_T)s_dev_state;
}

VOID_T app_state_register_change_cb(VOID_T (*cb)(dev_state_t, dev_state_t))
{
    s_state_change_cb = cb;
}

VOID_T app_state_register_power_cb(VOID_T (*on_cb)(VOID_T), VOID_T (*off_cb)(VOID_T))
{
    s_power_on_cb  = on_cb;
    s_power_off_cb = off_cb;
}
