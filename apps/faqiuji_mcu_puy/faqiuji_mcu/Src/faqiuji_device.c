#include "faqiuji_device.h"
#include "faqiuji_protocol.h"
#include "faqiuji_launcher.h"
#include "gpio_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "py32f040_hal_adc.h"

#define FAQIUJI_ADC_VDD_MV                 3300U
#define FAQIUJI_ADC_FULL_SCALE             4095U
#define FAQIUJI_BATTERY_DIVIDER_RATIO      2U
#define FAQIUJI_BATTERY_LOW_PERCENT        20U
#define FAQIUJI_BATTERY_FULL_PERCENT       90U
#define FAQIUJI_NTC_OVERHEAT_RAW           3800U
#define FAQIUJI_NTC_RECOVER_RAW            3500U

static ADC_HandleTypeDef sg_adc;
static TIM_HandleTypeDef sg_ie_tim;
static volatile uint8_t sg_mode;
static volatile uint8_t sg_enabled = 1U;
static volatile uint8_t sg_ball_last;
static volatile uint8_t sg_usb_last;
static volatile uint8_t sg_charge_last;
static volatile uint16_t sg_max_count = 20U;
static volatile uint16_t sg_count;
static volatile uint16_t sg_standby_min = 20U;
static volatile uint8_t sg_charge_blocked;
static volatile uint16_t sg_battery_percent;
static volatile uint8_t sg_charging;
static volatile uint8_t sg_ball_trigger;
static volatile uint8_t sg_radar_last;
static volatile uint8_t sg_work_state;
static volatile uint32_t sg_standby_until;
static volatile uint32_t sg_led_tick;
static volatile uint8_t sg_led_phase;
static volatile uint32_t sg_ie_tick;
static volatile uint8_t sg_ie_enabled;

static uint16_t faqiuji_adc_read(uint32_t channel)
{
  ADC_ChannelConfTypeDef config = {0};
  uint32_t sum = 0U;
  uint8_t i;

  config.Channel = channel;
  config.Rank = ADC_REGULAR_RANK_1;
  /* Match the PY32 ADC_TempSensor example for high-impedance sources. */
  config.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
  if (HAL_ADC_ConfigChannel(&sg_adc, &config) != HAL_OK) {
    return 0U;
  }
  for (i = 0U; i < 4U; ++i) {
    if (HAL_ADC_Start(&sg_adc) == HAL_OK &&
        HAL_ADC_PollForConversion(&sg_adc, 10U) == HAL_OK) {
      sum += HAL_ADC_GetValue(&sg_adc);
    }
    (void)HAL_ADC_Stop(&sg_adc);
  }
  return (uint16_t)(sum / 4U);
}

static uint8_t faqiuji_battery_percent(uint16_t raw)
{
  uint32_t mv = ((uint32_t)raw * FAQIUJI_ADC_VDD_MV *
                 FAQIUJI_BATTERY_DIVIDER_RATIO) /
                FAQIUJI_ADC_FULL_SCALE;

  if (mv >= 4200U) return 100U;
  if (mv <= 3250U) return 0U;
  return (uint8_t)(((mv - 3250U) * 100U) / (4200U - 3250U));
}

static void faqiuji_charge_led_task(uint8_t battery, uint8_t charging)
{
  static uint8_t blink;

  if (charging != 0U) {
    HAL_GPIO_WritePin(GPIOB, LED_R, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, LED_G, GPIO_PIN_SET);
  } else if (battery >= FAQIUJI_BATTERY_FULL_PERCENT) {
    HAL_GPIO_WritePin(GPIOB, LED_R, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, LED_G, GPIO_PIN_RESET);
  } else if (battery <= FAQIUJI_BATTERY_LOW_PERCENT) {
    blink ^= 1U;
    HAL_GPIO_WritePin(GPIOB, LED_R,
                      blink ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, LED_G, GPIO_PIN_SET);
  } else {
    HAL_GPIO_WritePin(GPIOB, LED_R, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, LED_G, GPIO_PIN_SET);
  }
}

static void faqiuji_ie_pwm_init(void)
{
  TIM_OC_InitTypeDef config = {0};

  sg_ie_tim.Instance = TIM1;
  sg_ie_tim.Init.Prescaler = 0U;
  sg_ie_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  sg_ie_tim.Init.Period = (HAL_RCC_GetPCLK1Freq() / 38000U) - 1U;
  sg_ie_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  sg_ie_tim.Init.RepetitionCounter = 0U;
  sg_ie_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&sg_ie_tim) != HAL_OK) return;

  config.OCMode = TIM_OCMODE_PWM1;
  config.Pulse = (sg_ie_tim.Init.Period + 1U) / 2U;
  config.OCPolarity = TIM_OCPOLARITY_HIGH;
  config.OCFastMode = TIM_OCFAST_DISABLE;
  config.OCIdleState = TIM_OCIDLESTATE_RESET;
  (void)HAL_TIM_PWM_ConfigChannel(&sg_ie_tim, &config, TIM_CHANNEL_1);
}

void faqiuji_device_init(void)
{
  ADC_ChannelConfTypeDef config = {0};

  __HAL_RCC_ADC_CLK_ENABLE();
  sg_adc.Instance = ADC1;
  sg_adc.Init.Resolution = ADC_RESOLUTION_12B;
  sg_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  sg_adc.Init.ScanConvMode = ADC_SCAN_DISABLE;
  sg_adc.Init.ContinuousConvMode = DISABLE;
  sg_adc.Init.NbrOfConversion = 1U;
  sg_adc.Init.DiscontinuousConvMode = DISABLE;
  sg_adc.Init.NbrOfDiscConversion = 1U;
  sg_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  if (HAL_ADC_Init(&sg_adc) != HAL_OK) {
    APP_ErrorHandler();
  }
  config.Channel = ADC_CHANNEL_7;
  config.Rank = ADC_REGULAR_RANK_1;
  config.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
  if (HAL_ADC_ConfigChannel(&sg_adc, &config) != HAL_OK) {
    APP_ErrorHandler();
  }
  (void)HAL_ADCEx_Calibration_Start(&sg_adc);

  faqiuji_ie_pwm_init();
  HAL_GPIO_WritePin(GPIOB, CHARGE_EN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, NTC_CON, GPIO_PIN_SET);
  sg_work_state = FAQIUJI_WORK_RUNNING;
  sg_radar_last = (HAL_GPIO_ReadPin(GPIOA, LEIDA_IN_O) == GPIO_PIN_SET) ? 1U : 0U;
  sg_ball_last = (HAL_GPIO_ReadPin(GPIOA, IR) == GPIO_PIN_SET) ? 1U : 0U;
  sg_usb_last = (HAL_GPIO_ReadPin(GPIOF, USB_DET) == GPIO_PIN_SET) ? 1U : 0U;
  sg_charge_last = (HAL_GPIO_ReadPin(GPIOB, CHARGE_STATE) == GPIO_PIN_SET) ? 1U : 0U;
  sg_charging = sg_charge_last;
}

void faqiuji_device_set_mode(uint8_t mode)
{
  if (mode <= FAQIUJI_LAUNCH_MODE_RANDOM) {
    sg_mode = mode;
  }
}

void faqiuji_device_set_config(uint8_t mode, uint16_t max_count,
                               uint16_t standby_min)
{
  faqiuji_device_set_mode(mode);
  sg_max_count = (max_count < 10U) ? 10U :
                 (max_count > 90U) ? 90U : max_count;
  sg_standby_min = (standby_min < 10U) ? 10U :
                   (standby_min > 30U) ? 30U : standby_min;
}

void faqiuji_device_set_enabled(uint8_t enabled)
{
  sg_enabled = enabled ? 1U : 0U;
}

uint8_t faqiuji_device_is_enabled(void)
{
  return sg_enabled;
}

static void faqiuji_device_update_distance_leds(void)
{
  GPIO_PinState low = GPIO_PIN_SET;
  GPIO_PinState middle = GPIO_PIN_SET;
  GPIO_PinState high = GPIO_PIN_SET;

  if (sg_enabled != 0U) {
    if (sg_mode == FAQIUJI_LAUNCH_MODE_10M) {
      middle = GPIO_PIN_RESET;
    } else if (sg_mode == FAQIUJI_LAUNCH_MODE_20M) {
      high = GPIO_PIN_RESET;
    } else {
      low = GPIO_PIN_RESET;
    }
  }

  if (sg_work_state == FAQIUJI_WORK_STANDBY) {
    if (sg_led_phase == 0U) {
      low = GPIO_PIN_SET;
      middle = GPIO_PIN_SET;
      high = GPIO_PIN_SET;
    }
  }
  HAL_GPIO_WritePin(GPIOA, LED_LOW, low);
  HAL_GPIO_WritePin(GPIOA, LED_MIDDLE, middle);
  HAL_GPIO_WritePin(GPIOA, LED_HIGH, high);
}

static void faqiuji_device_set_work_state(uint8_t state)
{
  uint8_t data = state;
  if (sg_work_state == state) {
    return;
  }
  sg_work_state = state;
  faqiuji_protocol_status_event(FAQIUJI_EVENT_WORK, &data, 1U);
}

void faqiuji_device_sensor_task(void *argument)
{
  uint8_t ball;
  uint8_t usb;
  uint8_t charging;
  uint8_t data[2];
  uint8_t radar;
  (void)argument;

  for (;;) {
    ball = (HAL_GPIO_ReadPin(GPIOA, IR) == GPIO_PIN_SET) ? 1U : 0U;
    usb = (HAL_GPIO_ReadPin(GPIOF, USB_DET) == GPIO_PIN_RESET) ? 1U : 0U;
    charging = (HAL_GPIO_ReadPin(GPIOB, CHARGE_STATE) == GPIO_PIN_SET) ?
               1U : 0U;
    radar = (HAL_GPIO_ReadPin(GPIOA, LEIDA_IN_O) == GPIO_PIN_SET) ? 1U : 0U;

    if (ball != sg_ball_last) {
      sg_ball_last = ball;
      data[0] = ball;
      faqiuji_protocol_status_event(FAQIUJI_EVENT_BALL, data, 1U);
      if (ball != 0U) {
        sg_ball_trigger = 1U;
      }
    }
    if (usb != sg_usb_last) {
      sg_usb_last = usb;
      data[0] = usb;
      faqiuji_protocol_status_event(FAQIUJI_EVENT_USB, data, 1U);
    }
    if (charging != sg_charge_last) {
      sg_charge_last = charging;
      sg_charging = charging;
      data[0] = charging;
      faqiuji_protocol_status_event(FAQIUJI_EVENT_CHARGE, data, 1U);
    }
    if (radar != sg_radar_last) {
      sg_radar_last = radar;
      data[0] = radar;
      faqiuji_protocol_status_event(FAQIUJI_EVENT_RADAR, data, 1U);
    }
    vTaskDelay(pdMS_TO_TICKS(20U));
  }
}

void faqiuji_device_power_task(void *argument)
{
  uint16_t battery_raw;
  uint16_t ntc_raw;
  uint8_t battery;
  uint8_t data[2];
  uint32_t last_sample = 0U;
  (void)argument;

  for (;;) {
    if ((HAL_GetTick() - last_sample) >= 1000U) {
      last_sample = HAL_GetTick();
      battery_raw = faqiuji_adc_read(ADC_CHANNEL_7);
      ntc_raw = faqiuji_adc_read(ADC_CHANNEL_0);
      battery = faqiuji_battery_percent(battery_raw);
      sg_battery_percent = battery;
      data[0] = battery;
      faqiuji_protocol_status_event(FAQIUJI_EVENT_BATTERY, data, 1U);
      faqiuji_charge_led_task(battery, sg_charging);

      if (ntc_raw >= FAQIUJI_NTC_OVERHEAT_RAW) {
        sg_charge_blocked = 1U;
      } else if (ntc_raw <= FAQIUJI_NTC_RECOVER_RAW) {
        sg_charge_blocked = 0U;
      }
      HAL_GPIO_WritePin(GPIOB, CHARGE_EN,
                        sg_charge_blocked ? GPIO_PIN_RESET : GPIO_PIN_SET);
      data[0] = (uint8_t)ntc_raw;
      data[1] = (uint8_t)(ntc_raw >> 8U);
      faqiuji_protocol_status_event(FAQIUJI_EVENT_TEMPERATURE, data, 2U);
    }
    vTaskDelay(pdMS_TO_TICKS(20U));
  }
}

void faqiuji_device_ir_task(void *argument)
{
  (void)argument;
  for (;;) {
    sg_ie_tick = HAL_GetTick();
    sg_ie_enabled = 1U;
    (void)HAL_TIM_PWM_Start(&sg_ie_tim, TIM_CHANNEL_1);
    vTaskDelay(pdMS_TO_TICKS(3U));
    sg_ie_enabled = 0U;
    (void)HAL_TIM_PWM_Stop(&sg_ie_tim, TIM_CHANNEL_1);
    vTaskDelay(pdMS_TO_TICKS(3U));
  }
}

void faqiuji_device_led_task(void *argument)
{
  (void)argument;
  for (;;) {
    if ((HAL_GetTick() - sg_led_tick) >= 250U) {
      sg_led_tick = HAL_GetTick();
      if (sg_work_state == FAQIUJI_WORK_STANDBY) {
        sg_led_phase ^= 1U;
      } else {
        sg_led_phase = 1U;
      }
      faqiuji_device_update_distance_leds();
    }
    faqiuji_device_update_distance_leds();
    vTaskDelay(pdMS_TO_TICKS(50U));
  }
}

void faqiuji_device_control_task(void *argument)
{
  uint8_t data[2];
  (void)argument;

  for (;;) {
    if (sg_work_state == FAQIUJI_WORK_STANDBY &&
        (int32_t)(HAL_GetTick() - sg_standby_until) >= 0) {
      sg_count = 0U;
      faqiuji_device_set_work_state(FAQIUJI_WORK_RUNNING);
      data[0] = 0U;
      data[1] = 0U;
      faqiuji_protocol_status_event(FAQIUJI_EVENT_COUNT, data, 2U);
    }

    if (sg_work_state != FAQIUJI_WORK_STANDBY) {
      if (sg_charging != 0U) {
        faqiuji_device_set_work_state(FAQIUJI_WORK_CHARGING);
      } else if (sg_battery_percent >= FAQIUJI_BATTERY_FULL_PERCENT) {
        faqiuji_device_set_work_state(FAQIUJI_WORK_CHARGE_DONE);
      } else if (sg_work_state == FAQIUJI_WORK_CHARGING ||
                 sg_work_state == FAQIUJI_WORK_CHARGE_DONE) {
        faqiuji_device_set_work_state(FAQIUJI_WORK_RUNNING);
      }
    }

    if (sg_ball_trigger != 0U) {
      sg_ball_trigger = 0U;
      if (sg_enabled != 0U && sg_work_state == FAQIUJI_WORK_RUNNING &&
          sg_count < sg_max_count &&
          faqiuji_launcher_is_running() == 0U &&
          faqiuji_launcher_start((FAQIUJI_LAUNCH_MODE_E)sg_mode) != 0U) {
        ++sg_count;
        data[0] = (uint8_t)sg_count;
        data[1] = (uint8_t)(sg_count >> 8U);
        faqiuji_protocol_status_event(FAQIUJI_EVENT_COUNT, data, 2U);
        if (sg_count >= sg_max_count) {
          sg_standby_until = HAL_GetTick() +
                             ((uint32_t)sg_standby_min * 60000UL);
          faqiuji_device_set_work_state(FAQIUJI_WORK_STANDBY);
          faqiuji_launcher_stop();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10U));
  }
}
