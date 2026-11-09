/**
 * @file app_charge.c
 * @brief 充电检测实现 — 双引脚联合判定充电状态
 * @version 1.2
 * @date 2026-06-17
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 双引脚判定：
 *   USB_DET (D3)     — 高低电平检测 USB 插入（高=有USB）
 *   CHARGE_STATE (A1) — 充电IC状态（低=充电中，高=满电/未充电）
 *
 * 状态组合：
 *   USB_DET=LOW                    → USB未插入，不充电
 *   USB_DET=HIGH, CHARGE_STATE=LOW → USB已插入，充电中 → DEV_STATE_CHARGING
 *   USB_DET=HIGH, CHARGE_STATE=HIGH→ USB已插入，已满电 → DEV_STATE_CHARGE_DONE
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"
#include "board.h"

#include "app_charge.h"
#include "app_state.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** GPIO 轮询间隔 ms */
#define CHARGE_POLL_MS              100

/** 充电状态枚举 */
typedef enum {
    CHG_STATE_NO_USB,       /**< USB未插入 */
    CHG_STATE_CHARGING,     /**< USB已插入，充电中 */
    CHG_STATE_CHARGE_DONE,  /**< USB已插入，已满电 */
} charge_state_t;

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* 轮询定时器 */
STATIC TIMER_ID s_charge_poll_timer_id = NULL;

/* 当前充电状态 */
STATIC charge_state_t s_charge_state = CHG_STATE_NO_USB;

/***********************************************************************
 ********************* static function prototypes **********************
 **********************************************************************/

STATIC VOID_T app_charge_poll_handler(TIMER_ID timer_id, VOID_T *arg);

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 读 USB_DET 引脚电平
 * @return TRUE=USB已插入（高电平）
 */
STATIC BOOL_T app_charge_read_usb_det(VOID_T)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_LOW;
    if (tal_gpio_read(USB_DET, &level) != OPRT_OK) {
        return FALSE;
    }
    return (level == TUYA_GPIO_LEVEL_HIGH);
}

/**
 * @brief 读 CHARGE_STATE 引脚电平
 * @return TRUE=充电中（低电平）
 */
STATIC BOOL_T app_charge_read_charge_state(VOID_T)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_LOW;
    if (tal_gpio_read(CHARGE_STATE, &level) != OPRT_OK) {
        return FALSE;
    }
    return (level == TUYA_GPIO_LEVEL_LOW);
}

/**
 * @brief 轮询定时器回调 — 双引脚联合判定充电状态
 *
 * USB_DET    | CHARGE_STATE | 结论
 * -----------+--------------+-------------------
 * LOW        | X            | USB未插入
 * HIGH       | LOW          | 充电中
 * HIGH       | HIGH         | 满电
 */
STATIC VOID_T app_charge_poll_handler(TIMER_ID timer_id, VOID_T *arg)
{
    BOOL_T usb_in    = app_charge_read_usb_det();
    BOOL_T is_active = app_charge_read_charge_state();

    charge_state_t new_state;

    if (!usb_in) {
        new_state = CHG_STATE_NO_USB;
    } else if (is_active) {
        new_state = CHG_STATE_CHARGING;
    } else {
        new_state = CHG_STATE_CHARGE_DONE;
    }

    if (new_state == s_charge_state) {
        return; /* 状态未变化 */
    }

    TAL_PR_INFO("[charge] state: %d -> %d (USB_DET=%d, CHARGE_STATE=%d)",
                s_charge_state, new_state, usb_in, is_active);

    s_charge_state = new_state;

    switch (new_state) {
    case CHG_STATE_CHARGING:
        app_state_set_charging(TRUE);
        break;
    case CHG_STATE_CHARGE_DONE:
        app_state_set_charging(TRUE);
        app_state_set_charge_done(TRUE);
        break;
    case CHG_STATE_NO_USB:
    default:
        app_state_set_charging(FALSE);
        app_state_set_charge_done(FALSE);
        break;
    }
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

VOID_T app_charge_init(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };

    /* 初始化两个 GPIO（不进中断，纯输入轮询） */
    tal_gpio_init(USB_DET, &gpio_cfg);
    tal_gpio_init(CHARGE_STATE, &gpio_cfg);

    /* 创建轮询定时器 */
    OPERATE_RET ret = tal_sw_timer_create(app_charge_poll_handler, NULL,
                                          &s_charge_poll_timer_id);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[charge] timer create fail: %d", ret);
        return;
    }

    /* 初始化时读取一次双引脚状态 */
    BOOL_T usb_in    = app_charge_read_usb_det();
    BOOL_T is_active = app_charge_read_charge_state();

    if (usb_in && is_active) {
        s_charge_state = CHG_STATE_CHARGING;
        app_state_set_charging(TRUE);
        TAL_PR_INFO("[charge] init: USB in + charging");
    } else if (usb_in && !is_active) {
        s_charge_state = CHG_STATE_CHARGE_DONE;
        app_state_set_charging(TRUE);
        app_state_set_charge_done(TRUE);
        TAL_PR_INFO("[charge] init: USB in + charge done");
    } else {
        app_state_set_charging(FALSE);
        s_charge_state = CHG_STATE_NO_USB;
        TAL_PR_INFO("[charge] init: no USB");
    }

    /* 启动周期轮询定时器 */
    tal_sw_timer_start(s_charge_poll_timer_id, CHARGE_POLL_MS, TAL_TIMER_CYCLE);

    TAL_PR_INFO("[charge] initialized (poll %dms), USB_DET=%d, CHARGE_STATE=%d",
                CHARGE_POLL_MS, usb_in, is_active);
}

BOOL_T app_charge_is_detected(VOID_T)
{
    return (s_charge_state == CHG_STATE_CHARGING);
}
