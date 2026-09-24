#ifndef FAQIUJI_LAUNCHER_H
#define FAQIUJI_LAUNCHER_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_LAUNCH_DISTANCE_MIN          3U
#define FAQIUJI_LAUNCH_DISTANCE_MAX          20U

typedef enum
{
  FAQIUJI_LAUNCH_MODE_10M = 0U,
  FAQIUJI_LAUNCH_MODE_20M = 1U,
  FAQIUJI_LAUNCH_MODE_RANDOM = 2U,
} FAQIUJI_LAUNCH_MODE_E;

/* These values are intentionally easy to tune after measuring the launcher. */
#define FAQIUJI_PWM_DUTY_10M                 55U
#define FAQIUJI_PWM_DUTY_20M                 75U
#define FAQIUJI_PWM_DUTY_RANDOM               65U

typedef enum
{
  FAQIUJI_LAUNCH_IDLE = 0U,
  FAQIUJI_LAUNCH_MOTOR_BEFORE_DCT,
  FAQIUJI_LAUNCH_DCT_ACTIVE,
  FAQIUJI_LAUNCH_MOTOR_AFTER_DCT,
} FAQIUJI_LAUNCH_STATE_E;

HAL_StatusTypeDef faqiuji_launcher_init(void);
uint8_t faqiuji_launcher_start(FAQIUJI_LAUNCH_MODE_E mode);
void faqiuji_launcher_stop(void);
void faqiuji_launcher_task(void *argument);
uint8_t faqiuji_launcher_is_running(void);
FAQIUJI_LAUNCH_STATE_E faqiuji_launcher_state(void);

#ifdef __cplusplus
}
#endif

#endif
