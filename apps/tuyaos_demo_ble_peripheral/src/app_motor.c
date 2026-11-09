/**
 * @file app_motor.c
 * @brief 电机控制实现 — 游戏模式时序序列状态机 (PWM 调速)
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 控制逻辑（PWM H 桥）：
 *   M_INA=PWM(duty), M_INB=0       → 正转（速度由 duty 决定）
 *   M_INA=0,          M_INB=PWM(duty) → 反转
 *   M_INA=0,          M_INB=0       → 停止
 *
 * 快/慢模式占空比：
 *   快速 → 100% (MOTOR_PWM_DUTY_100)
 *   慢速 → 50%  (MOTOR_PWM_DUTY_50)
 *
 * 快/慢模式：两者时序相同，差异在旋转速度（快速 100%，慢速 50%）。
 *   序列（12步循环）：
 *   正转7.2S → 停止5.2S → 反转5.2S → 停止3S → 正转3S → 停止7.2S
 *   → 反转7.2S → 停止3S → 正转3S → 停止5.2S → 反转7.2S → 停止7.2S
 *
 * 互动模式：振动激活一次完整序列（6步后停止），占空比同快速（100%）：
 *   正转7.2S → 停止5.2S → 反转5.2S → 停止3S → 正转3S → 停止5S
 *   旋转期间振动不激活，结束后振动可再次激活。
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_pwm.h"
#include "board.h"

#include "app_motor.h"
#include "app_state.h"
/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 快/慢模式时序序列 — 12步循环 */
#define MOTOR_SEQ_FAST_STEPS    12

/** 互动模式时序序列 — 6步单次 */
#define MOTOR_SEQ_INTERACTIVE_STEPS  6

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* 当前游戏模式 */
STATIC game_mode_t  s_game_mode = GAME_MODE_FAST;

/* 电机是否使能（由电源状态控制） */
STATIC BOOL_T       s_motor_enabled = FALSE;

/* 时序序列定时器 */
STATIC TIMER_ID     s_motor_timer_id = NULL;

/* 当前序列步进索引 */
STATIC UINT8_T      s_seq_index = 0;

/* 互动模式：序列是否已完整执行一次 */
STATIC BOOL_T       s_interactive_active = FALSE;

/* 互动模式：旋转期间锁定（防止振动重复触发） */
STATIC BOOL_T       s_rotation_locked = FALSE;

/***********************************************************************
 ********************* motor sequence tables ***************************
 **********************************************************************/

/**
 * 快/慢模式时序表（12步）
 * 注意：快和慢的时序相同，区别在于电机旋转速度。
 * 快速使用 100% 占空比，慢速使用 50% 占空比。
 */
STATIC const motor_step_t g_motor_seq_fast[MOTOR_SEQ_FAST_STEPS] = {
    {MOTOR_DIR_FORWARD, 7200},
    {MOTOR_DIR_STOP,    5200},
    {MOTOR_DIR_REVERSE, 5200},
    {MOTOR_DIR_STOP,    3000},
    {MOTOR_DIR_FORWARD, 3000},
    {MOTOR_DIR_STOP,    7200},
    {MOTOR_DIR_REVERSE, 7200},
    {MOTOR_DIR_STOP,    3000},
    {MOTOR_DIR_FORWARD, 3000},
    {MOTOR_DIR_STOP,    5200},
    {MOTOR_DIR_REVERSE, 7200},
    {MOTOR_DIR_STOP,    7200},
};

/**
 * 互动模式时序表（6步单次）— 最后停止5S，不同于快/慢的7.2S
 */
STATIC const motor_step_t g_motor_seq_interactive[MOTOR_SEQ_INTERACTIVE_STEPS] = {
    {MOTOR_DIR_FORWARD, 7200},
    {MOTOR_DIR_STOP,    5200},
    {MOTOR_DIR_REVERSE, 5200},
    {MOTOR_DIR_STOP,    3000},
    {MOTOR_DIR_FORWARD, 3000},
    {MOTOR_DIR_STOP,    5000},  /* 最后停止 5S */
};

/***********************************************************************
 ********************* static function prototypes **********************
 **********************************************************************/

STATIC UINT32_T app_motor_get_duty(VOID_T);
STATIC VOID_T app_motor_set_direction(UINT8_T direction);
STATIC VOID_T app_motor_seq_timeout_handler(TIMER_ID timer_id, VOID_T *arg);
STATIC VOID_T app_motor_seq_start(VOID_T);
STATIC VOID_T app_motor_seq_stop(VOID_T);

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 根据当前游戏模式获取 PWM 占空比
 * @return 占空比微百分比 (0~1000000)，供 tal_pwm_duty_set 使用
 */
STATIC UINT32_T app_motor_get_duty(VOID_T)
{
    switch (s_game_mode) {
        case GAME_MODE_FAST:
            return MOTOR_PWM_DUTY_100;
        case GAME_MODE_SLOW:
            return MOTOR_PWM_DUTY_75;
        case GAME_MODE_INTERACTIVE:
            /* 互动模式也用快速 100% 占空比 */
            return MOTOR_PWM_DUTY_100;
        default:
            return MOTOR_PWM_DUTY_0;
    }
}

/**
 * @brief 设置电机旋转方向（PWM 占空比控制 H 桥）
 * @param[in] direction MOTOR_DIR_STOP/FORWARD/REVERSE
 *
 * 根据当前游戏模式选择 PWM 占空比：
 *   快速 → 100%，慢速 → 50%
 */
STATIC VOID_T app_motor_set_direction(UINT8_T direction)
{
    UINT32_T duty = app_motor_get_duty();

    switch (direction) {
        case MOTOR_DIR_FORWARD:
            /* M_INA=PWM(duty), M_INB=0 → 正转 */
            tal_pwm_duty_set(MOTOR_PWM_CH_INA, duty);
            tal_pwm_duty_set(MOTOR_PWM_CH_INB, MOTOR_PWM_DUTY_0);
            TAL_PR_DEBUG("[motor] -> FORWARD, duty=%lu", duty);
            break;

        case MOTOR_DIR_REVERSE:
            /* M_INA=0, M_INB=PWM(duty) → 反转 */
            tal_pwm_duty_set(MOTOR_PWM_CH_INA, MOTOR_PWM_DUTY_0);
            tal_pwm_duty_set(MOTOR_PWM_CH_INB, duty);
            TAL_PR_DEBUG("[motor] -> REVERSE, duty=%lu", duty);
            break;

        case MOTOR_DIR_STOP:
        default:
            /* M_INA=0, M_INB=0 → 停止 */
            tal_pwm_duty_set(MOTOR_PWM_CH_INA, MOTOR_PWM_DUTY_0);
            tal_pwm_duty_set(MOTOR_PWM_CH_INB, MOTOR_PWM_DUTY_0);
            TAL_PR_DEBUG("[motor] -> STOP");
            break;
    }
}

/**
 * @brief 时序定时器回调 — 执行下一步
 *
 * 每步执行：
 *   1. 获取当前步的方向和持续时间
 *   2. 设置方向（自动选择对应模式的 PWM 占空比）
 *   3. 前进到下一步
 *   4. 重启定时器为本步持续时间
 */
STATIC VOID_T app_motor_seq_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    UINT16_T duration_ms;
    UINT8_T  direction;
    UINT8_T  total_steps;

    /* 互动模式使用自己的时序表 */
    if (s_game_mode == GAME_MODE_INTERACTIVE) {
        total_steps = MOTOR_SEQ_INTERACTIVE_STEPS;
    } else {
        total_steps = MOTOR_SEQ_FAST_STEPS;
    }

    /* 获取当前步 */
    if (s_game_mode == GAME_MODE_INTERACTIVE) {
        direction   = g_motor_seq_interactive[s_seq_index].direction;
        duration_ms = g_motor_seq_interactive[s_seq_index].duration_ms;
    } else {
        direction   = g_motor_seq_fast[s_seq_index].direction;
        duration_ms = g_motor_seq_fast[s_seq_index].duration_ms;
    }

    /* 更新旋转锁定状态（互动模式用） */
    if (s_game_mode == GAME_MODE_INTERACTIVE) {
        s_rotation_locked = (direction != MOTOR_DIR_STOP);
    }

    /* 设置方向（PWM 占空比由当前游戏模式决定） */
    app_motor_set_direction(direction);

    /* 前进到下一步 */
    s_seq_index++;

    /* 判断是否循环或停止 */
    if (s_seq_index >= total_steps) {
        if (s_game_mode == GAME_MODE_INTERACTIVE) {
            /* 互动模式：一次序列完成，停止 */
            s_interactive_active = FALSE;
            s_rotation_locked = FALSE;
            TAL_PR_INFO("[motor] interactive sequence complete");
            return;
        }
        /* 快/慢模式：从头循环 */
        s_seq_index = 0;
    }

    /* 重启定时器为本步持续时间 */
    tal_sw_timer_start(s_motor_timer_id, duration_ms, TAL_TIMER_ONCE);
}

/**
 * @brief 启动时序序列（从第一步开始）
 */
STATIC VOID_T app_motor_seq_start(VOID_T)
{
    UINT16_T first_duration;
    UINT8_T  first_dir;

    if (s_motor_timer_id == NULL) {
        TAL_PR_ERR("[motor] timer not initialized");
        return;
    }

    s_seq_index = 0;

    /* 从时序表第一步开始 */
    if (s_game_mode == GAME_MODE_INTERACTIVE) {
        first_dir       = g_motor_seq_interactive[0].direction;
        first_duration  = g_motor_seq_interactive[0].duration_ms;
        s_interactive_active = TRUE;
        s_rotation_locked = TRUE;
    } else {
        first_dir       = g_motor_seq_fast[0].direction;
        first_duration  = g_motor_seq_fast[0].duration_ms;
    }

    app_motor_set_direction(first_dir);
    s_seq_index = 1;

    TAL_PR_INFO("[motor] sequence started, mode=%d", s_game_mode);

    /* 启动第一步的定时器 */
    tal_sw_timer_start(s_motor_timer_id, first_duration, TAL_TIMER_ONCE);
}

/**
 * @brief 停止时序序列
 */
STATIC VOID_T app_motor_seq_stop(VOID_T)
{
    /* 停止定时器 */
    if (s_motor_timer_id != NULL) {
        tal_sw_timer_stop(s_motor_timer_id);
    }

    /* 电机停止（PWM 占空比置 0） */
    app_motor_set_direction(MOTOR_DIR_STOP);

    /* 清除状态 */
    s_seq_index = 0;
    s_interactive_active = FALSE;
    s_rotation_locked = FALSE;

    TAL_PR_INFO("[motor] sequence stopped");
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

VOID_T app_motor_init(VOID_T)
{
    OPERATE_RET ret;

    /* 初始化 PWM 通道：M_INA (PB4 → PWM4) */
    TUYA_PWM_BASE_CFG_T pwm_cfg = {
        .polarity   = TUYA_PWM_POSITIVE,
        .count_mode = TUYA_PWM_CNT_UP,
        .duty       = 0,              /* 初始占空比 0%（电机停止） */
        .cycle      = 100,            /* 周期基数 100（0~100% 映射） */
        .frequency  = MOTOR_PWM_FREQ_HZ,
    };

    ret = tal_pwm_init(MOTOR_PWM_CH_INA, &pwm_cfg);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[motor] PWM init failed for ch INA, ret=%d", ret);
    }

    ret = tal_pwm_init(MOTOR_PWM_CH_INB, &pwm_cfg);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[motor] PWM init failed for ch INB, ret=%d", ret);
    }

    /* 创建时序定时器（不自动启动） */
    tal_sw_timer_create(app_motor_seq_timeout_handler, NULL, &s_motor_timer_id);

    s_game_mode      = GAME_MODE_FAST;
    s_motor_enabled  = FALSE;
    s_seq_index      = 0;
    s_interactive_active = FALSE;
    s_rotation_locked    = FALSE;

    TAL_PR_INFO("[motor] initialized (PWM mode, freq=%dHz)", MOTOR_PWM_FREQ_HZ);
}

VOID_T app_motor_set_mode(game_mode_t mode)
{
    if (mode > GAME_MODE_INTERACTIVE) {
        TAL_PR_ERR("[motor] invalid mode: %d", mode);
        return;
    }

    /* 如果正在运行，先停止 */
    if (s_motor_enabled) {
        app_motor_seq_stop();
    }

    s_game_mode = mode;

    // 模式设置完后，如果当前为开机状态，则自动启动新模式的序列
    // 交互模式由检测到震动触发开关
    if (app_state_is_powered_on() && s_game_mode != GAME_MODE_INTERACTIVE) {
        app_motor_seq_start();
    }
    TAL_PR_INFO("[motor] mode set to %d", mode);
}

game_mode_t app_motor_get_mode(VOID_T)
{
    return s_game_mode;
}

VOID_T app_motor_start(VOID_T)
{
    if (!s_motor_enabled) {
        s_motor_enabled = TRUE;
        app_motor_seq_start();
        TAL_PR_INFO("[motor] started");
    }
}

VOID_T app_motor_stop(VOID_T)
{
    if (s_motor_enabled) {
        app_motor_seq_stop();
        s_motor_enabled = FALSE;
        TAL_PR_INFO("[motor] stopped");
    }
}

VOID_T app_motor_interactive_trigger(VOID_T)
{
    /* 仅互动模式下 && 已使能 && 未激活 && 不在旋转锁定期间 才触发 */
    if (s_game_mode != GAME_MODE_INTERACTIVE) {
        return;
    }
    if (!s_motor_enabled) {
        return;
    }
    if (s_interactive_active) {
        TAL_PR_DEBUG("[motor] interactive already active, ignore");
        return;
    }
    if (s_rotation_locked) {
        TAL_PR_DEBUG("[motor] rotation locked, cannot trigger");
        return;
    }

    TAL_PR_INFO("[motor] interactive triggered by vibration");
    app_motor_seq_start();
}

BOOL_T app_motor_is_running(VOID_T)
{
    /* 电机正在旋转：当前方向非 STOP 且已使能 */
    if (!s_motor_enabled) {
        return FALSE;
    }
    /* 若互动模式未激活，不算运行 */
    if (s_game_mode == GAME_MODE_INTERACTIVE && !s_interactive_active) {
        return FALSE;
    }
    /* 检查是否有定时器在运行 */
    return (s_seq_index > 0);
}
