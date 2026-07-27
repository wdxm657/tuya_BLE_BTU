/**
 * @file app_state.c
 * @brief afp pawswiff toy state machine.
 */

#include "tal_log.h"
#include "tal_sw_timer.h"

#include "app_state.h"

STATIC dev_state_t s_dev_state = DEV_STATE_WORK;
STATIC BOOL_T s_machine_powered = TRUE;
STATIC BOOL_T s_app_powered = TRUE;
STATIC BOOL_T s_charging = FALSE;
STATIC BOOL_T s_charge_done = FALSE;
STATIC BOOL_T s_low_voltage_lock = FALSE;
STATIC TIMER_ID s_work_timer_id = NULL;
STATIC TIMER_ID s_sleep_timer_id = NULL;
STATIC BOOL_T s_run_active = FALSE;
STATIC UINT8_T s_work_sleep_cycle_count = 0;
STATIC BOOL_T s_cycle_limit_reached = FALSE;

STATIC VOID_T (*s_state_change_cb)(dev_state_t, dev_state_t) = NULL;
STATIC VOID_T (*s_run_on_cb)(VOID_T) = NULL;
STATIC VOID_T (*s_run_off_cb)(VOID_T) = NULL;
STATIC VOID_T (*s_machine_on_cb)(VOID_T) = NULL;
STATIC VOID_T (*s_machine_off_cb)(VOID_T) = NULL;

STATIC BOOL_T app_state_should_run(VOID_T)
{
    return (s_machine_powered && s_app_powered && !s_low_voltage_lock && !s_cycle_limit_reached && s_dev_state != DEV_STATE_SLEEP);
}

STATIC VOID_T app_state_notify_run_if_needed(VOID_T)
{
    BOOL_T should_run = app_state_should_run();

    if (should_run == s_run_active) {
        return;
    }

    s_run_active = should_run;
    if (s_run_active) {
        if (s_run_on_cb != NULL) {
            s_run_on_cb();
        }
    } else {
        if (s_run_off_cb != NULL) {
            s_run_off_cb();
        }
    }
}

STATIC VOID_T app_state_set(dev_state_t new_state)
{
    if (new_state == s_dev_state) {
        app_state_notify_run_if_needed();
        return;
    }

    dev_state_t old_state = s_dev_state;
    s_dev_state = new_state;
    TAL_PR_INFO("[state] %d -> %d", old_state, new_state);

    if (s_state_change_cb != NULL) {
        s_state_change_cb(old_state, new_state);
    }

    app_state_notify_run_if_needed();
}

STATIC VOID_T app_state_start_work_timer(VOID_T)
{
    if (s_work_timer_id == NULL) {
        return;
    }
    tal_sw_timer_stop(s_work_timer_id);
    tal_sw_timer_start(s_work_timer_id, WORK_PERIOD_MS, TAL_TIMER_ONCE);
}

STATIC VOID_T app_state_reset_cycle_tracking(VOID_T)
{
    s_work_sleep_cycle_count = 0;
    s_cycle_limit_reached = FALSE;
}

STATIC VOID_T app_state_stop_cycle_timers(VOID_T)
{
    if (s_work_timer_id != NULL) {
        tal_sw_timer_stop(s_work_timer_id);
    }
    if (s_sleep_timer_id != NULL) {
        tal_sw_timer_stop(s_sleep_timer_id);
    }
}

STATIC VOID_T app_state_work_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    if (!s_machine_powered || !s_app_powered || s_low_voltage_lock) {
        return;
    }

    app_state_set(DEV_STATE_SLEEP);
    if (s_sleep_timer_id != NULL) {
        tal_sw_timer_start(s_sleep_timer_id, SLEEP_PERIOD_MS, TAL_TIMER_ONCE);
    }
}

STATIC VOID_T app_state_sleep_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    if (!s_machine_powered || !s_app_powered || s_low_voltage_lock) {
        return;
    }

    s_work_sleep_cycle_count++;
    if (s_work_sleep_cycle_count >= WORK_SLEEP_CYCLES) {
        s_cycle_limit_reached = TRUE;
        app_state_stop_cycle_timers();
        app_state_set(DEV_STATE_SLEEP);
        app_state_set_app_power(FALSE);
        TAL_PR_INFO("[state] cycle limit reached, power off");
        return;
    }

    app_state_set(DEV_STATE_WORK);
    app_state_start_work_timer();
}

VOID_T app_state_init(VOID_T)
{
    tal_sw_timer_create(app_state_work_timeout_handler, NULL, &s_work_timer_id);
    tal_sw_timer_create(app_state_sleep_timeout_handler, NULL, &s_sleep_timer_id);

    s_machine_powered = TRUE;
    s_app_powered = TRUE;
    s_charging = FALSE;
    s_charge_done = FALSE;
    s_low_voltage_lock = FALSE;
    s_run_active = FALSE;
    app_state_reset_cycle_tracking();
    s_dev_state = DEV_STATE_WORK;
    app_state_start_work_timer();

    TAL_PR_INFO("[state] initialized");
}

dev_state_t app_state_get(VOID_T)
{
    return s_dev_state;
}

BOOL_T app_state_toggle_power(VOID_T)
{
    return app_state_set_power(!s_machine_powered);
}

BOOL_T app_state_set_power(BOOL_T on)
{
    if (s_machine_powered == on) {
        app_state_notify_run_if_needed();
        return s_machine_powered;
    }

    s_machine_powered = on;
    TAL_PR_INFO("[state] machine power=%d", on);

    if (s_machine_powered) {
        app_state_reset_cycle_tracking();
        if (s_machine_on_cb != NULL) {
            s_machine_on_cb();
        }
        app_state_set(DEV_STATE_WORK);
        app_state_start_work_timer();
    } else {
        app_state_stop_cycle_timers();
        app_state_notify_run_if_needed();
        if (s_machine_off_cb != NULL) {
            s_machine_off_cb();
        }
    }

    return s_machine_powered;
}

BOOL_T app_state_is_machine_powered_on(VOID_T)
{
    return s_machine_powered;
}

BOOL_T app_state_set_app_power(BOOL_T on)
{
    if (s_app_powered == on) {
        app_state_notify_run_if_needed();
        return s_app_powered;
    }

    s_app_powered = on;
    TAL_PR_INFO("[state] app power=%d", on);

    if (s_app_powered && s_machine_powered && !s_low_voltage_lock) {
        app_state_reset_cycle_tracking();
        app_state_set(DEV_STATE_WORK);
        app_state_start_work_timer();
    } else {
        app_state_stop_cycle_timers();
        app_state_notify_run_if_needed();
    }

    return s_app_powered;
}

BOOL_T app_state_is_app_power_on(VOID_T)
{
    return s_app_powered;
}

BOOL_T app_state_is_powered_on(VOID_T)
{
    return app_state_should_run();
}

VOID_T app_state_set_charging(BOOL_T charging)
{
    if (charging)
    {
        app_state_set(DEV_STATE_CHARGING);
    }
    s_charging = charging;
}

BOOL_T app_state_is_charging(VOID_T)
{
    return s_charging;
}

VOID_T app_state_set_charge_done(BOOL_T done)
{
    if (done)
    {
        app_state_set(DEV_STATE_CHARGE_DONE);
    }
    s_charge_done = done;
}

BOOL_T app_state_is_charge_done(VOID_T)
{
    return s_charge_done;
}

VOID_T app_state_set_low_voltage_lock(BOOL_T locked)
{
    if (s_low_voltage_lock == locked) {
        return;
    }

    s_low_voltage_lock = locked;
    TAL_PR_WARN("[state] low voltage lock=%d", locked);
    if (locked) {
        app_state_set_app_power(FALSE);
    } else if (s_machine_powered && s_app_powered && !s_cycle_limit_reached) {
        app_state_reset_cycle_tracking();
        app_state_set(DEV_STATE_WORK);
        app_state_start_work_timer();
    }
    app_state_notify_run_if_needed();
}

BOOL_T app_state_is_low_voltage_locked(VOID_T)
{
    return s_low_voltage_lock;
}

VOID_T app_state_reset_work_cycle(VOID_T)
{
    if (!s_machine_powered || !s_app_powered || s_low_voltage_lock) {
        return;
    }
    if (s_cycle_limit_reached) {
        return;
    }
    app_state_set(DEV_STATE_WORK);
    app_state_start_work_timer();
}

VOID_T app_state_process(VOID_T)
{
}

UINT8_T app_state_get_dp_enum(VOID_T)
{
    return app_state_should_run() ? (UINT8_T)DEV_STATE_WORK : (UINT8_T)DEV_STATE_STANDBY;
    // if (s_app_powered)
    // {
    //     return app_state_should_run() ? (UINT8_T)DEV_STATE_WORK : (UINT8_T)DEV_STATE_STANDBY;
    // }
    // else{
    //     if (s_charge_done || s_charging)
    //     {
    //         if (s_charge_done) {
    //             return (UINT8_T)DEV_STATE_CHARGE_DONE;
    //         }
    //         if (s_charging) {
    //             return (UINT8_T)DEV_STATE_CHARGING;
    //         }
    //     }
    //     return app_state_should_run() ? (UINT8_T)DEV_STATE_WORK : (UINT8_T)DEV_STATE_STANDBY;
    // }    
}

VOID_T app_state_register_change_cb(VOID_T (*cb)(dev_state_t, dev_state_t))
{
    s_state_change_cb = cb;
}

VOID_T app_state_register_power_cb(VOID_T (*on_cb)(VOID_T), VOID_T (*off_cb)(VOID_T))
{
    s_run_on_cb = on_cb;
    s_run_off_cb = off_cb;
    app_state_notify_run_if_needed();
}

VOID_T app_state_register_machine_power_cb(VOID_T (*on_cb)(VOID_T), VOID_T (*off_cb)(VOID_T))
{
    s_machine_on_cb = on_cb;
    s_machine_off_cb = off_cb;
}
