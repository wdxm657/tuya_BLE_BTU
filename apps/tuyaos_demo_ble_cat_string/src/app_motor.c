/**
 * @file app_motor.c
 * @brief afp pawswiff motor control.
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_pwm.h"
#include "tal_system.h"

#include "board.h"
#include "app_motor.h"
#include "app_battery.h"
#include "app_charge.h"
#include "app_dp_parser.h"

#define VARIABLE_SEQ_STEPS 4
#define FIXED_SEQ_STEPS (sizeof(s_fixed_seq) / sizeof(s_fixed_seq[0]))
#define VARIABLE_SEQ_CYCLES 5
#define RANDOM_BURST_MS 4000
#define RANDOM_BURST_RAMP_STEPS 12
#define RANDOM_BURST_STEP_MS (RANDOM_BURST_MS / RANDOM_BURST_RAMP_STEPS)
#define RANDOM_STOP_MS 2000
#define RANDOM_REVERSE_WAIT_MS 5000
#define FIXED_INITIAL_DIRECTION_MS (4UL * 60UL * 1000UL)
#define MOTOR_DUTY_MIN_PERCENT 28
#define MOTOR_DUTY_MAX_PERCENT 36
#define MOTOR_DUTY_DEFAULT_PERCENT (MOTOR_DUTY_MIN_PERCENT + MOTOR_DUTY_MAX_PERCENT) / 2
#define MOTOR_STEPLESS_DEFAULT_PERCENT \
    (((MOTOR_DUTY_DEFAULT_PERCENT - MOTOR_DUTY_MIN_PERCENT) * 100) / \
     (MOTOR_DUTY_MAX_PERCENT - MOTOR_DUTY_MIN_PERCENT))

#define DUTY_BOOST_MAX          12      /* N 最大增加值 */

STATIC work_mode_t s_work_mode = WORK_MODE_FIXED;
STATIC UINT8_T s_app_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
STATIC UINT8_T s_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
#if APP_ENABLE_MOTOR_TUNING_DPS
STATIC UINT8_T s_motor_duty_min_percent = MOTOR_DUTY_MIN_PERCENT;
STATIC UINT8_T s_motor_duty_max_percent = MOTOR_DUTY_MAX_PERCENT;
STATIC UINT8_T s_duty_boost_max = DUTY_BOOST_MAX;
#define MOTOR_DUTY_MIN_CURRENT   s_motor_duty_min_percent
#define MOTOR_DUTY_MAX_CURRENT   s_motor_duty_max_percent
#define DUTY_BOOST_MAX_CURRENT  s_duty_boost_max
#else
#define MOTOR_DUTY_MIN_CURRENT   MOTOR_DUTY_MIN_PERCENT
#define MOTOR_DUTY_MAX_CURRENT   MOTOR_DUTY_MAX_PERCENT
#define DUTY_BOOST_MAX_CURRENT  DUTY_BOOST_MAX
#endif
STATIC BOOL_T s_random_duty_active = FALSE;
STATIC BOOL_T s_motor_enabled = FALSE;
STATIC BOOL_T s_motor_running = FALSE;
STATIC TIMER_ID s_motor_timer_id = NULL;
STATIC UINT8_T s_seq_index = 0;
STATIC UINT8_T s_var_seq_cycle_count = 0;
STATIC UINT32_T s_fixed_start_ms = 0;
STATIC BOOL_T s_fixed_seq_started = FALSE;
#define BURST_SEQ_SIZE 20

STATIC UINT8_T s_var_rand_seed = 0;
STATIC UINT8_T s_var_burst_idx = 0;
STATIC UINT8_T s_var_burst_step = 0;
STATIC UINT8_T s_var_burst_dir = MOTOR_DIR_STOP;
STATIC UINT8_T s_var_last_seq_dir = MOTOR_DIR_STOP;
STATIC UINT8_T s_current_direction = MOTOR_DIR_STOP;
STATIC UINT32_T s_current_duty = MOTOR_PWM_DUTY_0;

typedef enum
{
    VAR_SUB_SEQ,
    VAR_SUB_BURST_WAIT,
    VAR_SUB_RANDOM,
    VAR_SUB_STOP,
} var_substate_t;

STATIC var_substate_t s_var_substate = VAR_SUB_SEQ;

/** 单步：方向 + 持续时间（毫秒）。调试时序只改 s_sm_slow_fast_seq[]。 */
typedef struct
{
    UINT8_T dir;
    UINT16_T duration_ms;
} sm_simple_seq_step_t;

#define FIX_STOP 1000
STATIC const sm_simple_seq_step_t s_fixed_seq[] = {
    // 4分钟运动结束后先停3秒
    {MOTOR_DIR_STOP, FIX_STOP * 3},
    {MOTOR_DIR_FORWARD, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 800},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1300},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 2500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1200},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1400},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1300},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 800},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 700},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1200},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1700},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 800},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 900},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 700},
};

STATIC const sm_simple_seq_step_t s_variable_seq[VARIABLE_SEQ_STEPS] = {
    {MOTOR_DIR_FORWARD, 250},
    {MOTOR_DIR_STOP, 5000},
    {MOTOR_DIR_REVERSE, 250},
    {MOTOR_DIR_STOP, 5000},
};

/* 随机爆发序列 — 方向与挡位预编排，避免连续同方向 */
typedef struct
{
    UINT8_T dir;
} motor_burst_step_t;

STATIC const motor_burst_step_t s_rand_burst_seq[BURST_SEQ_SIZE] = {
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_FORWARD},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_REVERSE},
    {MOTOR_DIR_FORWARD},
};

STATIC CONST struct {
    INT32_T  voltage_precent;
    UINT8_T  percent;
} s_battery_boost_lut[] = {
    {100,   0},
    { 95,   5},
    { 90,  10},
    { 85,  15},
    { 80,  20},
    { 75,  25},
    { 70,  30},
    { 65,  35},
    { 60,  40},
    { 55,  45},
    { 50,  50},
    { 45,  55},
    { 40,  60},
    { 35,  65},
    { 30,  70},
    { 25,  80},
    { 10, 100},
};

/* Return PWM duty units to preserve fractional percentage points. */
STATIC UINT32_T app_motor_battery_boost(INT32_T voltage_percent)
{
    UINT32_T i;
    UINT32_T count = sizeof(s_battery_boost_lut) / sizeof(s_battery_boost_lut[0]);
    UINT32_T max_boost = DUTY_BOOST_MAX_CURRENT * MOTOR_PWM_DUTY_1;

    if (voltage_percent >= s_battery_boost_lut[0].voltage_precent) {
        return max_boost * s_battery_boost_lut[0].percent / 100U;
    }

    for (i = 1; i < count; i++) {
        if (voltage_percent >= s_battery_boost_lut[i].voltage_precent) {
            UINT32_T span = s_battery_boost_lut[i - 1].voltage_precent -
                            s_battery_boost_lut[i].voltage_precent;
            UINT32_T drop = s_battery_boost_lut[i - 1].voltage_precent - voltage_percent;
            UINT32_T high_boost = max_boost * s_battery_boost_lut[i - 1].percent / 100U;
            UINT32_T low_boost = max_boost * s_battery_boost_lut[i].percent / 100U;

            return high_boost + (low_boost - high_boost) * drop / span;
        }
    }

    return max_boost * s_battery_boost_lut[count - 1].percent / 100U;
}

STATIC UINT32_T app_motor_duty_for_stepless(UINT8_T percent)
{
    UINT32_T base_percent;
    UINT32_T duty;
    INT32_T voltage_percent;
    UINT32_T boost;

    if (percent > 100) {
        percent = 100;
    }

    if (s_random_duty_active) {
        if (percent < MOTOR_DUTY_MIN_CURRENT) {
            percent = MOTOR_DUTY_MIN_CURRENT;
        }
        if (percent > MOTOR_DUTY_MAX_CURRENT) {
            percent = MOTOR_DUTY_MAX_CURRENT;
        }
        base_percent = percent;
    } else {
        base_percent = MOTOR_DUTY_MIN_CURRENT +
                       ((UINT32_T)percent * (MOTOR_DUTY_MAX_CURRENT - MOTOR_DUTY_MIN_CURRENT)) / 100;
    }

    /* USB inserted or charge done: use the mapped duty without voltage compensation. */
    if (app_charge_is_detected() || app_charge_is_full())
    {
        return MOTOR_PWM_DUTY_1 * base_percent;
    }

    /* Battery compensation follows the voltage table. */
    voltage_percent = app_battery_get_percent();
    boost = app_motor_battery_boost(voltage_percent);
    TAL_PR_INFO("[motor] voltage %d base %d boost_pwm %d", voltage, base_percent, boost);

    duty = MOTOR_PWM_DUTY_1 * base_percent + boost;
    if (duty > MOTOR_PWM_DUTY_1 * 100U) {
        duty = MOTOR_PWM_DUTY_1 * 100U;
    }

    return duty;
}

STATIC VOID_T app_motor_set_direction(UINT8_T direction, UINT32_T duty)
{
    if (duty == NULL)
    {
        duty = app_motor_duty_for_stepless(s_stepless_percent);
    }

    s_current_direction = direction;
    s_current_duty = duty;

    switch (direction)
    {
    case MOTOR_DIR_FORWARD:
        tal_pwm_duty_set(MOTOR_PWM_CH_FOR, duty);
        tal_pwm_duty_set(MOTOR_PWM_CH_REV, MOTOR_PWM_DUTY_0);
        s_motor_running = (duty > 0);
        break;
    case MOTOR_DIR_REVERSE:
        tal_pwm_duty_set(MOTOR_PWM_CH_FOR, MOTOR_PWM_DUTY_0);
        tal_pwm_duty_set(MOTOR_PWM_CH_REV, duty);
        s_motor_running = (duty > 0);
        break;
    case MOTOR_DIR_STOP:
    default:
        tal_pwm_duty_set(MOTOR_PWM_CH_FOR, MOTOR_PWM_DUTY_0);
        tal_pwm_duty_set(MOTOR_PWM_CH_REV, MOTOR_PWM_DUTY_0);
        s_motor_running = FALSE;
        break;
    }
}

STATIC VOID_T app_motor_report_stepless_percent(VOID_T);

STATIC UINT8_T app_motor_random_burst_duty_percent(VOID_T)
{
    UINT16_T min = MOTOR_DUTY_MIN_CURRENT + 10U;
    UINT16_T max = MOTOR_DUTY_MAX_CURRENT;
    UINT32_T range;

    if (min > max) {
        min = max;
    }
    range = max - min;
    if (s_var_burst_step >= RANDOM_BURST_RAMP_STEPS - 1U) {
        return (UINT8_T)max;
    }

    return (UINT8_T)(min +
                     (range * s_var_burst_step) / (RANDOM_BURST_RAMP_STEPS - 1U));
}

STATIC VOID_T app_motor_start_random_burst_step(VOID_T)
{
    UINT8_T duty_percent = app_motor_random_burst_duty_percent();

    app_motor_set_direction(s_var_burst_dir, (UINT32_T)duty_percent * MOTOR_PWM_DUTY_1);
    s_var_burst_step++;
}

STATIC VOID_T app_motor_restore_app_duty(VOID_T)
{
    s_random_duty_active = FALSE;
    s_stepless_percent = s_app_stepless_percent;
    app_motor_set_direction(s_current_direction,NULL);
}

STATIC VOID_T app_motor_report_stepless_percent(VOID_T)
{
    UINT8_T buf[4] = {0};

    buf[3] = s_stepless_percent;
    app_dp_report(DP_ID_STEPLESS_CONTROL, buf, 4);
}

STATIC VOID_T app_motor_timer_handler(TIMER_ID timer_id, VOID_T *arg)
{
    if (!s_motor_enabled)
    {
        app_motor_set_direction(MOTOR_DIR_STOP,NULL);
        return;
    }

    if (s_work_mode == WORK_MODE_FIXED)
    {
        if (!s_fixed_seq_started) {
            UINT32_T elapsed_ms = tal_system_get_millisecond() - s_fixed_start_ms;

            if (elapsed_ms < FIXED_INITIAL_DIRECTION_MS) {
                app_motor_set_direction(MOTOR_DIR_FORWARD, NULL);
                tal_sw_timer_start(s_motor_timer_id,
                                   FIXED_INITIAL_DIRECTION_MS - elapsed_ms,
                                   TAL_TIMER_ONCE);
                return;
            }

            s_fixed_seq_started = TRUE;
            s_seq_index = 0;
        }

        /* After the initial forward run, follow the fixed sequence until work timeout. */
        const sm_simple_seq_step_t *step = &s_fixed_seq[s_seq_index];
        app_motor_set_direction(step->dir,NULL);
        s_seq_index++;
        if (s_seq_index >= FIXED_SEQ_STEPS)
        {
            s_seq_index = 0;
        }
        tal_sw_timer_start(s_motor_timer_id, step->duration_ms, TAL_TIMER_ONCE);
        return;
    }

    /* 随机模式 */
    if (s_var_substate == VAR_SUB_SEQ)
    {
        /* s_variable_seq 循环 5 次 */
        const sm_simple_seq_step_t *step = &s_variable_seq[s_seq_index];
        app_motor_set_direction(step->dir,70 * MOTOR_PWM_DUTY_1);
        if (step->dir != MOTOR_DIR_STOP) {
            s_var_last_seq_dir = step->dir;
        }
        s_seq_index++;
        if (s_seq_index >= VARIABLE_SEQ_STEPS)
        {
            s_seq_index = 0;
            s_var_seq_cycle_count++;
            if (s_var_seq_cycle_count >= VARIABLE_SEQ_CYCLES)
            {
                /* 5 次循环结束 → 随机方向 6s 爆发 */
                s_var_substate = VAR_SUB_RANDOM;
                s_var_seq_cycle_count = 0;
                const motor_burst_step_t *burst = &s_rand_burst_seq[s_var_burst_idx];
                s_var_burst_dir = burst->dir;
                s_var_burst_step = 0;
                s_var_burst_idx = (s_var_burst_idx + 1U) % BURST_SEQ_SIZE;
                if (s_var_last_seq_dir != MOTOR_DIR_STOP &&
                    s_var_burst_dir != s_var_last_seq_dir) {
                    s_var_substate = VAR_SUB_BURST_WAIT;
                    app_motor_set_direction(MOTOR_DIR_STOP, NULL);
                    tal_sw_timer_start(s_motor_timer_id, RANDOM_REVERSE_WAIT_MS, TAL_TIMER_ONCE);
                    return;
                }
                app_motor_start_random_burst_step();
                s_var_substate = VAR_SUB_RANDOM;
                tal_sw_timer_start(s_motor_timer_id, RANDOM_BURST_STEP_MS, TAL_TIMER_ONCE);
                return;
            }
        }
        tal_sw_timer_start(s_motor_timer_id, step->duration_ms, TAL_TIMER_ONCE);
    }
    else if (s_var_substate == VAR_SUB_BURST_WAIT)
    {
        app_motor_start_random_burst_step();
        s_var_substate = VAR_SUB_RANDOM;
        tal_sw_timer_start(s_motor_timer_id, RANDOM_BURST_STEP_MS, TAL_TIMER_ONCE);
    }
    else if (s_var_substate == VAR_SUB_RANDOM)
    {
        if (s_var_burst_step < RANDOM_BURST_RAMP_STEPS)
        {
            app_motor_start_random_burst_step();
            tal_sw_timer_start(s_motor_timer_id, RANDOM_BURST_STEP_MS, TAL_TIMER_ONCE);
            return;
        }

        /* VAR_SUB_RANDOM：6s 线性爆发结束，停止 2s 后再切回 seq 循环 */
        s_var_substate = VAR_SUB_STOP;
        app_motor_set_direction(MOTOR_DIR_STOP,NULL);
        tal_sw_timer_start(s_motor_timer_id, RANDOM_STOP_MS, TAL_TIMER_ONCE);
    }
    else
    {
        /* VAR_SUB_STOP：2s 停止结束，切回 seq 循环 */
        s_var_substate = VAR_SUB_SEQ;
        s_seq_index = 0;
        const sm_simple_seq_step_t *step = &s_variable_seq[0];
        // app_motor_restore_app_duty();
        // app_motor_report_stepless_percent();
        app_motor_set_direction(step->dir,70 * MOTOR_PWM_DUTY_1);
        s_seq_index = 1;
        tal_sw_timer_start(s_motor_timer_id, step->duration_ms, TAL_TIMER_ONCE);
    }
}

VOID_T app_motor_init(VOID_T)
{
    TUYA_PWM_BASE_CFG_T pwm_cfg = {
        .polarity = TUYA_PWM_POSITIVE,
        .count_mode = TUYA_PWM_CNT_UP,
        .duty = 0,
        .cycle = 100,
        .frequency = MOTOR_PWM_FREQ_HZ,
    };

    tal_pwm_init(MOTOR_PWM_CH_FOR, &pwm_cfg);
    tal_pwm_init(MOTOR_PWM_CH_REV, &pwm_cfg);
}

VOID_T app_motor_timer_init(VOID_T)
{
    if (s_motor_timer_id != NULL) {
        return;
    }

    s_work_mode = WORK_MODE_FIXED;
    s_app_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
    s_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
#if APP_ENABLE_MOTOR_TUNING_DPS
    s_motor_duty_min_percent = MOTOR_DUTY_MIN_PERCENT;
    s_motor_duty_max_percent = MOTOR_DUTY_MAX_PERCENT;
    s_duty_boost_max = DUTY_BOOST_MAX;
#endif
    s_random_duty_active = FALSE;
    s_motor_enabled = FALSE;
    s_motor_running = FALSE;
    s_seq_index = 0;
    s_fixed_start_ms = 0;
    s_fixed_seq_started = FALSE;
    s_var_substate = VAR_SUB_SEQ;
    s_var_seq_cycle_count = 0;
    s_var_rand_seed = 0;
    s_var_burst_idx = 0;
    s_var_burst_step = 0;
    s_var_burst_dir = MOTOR_DIR_STOP;
    s_var_last_seq_dir = MOTOR_DIR_STOP;
    s_current_direction = MOTOR_DIR_STOP;
    s_current_duty = MOTOR_PWM_DUTY_0;

    tal_sw_timer_create(app_motor_timer_handler, NULL, &s_motor_timer_id);

    TAL_PR_INFO("[motor] initialized");
}

VOID_T app_motor_set_mode(work_mode_t mode)
{
    if (mode > WORK_MODE_VARIABLE)
    {
        return;
    }

    s_work_mode = mode;
    s_seq_index = 0;
    s_fixed_seq_started = FALSE;
    s_fixed_start_ms = tal_system_get_millisecond();
    s_var_substate = VAR_SUB_SEQ;
    s_var_seq_cycle_count = 0;
    s_var_burst_step = 0;
    s_var_burst_dir = MOTOR_DIR_STOP;
    s_var_last_seq_dir = MOTOR_DIR_STOP;
    app_motor_restore_app_duty();
    s_var_rand_seed = (s_var_rand_seed * 13U + 7U) & 0xFFU;
    s_var_burst_idx = s_var_rand_seed % BURST_SEQ_SIZE;
    if (s_motor_enabled)
    {
        app_motor_timer_handler(s_motor_timer_id, NULL);
    }
    TAL_PR_INFO("[motor] mode=%d", mode);
}

work_mode_t app_motor_get_mode(VOID_T)
{
    return s_work_mode;
}

VOID_T app_motor_set_stepless_percent(UINT8_T percent)
{
    if (percent > 100) {
        percent = 100;
    }

    s_app_stepless_percent = percent;
    if (!s_random_duty_active) {
        s_stepless_percent = s_app_stepless_percent;
    }
    if (s_motor_enabled && !s_random_duty_active) {
        app_motor_set_direction(s_current_direction,NULL);
    }
    UINT8_T t = MOTOR_DUTY_MIN_CURRENT +
                       ((UINT32_T)s_stepless_percent *
                        (MOTOR_DUTY_MAX_CURRENT - MOTOR_DUTY_MIN_CURRENT)) / 100;
    TAL_PR_INFO("[motor] stepless percent=%d %d", s_stepless_percent, t);
}

VOID_T app_motor_refresh_duty(VOID_T)
{
    if (s_motor_enabled && s_current_direction != MOTOR_DIR_STOP) {
        app_motor_set_direction(s_current_direction, NULL);
    }
}

UINT8_T app_motor_get_stepless_percent(VOID_T)
{
    return s_stepless_percent;
}

#if APP_ENABLE_MOTOR_TUNING_DPS
BOOL_T app_motor_set_tuning_param(app_motor_tuning_param_t param, UINT8_T value)
{
    if (value > 100) {
        return FALSE;
    }

    switch (param) {
    case MOTOR_TUNING_DUTY_MIN:
        if (value > s_motor_duty_max_percent) {
            return FALSE;
        }
        s_motor_duty_min_percent = value;
        TAL_PR_INFO("[motor] min_percent %d", value);
        break;
    case MOTOR_TUNING_DUTY_MAX:
        if (value < s_motor_duty_min_percent) {
            return FALSE;
        }
        s_motor_duty_max_percent = value;
        TAL_PR_INFO("[motor] max_percent %d", value);
        break;
    case MOTOR_TUNING_DUTY_BOOST_MAX:
        s_duty_boost_max = value;
        TAL_PR_INFO("[motor] boost_max %d", value);
        break;
    default:
        return FALSE;
    }

    if (s_motor_enabled && !s_random_duty_active) {
        app_motor_set_direction(s_current_direction, NULL);
    }
    return TRUE;
}

UINT8_T app_motor_get_tuning_param(app_motor_tuning_param_t param)
{
    switch (param) {
    case MOTOR_TUNING_DUTY_MIN:
        return s_motor_duty_min_percent;
    case MOTOR_TUNING_DUTY_MAX:
        return s_motor_duty_max_percent;
    case MOTOR_TUNING_DUTY_BOOST_MAX:
        return s_duty_boost_max;
    default:
        return 0;
    }
}
#endif

VOID_T app_motor_start(VOID_T)
{
    if (s_motor_enabled)
    {
        return;
    }
    s_motor_enabled = TRUE;
    s_seq_index = 0;
    s_fixed_start_ms = tal_system_get_millisecond();
    s_fixed_seq_started = FALSE;
    s_var_substate = VAR_SUB_SEQ;
    s_var_seq_cycle_count = 0;
    s_var_burst_step = 0;
    s_var_burst_dir = MOTOR_DIR_STOP;
    s_var_last_seq_dir = MOTOR_DIR_STOP;
    app_motor_restore_app_duty();
    s_var_rand_seed = (s_var_rand_seed * 13U + 7U) & 0xFFU;
    s_var_burst_idx = s_var_rand_seed % BURST_SEQ_SIZE;
    s_current_direction = MOTOR_DIR_STOP;
    app_motor_timer_handler(s_motor_timer_id, NULL);
}

VOID_T app_motor_stop(VOID_T)
{
    if (s_motor_timer_id != NULL)
    {
        tal_sw_timer_stop(s_motor_timer_id);
    }
    s_motor_enabled = FALSE;
    app_motor_restore_app_duty();
    app_motor_set_direction(MOTOR_DIR_STOP,NULL);
}

BOOL_T app_motor_is_running(VOID_T)
{
    return s_motor_running;
}
