/**
 * @file app_motor.h
 * @brief afp pawswiff motor control.
 */

#ifndef __APP_MOTOR_H__
#define __APP_MOTOR_H__

#include "tuya_cloud_types.h"
#include "tal_pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_DIR_STOP      0
#define MOTOR_DIR_FORWARD   1
#define MOTOR_DIR_REVERSE   2

#define MOTOR_PWM_CH_FOR        MOTOR_FOR
#define MOTOR_PWM_CH_REV        MOTOR_REV
#define MOTOR_PWM_FREQ_HZ       100

/* tal_pwm_duty_set() on TLSR825x uses 0~1000000, independent of cfg.cycle. */
#define MOTOR_PWM_DUTY_0        0U
#define MOTOR_PWM_DUTY_1        10000U
#define MOTOR_PWM_DUTY_100      (MOTOR_PWM_DUTY_1 * 10000)

typedef enum {
    WORK_MODE_FIXED   = 0,
    WORK_MODE_VARIABLE = 1,
} work_mode_t;

VOID_T app_motor_init(VOID_T);
/* Call once on normal boot, after IO initialization. */
VOID_T app_motor_timer_init(VOID_T);
VOID_T app_motor_set_mode(work_mode_t mode);
work_mode_t app_motor_get_mode(VOID_T);
VOID_T app_motor_set_stepless_percent(UINT8_T percent);
UINT8_T app_motor_get_stepless_percent(VOID_T);
VOID_T app_motor_start(VOID_T);
VOID_T app_motor_stop(VOID_T);
BOOL_T app_motor_is_running(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
