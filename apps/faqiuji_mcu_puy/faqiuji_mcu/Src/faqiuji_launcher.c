#include "faqiuji_launcher.h"
#include "gpio_config.h"
#include "FreeRTOS.h"
#include "task.h"

#define FAQIUJI_PWM_FREQUENCY_HZ             4500UL
#define FAQIUJI_MOTOR_BEFORE_DCT_MS          3700U
#define FAQIUJI_DCT_ACTIVE_MS                 800U
#define FAQIUJI_MOTOR_AFTER_DCT_MS           2000U

static TIM_HandleTypeDef sg_tim;
static volatile uint8_t sg_start_request;
static volatile uint8_t sg_stop_request;
static volatile FAQIUJI_LAUNCH_MODE_E sg_mode;
static volatile uint8_t sg_distance_m;
static volatile FAQIUJI_LAUNCH_STATE_E sg_state;

static uint8_t faqiuji_launcher_random_distance(void)
{
  static uint32_t seed = 0x13579BDFU;

  seed = seed * 1664525U + 1013904223U;
  return (uint8_t)(FAQIUJI_LAUNCH_DISTANCE_MIN +
                   (seed % (FAQIUJI_LAUNCH_DISTANCE_MAX -
                            FAQIUJI_LAUNCH_DISTANCE_MIN + 1U)));
}

static uint8_t faqiuji_launcher_resolve_distance(FAQIUJI_LAUNCH_MODE_E mode)
{
  if (mode == FAQIUJI_LAUNCH_MODE_10M) {
    return 10U;
  }
  if (mode == FAQIUJI_LAUNCH_MODE_20M) {
    return 20U;
  }
  return faqiuji_launcher_random_distance();
}

static uint32_t faqiuji_launcher_duty(FAQIUJI_LAUNCH_MODE_E mode)
{
  if (mode == FAQIUJI_LAUNCH_MODE_10M) {
    return FAQIUJI_PWM_DUTY_10M;
  }
  if (mode == FAQIUJI_LAUNCH_MODE_20M) {
    return FAQIUJI_PWM_DUTY_20M;
  }
  return FAQIUJI_PWM_DUTY_RANDOM;
}

static uint32_t faqiuji_launcher_period(void)
{
  uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
  return (timer_clock / FAQIUJI_PWM_FREQUENCY_HZ) - 1U;
}

static void faqiuji_launcher_motor_start(FAQIUJI_LAUNCH_MODE_E mode)
{
  uint32_t period = faqiuji_launcher_period();
  uint32_t duty = faqiuji_launcher_duty(mode);

  /* Resolve the requested distance locally on the PUYA controller. */
  sg_distance_m = faqiuji_launcher_resolve_distance(mode);
  if (duty > 100U) {
    duty = 100U;
  }
  __HAL_TIM_SET_AUTORELOAD(&sg_tim, period);
  __HAL_TIM_SET_COMPARE(&sg_tim, TIM_CHANNEL_3,
                        (period + 1U) * duty / 100U);
  (void)HAL_TIM_PWM_Start(&sg_tim, TIM_CHANNEL_3);
}

static void faqiuji_launcher_motor_stop(void)
{
  (void)HAL_TIM_PWM_Stop(&sg_tim, TIM_CHANNEL_3);
  __HAL_TIM_SET_COMPARE(&sg_tim, TIM_CHANNEL_3, 0U);
  HAL_GPIO_WritePin(GPIOA, MOTOR_PWM, GPIO_PIN_RESET);
}

HAL_StatusTypeDef faqiuji_launcher_init(void)
{
  TIM_OC_InitTypeDef config = {0};

  sg_tim.Instance = TIM2;
  sg_tim.Init.Prescaler = 0U;
  sg_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  sg_tim.Init.Period = faqiuji_launcher_period();
  sg_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  sg_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  if (HAL_TIM_PWM_Init(&sg_tim) != HAL_OK) {
    return HAL_ERROR;
  }

  config.OCMode = TIM_OCMODE_PWM1;
  config.Pulse = 0U;
  config.OCPolarity = TIM_OCPOLARITY_HIGH;
  config.OCFastMode = TIM_OCFAST_DISABLE;
  config.OCIdleState = TIM_OCIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&sg_tim, &config, TIM_CHANNEL_3) != HAL_OK) {
    return HAL_ERROR;
  }

  faqiuji_launcher_motor_stop();
  HAL_GPIO_WritePin(GPIOC, DCT_CON, GPIO_PIN_RESET);
  sg_state = FAQIUJI_LAUNCH_IDLE;
  return HAL_OK;
}

uint8_t faqiuji_launcher_start(FAQIUJI_LAUNCH_MODE_E mode)
{
  if (mode > FAQIUJI_LAUNCH_MODE_RANDOM) {
    return 0U;
  }
  taskENTER_CRITICAL();
  if (sg_state != FAQIUJI_LAUNCH_IDLE || sg_start_request != 0U) {
    taskEXIT_CRITICAL();
    return 0U;
  }
  sg_mode = mode;
  sg_stop_request = 0U;
  sg_start_request = 1U;
  taskEXIT_CRITICAL();
  return 1U;
}

void faqiuji_launcher_stop(void)
{
  sg_stop_request = 1U;
  sg_start_request = 0U;
  faqiuji_launcher_motor_stop();
  HAL_GPIO_WritePin(GPIOC, DCT_CON, GPIO_PIN_RESET);
  sg_state = FAQIUJI_LAUNCH_IDLE;
}

uint8_t faqiuji_launcher_is_running(void)
{
  return (sg_state != FAQIUJI_LAUNCH_IDLE || sg_start_request != 0U) ? 1U : 0U;
}

FAQIUJI_LAUNCH_STATE_E faqiuji_launcher_state(void)
{
  return sg_state;
}

void faqiuji_launcher_task(void *argument)
{
  (void)argument;

  for (;;) {
    if (sg_start_request != 0U) {
      FAQIUJI_LAUNCH_MODE_E mode = sg_mode;
      sg_start_request = 0U;
      sg_stop_request = 0U;

      sg_state = FAQIUJI_LAUNCH_MOTOR_BEFORE_DCT;
      faqiuji_launcher_motor_start(mode);
      vTaskDelay(pdMS_TO_TICKS(FAQIUJI_MOTOR_BEFORE_DCT_MS));
      if (sg_stop_request != 0U) {
        faqiuji_launcher_stop();
        continue;
      }

      sg_state = FAQIUJI_LAUNCH_DCT_ACTIVE;
      HAL_GPIO_WritePin(GPIOC, DCT_CON, GPIO_PIN_SET);
      vTaskDelay(pdMS_TO_TICKS(FAQIUJI_DCT_ACTIVE_MS));
      HAL_GPIO_WritePin(GPIOC, DCT_CON, GPIO_PIN_RESET);
      if (sg_stop_request != 0U) {
        faqiuji_launcher_stop();
        continue;
      }

      sg_state = FAQIUJI_LAUNCH_MOTOR_AFTER_DCT;
      vTaskDelay(pdMS_TO_TICKS(FAQIUJI_MOTOR_AFTER_DCT_MS));
      faqiuji_launcher_stop();
    } else {
      vTaskDelay(pdMS_TO_TICKS(10U));
    }
  }
}
