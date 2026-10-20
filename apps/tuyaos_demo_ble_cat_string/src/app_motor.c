/**
 * @file app_motor.c
 * @brief afp pawswiff motor control.
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_pwm.h"

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
#define MOTOR_DUTY_MIN_PERCENT 30
#define MOTOR_DUTY_MAX_PERCENT 50
#define MOTOR_DUTY_DEFAULT_PERCENT 40
#define MOTOR_STEPLESS_DEFAULT_PERCENT \
    (((MOTOR_DUTY_DEFAULT_PERCENT - MOTOR_DUTY_MIN_PERCENT) * 100) / \
     (MOTOR_DUTY_MAX_PERCENT - MOTOR_DUTY_MIN_PERCENT))

/* 电池电压补偿参数（mV） */
#define BATTERY_VOLTAGE_MIN     3500
#define BATTERY_VOLTAGE_MAX     4200
#define DUTY_BOOST_MAX          20      /* N 最大增加值 */

STATIC work_mode_t s_work_mode = WORK_MODE_FIXED;
STATIC UINT8_T s_app_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
STATIC UINT8_T s_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
STATIC BOOL_T s_random_duty_active = FALSE;
STATIC BOOL_T s_motor_enabled = FALSE;
STATIC BOOL_T s_motor_running = FALSE;
STATIC TIMER_ID s_motor_timer_id = NULL;
STATIC UINT8_T s_seq_index = 0;
STATIC UINT8_T s_var_seq_cycle_count = 0;
#define BURST_SEQ_SIZE 20

STATIC UINT8_T s_var_rand_seed = 0;
STATIC UINT8_T s_var_burst_idx = 0;
STATIC UINT8_T s_var_burst_step = 0;
STATIC UINT8_T s_var_burst_dir = MOTOR_DIR_STOP;
STATIC UINT8_T s_current_direction = MOTOR_DIR_STOP;

typedef enum
{
    VAR_SUB_SEQ,
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

#define FIX_STOP 500
STATIC const sm_simple_seq_step_t s_fixed_seq[] = {
    {MOTOR_DIR_FORWARD, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 2500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1500},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_FORWARD, 1000},
    {MOTOR_DIR_STOP, FIX_STOP},
    {MOTOR_DIR_REVERSE, 1000},
    {MOTOR_DIR_STOP, 3000},
};

STATIC const sm_simple_seq_step_t s_variable_seq[VARIABLE_SEQ_STEPS] = {
    {MOTOR_DIR_FORWARD, 250},
    {MOTOR_DIR_STOP, 100},
    {MOTOR_DIR_REVERSE, 250},
    {MOTOR_DIR_STOP, 100},
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

STATIC UINT32_T app_motor_duty_for_stepless(UINT8_T percent)
{
    UINT32_T base_percent;
    UINT32_T duty_percent;
    INT32_T voltage;
    UINT32_T range;
    UINT32_T drop;
    UINT32_T boost;

    if (percent > 100) {
        percent = 100;
    }

    if (s_random_duty_active) {
        if (percent < MOTOR_DUTY_MIN_PERCENT) {
            percent = MOTOR_DUTY_MIN_PERCENT;
        }
        if (percent > MOTOR_DUTY_MAX_PERCENT) {
            percent = MOTOR_DUTY_MAX_PERCENT;
        }
        base_percent = percent;
    } else {
        base_percent = MOTOR_DUTY_MIN_PERCENT +
                       ((UINT32_T)percent * (MOTOR_DUTY_MAX_PERCENT - MOTOR_DUTY_MIN_PERCENT)) / 100;
    }

    /* USB inserted or charge done: use the mapped duty without voltage compensation. */
    if (app_charge_is_detected() || app_charge_is_full())
    {
        return MOTOR_PWM_DUTY_1 * base_percent;
    }

    /* 电池供电：电压越低占空比越高（线性反比例补偿） */
    voltage = app_battery_get_voltage();
    if (voltage < (INT32_T)BATTERY_VOLTAGE_MIN)
    {
        voltage = (INT32_T)BATTERY_VOLTAGE_MIN;
    }
    if (voltage > (INT32_T)BATTERY_VOLTAGE_MAX)
    {
        voltage = (INT32_T)BATTERY_VOLTAGE_MAX;
    }

    /* 补偿量：4200mV→0, 3300mV→DUTY_BOOST_MAX（线性插值） */
    range = BATTERY_VOLTAGE_MAX - BATTERY_VOLTAGE_MIN;
    drop = (UINT32_T)(BATTERY_VOLTAGE_MAX - (UINT32_T)voltage);
    boost = (drop * DUTY_BOOST_MAX) / range;

    /* Mapped base percent plus low-voltage boost, capped at 100%. */
    duty_percent = base_percent + boost;
    if (duty_percent > 100) {
        duty_percent = 100;
    }

    return MOTOR_PWM_DUTY_1 * duty_percent;
}

STATIC VOID_T app_motor_set_direction(UINT8_T direction, UINT32_T duty)
{
    if (duty == NULL)
    {
        duty = app_motor_duty_for_stepless(s_stepless_percent);
    }

    s_current_direction = direction;

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
    UINT16_T min = MOTOR_DUTY_MIN_PERCENT + 10;
    UINT32_T range = MOTOR_DUTY_MAX_PERCENT - min;

    if (s_var_burst_step >= RANDOM_BURST_RAMP_STEPS - 1U) {
        return MOTOR_DUTY_MAX_PERCENT;
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
        /* 固定模式：按 s_fixed_seq 表逐歩运动，占空比使用 DP30 映射值 */
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
                app_motor_start_random_burst_step();
                s_var_burst_idx = (s_var_burst_idx + 1U) % BURST_SEQ_SIZE;
                tal_sw_timer_start(s_motor_timer_id, RANDOM_BURST_STEP_MS, TAL_TIMER_ONCE);
                return;
            }
        }
        tal_sw_timer_start(s_motor_timer_id, step->duration_ms, TAL_TIMER_ONCE);
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
    tal_sw_timer_create(app_motor_timer_handler, NULL, &s_motor_timer_id);

    s_work_mode = WORK_MODE_FIXED;
    s_app_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
    s_stepless_percent = MOTOR_STEPLESS_DEFAULT_PERCENT;
    s_random_duty_active = FALSE;
    s_motor_enabled = FALSE;
    s_motor_running = FALSE;
    s_seq_index = 0;
    s_var_substate = VAR_SUB_SEQ;
    s_var_seq_cycle_count = 0;
    s_var_rand_seed = 0;
    s_var_burst_idx = 0;
    s_var_burst_step = 0;
    s_var_burst_dir = MOTOR_DIR_STOP;
    s_current_direction = MOTOR_DIR_STOP;

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
    s_var_substate = VAR_SUB_SEQ;
    s_var_seq_cycle_count = 0;
    s_var_burst_step = 0;
    s_var_burst_dir = MOTOR_DIR_STOP;
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
    TAL_PR_INFO("[motor] stepless percent=%d", s_stepless_percent);
}

UINT8_T app_motor_get_stepless_percent(VOID_T)
{
    return s_stepless_percent;
}

VOID_T app_motor_start(VOID_T)
{
    if (s_motor_enabled)
    {
        return;
    }
    s_motor_enabled = TRUE;
    s_seq_index = 0;
    s_var_substate = VAR_SUB_SEQ;
    s_var_seq_cycle_count = 0;
    s_var_burst_step = 0;
    s_var_burst_dir = MOTOR_DIR_STOP;
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
