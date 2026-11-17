/** @file app_power_manager.c
 * @brief 电源、欠压保护与 retention 管理
 */
#include "board.h"
#include "tal_log.h"
#include "tal_gpio.h"
#include "tal_flash.h"
#include "tal_system.h"
#include "tal_sw_timer.h"
#include "app_state.h"
#include "app_key.h"
#include "app_led.h"
#include "app_battery.h"
#include "app_charge.h"
#include "app_motor.h"
#include "app_vibration.h"
#include "tal_adc.h"
#include "tal_sleep.h"
#include "tuya_ble_api.h"
#include "tuya_sdk_callback.h"
#include "app_power_manager.h"

/* 每60秒计数一次，累计15次（15分钟）后进入超低功耗。 */
#define LOW_POWER_DELAY_MS             (60UL * 1000UL)
#define RETENTION_LOW_POWER_COUNT_MAX  15U
STATIC BOOL_T   s_low_power_entered = FALSE;
STATIC BOOL_T   s_undervoltage_shutdown_active = FALSE;
STATIC BOOL_T   s_low_power_requested = FALSE;
STATIC BOOL_T   s_low_power_disconnect_requested = FALSE;

/* Retention sleep 持续15分钟后停止BLE活动，进入超低功耗。 */
STATIC TIMER_ID s_retention_timer_id = NULL;
STATIC volatile UINT8_T s_retention_low_power_count = 0;
STATIC BOOL_T s_retention_tracking = FALSE;
STATIC BOOL_T s_retention_ultra_low_power = FALSE;
STATIC volatile BOOL_T s_retention_check_pending = FALSE;
STATIC volatile BOOL_T s_retention_hw_wakeup_pending = FALSE;
STATIC BOOL_T s_retention_key_press_seen = FALSE;

extern BOOL_T tkl_cpu_is_sleep(VOID_T);

STATIC VOID_T tuya_pre_sleep_cb(VOID_T);
STATIC VOID_T tuya_post_wakeup_cb(VOID_T);
STATIC VOID_T tuya_retention_timer_handler(TIMER_ID timer_id, VOID_T *arg);
STATIC VOID_T tuya_retention_sleep_process(VOID_T);
STATIC VOID_T app_peripherals_resume_after_sleep(VOID_T);
STATIC VOID_T usb_on_removed(VOID_T);
STATIC TUYA_SLEEP_CB_T tal_sleep_cb = {
    .pre_sleep_cb = tuya_pre_sleep_cb,
    .post_wakeup_cb = tuya_post_wakeup_cb,
};

/**
 * @brief 执行低功耗前的统一硬件处理
 */
STATIC VOID_T enter_low_power_now(VOID_T)
{
    if (s_low_power_entered) {
        return;
    }

    s_low_power_entered = TRUE;
    app_battery_save_percent();
    /* 两闪结束即锁定熄灯，等待 BLE 断开期间也不能被状态刷新点亮。 */
    app_led_prepare_sleep();
    /* 由主循环完成 BLE 断开和停广播，再允许进入超低功耗。 */
    s_low_power_requested = TRUE;
    s_low_power_disconnect_requested = FALSE;
}

/**
 * @brief 临界低电 → 硬件关机（适配 app_battery_register_critical_cb 签名）
 */
STATIC VOID_T battery_critical_poweroff(VOID_T)
{
    if (app_state_is_charging() || s_low_power_entered ||
        s_undervoltage_shutdown_active) {
        return;
    }

    s_undervoltage_shutdown_active = TRUE;

    /* 欠压时停止产品工作。 */
    app_state_set_power(FALSE);

    /* 红灯快闪两次，完成后立即进入低功耗。 */
    app_led_red_flash_twice(enter_low_power_now);
}

STATIC VOID_T tuya_pre_sleep_cb(VOID_T)
{
    OPERATE_RET ret;

    app_battery_suspend();
    app_led_prepare_sleep();
    app_key_suspend();
    app_charge_suspend();
    app_vibration_suspend();

    if (s_retention_tracking || s_retention_ultra_low_power) {
        return;
    }

    if (s_retention_timer_id == NULL) {
        ret = tal_sw_timer_create(tuya_retention_timer_handler, NULL,
                                  &s_retention_timer_id);
        if (ret != OPRT_OK) {
            TAL_PR_ERR("[retention] timer create failed: %d", ret);
            return;
        }
    }

    s_retention_low_power_count = 0;
    s_retention_check_pending = FALSE;
    ret = tal_sw_timer_start(s_retention_timer_id,
                             LOW_POWER_DELAY_MS,
                             TAL_TIMER_CYCLE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[retention] timer start failed: %d", ret);
        return;
    }
    s_retention_tracking = TRUE;
}

STATIC VOID_T tuya_post_wakeup_cb(VOID_T)
{
    /* 只记录硬件唤醒；不能在此退出 sleep 或恢复轮询定时器。 */
    s_retention_hw_wakeup_pending = TRUE;
}

STATIC VOID_T tuya_retention_timer_handler(TIMER_ID timer_id, VOID_T *arg)
{
    /* 饱和计数，达到阈值后不再递增，避免长时间运行溢出。 */
    if (s_retention_low_power_count < RETENTION_LOW_POWER_COUNT_MAX) {
        s_retention_low_power_count++;
    }
    s_retention_check_pending = TRUE;
}

STATIC VOID_T tuya_retention_sleep_process(VOID_T)
{
    OPERATE_RET ret;

    if (!tkl_cpu_is_sleep()) {
        if (s_retention_tracking && s_retention_timer_id != NULL) {
            tal_sw_timer_stop(s_retention_timer_id);
        }
        s_retention_tracking = FALSE;
        s_retention_ultra_low_power = FALSE;
        s_retention_check_pending = FALSE;
        s_retention_low_power_count = 0;
        return;
    }

    if (!s_retention_tracking || !s_retention_check_pending) {
        return;
    }
    s_retention_check_pending = FALSE;

    TAL_PR_INFO("[retention] low-power count: %d/%d",
                s_retention_low_power_count,
                RETENTION_LOW_POWER_COUNT_MAX);
    if (s_retention_low_power_count < RETENTION_LOW_POWER_COUNT_MAX) {
        return;
    }

    if (tuya_app_get_conn_handle() != 0xFFFF) {
        tuya_ble_gap_disconnect();
        /* 等待断开事件处理完成，避免其自动重启广播后覆盖下面的停止操作。 */
        tal_system_sleep(1000);
        TAL_PR_INFO("[retention] BLE disconnected after 15 minutes");
    }

    ret = tal_ble_advertising_stop();
    if (ret != OPRT_OK) {
        TAL_PR_WARN("[retention] stop advertising failed: %d", ret);
        return;
    }

    tal_sw_timer_stop(s_retention_timer_id);
    s_retention_tracking = FALSE;
    s_retention_ultra_low_power = TRUE;
    TAL_PR_INFO("[retention] enter ultra-low-power after 15 minutes");
}

/* 普通关机：充电或 BLE 连接时保持唤醒，其余情况进入 retention。
 * 主循环也执行此检查，覆盖断开、重连及启动时未连接的情况。 */
STATIC VOID_T app_update_sleep_policy(VOID_T)
{
    if (app_state_is_powered_on() || s_undervoltage_shutdown_active) {
        return;
    }

    if (app_state_is_charging() || tuya_app_get_conn_handle() != 0xFFFF) {
        if (tkl_cpu_is_sleep()) {
            tal_cpu_force_wakeup();
            s_retention_hw_wakeup_pending = FALSE;
            s_retention_key_press_seen = FALSE;
            app_peripherals_resume_after_sleep();
            TAL_PR_DEBUG("[power] powered off, stay awake for charging/BLE");
        }
        return;
    }

    if (!tkl_cpu_is_sleep()) {
        app_battery_save_percent();
        tal_cpu_allow_sleep();
        TAL_PR_DEBUG("[power] powered off and BLE disconnected -> enter retention");
    }
}

STATIC VOID_T power_off_cb(VOID_T){
    app_motor_stop();
    app_battery_save_percent();
    app_update_sleep_policy();
}

STATIC VOID_T app_peripherals_resume_after_sleep(VOID_T)
{
    app_key_resume();
    app_charge_resume();
    app_vibration_resume();
    app_battery_resume();
    app_led_resume();
}

STATIC VOID_T usb_on_removed(VOID_T)
{
    if (app_state_is_powered_on()) {
        return;
    }

    if (app_battery_is_critical()) {
        s_low_power_entered = FALSE;
        s_undervoltage_shutdown_active = FALSE;
        battery_critical_poweroff();
        return;
    }

    TAL_PR_INFO("[power] USB removed while powered off -> check sleep conditions");
    app_update_sleep_policy();
}

STATIC VOID_T power_on_cb(VOID_T){
    /* 明确开机后退出 sleep，并恢复业务轮询。 */
    tal_cpu_force_wakeup();
    app_peripherals_resume_after_sleep();

    /* 欠压低功耗被按键唤醒时，不允许电机启动。 */
    if (app_battery_is_critical() && !app_state_is_charging()) {
        TAL_PR_WARN("[power] power-on rejected: battery undervoltage");
        s_undervoltage_shutdown_active = FALSE;
        s_low_power_entered = FALSE;
        battery_critical_poweroff();
        return;
    }

    app_motor_start();
}

VOID_T app_power_manager_init(VOID_T)
{
    app_state_register_power_cb(power_on_cb, power_off_cb);
    app_battery_register_critical_cb(battery_critical_poweroff);
    app_charge_register_usb_removed_cb(usb_on_removed);
    tal_cpu_sleep_callback_register(&tal_sleep_cb);
    if (app_battery_is_critical() && !app_state_is_charging()) {
        battery_critical_poweroff();
    }
}

VOID_T app_power_manager_process(VOID_T)
{
    TUYA_GPIO_LEVEL_E key_level = TUYA_GPIO_LEVEL_HIGH;
    TUYA_GPIO_LEVEL_E usb_level = TUYA_GPIO_LEVEL_LOW;

    /*
     * Deep-retention 唤醒后先确认按键确实按下，再等待松开才开机。
     * 不使用软件定时器，避免在 retention 中制造周期唤醒。
     * USB 唤醒时按键始终为高，不会误触发开机。
     */
    if (tkl_cpu_is_sleep()) {
        /* USB 插入为高电平：立即退出 retention，但产品仍保持关机。 */
        if (tal_gpio_read(USB_DET, &usb_level) == OPRT_OK &&
            usb_level == TUYA_GPIO_LEVEL_HIGH) {
            s_retention_hw_wakeup_pending = FALSE;
            s_retention_key_press_seen = FALSE;
            TAL_PR_INFO("[power] USB inserted in retention -> wake up");
            tal_cpu_force_wakeup();
            s_low_power_entered = FALSE;
            s_undervoltage_shutdown_active = FALSE;
            app_peripherals_resume_after_sleep();
            app_charge_refresh();
        } else if (s_retention_hw_wakeup_pending &&
                   tal_gpio_read(BOARD_KEY_PIN, &key_level) == OPRT_OK) {
            if (key_level == TUYA_GPIO_LEVEL_LOW) {
                s_retention_key_press_seen = TRUE;
            } else if (s_retention_key_press_seen) {
                s_retention_hw_wakeup_pending = FALSE;
                s_retention_key_press_seen = FALSE;
                TAL_PR_INFO("[power] retention key released -> power on");
                /* 轮询暂停期间可能拔线，先清除已过期的充电状态。 */
                app_charge_refresh();
                app_state_set_power(TRUE);
            }
        }
    }

    app_state_process();

    /* 两次红灯提示结束后立即执行，跳过普通 retention 倒计时。
     * 等待断开事件完成后再停广播，避免被断开回调重新开启。 */
    if (s_low_power_requested) {
        if (app_state_is_charging()) {
            s_low_power_requested = FALSE;
            s_low_power_disconnect_requested = FALSE;
            s_low_power_entered = FALSE;
            s_undervoltage_shutdown_active = FALSE;
            app_led_resume();
        } else if (tuya_app_get_conn_handle() != 0xFFFF) {
            if (!s_low_power_disconnect_requested &&
                tuya_ble_gap_disconnect() == TUYA_BLE_SUCCESS) {
                s_low_power_disconnect_requested = TRUE;
            }
            return;
        } else {
            if (tal_ble_advertising_stop() != OPRT_OK) {
                return;
            }
            if (s_retention_timer_id != NULL) {
                tal_sw_timer_stop(s_retention_timer_id);
            }
            s_retention_tracking = FALSE;
            s_retention_check_pending = FALSE;
            s_retention_ultra_low_power = TRUE;
            s_low_power_requested = FALSE;
            app_battery_suspend();
            TAL_PR_INFO("[power] battery <=5%% -> enter ultra-low-power now");
            tal_cpu_allow_sleep();
            return;
        }
    }
    app_update_sleep_policy();
    tuya_retention_sleep_process();
}
