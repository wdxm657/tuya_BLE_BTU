/**
 * @file app_key.c
 * @brief 按键实现 — 自稳定状态机，周期轮询检测，短按开关机，长按3s松开恢复出厂
 * @version 1.2
 * @date 2026-06-16
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 按键物理引脚：C1 (TUYA_GPIO_NUM_17)，低电平有效（按下=低）
 *
 * 行为：
 *   短按（<3s 松开）-> 开关机（切换低功耗模式）
 *   长按（>=3s 松开）-> 蓝牙恢复出厂设置
 *
 * 算法说明：
 *   10ms 周期轮询 GPIO 电平，自稳定状态机处理去抖和长短按判定：
 *   IDLE ──连续5次Low──> DEBOUNCE_PRESS ──完成──> PRESSED
 *   PRESSED ──连续5次High──> DEBOUNCE_RELEASE ──完成──> IDLE
 *   在 RELEASE 时根据按下持续时间决定短按/长按动作。
 *
 *   不使用 tal_key 库（该库存在稳定性缺陷）。
 */

#include "board.h"

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"
#include "tal_system.h"
#include "tal_flash.h"

#include "tuya_ble_api.h"
#include "tuya_ble_protocol_callback.h"

#include "app_key.h"
#include "app_state.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 按键引脚 */
#define APP_KEY_PIN                     BOARD_KEY_PIN

/** 按键轮询间隔 ms */
#define KEY_POLL_MS                     10

/** 去抖采样次数（连续 N 次相同电平才确认状态切换） */
#define KEY_DEBOUNCE_SAMPLES            5

/** 长按判定阈值 ms */
#define KEY_LONG_PRESS_MS               3000

/***********************************************************************
 ********************* state machine enum  *****************************
 **********************************************************************/

/** 按键状态机状态 */
typedef enum {
    KEY_STATE_IDLE,                 /**< 空闲，等待按下 */
    KEY_STATE_DEBOUNCE_PRESS,       /**< 按下去抖中（连续 Low 计数） */
    KEY_STATE_PRESSED,              /**< 已确认按下，等待松开 */
    KEY_STATE_DEBOUNCE_RELEASE,     /**< 松开去抖中（连续 High 计数） */
} key_state_t;

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* 轮询定时器 */
STATIC TIMER_ID s_app_key_timer_id = NULL;

/* 按键状态机状态 */
STATIC key_state_t s_key_state = KEY_STATE_IDLE;

/* 去抖计数器 */
STATIC UINT8_T s_debounce_cnt = 0;

/* 按下时刻的系统滴答（ms） */
STATIC UINT32_T s_press_tick_ms = 0;

/** 标记：在长按阈值到达时设为 TRUE，释放时据此判断是否执行长按动作 */
STATIC BOOL_T s_long_press_eligible = FALSE;

/** 开机时刻的系统滴答（ms），用于屏蔽开机3s内的短按 */
STATIC UINT32_T s_boot_tick_ms = 0;

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 轮询定时器回调 — 按键状态机
 *
 * 每 10ms 读取一次 GPIO 电平，自稳定状态机处理：
 *
 * IDLE:               检测到 Low → 进入 DEBOUNCE_PRESS
 * DEBOUNCE_PRESS:     累计 5 次 Low → 进入 PRESSED，记录按下时间
 *                      检测到 High → 回退 IDLE（噪声/抖动）
 * PRESSED:            检测到 High → 进入 DEBOUNCE_RELEASE
 *                      持续 Low → 检查长按阈值（>=3s 时标记）
 * DEBOUNCE_RELEASE:   累计 5 次 High → 进入 IDLE，执行短按/长按动作
 *                      检测到 Low → 回退 PRESSED（抖动/噪声）
 */
STATIC VOID_T app_key_poll_handler(TIMER_ID timer_id, VOID_T *arg)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    UINT32_T now_ms;
    UINT32_T press_duration;

    /* 读取 GPIO 电平 */
    if (tal_gpio_read(APP_KEY_PIN, &level) != OPRT_OK) {
        return;
    }

    switch (s_key_state) {

        /* ======================== IDLE ======================== */
        case KEY_STATE_IDLE: {
            if (level == TUYA_GPIO_LEVEL_LOW) {
                /* 检测到按下，进入去抖 */
                s_key_state = KEY_STATE_DEBOUNCE_PRESS;
                s_debounce_cnt = 1;
            }
        } break;

        /* ======================== DEBOUNCE_PRESS ======================== */
        case KEY_STATE_DEBOUNCE_PRESS: {
            if (level == TUYA_GPIO_LEVEL_LOW) {
                s_debounce_cnt++;
                if (s_debounce_cnt >= KEY_DEBOUNCE_SAMPLES) {
                    /* 去抖通过，确认已按下 */
                    s_key_state = KEY_STATE_PRESSED;
                    s_press_tick_ms = tal_system_get_millisecond();
                    s_long_press_eligible = FALSE;
                    s_debounce_cnt = 0;

                    TAL_PR_DEBUG("[key] press confirmed");
                }
            } else {
                /* 去抖过程中电平恢复 → 视为噪声，回退 IDLE */
                s_key_state = KEY_STATE_IDLE;
                s_debounce_cnt = 0;
            }
        } break;

        /* ======================== PRESSED ======================== */
        case KEY_STATE_PRESSED: {
            if (level == TUYA_GPIO_LEVEL_HIGH) {
                /* 检测到松开，进入释放去抖 */
                s_key_state = KEY_STATE_DEBOUNCE_RELEASE;
                s_debounce_cnt = 1;
            } else {
                /* 持续按下：检查长按阈值 */
                now_ms = tal_system_get_millisecond();
                press_duration = now_ms - s_press_tick_ms;

                if (!s_long_press_eligible && press_duration >= KEY_LONG_PRESS_MS) {
                    s_long_press_eligible = TRUE;
                    TAL_PR_INFO("[key] long press 3s threshold reached");
                }
            }
        } break;

        /* ======================== DEBOUNCE_RELEASE ======================== */
        case KEY_STATE_DEBOUNCE_RELEASE: {
            if (level == TUYA_GPIO_LEVEL_HIGH) {
                s_debounce_cnt++;
                if (s_debounce_cnt >= KEY_DEBOUNCE_SAMPLES) {
                    /* 去抖通过，确认已释放 → 判定长短按 */
                    now_ms = tal_system_get_millisecond();
                    press_duration = now_ms - s_press_tick_ms;

                    if (s_long_press_eligible || press_duration >= KEY_LONG_PRESS_MS) {
                        /* 长按松开 → 恢复出厂 */
                        TAL_PR_INFO("[key] long press release (%dms) -> factory reset",
                                    press_duration);
                        // tuya_ble_device_unbind();
                        tuya_ble_device_factory_reset();
                        tuya_ble_disconnect_and_reset_timer_start();
                        // 写一个标志位到FLASH：标记本次为长按复位后冷启动
                        tal_flash_erase(APP_DATA_FLASH_ADDR, 0x1000);
                        {
                            UINT8_T f_reset = 0x03;
                            tal_flash_write(APP_DATA_FLASH_ADDR, &f_reset, 1);
                        }
                    } else {
                        /* 开机3s内的短按不做处理（防止唤醒按键被误认为开关机） */
                        if (s_boot_tick_ms != 0 && (now_ms - s_boot_tick_ms) < 3000) {
                            TAL_PR_DEBUG("[key] short press ignored (within 3s of boot)");
                        } else {
                            /* 短按松开 → 开关机 */
                            TAL_PR_INFO("[key] short press release (%dms) -> toggle power",
                                        press_duration);
                            app_state_toggle_power();
                        }
                    }

                    /* 回退 IDLE */
                    s_key_state = KEY_STATE_IDLE;
                    s_debounce_cnt = 0;
                    s_long_press_eligible = FALSE;
                }
            } else {
                /* 释放去抖中电平又变回 Low → 视为抖动，回退 PRESSED */
                s_key_state = KEY_STATE_PRESSED;
                s_debounce_cnt = 0;
            }
        } break;

        default: {
            /* 异常状态保护 */
            s_key_state = KEY_STATE_IDLE;
            s_debounce_cnt = 0;
            s_long_press_eligible = FALSE;
        } break;
    }
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

VOID_T app_key_init(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode   = TUYA_GPIO_PULLUP,         /* 上拉输入 */
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };

    /* 初始化 GPIO（不进中断，纯输入轮询） */
    tal_gpio_init(APP_KEY_PIN, &gpio_cfg);

    /* 创建并启动 10ms 周期轮询定时器 */
    tal_sw_timer_create(app_key_poll_handler, NULL, &s_app_key_timer_id);
    tal_sw_timer_start(s_app_key_timer_id, KEY_POLL_MS, TAL_TIMER_CYCLE);

    /* 状态机初始值 */
    s_key_state          = KEY_STATE_IDLE;
    s_debounce_cnt       = 0;
    s_press_tick_ms      = 0;
    s_long_press_eligible = FALSE;
    s_boot_tick_ms       = tal_system_get_millisecond();

    TAL_PR_INFO("[key] custom driver initialized (poll %dms), pin %d",
                KEY_POLL_MS, APP_KEY_PIN);
}
