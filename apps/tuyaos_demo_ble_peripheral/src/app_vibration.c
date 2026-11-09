/**
 * @file app_vibration.c
 * @brief 振动传感器实现 — 周期轮询检测，互动模式触发
 * @version 1.1
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 通过每 10ms 轮询 GPIO 电平检测振动信号。
 * 连续读到 Low 电平 3 次（30ms）视为有效振动，过滤干扰毛刺。
 * 触发后执行注册的回调函数（由 tuya_sdk_callback 注册为 app_motor_interactive_trigger）。
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"
#include "tal_system.h"
#include "board.h"

#include "app_vibration.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 振动轮询间隔 ms */
#define VIBRATION_POLL_MS           10

/** 去抖计数器阈值（连续读到 Low 的次数） */
#define VIBRATION_DEBOUNCE_CNT      3

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* 振动触发回调 */
STATIC VOID_T (*s_vibration_cb)(VOID_T) = NULL;

/* 轮询定时器 */
STATIC TIMER_ID s_poll_timer_id = NULL;

/* 上次触发时间戳 ms */
STATIC UINT32_T s_last_trigger_tick = 0;

/* 去抖计数器：连续检测到 Low 的次数 */
STATIC UINT8_T s_debounce_cnt = 0;

/* 是否已触发（等待电平恢复后才允许下次触发） */
STATIC BOOL_T s_triggered = FALSE;

/***********************************************************************
 ********************* static function prototypes **********************
 **********************************************************************/

STATIC VOID_T app_vibration_poll_handler(TIMER_ID timer_id, VOID_T *arg);

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 轮询定时器回调 — 读取 GPIO 电平，去抖后触发回调
 *
 * 振动传感器默认高电平，振动时输出低电平脉冲。
 * 连续读到 3 次 Low 视为有效振动，触发回调后等待电平恢复。
 */
STATIC VOID_T app_vibration_poll_handler(TIMER_ID timer_id, VOID_T *arg)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    OPERATE_RET ret = tal_gpio_read(VIBRATION_SENSOR_PIN, &level);
    if (ret != OPRT_OK) {
        return;
    }

    if (level == TUYA_GPIO_LEVEL_HIGH) {
        /* 检测到 High（振动激活） */
        if (!s_triggered) {
            s_debounce_cnt++;
            if (s_debounce_cnt >= VIBRATION_DEBOUNCE_CNT) {
                /* 去抖通过，触发 */
                s_triggered = TRUE;
                s_debounce_cnt = 0;
                s_last_trigger_tick = tal_system_get_millisecond();
                TAL_PR_DEBUG("[vibration] detected");

                if (s_vibration_cb != NULL) {
                    s_vibration_cb();
                }
            }
        }
    } else {
        /* 低电平（未振动），复位去抖计数 */
        s_debounce_cnt = 0;
        /* 电平恢复，允许下次触发 */
        s_triggered = FALSE;
    }
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

VOID_T app_vibration_init(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode   = TUYA_GPIO_PULLUP,      /* 上拉输入，默认高电平 */
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };

    /* 初始化 GPIO（不进中断，纯输入轮询） */
    tal_gpio_init(VIBRATION_SENSOR_PIN, &gpio_cfg);

    /* 创建并启动 10ms 周期轮询定时器 */
    tal_sw_timer_create(app_vibration_poll_handler, NULL, &s_poll_timer_id);
    tal_sw_timer_start(s_poll_timer_id, VIBRATION_POLL_MS, TAL_TIMER_CYCLE);

    s_vibration_cb     = NULL;
    s_last_trigger_tick = 0;
    s_debounce_cnt      = 0;
    s_triggered         = FALSE;

    TAL_PR_INFO("[vibration] sensor initialized (poll %dms), pin %d",
                VIBRATION_POLL_MS, VIBRATION_SENSOR_PIN);
}

VOID_T app_vibration_register_cb(VOID_T (*cb)(VOID_T))
{
    s_vibration_cb = cb;
}

UINT32_T app_vibration_get_last_tick_ms(VOID_T)
{
    return s_last_trigger_tick;
}
