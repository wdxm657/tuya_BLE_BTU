/**
 * @file app_led.h
 * @brief Work-status and power-status LEDs.
 */

#ifndef __APP_LED_H__
#define __APP_LED_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_MODE_OFF,
    LED_MODE_BLUE_SOLID,
    LED_MODE_BLUE_BLINK,
    LED_MODE_GREEN_BLINK,
    LED_MODE_RED_BLINK,
    LED_MODE_RED_SOLID,
    LED_MODE_CHARGE_GREEN_SOLID,
} led_mode_t;

VOID_T app_led_init(VOID_T);
/* Call once on normal boot, after the software timer service is ready. */
VOID_T app_led_timer_init(VOID_T);
VOID_T app_led_set_mode(led_mode_t mode);
VOID_T app_led_update(VOID_T);
VOID_T app_led_prepare_sleep(VOID_T);
VOID_T app_led_resume(VOID_T);
led_mode_t app_led_get_mode(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
