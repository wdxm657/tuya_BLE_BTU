/**
 * @file app_led.c
 * @brief LED 指示灯实现 — 根据设备状态切换 灭/蓝闪/绿闪/红闪/红常亮
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 产品定义：
 *   熄灭：关机（app上操作），或电池已充满电
 *   蓝色灯闪烁：蓝牙未连接
 *   绿色灯闪烁：工作状态
 *   红色闪烁：电量不足，请尽快为设备充电
 *   红色常亮：设备正在充电中
 *
 * 显示优先级（高→低）：
 *   充电中 → 红灯常亮
 *   低电量（工作中）→ 红灯闪烁
 *   蓝牙未连接（工作中）→ 蓝灯闪烁
 *   工作正常 → 绿灯闪烁
 *   待机/满电/关机 → 熄灭
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"
#include "board.h"

#include "app_led.h"
#include "app_state.h"
#include "app_battery.h"
#include "tuya_sdk_callback.h"
#include "tuya_ble_main.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 闪烁周期 ms（半周期翻转一次） */
#define LED_BLINK_PERIOD_MS 250

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* 闪烁定时器 */
STATIC TIMER_ID s_led_timer_id = NULL;

/* 当前 LED 模式 */
STATIC led_mode_t s_led_mode = LED_MODE_OFF;

/* 当前 LED 亮灭状态（用于闪烁） */
STATIC BOOL_T s_led_on = FALSE;

/* 保存当前 LED 引脚状态：用于 GPIO 写入时避免不必要调用 */
STATIC TUYA_GPIO_LEVEL_E s_r_level = TUYA_GPIO_LEVEL_HIGH;
STATIC TUYA_GPIO_LEVEL_E s_g_level = TUYA_GPIO_LEVEL_HIGH;
STATIC TUYA_GPIO_LEVEL_E s_b_level = TUYA_GPIO_LEVEL_HIGH;

/***********************************************************************
 ********************* static function prototypes **********************
 **********************************************************************/

STATIC VOID_T app_led_set_pins(TUYA_GPIO_LEVEL_E r, TUYA_GPIO_LEVEL_E g, TUYA_GPIO_LEVEL_E b);

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 设置 R/G/B 三引脚电平（LED 共阳，低电平点亮）
 * @param[in] r 红灯引脚电平
 * @param[in] g 绿灯引脚电平
 * @param[in] b 蓝灯引脚电平
 */
STATIC VOID_T app_led_set_pins(TUYA_GPIO_LEVEL_E r, TUYA_GPIO_LEVEL_E g, TUYA_GPIO_LEVEL_E b)
{
    /* 仅在电平变化时写入，减少 GPIO 调用 */
    if (r != s_r_level)
    {
        tal_gpio_write(LED_R, r);
        s_r_level = r;
    }
    if (g != s_g_level)
    {
        tal_gpio_write(LED_G, g);
        s_g_level = g;
    }
    if (b != s_b_level)
    {
        tal_gpio_write(LED_B, b);
        s_b_level = b;
    }
}

/**
 * @brief 定时器回调 — 闪烁模式下翻转 LED 亮灭
 *
 * 非闪烁模式（ON/SOLID/OFF）不启动本定时器。
 */
STATIC VOID_T app_led_blink_handler(TIMER_ID timer_id, VOID_T *arg)
{
    s_led_on = !s_led_on;

    /* 共阳：低电平点亮，高电平熄灭 */
    if (s_led_on)
    {
        switch (s_led_mode)
        {
        case LED_MODE_BLUE_BLINK:
            app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_LOW);
            break;
        case LED_MODE_GREEN_BLINK:
            app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_LOW, TUYA_GPIO_LEVEL_HIGH);
            break;
        case LED_MODE_RED_BLINK:
            app_led_set_pins(TUYA_GPIO_LEVEL_LOW, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH);
            break;
        default:
            break;
        }
    }
    else
    {
        /* 熄灭 */
        app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH);
    }
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

VOID_T app_led_init(VOID_T)
{
    // 初始化所有LED IO
    TUYA_GPIO_BASE_CFG_T gpio_out_high = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };

    tal_gpio_init(LED_R, &gpio_out_high);
    tal_gpio_init(LED_G, &gpio_out_high);
    tal_gpio_init(LED_B, &gpio_out_high);
    /* 创建闪烁定时器 */
    tal_sw_timer_create(app_led_blink_handler, NULL, &s_led_timer_id);

    /* 初始状态：所有 LED 熄灭 */
    app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH);
    s_led_mode = LED_MODE_OFF;
    s_led_on = FALSE;

    TAL_PR_INFO("[led] initialized");
}

VOID_T app_led_set_mode(led_mode_t mode)
{
    if (mode == s_led_mode)
    {
        return; /* 模式未变化，不做处理 */
    }

    if (mode > LED_MODE_RED_SOLID)
    {
        TAL_PR_ERR("[led] invalid mode: %d", mode);
        return;
    }

    TAL_PR_DEBUG("[led] mode: %d -> %d", s_led_mode, mode);
    s_led_mode = mode;

    /* 停止之前的定时器 */
    if (s_led_timer_id != NULL)
    {
        tal_sw_timer_stop(s_led_timer_id);
    }

    s_led_on = FALSE;

    switch (mode)
    {
    case LED_MODE_OFF:
        /* 全部熄灭 */
        app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH);
        break;

    case LED_MODE_RED_SOLID:
        /* 红灯常亮 */
        app_led_set_pins(TUYA_GPIO_LEVEL_LOW, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH);
        break;

    case LED_MODE_BLUE_BLINK:
        /* 蓝灯闪烁：启动 250ms 周期定时器 */
        s_led_on = TRUE;
        app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_LOW);
        tal_sw_timer_start(s_led_timer_id, LED_BLINK_PERIOD_MS, TAL_TIMER_CYCLE);
        break;

    case LED_MODE_GREEN_BLINK:
        /* 绿灯闪烁：启动 250ms 周期定时器 */
        s_led_on = TRUE;
        app_led_set_pins(TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_LOW, TUYA_GPIO_LEVEL_HIGH);
        tal_sw_timer_start(s_led_timer_id, LED_BLINK_PERIOD_MS, TAL_TIMER_CYCLE);
        break;

    case LED_MODE_RED_BLINK:
        /* 红灯闪烁：启动 250ms 周期定时器 */
        s_led_on = TRUE;
        app_led_set_pins(TUYA_GPIO_LEVEL_LOW, TUYA_GPIO_LEVEL_HIGH, TUYA_GPIO_LEVEL_HIGH);
        tal_sw_timer_start(s_led_timer_id, LED_BLINK_PERIOD_MS, TAL_TIMER_CYCLE);
        break;

    default:
        break;
    }
}

/**
 * @brief 根据当前设备状态统一刷新 LED 模式（由状态变更/BLE 连接/低电量触发）
 *
 * 优先级（高→低）：
 *   充电中 → 红灯常亮
 *   低电量（工作中）→ 红灯闪烁
 *   待机/关机 → 熄灭
 *   蓝牙未连接（工作中）→ 蓝灯闪烁
 *   工作正常 → 绿灯闪烁
 */
VOID_T app_led_update(VOID_T)
{
    led_mode_t new_mode;

    if (app_state_is_charging())
    {
        new_mode = LED_MODE_RED_SOLID;
    }
    /* 工作状态 */
    else if (app_battery_is_low())
    {
        new_mode = LED_MODE_RED_BLINK;
    }
    else if (!app_state_is_powered_on())
    {
        /* 关机：据配网状态区分指示 — 已配网则灭灯，未配网则蓝灯闪烁提示 */
        tuya_ble_connect_status_t conn_st = tuya_ble_connect_status_get();
        // TAL_PR_DEBUG("conn_st %d", tuya_ble_connect_status_get());
        if (conn_st == BONDING_UNCONN || conn_st == BONDING_CONN) {
            new_mode = LED_MODE_OFF;
        } else {
            // 若标志位为0x03则认为是长按3秒后的复位启动
            UINT8_T f_reset = 0xFF;
            tal_flash_read(APP_DATA_FLASH_ADDR, &f_reset, 1);
            if (f_reset == 0x03) {
                TAL_PR_INFO("[boot] factory-reset flag detected, staying off");
                new_mode = LED_MODE_BLUE_BLINK;
            } else {
                new_mode = LED_MODE_OFF;
            }
        }
    }
    // else if (app_state_get() == DEV_STATE_CHARGE_DONE)
    // {
    //     new_mode = LED_MODE_OFF;
    // }
    else if (app_state_get() == DEV_STATE_STANDBY)
    {
        new_mode = LED_MODE_OFF;
    }
    else
    {
        new_mode = LED_MODE_GREEN_BLINK;
    }

    app_led_set_mode(new_mode);
}

led_mode_t app_led_get_mode(VOID_T)
{
    return s_led_mode;
}
