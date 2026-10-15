#ifndef FAQIUJI_LAUNCHER_H
#define FAQIUJI_LAUNCHER_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The old fixed 10 m / 20 m / random duty values are no longer used. */
/* #define FAQIUJI_PWM_DUTY_10M              35U */
/* #define FAQIUJI_PWM_DUTY_20M              55U */
/* #define FAQIUJI_PWM_DUTY_RANDOM           75U */

typedef enum
{
  FAQIUJI_LAUNCH_IDLE = 0U,
  FAQIUJI_LAUNCH_MOTOR_BEFORE_DCT,
  FAQIUJI_LAUNCH_DCT_ACTIVE,
  FAQIUJI_LAUNCH_MOTOR_AFTER_DCT,
} FAQIUJI_LAUNCH_STATE_E;

HAL_StatusTypeDef faqiuji_launcher_init(void);
uint8_t faqiuji_launcher_start(void);
void faqiuji_launcher_set_motor_speed(uint8_t motor_speed);
void faqiuji_launcher_stop(void);
void faqiuji_launcher_task(void *argument);
uint8_t faqiuji_launcher_is_running(void);
FAQIUJI_LAUNCH_STATE_E faqiuji_launcher_state(void);

#ifdef __cplusplus
}
#endif

#endif
