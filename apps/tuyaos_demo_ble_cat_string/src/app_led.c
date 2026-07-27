/**
 * @file app_led.c
 * @brief Work-status LED and power-status LED handling.
 */

#include "tal_sw_timer.h"
#include "tal_gpio.h"

#include "board.h"
#include "tuya_ble_main.h"
#include "app_led.h"
#include "app_state.h"
#include "app_battery.h"
#include "tal_log.h"

#define LED_BLINK_PERIOD_MS 250

STATIC TIMER_ID s_led_timer_id = NULL;
STATIC led_mode_t s_status_mode = LED_MODE_OFF;
STATIC led_mode_t s_power_mode = LED_MODE_OFF;
STATIC BOOL_T s_blink_on = FALSE;

STATIC VOID_T app_led_write(TUYA_GPIO_NUM_E pin, BOOL_T on)
{
    tal_gpio_write(pin, on);
}

STATIC VOID_T app_led_output_init(TUYA_GPIO_NUM_E pin)
{
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };

    tal_gpio_init(pin, &gpio_cfg);
}

STATIC VOID_T app_led_pulldown_input(TUYA_GPIO_NUM_E pin)
{
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode = TUYA_GPIO_PULLDOWN,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };

    tal_gpio_write(pin, TUYA_GPIO_LEVEL_LOW);
    tal_gpio_init(pin, &gpio_cfg);
}

STATIC BOOL_T app_led_is_pairing(VOID_T)
{
    tuya_ble_connect_status_t st = tuya_ble_connect_status_get();
    return (st == BONDING_CONN || st == BONDING_UNCONN);
}

STATIC BOOL_T app_led_mode_blinks(led_mode_t mode)
{
    return (mode == LED_MODE_BLUE_BLINK || mode == LED_MODE_GREEN_BLINK || mode == LED_MODE_RED_BLINK);
}

STATIC VOID_T app_led_apply(VOID_T)
{
    switch (s_status_mode) {
    case LED_MODE_BLUE_SOLID:
        app_led_write(LED_B, TRUE);
        app_led_write(LED_G, FALSE);
        break;
    case LED_MODE_BLUE_BLINK:
        app_led_write(LED_B, s_blink_on);
        app_led_write(LED_G, FALSE);
        break;
    case LED_MODE_GREEN_BLINK:
        app_led_write(LED_B, FALSE);
        app_led_write(LED_G, s_blink_on);
        break;
    default:
        app_led_write(LED_B, FALSE);
        app_led_write(LED_G, FALSE);
        break;
    }

    switch (s_power_mode) {
    case LED_MODE_RED_BLINK:
        app_led_write(CHARGE_R, s_blink_on);
        app_led_write(CHARGE_G, FALSE);
        break;
    case LED_MODE_RED_SOLID:
        app_led_write(CHARGE_R, TRUE);
        app_led_write(CHARGE_G, FALSE);
        break;
    case LED_MODE_CHARGE_GREEN_SOLID:
        app_led_write(CHARGE_R, FALSE);
        app_led_write(CHARGE_G, TRUE);
        break;
    default:
        app_led_write(CHARGE_R, FALSE);
        app_led_write(CHARGE_G, FALSE);
        break;
    }
}

STATIC VOID_T app_led_restart_timer_if_needed(VOID_T)
{
    BOOL_T need_blink = app_led_mode_blinks(s_status_mode) || app_led_mode_blinks(s_power_mode);

    if (s_led_timer_id != NULL) {
        tal_sw_timer_stop(s_led_timer_id);
    }
    s_blink_on = TRUE;
    app_led_apply();

    if (need_blink) {
        tal_sw_timer_start(s_led_timer_id, LED_BLINK_PERIOD_MS, TAL_TIMER_CYCLE);
    }
}

STATIC VOID_T app_led_blink_handler(TIMER_ID timer_id, VOID_T *arg)
{
    s_blink_on = !s_blink_on;
    app_led_apply();
}

VOID_T app_led_init(VOID_T)
{
    app_led_output_init(LED_B);
    app_led_output_init(LED_G);
    app_led_output_init(CHARGE_R);
    app_led_output_init(CHARGE_G);
    tal_sw_timer_create(app_led_blink_handler, NULL, &s_led_timer_id);

    s_status_mode = LED_MODE_OFF;
    s_power_mode = LED_MODE_OFF;
    s_blink_on = FALSE;
    app_led_apply();
}

VOID_T app_led_set_mode(led_mode_t mode)
{
    s_status_mode = mode;
    s_power_mode = LED_MODE_OFF;
    app_led_restart_timer_if_needed();
}

VOID_T app_led_update(VOID_T)
{
    led_mode_t status_mode = LED_MODE_OFF;
    led_mode_t power_mode = LED_MODE_OFF;

    if (app_state_is_machine_powered_on()) {

         if (!app_led_is_pairing()) 
            status_mode = LED_MODE_BLUE_SOLID;
        else if(!app_state_is_app_power_on()) {
            status_mode = LED_MODE_OFF;
        } else if (app_state_get() != DEV_STATE_SLEEP) {
            status_mode = LED_MODE_GREEN_BLINK;
        }else{
            status_mode = LED_MODE_OFF;
        }

        if (app_state_is_charging()) {
            power_mode = LED_MODE_RED_SOLID;
        } else if (app_state_is_charge_done()) {
            power_mode = LED_MODE_CHARGE_GREEN_SOLID;
        } else if (app_state_is_low_voltage_locked() || app_battery_is_low()) {
            power_mode = LED_MODE_RED_BLINK;
        }else{
            power_mode = LED_MODE_OFF;
        }
    }

    if (status_mode == s_status_mode && power_mode == s_power_mode) {
        return;
    }

    s_status_mode = status_mode;
    s_power_mode = power_mode;
    app_led_restart_timer_if_needed();
}

VOID_T app_led_prepare_sleep(VOID_T)
{
    if (s_led_timer_id != NULL) {
        tal_sw_timer_stop(s_led_timer_id);
    }
    s_status_mode = LED_MODE_OFF;
    s_power_mode = LED_MODE_OFF;
    s_blink_on = FALSE;

    app_led_pulldown_input(LED_B);
    app_led_pulldown_input(LED_G);
    app_led_pulldown_input(CHARGE_R);
    app_led_pulldown_input(CHARGE_G);
}

VOID_T app_led_resume(VOID_T)
{
    app_led_output_init(LED_B);
    app_led_output_init(LED_G);
    app_led_output_init(CHARGE_R);
    app_led_output_init(CHARGE_G);
    s_status_mode = LED_MODE_OFF;
    s_power_mode = LED_MODE_OFF;
    s_blink_on = FALSE;
    app_led_update();
}

led_mode_t app_led_get_mode(VOID_T)
{
    return s_status_mode;
}
