/**
 * @file app_charge.c
 * @brief USB charge-state detection for laser bug toy.
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"

#include "board.h"
#include "app_charge.h"
#include "app_state.h"
#include "app_led.h"
#include "app_motor.h"

#define CHARGE_POLL_MS 100

STATIC TIMER_ID s_charge_timer_id = NULL;
STATIC BOOL_T s_usb_detected = FALSE;

STATIC BOOL_T app_charge_read_usb(VOID_T)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    if (tal_gpio_read(USB_DET, &level) != OPRT_OK) {
        return FALSE;
    }
    return level == TUYA_GPIO_LEVEL_LOW;
}

STATIC VOID_T app_charge_apply_state(BOOL_T usb_detected)
{
    s_usb_detected = usb_detected;
    app_state_set_charging(usb_detected);
    app_state_set_charge_done(FALSE);
    app_motor_refresh_m3_duty();
    app_led_update();
}

STATIC VOID_T app_charge_poll_handler(TIMER_ID timer_id, VOID_T *arg)
{
    BOOL_T usb = app_charge_read_usb();

    if (usb != s_usb_detected) {
        TAL_PR_INFO("[charge] usb=%d", usb);
        app_charge_apply_state(usb);
    }
}

VOID_T app_charge_init(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T input_cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };

    tal_gpio_init(USB_DET, &input_cfg);

    s_usb_detected = FALSE;
    tal_sw_timer_create(app_charge_poll_handler, NULL, &s_charge_timer_id);
    app_charge_poll_handler(s_charge_timer_id, NULL);
    tal_sw_timer_start(s_charge_timer_id, CHARGE_POLL_MS, TAL_TIMER_CYCLE);
}

BOOL_T app_charge_is_detected(VOID_T)
{
    return s_usb_detected;
}

BOOL_T app_charge_is_full(VOID_T)
{
    return FALSE;
}
