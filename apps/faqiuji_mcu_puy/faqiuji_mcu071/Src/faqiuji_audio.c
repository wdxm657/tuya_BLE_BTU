#include "main.h"
#include "faqiuji_audio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define AUDIO_SAMPLE_RATE_HZ 16000U
#define AUDIO_DAC_MIDPOINT   2048U
#define AUDIO_FLASH_BASE      0x08008000UL
#define AUDIO_FLASH_LIMIT     0x08020000UL
#define AUDIO_HEADER_SIZE     16U
#define AUDIO_HEADER_MAGIC   0x31415146UL
#define AUDIO_VOLUME_MAX     100U
#define AUDIO_REPEAT_DELAY_MS 2000U
#define AUDIO_FADE_SAMPLES (AUDIO_SAMPLE_RATE_HZ / 100U)
#define AUDIO_DMA_HALF 64U

#if FAQIUJI_AUDIO_VOLUME_PERCENT > AUDIO_VOLUME_MAX
#error "FAQIUJI_AUDIO_VOLUME_PERCENT must be between 0 and 100"
#endif

static DAC_HandleTypeDef sg_audio_dac;
static TIM_HandleTypeDef sg_audio_tim;
static DMA_HandleTypeDef sg_audio_dma;
static const volatile uint8_t *sg_audio_samples;
static volatile uint32_t sg_audio_sample_count;
static volatile uint32_t sg_audio_sample_index;
static int16_t sg_audio_pcm_lut[256];
static uint16_t sg_audio_buffer[AUDIO_DMA_HALF * 2U];
static uint32_t sg_audio_fade_samples;
static uint32_t sg_audio_fade_step_q15;
static volatile uint8_t sg_audio_playing;
static volatile uint32_t sg_audio_irq_overruns;
static volatile uint32_t sg_audio_consumed;
static volatile uint32_t sg_audio_done_tick;
static SemaphoreHandle_t sg_audio_lock;
static volatile uint8_t sg_record_requested;
static volatile uint32_t sg_record_generation;

void faqiuji_audio_record_begin(void)
{
  sg_record_requested = 1U;
  sg_record_generation++;
  if (xSemaphoreTake(sg_audio_lock, portMAX_DELAY) != pdTRUE) {
    APP_ErrorHandler();
  }
#if FAQIUJI_AUDIO_RECORD_MUTE_LOW
  {
    GPIO_InitTypeDef gpio = {0};
    if (HAL_DAC_Stop(&sg_audio_dac, DAC_CHANNEL_1) != HAL_OK) {
      APP_ErrorHandler();
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
  }
#endif
}

void faqiuji_audio_record_end(uint8_t saved)
{
  (void)saved;
#if FAQIUJI_AUDIO_RECORD_MUTE_LOW
  {
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
    DAC1->DHR12R1 = AUDIO_DAC_MIDPOINT;
    if (HAL_DAC_Start(&sg_audio_dac, DAC_CHANNEL_1) != HAL_OK) {
      APP_ErrorHandler();
    }
  }
#endif
  sg_record_requested = 0U;
  xSemaphoreGive(sg_audio_lock);
}

static uint32_t audio_read_u32(uint32_t address)
{
  const volatile uint32_t *value = (const volatile uint32_t *)address;

  return *value;
}

static void audio_fill_half(uint32_t half)
{
  uint32_t index;
  uint32_t count;
  uint32_t envelope;
  uint32_t i;
  int32_t output;

  index = sg_audio_sample_index;
  count = sg_audio_sample_count;
  for (i = 0U; i < AUDIO_DMA_HALF; i++) {
    output = 0;
    if (index < count) {
      output = sg_audio_pcm_lut[sg_audio_samples[index]];
      envelope = index;
      if (envelope > count - 1U - index) {
        envelope = count - 1U - index;
      }
      if (envelope < sg_audio_fade_samples) {
        output = (output * (int32_t)(envelope * sg_audio_fade_step_q15)) >> 15;
      }
      index++;
    }
    output += AUDIO_DAC_MIDPOINT;
    if (output < 0) {
      output = 0;
    } else if (output > 4095) {
      output = 4095;
    }
    sg_audio_buffer[half * AUDIO_DMA_HALF + i] = (uint16_t)output;
  }
  sg_audio_sample_index = index;
}

static void audio_dma_stop(void)
{
  TIM6->CR1 &= ~TIM_CR1_CEN;
  DAC1->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_TEN1);
  DAC1->DHR12R1 = AUDIO_DAC_MIDPOINT;
  sg_audio_done_tick = HAL_GetTick();
  sg_audio_playing = 0U;
}

static void audio_half_complete(uint32_t half)
{
  uint32_t remaining;
  if (sg_audio_playing == 0U) {
    return;
  }
  sg_audio_consumed += AUDIO_DMA_HALF;
  /* DMA completion means the last value is in DHR, not yet at the DAC output.
     Drain one padded block so the final faded sample has reached the pin. */
  if (sg_audio_consumed > sg_audio_sample_count || sg_record_requested != 0U) {
    audio_dma_stop();
    return;
  }
  audio_fill_half(half);
  remaining = __HAL_DMA_GET_COUNTER(&sg_audio_dma);
  if ((half == 0U && (remaining == 0U || remaining > AUDIO_DMA_HALF)) ||
      (half == 1U && remaining <= AUDIO_DMA_HALF)) {
    sg_audio_irq_overruns++;
    audio_dma_stop();
  }
}

void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef *dac)
{
  if (dac == &sg_audio_dac) {
    audio_half_complete(0U);
  }
}

void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef *dac)
{
  if (dac == &sg_audio_dac) {
    audio_half_complete(1U);
  }
}

void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef *dac)
{
  if (dac == &sg_audio_dac) {
    sg_audio_irq_overruns++;
    audio_dma_stop();
  }
}

void HAL_DAC_DMAUnderrunCallbackCh1(DAC_HandleTypeDef *dac)
{
  HAL_DAC_ErrorCallbackCh1(dac);
}

void faqiuji_audio_dma_irq_handler(void)
{
  HAL_DMA_IRQHandler(&sg_audio_dma);
}

void faqiuji_audio_init(void)
{
  DAC_ChannelConfTypeDef dac_config = {0};
  RCC_ClkInitTypeDef clock_config = {0};
  uint32_t flash_latency = 0U;
  uint32_t timer_clock;
  TIM_MasterConfigTypeDef master = {0};

  sg_audio_lock = xSemaphoreCreateMutex();
  if (sg_audio_lock == NULL) {
    APP_ErrorHandler();
  }
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
  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&sg_audio_tim, &master) != HAL_OK) {
    APP_ErrorHandler();
  }
  __HAL_RCC_DMA_CLK_ENABLE();
  sg_audio_dma.Instance = DMA1_Channel2;
  sg_audio_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
  sg_audio_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  sg_audio_dma.Init.MemInc = DMA_MINC_ENABLE;
  sg_audio_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  sg_audio_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  sg_audio_dma.Init.Mode = DMA_CIRCULAR;
  sg_audio_dma.Init.Priority = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&sg_audio_dma) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_DMA_ChannelMap(&sg_audio_dma, DMA_CHANNEL_MAP_DAC1);
  __HAL_LINKDMA(&sg_audio_dac, DMA_Handle1, sg_audio_dma);
  HAL_NVIC_SetPriority(DMA1_Channel2_3_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
  HAL_NVIC_SetPriority(TIM6_LPTIM1_DAC_IRQn, 3U, 0U);
  HAL_NVIC_EnableIRQ(TIM6_LPTIM1_DAC_IRQn);
}

void faqiuji_audio_tim6_irq_handler(void)
{
  HAL_DAC_IRQHandler(&sg_audio_dac);
}

void faqiuji_audio_task(void *argument)
{
  uint32_t sample_rate;
  uint32_t sample_count;
  uint32_t record_generation;
  TickType_t pause_start;
  uint32_t sum;
  uint32_t i;
  uint32_t play_start;
  uint32_t play_elapsed;
  uint32_t played_samples;
  uint32_t hardware_elapsed;
  uint32_t play_number = 0U;
  uint32_t midpoint;
  int32_t volume_scale_q8;
  uint8_t interrupted;
  (void)argument;

  volume_scale_q8 =
      (int32_t)((16U * FAQIUJI_AUDIO_VOLUME_PERCENT * 256U) / 100U);
  for (;;) {
    if (sg_record_requested != 0U) {
      vTaskDelay(pdMS_TO_TICKS(10U));
      continue;
    }
    if (xSemaphoreTake(sg_audio_lock, portMAX_DELAY) != pdTRUE) {
      APP_ErrorHandler();
    }
    if (sg_record_requested != 0U) {
      xSemaphoreGive(sg_audio_lock);
      continue;
    }
    sample_rate = audio_read_u32(AUDIO_FLASH_BASE + 4U);
    sample_count = audio_read_u32(AUDIO_FLASH_BASE + 8U);
    if (audio_read_u32(AUDIO_FLASH_BASE) != AUDIO_HEADER_MAGIC ||
        audio_read_u32(AUDIO_FLASH_BASE + 12U) != 1U ||
        sample_rate != AUDIO_SAMPLE_RATE_HZ || sample_count == 0U ||
        sample_count > AUDIO_FLASH_LIMIT - AUDIO_FLASH_BASE - AUDIO_HEADER_SIZE) {
      /* No usable header: play the reserved region as raw unsigned PCM. */
      sg_audio_samples = (const volatile uint8_t *)AUDIO_FLASH_BASE;
      sample_count = AUDIO_FLASH_LIMIT - AUDIO_FLASH_BASE;
    } else {
      sg_audio_samples =
          (const volatile uint8_t *)(AUDIO_FLASH_BASE + AUDIO_HEADER_SIZE);
    }
    sg_audio_sample_count = sample_count;
    /* Remove microphone DC bias before applying the playback envelope. */
    sum = 0U;
    for (i = 0U; i < sample_count; i++) {
      sum += sg_audio_samples[i];
    }
    midpoint = (sum + sample_count / 2U) / sample_count;
    for (i = 0U; i < 256U; i++) {
      sg_audio_pcm_lut[i] =
          (int16_t)(((int32_t)i - (int32_t)midpoint) * volume_scale_q8 / 256);
    }
    sg_audio_fade_samples = sample_count / 2U;
    if (sg_audio_fade_samples > AUDIO_FADE_SAMPLES) {
      sg_audio_fade_samples = AUDIO_FADE_SAMPLES;
    }
    sg_audio_fade_step_q15 = sg_audio_fade_samples != 0U ?
                            32768U / sg_audio_fade_samples : 0U;
    APP_LOG("Audio #%lu: samples=%lu expected=%lu ms PCLK=%lu PSC=%lu ARR=%lu\r\n",
            (unsigned long)++play_number, (unsigned long)sample_count,
            (unsigned long)(sample_count * 1000U / AUDIO_SAMPLE_RATE_HZ),
            (unsigned long)HAL_RCC_GetPCLK1Freq(),
            (unsigned long)sg_audio_tim.Instance->PSC,
            (unsigned long)sg_audio_tim.Instance->ARR);
    /* Reload the prescaler and reset the timer phase identically on every loop. */
    sg_audio_tim.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_SET_COUNTER(&sg_audio_tim, 0U);
    __HAL_TIM_CLEAR_IT(&sg_audio_tim, TIM_IT_UPDATE);
    HAL_NVIC_ClearPendingIRQ(TIM6_LPTIM1_DAC_IRQn);
    sg_audio_sample_index = 0U;
    sg_audio_consumed = 0U;
    audio_fill_half(0U);
    audio_fill_half(1U);
    sg_audio_irq_overruns = 0U;
    sg_audio_done_tick = 0U;
    DAC1->SR = DAC_SR_DMAUDR1;
    DAC1->DHR12R1 = AUDIO_DAC_MIDPOINT;
    DAC1->CR |= DAC_CR_TEN1;
    sg_audio_playing = 1U;
    play_start = HAL_GetTick();
    if (HAL_DAC_Start_DMA(&sg_audio_dac, DAC_CHANNEL_1,
                          (uint32_t *)sg_audio_buffer, AUDIO_DMA_HALF * 2U,
                          DAC_ALIGN_12B_R) != HAL_OK ||
        HAL_TIM_Base_Start(&sg_audio_tim) != HAL_OK) {
      APP_ErrorHandler();
    }
    while (sg_audio_playing != 0U && sg_record_requested == 0U) {
      vTaskDelay(1U);
    }

    if (sg_audio_playing != 0U) {
      audio_dma_stop();
    }
    hardware_elapsed = sg_audio_done_tick - play_start;
    /* Keep EN1 set: Stop_DMA disables the analog output and causes a
       disable/enable transient even when both endpoints are at midpoint. */
    if (HAL_TIM_Base_Stop(&sg_audio_tim) != HAL_OK ||
        HAL_DMA_Abort(&sg_audio_dma) != HAL_OK) {
      APP_ErrorHandler();
    }
    __HAL_DAC_DISABLE_IT(&sg_audio_dac, DAC_IT_DMAUDR1);
    DAC1->SR = DAC_SR_DMAUDR1;
    sg_audio_dac.State = HAL_DAC_STATE_READY;
    play_elapsed = HAL_GetTick() - play_start;
    interrupted = sg_record_requested;
    played_samples = sg_audio_consumed;
    if (played_samples > sg_audio_sample_count) {
      played_samples = sg_audio_sample_count;
    }
    sg_audio_playing = 0U;
    __HAL_TIM_CLEAR_IT(&sg_audio_tim, TIM_IT_UPDATE);
    HAL_NVIC_ClearPendingIRQ(TIM6_LPTIM1_DAC_IRQn);
    HAL_DAC_SetValue(&sg_audio_dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                     AUDIO_DAC_MIDPOINT);
    APP_LOG("Audio #%lu: elapsed=%lu ms task=%lu ms played=%lu DMA_errors=%lu interrupted=%u\r\n",
            (unsigned long)play_number, (unsigned long)hardware_elapsed,
            (unsigned long)play_elapsed,
            (unsigned long)played_samples,
            (unsigned long)sg_audio_irq_overruns, (unsigned int)interrupted);
    record_generation = sg_record_generation;
    pause_start = xTaskGetTickCount();
    xSemaphoreGive(sg_audio_lock);
    /* A new recording cancels the gap and restarts playback after saving. */
    while (sg_record_requested == 0U &&
           sg_record_generation == record_generation &&
           xTaskGetTickCount() - pause_start <
               pdMS_TO_TICKS(AUDIO_REPEAT_DELAY_MS)) {
      vTaskDelay(pdMS_TO_TICKS(10U));
    }
  }
}
