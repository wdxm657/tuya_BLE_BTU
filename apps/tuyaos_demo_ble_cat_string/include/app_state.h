/**
 * @file app_state.h
 * @brief afp pawswiff toy state machine.
 */

#ifndef __APP_STATE_H__
#define __APP_STATE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WORK_PERIOD_MIN 15 * 60
#define SLEEP_PERIOD_MIN 120 * 60
#define WORK_PERIOD_MS      (5 * 1000UL)
#define SLEEP_PERIOD_MS     (10 * 1000UL)
#define WORK_SLEEP_CYCLES   5

typedef enum {
    DEV_STATE_WORK        = 0,
    DEV_STATE_STANDBY     = 1,
    DEV_STATE_CHARGING    = 2,
    DEV_STATE_CHARGE_DONE = 3,
    DEV_STATE_SLEEP       = DEV_STATE_STANDBY,
} dev_state_t;

VOID_T app_state_init(VOID_T);
dev_state_t app_state_get(VOID_T);
VOID_T app_state_force_retention_wakeup(VOID_T);

BOOL_T app_state_toggle_power(VOID_T);
BOOL_T app_state_set_power(BOOL_T on);
BOOL_T app_state_is_machine_powered_on(VOID_T);

BOOL_T app_state_set_app_power(BOOL_T on);
BOOL_T app_state_is_app_power_on(VOID_T);
BOOL_T app_state_is_powered_on(VOID_T);

VOID_T app_state_set_charging(BOOL_T charging);
BOOL_T app_state_is_charging(VOID_T);
VOID_T app_state_set_charge_done(BOOL_T done);
BOOL_T app_state_is_charge_done(VOID_T);

VOID_T app_state_set_low_voltage_lock(BOOL_T locked);
BOOL_T app_state_is_low_voltage_locked(VOID_T);

VOID_T app_state_reset_work_cycle(VOID_T);
VOID_T app_state_process(VOID_T);
UINT8_T app_state_get_dp_enum(VOID_T);

VOID_T app_state_register_change_cb(VOID_T (*cb)(dev_state_t old_state, dev_state_t new_state));
VOID_T app_state_register_power_cb(VOID_T (*on_cb)(VOID_T), VOID_T (*off_cb)(VOID_T));
VOID_T app_state_register_machine_power_cb(VOID_T (*on_cb)(VOID_T), VOID_T (*off_cb)(VOID_T));

#ifdef __cplusplus
}
#endif

#endif
