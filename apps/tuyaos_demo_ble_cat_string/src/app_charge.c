/**
 * @file app_charge.c
 * @brief USB and charge-state detection.
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"
#include "tal_system.h"

#include "board.h"
#include "app_charge.h"
#include "app_state.h"
#include "app_led.h"

#define CHARGE_POLL_MS 100

typedef enum {
    CHG_STATE_NO_USB,
    CHG_STATE_CHARGING,
    CHG_STATE_FULL,
} charge_state_t;

STATIC TIMER_ID s_charge_timer_id = NULL;
STATIC charge_state_t s_charge_state = CHG_STATE_NO_USB;
STATIC volatile BOOL_T s_usb_irq_pending = FALSE;

STATIC VOID_T app_charge_irq_handler(VOID_T *arg)
{
    s_usb_irq_pending = TRUE;
}

BOOL_T app_charge_read_usb(VOID_T)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    if (tal_gpio_read(USB_DET, &level) != OPRT_OK) {
        return FALSE;
    }
    return level == TUYA_GPIO_LEVEL_LOW;
}

STATIC BOOL_T app_charge_read_active(VOID_T)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    if (tal_gpio_read(CHARGE_STATE, &level) != OPRT_OK) {
        return FALSE;
    }
    return level == TUYA_GPIO_LEVEL_LOW;
}

STATIC VOID_T app_charge_apply_state(charge_state_t state)
{
    s_charge_state = state;

    if (state == CHG_STATE_CHARGING) {
        app_state_set_charging(TRUE);
        app_state_set_charge_done(FALSE);
        // tal_gpio_write(Set_Charg_I, app_state_is_powered_on() ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
    } else if (state == CHG_STATE_FULL) {
        app_state_set_charging(FALSE);
        app_state_set_charge_done(TRUE);
        // tal_gpio_write(Set_Charg_I, TUYA_GPIO_LEVEL_LOW);
    } else {
        app_state_set_charging(FALSE);
        app_state_set_charge_done(FALSE);
        // tal_gpio_write(Set_Charg_I, TUYA_GPIO_LEVEL_LOW);
    }

    app_led_update();
}

STATIC VOID_T app_charge_poll_handler(TIMER_ID timer_id, VOID_T *arg)
{
    BOOL_T usb = app_charge_read_usb();
    BOOL_T active = app_charge_read_active();
    charge_state_t new_state;

    if (!usb) {
        new_state = CHG_STATE_NO_USB;
    } else if (active) {
        new_state = CHG_STATE_CHARGING;
    } else {
        new_state = CHG_STATE_FULL;
    }

    if (new_state != s_charge_state) {
        TAL_PR_INFO("[charge] %d -> %d", s_charge_state, new_state);
        app_charge_apply_state(new_state);
    }
    if (!usb) {
        tal_sw_timer_stop(s_charge_timer_id);
    }
}

VOID_T app_charge_init(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T input_cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    TUYA_GPIO_BASE_CFG_T output_cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    TUYA_GPIO_IRQ_T irq_cfg = {
        .mode = TUYA_GPIO_IRQ_RISE_FALL,
        .cb = app_charge_irq_handler,
        .arg = NULL,
    };

    tal_gpio_init(USB_DET, &input_cfg);
    tal_gpio_init(CHARGE_STATE, &input_cfg);
    tal_gpio_init(CHARGE_EN, &output_cfg);
    // tal_gpio_init(Set_Charg_I, &output_cfg);

    tal_gpio_write(CHARGE_EN, TUYA_GPIO_LEVEL_HIGH);
    // tal_gpio_write(Set_Charg_I, TUYA_GPIO_LEVEL_LOW);
    tal_gpio_irq_init(USB_DET, &irq_cfg);
    tal_gpio_irq_enable(USB_DET);
}

VOID_T app_charge_timer_init(VOID_T)
{
    if (s_charge_timer_id != NULL) {
        return;
    }

    s_charge_state = CHG_STATE_NO_USB;
    if (tal_sw_timer_create(app_charge_poll_handler, NULL, &s_charge_timer_id) != OPRT_OK) {
        return;
    }
    /* Check for USB already connected at boot. */
    s_usb_irq_pending = TRUE;
}

VOID_T app_charge_process(VOID_T)
{
    UINT32_T irq_mask;
    BOOL_T pending;

    if (s_charge_timer_id == NULL) {
        return;
    }
    irq_mask = tal_system_enter_critical();
    pending = s_usb_irq_pending;
    s_usb_irq_pending = FALSE;
    tal_system_exit_critical(irq_mask);
    if (!pending) {
        return;
    }

    if (app_charge_read_usb()) {
        // usb 插入时不进retention低功耗,若在低功耗则退出
        if (tkl_cpu_is_sleep())
        {
            tal_cpu_force_wakeup();
            app_led_resume();
        }
        if (!tal_sw_timer_is_running(s_charge_timer_id)) {
            tal_sw_timer_start(s_charge_timer_id, CHARGE_POLL_MS, TAL_TIMER_CYCLE);
        }
    } else {
        tal_sw_timer_stop(s_charge_timer_id);
        app_charge_apply_state(CHG_STATE_NO_USB);
    }
}

BOOL_T app_charge_is_detected(VOID_T)
{
    return s_charge_state == CHG_STATE_CHARGING;
}

BOOL_T app_charge_is_full(VOID_T)
{
    return s_charge_state == CHG_STATE_FULL;
}
