/** @file app_control.c
 * @brief 业务模块初始化、工作状态与振动联动
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
#include "app_control.h"
#include "app_dp_parser.h"
#include "app_power_manager.h"

/**
 * @brief 状态变更处理：刷新 LED，并根据 work/standby 状态启停电机
 */
STATIC VOID_T device_on_state_change(dev_state_t old_state, dev_state_t new_state)
{
    app_led_update();

    /* 离开工作状态时统一停止电机，覆盖待机、关机和充电。 */
    if (new_state != DEV_STATE_WORK) {
        app_motor_stop();
    } else if (new_state == DEV_STATE_WORK &&
               app_state_is_powered_on() &&
               !app_battery_is_critical()) {
        /* 从关机/待机恢复工作时，按当前模式启动电机。 */
        app_motor_start();
    }
}

/**
 * @brief 振动处理：待机唤醒并重启5分钟工作周期；工作中触发互动模式
 */
STATIC VOID_T vibration_on_detected(VOID_T)
{
    if (!app_state_is_powered_on() || app_state_is_charging()) {
        return;
    }

    if (app_state_get() == DEV_STATE_STANDBY) {
        TAL_PR_INFO("[state] vibration wakeup -> restart 5-minute work cycle");
        app_state_reset_standby_timer();
        return;
    }

    /* 非待机状态下，保留互动模式原有的振动触发逻辑。 */
    app_motor_interactive_trigger();
}

VOID_T app_control_init(VOID_T)
{
    /* ---- 业务模块初始化（third 阶段只初始化 GPIO） ---- */
    /* 状态机须先于会读写设备状态的其他模块初始化。 */
    app_state_init();
    app_key_init();
    app_led_init();
    app_battery_init();
    app_charge_init();
    app_motor_init();
    app_dp_config_restore();
    app_vibration_init();

    app_vibration_register_cb(vibration_on_detected);
    app_state_register_change_cb(device_on_state_change);
}

VOID_T app_control_boot(VOID_T)
{
    // 初始化结束后从FLASH中读取标志位
    // 若标志位为0x03则认为是长按3秒后的复位启动，此时不能开机
    {
        UINT8_T f_reset = 0xFF;
        tal_flash_read(APP_DATA_FLASH_ADDR, &f_reset, 1);
        if (app_battery_is_critical() && !app_state_is_charging()) {
            TAL_PR_WARN("[boot] battery undervoltage, keep powered off");
        } else if (f_reset == 0x03) {
            TAL_PR_WARN("[boot] app_power_manager_start_pairing");
            app_power_manager_start_pairing();
        } else {
            // 启动默认开机
            app_state_set_power(1);
        }
    }
}
