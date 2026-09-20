/**
 * @file app_motor.h
 * @brief Laser bug toy motor and game-mode control.
 */

#ifndef __APP_MOTOR_H__
#define __APP_MOTOR_H__

#include "tuya_cloud_types.h"
#include "tal_pwm.h"

#ifndef APP_FACTORY_TEST
#define APP_FACTORY_TEST 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_DIR_STOP      0
#define MOTOR_DIR_FORWARD   1
#define MOTOR_DIR_REVERSE   2

#define MOTOR_PWM_FREQ_HZ   25000
#define MOTOR_PWM_FREQ_HZ_M3 100

/* tal_pwm_duty_set() on TLSR825x uses 0~1000000, independent of cfg.cycle. */
#define MOTOR_PWM_DUTY_0    0U
#define MOTOR_PWM_DUTY_1    10000U

typedef enum {
    GAME_MODE_LASER_CHASE = 0,
    GAME_MODE_BUG_HUNT    = 1,
    GAME_MODE_ALTERNATING = 2,
    GAME_MODE_SLEEP       = 0xFF,
} game_mode_t;

VOID_T app_motor_init(VOID_T);
VOID_T app_motor_set_mode(game_mode_t mode);
game_mode_t app_motor_get_mode(VOID_T);
game_mode_t app_motor_get_report_mode(VOID_T);
VOID_T app_motor_enter_sleep_mode(VOID_T);
VOID_T app_motor_wake_last_mode(VOID_T);
VOID_T app_motor_set_stepless_percent(UINT8_T percent);
UINT8_T app_motor_get_stepless_percent(VOID_T);
VOID_T app_motor_set_alt_laser_time(UINT16_T seconds);
UINT16_T app_motor_get_alt_laser_time(VOID_T);
VOID_T app_motor_set_alt_bug_time(UINT16_T seconds);
UINT16_T app_motor_get_alt_bug_time(VOID_T);
UINT32_T app_motor_get_mode_timeout_ms(VOID_T);
VOID_T app_motor_start(VOID_T);
VOID_T app_motor_stop(VOID_T);
BOOL_T app_motor_is_running(VOID_T);

#if (APP_FACTORY_TEST == 1)
BOOL_T app_motor_factory_test_toggle(VOID_T);
#endif

#ifdef __cplusplus
}
#endif

#endif
