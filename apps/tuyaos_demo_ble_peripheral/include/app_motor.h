/**
 * @file app_motor.h
 * @brief 电机控制模块 — 游戏模式时序序列（快/慢/互动）
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __APP_MOTOR_H__
#define __APP_MOTOR_H__

#include "tuya_cloud_types.h"
#include "tal_pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/

/** 电机方向 */
#define MOTOR_DIR_STOP      0   /**< 停止 */
#define MOTOR_DIR_FORWARD   1   /**< 正转 */
#define MOTOR_DIR_REVERSE   2   /**< 反转 */

/** PWM 通道映射：M_INA=PB4→PWM4, M_INB=PB5→PWM5 */
#define MOTOR_PWM_CH_INA        TUYA_PWM_NUM_4
#define MOTOR_PWM_CH_INB        TUYA_PWM_NUM_5

/** PWM 频率 (1kHz，适用于直流电机 H 桥) */
#define MOTOR_PWM_FREQ_HZ       1000

/**
 * PWM 占空比 (tal_pwm_duty_set 使用 0~1000000 微百分比)
 * 100% = 1000000,  50% = 500000
 */
#define MOTOR_PWM_DUTY_100      1000000U
#define MOTOR_PWM_DUTY_75       750000U
#define MOTOR_PWM_DUTY_50       500000U
#define MOTOR_PWM_DUTY_0        0U

/** 游戏模式枚举 — 与 DP_ID_MODE 枚举值一致 */
typedef enum {
    GAME_MODE_FAST        = 0,  /**< 快模式：高速旋转 + 时序序列 */
    GAME_MODE_SLOW        = 1,  /**< 慢模式：低速旋转 + 时序序列 */
    GAME_MODE_INTERACTIVE = 2,  /**< 互动模式：振动激活一次序列 */
} game_mode_t;

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/

/** 电机时序步进 */
#pragma pack(1)
typedef struct {
    UINT8_T  direction;      /**< 方向: MOTOR_DIR_STOP/FORWARD/REVERSE */
    UINT16_T duration_ms;    /**< 持续时间 ms */
} motor_step_t;
#pragma pack()

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief 初始化电机模块（GPIO、定时器）
 */
VOID_T app_motor_init(VOID_T);

/**
 * @brief 设置游戏模式
 * @param[in] mode 游戏模式 @ref game_mode_t
 */
VOID_T app_motor_set_mode(game_mode_t mode);

/**
 * @brief 获取当前游戏模式
 * @return game_mode_t
 */
game_mode_t app_motor_get_mode(VOID_T);

/**
 * @brief 启动电机时序序列
 */
VOID_T app_motor_start(VOID_T);

/**
 * @brief 停止电机并复位时序
 */
VOID_T app_motor_stop(VOID_T);

/**
 * @brief 互动模式：振动触发一次序列（仅在停止期间有效）
 */
VOID_T app_motor_interactive_trigger(VOID_T);

/**
 * @brief 查询电机是否正在旋转中（非停止状态）
 * @return TRUE=正在旋转, FALSE=停止
 */
BOOL_T app_motor_is_running(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MOTOR_H__ */
