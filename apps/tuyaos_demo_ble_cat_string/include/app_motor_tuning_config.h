/**
 * @file app_motor_tuning_config.h
 * @brief Temporary motor calibration DPs, disabled for production builds.
 */

#ifndef __APP_MOTOR_TUNING_CONFIG_H__
#define __APP_MOTOR_TUNING_CONFIG_H__

#ifndef APP_ENABLE_MOTOR_TUNING_DPS
#define APP_ENABLE_MOTOR_TUNING_DPS 1
#endif

#if APP_ENABLE_MOTOR_TUNING_DPS
#define DP_ID_DEBUG_MOTOR_DUTY_MIN   12 /* value rw: 0~100 */
#define DP_ID_DEBUG_MOTOR_DUTY_MAX   102 /* value rw: 0~100 */
#define DP_ID_DEBUG_DUTY_BOOST_MAX  103 /* value rw: 0~100 */
#endif

#endif
