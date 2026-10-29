#include "main.h"
#include "faqiuji_audio.h"
#include "FreeRTOS.h"
#include "task.h"

#define AUDIO_SAMPLE_RATE_HZ 16000U
#define AUDIO_DAC_MIDPOINT   2048U
#define AUDIO_FLASH_BASE      0x08004000UL
#define AUDIO_FLASH_LIMIT     0x08020000UL
#define AUDIO_HEADER_SIZE     16U
#define AUDIO_HEADER_MAGIC   0x31415146UL
#define AUDIO_VOLUME_MAX     100U

#if FAQIUJI_AUDIO_VOLUME_PERCENT > AUDIO_VOLUME_MAX
#error "FAQIUJI_AUDIO_VOLUME_PERCENT must be between 0 and 100"
#endif

static DAC_HandleTypeDef sg_audio_dac;
static TIM_HandleTypeDef sg_audio_tim;
static const volatile uint8_t *sg_audio_samples;
static volatile uint32_t sg_audio_sample_count;
static volatile uint32_t sg_audio_sample_index;
static volatile uint16_t sg_audio_volume_scale_q8;
static volatile uint8_t sg_audio_playing;

static uint32_t audio_read_u32(uint32_t address)
{
  const volatile uint32_t *value = (const volatile uint32_t *)address;

  return *value;
}

static void audio_sample_update(void)
{
  uint8_t pcm_sample;
  int32_t centered_sample;
  uint16_t dac_sample;

  if (sg_audio_playing == 0U || sg_audio_sample_count == 0U) {
    return;
  }

  pcm_sample = sg_audio_samples[sg_audio_sample_index];
  centered_sample = (int32_t)pcm_sample - 128;
  dac_sample =
      (uint16_t)((int32_t)AUDIO_DAC_MIDPOINT +
                 ((centered_sample * (int32_t)sg_audio_volume_scale_q8) / 256));
  DAC1->DHR12R1 = dac_sample;

  sg_audio_sample_index++;
  if (sg_audio_sample_index >= sg_audio_sample_count) {
    sg_audio_sample_index = 0U;
    sg_audio_playing = 0U;
  }
}

void faqiuji_audio_init(void)
{
  DAC_ChannelConfTypeDef dac_config = {0};
  RCC_ClkInitTypeDef clock_config = {0};
  uint32_t flash_latency = 0U;
  uint32_t timer_clock;

  __HAL_RCC_DAC1_CLK_ENABLE();
  sg_audio_dac.Instance = DAC1;
  if (HAL_DAC_Init(&sg_audio_dac) != HAL_OK) {
    APP_ErrorHandler();
  }

  dac_config.DAC_Trigger = DAC_TRIGGER_NONE;
  dac_config.DAC_OutputBuffer = DAC_OUTPUTBUFFER_DISABLE;
  if (HAL_DAC_ConfigChannel(&sg_audio_dac, &dac_config,
                            DAC_CHANNEL_1) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_DAC_SetValue(&sg_audio_dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                   AUDIO_DAC_MIDPOINT);
  if (HAL_DAC_Start(&sg_audio_dac, DAC_CHANNEL_1) != HAL_OK) {
    APP_ErrorHandler();
  }

  HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
  timer_clock = HAL_RCC_GetPCLK1Freq();
  if (clock_config.APB1CLKDivider != RCC_HCLK_DIV1) {
    timer_clock *= 2U;
  }
  if (timer_clock < AUDIO_SAMPLE_RATE_HZ) {
    APP_ErrorHandler();
  }

  __HAL_RCC_TIM6_CLK_ENABLE();
  sg_audio_tim.Instance = TIM6;
  sg_audio_tim.Init.Prescaler = 0U;
  sg_audio_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  sg_audio_tim.Init.Period = (timer_clock / AUDIO_SAMPLE_RATE_HZ) - 1U;
  sg_audio_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  sg_audio_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&sg_audio_tim) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_NVIC_SetPriority(TIM6_LPTIM1_DAC_IRQn, 3U, 0U);
  HAL_NVIC_EnableIRQ(TIM6_LPTIM1_DAC_IRQn);
  if (HAL_TIM_Base_Start_IT(&sg_audio_tim) != HAL_OK) {
    APP_ErrorHandler();
  }
}

void faqiuji_audio_tim6_irq_handler(void)
{
  if (__HAL_TIM_GET_FLAG(&sg_audio_tim, TIM_FLAG_UPDATE) != RESET &&
      __HAL_TIM_GET_IT_SOURCE(&sg_audio_tim, TIM_IT_UPDATE) != RESET) {
    __HAL_TIM_CLEAR_IT(&sg_audio_tim, TIM_IT_UPDATE);
    audio_sample_update();
  }
}

void faqiuji_audio_task(void *argument)
{
  uint32_t sample_rate;
  uint32_t sample_count;
  (void)argument;

  if (audio_read_u32(AUDIO_FLASH_BASE) != AUDIO_HEADER_MAGIC) {
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000U));
    }
  }

  sample_rate = audio_read_u32(AUDIO_FLASH_BASE + 4U);
  sample_count = audio_read_u32(AUDIO_FLASH_BASE + 8U);
  if (sample_rate != AUDIO_SAMPLE_RATE_HZ || sample_count == 0U ||
      sample_count > (AUDIO_FLASH_LIMIT - AUDIO_FLASH_BASE -
                     AUDIO_HEADER_SIZE)) {
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000U));
    }
  }

  sg_audio_samples =
      (const volatile uint8_t *)(AUDIO_FLASH_BASE + AUDIO_HEADER_SIZE);
  sg_audio_sample_count = sample_count;
  sg_audio_sample_index = 0U;
  sg_audio_volume_scale_q8 =
      (uint16_t)((16U * FAQIUJI_AUDIO_VOLUME_PERCENT * 256U) / 100U);
  for (;;) {
    sg_audio_sample_index = 0U;
    sg_audio_playing = 1U;
    while (sg_audio_playing != 0U) {
      vTaskDelay(1U);
    }

    HAL_DAC_SetValue(&sg_audio_dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                     AUDIO_DAC_MIDPOINT);
    vTaskDelay(pdMS_TO_TICKS(3000U));
  }
}
