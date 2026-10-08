#include "faqiuji_device.h"
#include "faqiuji_protocol.h"
#include "faqiuji_launcher.h"
#include "gpio_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "py32f040_hal_adc.h"

#define FAQIUJI_ADC_VREF_MV                2140U
#define FAQIUJI_ADC_FULL_SCALE             4095U
#define FAQIUJI_BATTERY_DIVIDER_RATIO      2U
#define FAQIUJI_BATTERY_LOW_PERCENT        20U
#define FAQIUJI_BATTERY_FULL_PERCENT       90U

static TIM_HandleTypeDef sg_ie_tim;
static volatile uint8_t sg_mode;
static volatile uint8_t sg_enabled = 1U;
static volatile uint8_t sg_ball_last;
static volatile uint8_t sg_usb_last;
static volatile uint8_t sg_charge_last;
static volatile uint16_t sg_max_count = 20U;
static volatile uint16_t sg_count;
static volatile uint16_t sg_standby_min = 20U;
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

ADC_HandleTypeDef        AdcHandle;
TIM_HandleTypeDef        TimHandle;
TIM_MasterConfigTypeDef  sMasterConfig;
/**
  * @brief  ADC Configuration Function
  * @param  None
  * @retval None
  */
void APP_AdcConfig(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  AdcHandle.Instance = ADC1;

  AdcHandle.Init.Resolution            = ADC_RESOLUTION_12B;             /* 12-bit resolution for converted data */
  AdcHandle.Init.DataAlign             = ADC_DATAALIGN_RIGHT;            /* Right-alignment for converted data */
  AdcHandle.Init.ScanConvMode          = ADC_SCAN_DISABLE;               /* Scan mode off */
  AdcHandle.Init.ContinuousConvMode    = DISABLE;                        /* Single mode */
  AdcHandle.Init.NbrOfConversion       = 1;                              /* Number of conversion channels 1 */
  AdcHandle.Init.DiscontinuousConvMode = DISABLE;                        /* Discontinuous mode not enabled */
  AdcHandle.Init.NbrOfDiscConversion   = 1;                              /* Discontinuous mode short sequence length is 1 */
  AdcHandle.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T15_TRGO;  /* TIM15 TRGO triggered */
  /* ADC initialization */
  if (HAL_ADC_Init(&AdcHandle) != HAL_OK)
  {
    APP_ErrorHandler();
  }

  /* Configure VrefBuf 2.14V */
  HAL_ADC_ConfigVrefBuf(&AdcHandle,ADC_VREFBUF_2P14V);

  sConfig.Channel      = ADC_CHANNEL_7;
  sConfig.Rank         = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_28CYCLES_5;
  /* ADC channel configuration */
  if (HAL_ADC_ConfigChannel(&AdcHandle, &sConfig) != HAL_OK)
  {
    APP_ErrorHandler();
  }

  /* ADC calibration */
  if(HAL_ADCEx_Calibration_Start(&AdcHandle) != HAL_OK)
  {
    APP_ErrorHandler();
  }

/*  if(HAL_ADCEx_Calibration_GetStatus(&AdcHandle) != HAL_ADCCALIBOK)   */
/*  {                                                                   */
/*    APP_ErrorHandler();                                               */
/*  }                                                                   */

    /* ADC Enable Conversion */
    HAL_ADC_Start(&AdcHandle);
}

/**
  * @brief  TIM Configuration Function
  * @param  None
  * @retval None
  */
void APP_TimConfig(void)
{
  TimHandle.Instance = TIM15;                                         /* TIM15 */
  TimHandle.Init.Period            = 8000 - 1;                        /* Period = 8000-1 */
  TimHandle.Init.Prescaler         = 1000 - 1;                        /* Prescaler = 1000-1 */
  TimHandle.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;          /* ClockDivision = 0 */
  TimHandle.Init.CounterMode       = TIM_COUNTERMODE_UP;              /* Counter direction = Up */
  TimHandle.Init.RepetitionCounter = 0;                               /* Repetition = 0 */
  TimHandle.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;  /* Auto-reload register not buffered */
  if (HAL_TIM_Base_Init(&TimHandle) != HAL_OK)                        /* Initialize TIM15 */
  {
    APP_ErrorHandler();
  }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;                /* Select Update Event as Trigger Source */
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;        /* Master/Slave mode has no effect */
  HAL_TIMEx_MasterConfigSynchronization(&TimHandle, &sMasterConfig);  /* Configure TIM15*/
  if (HAL_TIM_Base_Start(&TimHandle) != HAL_OK)                       /* TIM15 start */
  {
    APP_ErrorHandler();
  }
}


static uint16_t faqiuji_adc_read_battery_mv(void)
{
  uint16_t raw;

  if (HAL_ADC_PollForConversion(&AdcHandle, 2000U) != HAL_OK) {
    return 0U;
  }

  raw = (uint16_t)HAL_ADC_GetValue(&AdcHandle);
  return (uint16_t)(((uint32_t)raw * FAQIUJI_ADC_VREF_MV *
                     FAQIUJI_BATTERY_DIVIDER_RATIO) /
                    FAQIUJI_ADC_FULL_SCALE);
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

  /* Start enabled: 38 kHz at 50% duty. */
  __HAL_TIM_SET_COMPARE(&sg_ie_tim, TIM_CHANNEL_1,
                        (sg_ie_tim.Init.Period + 1U) / 2U);
  (void)HAL_TIM_PWM_Start(&sg_ie_tim, TIM_CHANNEL_1);
}

void faqiuji_device_init(void)
{
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
    // ball = (HAL_GPIO_ReadPin(GPIOA, IR) == GPIO_PIN_SET) ? 1U : 0U;
    usb = (HAL_GPIO_ReadPin(GPIOF, USB_DET) == GPIO_PIN_RESET) ? 1U : 0U;
    charging = (HAL_GPIO_ReadPin(GPIOB, CHARGE_STATE) == GPIO_PIN_SET) ?
               1U : 0U;
    radar = (HAL_GPIO_ReadPin(GPIOA, LEIDA_IN_O) == GPIO_PIN_SET) ? 1U : 0U;

    if (ball != sg_ball_last) {
      sg_ball_last = ball;
      data[0] = ball;
      // faqiuji_protocol_status_event(FAQIUJI_EVENT_BALL, data, 1U);
      // if (ball != 0U) {
      //   sg_ball_trigger = 1U;
      // }
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
  uint16_t battery_mv;
  uint8_t battery;
  uint8_t data[2];
  uint32_t last_sample = 0U;
  (void)argument;

  for (;;) {
    if ((HAL_GetTick() - last_sample) >= 1000U) {
      last_sample = HAL_GetTick();
      battery_mv = faqiuji_adc_read_battery_mv();
      battery = (battery_mv >= 4200U) ? 100U :
                (battery_mv <= 3250U) ? 0U :
                (uint8_t)(((battery_mv - 3250U) * 100U) /
                           (4200U - 3250U));
      sg_battery_percent = battery;
      data[0] = (uint8_t)battery_mv;
      data[1] = (uint8_t)(battery_mv >> 8);
      faqiuji_protocol_status_event(FAQIUJI_EVENT_BATTERY, data, 2U);
      faqiuji_charge_led_task(battery, sg_charging);
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
    /* Active phase: 38 kHz at 50% duty. */
    __HAL_TIM_SET_COMPARE(&sg_ie_tim, TIM_CHANNEL_1,
                          (sg_ie_tim.Init.Period + 1U) / 2U);
    vTaskDelay(pdMS_TO_TICKS(3U));
    sg_ie_enabled = 0U;
    /* Inactive phase: keep the output continuously high. */
    __HAL_TIM_SET_COMPARE(&sg_ie_tim, TIM_CHANNEL_1,
                          sg_ie_tim.Init.Period + 1U);
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
