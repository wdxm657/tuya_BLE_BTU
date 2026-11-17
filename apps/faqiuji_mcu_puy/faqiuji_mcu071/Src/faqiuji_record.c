#include "faqiuji_record.h"
#include "faqiuji_audio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define RECORD_RATE 16000U
#define RECORD_BASE 0x08004000UL
#define RECORD_LIMIT 0x08020000UL
#define RECORD_MAGIC 0x31415146UL
#define RECORD_HEADER 16U
#define RECORD_HALF 512U
#define RECORD_COUNT (RECORD_RATE * FAQIUJI_RECORD_SECONDS)

#if FAQIUJI_RECORD_SECONDS < 1U || FAQIUJI_RECORD_SECONDS > 5U
#error "FAQIUJI_RECORD_SECONDS must be between 1 and 5"
#endif

volatile uint32_t g_record_state;
volatile uint32_t g_record_samples;
volatile uint32_t g_record_error;
static ADC_HandleTypeDef sg_adc;
static TIM_HandleTypeDef sg_timer;
static DMA_HandleTypeDef sg_dma;
static volatile uint16_t sg_capture[RECORD_HALF * 2U];
static uint8_t sg_pcm[RECORD_HALF];
static uint32_t sg_page[FLASH_PAGE_SIZE / 4U];
static uint32_t sg_first_page[FLASH_PAGE_SIZE / 4U];
static volatile uint32_t sg_produced;
static volatile uint32_t sg_consumed;
static volatile uint32_t sg_fault;
static volatile uint32_t sg_capture_limit;
static volatile uint8_t sg_limit_reached;

static uint8_t record_key_down(void)
{
  return HAL_GPIO_ReadPin(FAQIUJI_RECORD_KEY_PORT,
                         FAQIUJI_RECORD_KEY_PIN) == FAQIUJI_RECORD_KEY_ACTIVE;
}

static void record_half_ready(uint32_t half)
{
  if (sg_produced != sg_consumed || half != (sg_produced & 1U)) {
    sg_fault = 1U;
    TIM15->CR1 &= ~TIM_CR1_CEN;
  } else {
    __DMB();
    sg_produced++;
    if (sg_produced * RECORD_HALF >= sg_capture_limit) {
      TIM15->CR1 &= ~TIM_CR1_CEN;
      sg_limit_reached = 1U;
    }
  }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *adc)
{
  if (adc == &sg_adc) {
    record_half_ready(0U);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *adc)
{
  if (adc == &sg_adc) {
    record_half_ready(1U);
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc)
{
  if (adc == &sg_adc) {
    sg_fault = 1U;
    TIM15->CR1 &= ~TIM_CR1_CEN;
  }
}

void faqiuji_record_dma_irq_handler(void)
{
  HAL_DMA_IRQHandler(&sg_dma);
}

static HAL_StatusTypeDef record_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  RCC_PeriphCLKInitTypeDef peripheral = {0};
  RCC_ClkInitTypeDef clocks = {0};
  ADC_ChannelConfTypeDef channel = {0};
  TIM_MasterConfigTypeDef master = {0};
  uint32_t latency;
  uint32_t timer_clock;

  __HAL_RCC_GPIOB_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &gpio);
  __HAL_RCC_ADC_CLK_ENABLE();
  peripheral.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  peripheral.ADCClockSelection = RCC_ADCCLKSOURCE_PCLK_DIV4;
  if (HAL_RCCEx_PeriphCLKConfig(&peripheral) != HAL_OK) {
    return HAL_ERROR;
  }
  sg_adc.Instance = ADC1;
  sg_adc.Init.Resolution = ADC_RESOLUTION_12B;
  sg_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  sg_adc.Init.ScanConvMode = ADC_SCAN_DISABLE;
  sg_adc.Init.ContinuousConvMode = DISABLE;
  sg_adc.Init.NbrOfConversion = 1U;
  sg_adc.Init.DiscontinuousConvMode = DISABLE;
  sg_adc.Init.NbrOfDiscConversion = 1U;
  sg_adc.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T15_TRGO;
  if (HAL_ADC_Init(&sg_adc) != HAL_OK) {
    return HAL_ERROR;
  }
  channel.Channel = ADC_CHANNEL_8;
  channel.Rank = ADC_REGULAR_RANK_1;
  /* At 2 MHz ADC clock: (41.5 + 12.5) cycles = 27 us < 62.5 us. */
  channel.SamplingTime = ADC_SAMPLETIME_41CYCLES_5;
  if (HAL_ADC_ConfigChannel(&sg_adc, &channel) != HAL_OK ||
      HAL_ADCEx_Calibration_Start(&sg_adc) != HAL_OK) {
    return HAL_ERROR;
  }
  __HAL_RCC_DMA_CLK_ENABLE();
  sg_dma.Instance = DMA1_Channel1;
  sg_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
  sg_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  sg_dma.Init.MemInc = DMA_MINC_ENABLE;
  sg_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  sg_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  sg_dma.Init.Mode = DMA_CIRCULAR;
  sg_dma.Init.Priority = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&sg_dma) != HAL_OK) {
    return HAL_ERROR;
  }
  HAL_DMA_ChannelMap(&sg_dma, DMA_CHANNEL_MAP_ADC1);
  __HAL_LINKDMA(&sg_adc, DMA_Handle, sg_dma);
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

  HAL_RCC_GetClockConfig(&clocks, &latency);
  timer_clock = HAL_RCC_GetPCLK1Freq();
  if (clocks.APB1CLKDivider != RCC_HCLK_DIV1) {
    timer_clock *= 2U;
  }
  if (timer_clock < RECORD_RATE || timer_clock % RECORD_RATE != 0U) {
    return HAL_ERROR;
  }
  __HAL_RCC_TIM15_CLK_ENABLE();
  sg_timer.Instance = TIM15;
  sg_timer.Init.Prescaler = 0U;
  sg_timer.Init.Period = timer_clock / RECORD_RATE - 1U;
  sg_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  sg_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  sg_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&sg_timer) != HAL_OK) {
    return HAL_ERROR;
  }
  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  return HAL_TIMEx_MasterConfigSynchronization(&sg_timer, &master);
}

static HAL_StatusTypeDef record_page(uint32_t address, uint32_t *page)
{
  uint32_t i;
  if (address < RECORD_BASE || address > RECORD_LIMIT - FLASH_PAGE_SIZE ||
      address % FLASH_PAGE_SIZE != 0U) {
    return HAL_ERROR;
  }
  if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_PAGE, address, page) != HAL_OK) {
    return HAL_ERROR;
  }
  for (i = 0U; i < FLASH_PAGE_SIZE / 4U; i++) {
    if (((const volatile uint32_t *)address)[i] != page[i]) {
      return HAL_ERROR;
    }
  }
  return HAL_OK;
}

static uint32_t record_save(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t erase_error;
  uint32_t address = RECORD_BASE;
  uint32_t used = RECORD_HEADER;
  uint32_t i;
  uint32_t n;
  uint32_t half;
  uint32_t remaining;
  uint32_t captured = 0U;
  uint8_t stopped = 0U;
  uint8_t released = 0U;
  TickType_t release_start = 0U;
  TickType_t wait_start;
  uint32_t *header = sg_first_page;

#define RECORD_CHECK(condition) do { if (!(condition)) { \
    g_record_error = __LINE__; goto failed; } } while (0)

  RECORD_CHECK(RECORD_COUNT + RECORD_HEADER <= RECORD_LIMIT - RECORD_BASE);
  RECORD_CHECK(record_init() == HAL_OK);
  /* A short press released during setup must not erase the previous clip. */
  if (record_key_down() == 0U) {
    HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
    return 0U;
  }
  RECORD_CHECK(HAL_FLASH_Unlock() == HAL_OK);
  g_record_state = 1U;
  erase.TypeErase = FLASH_TYPEERASE_PAGEERASE;
  erase.PageAddress = RECORD_BASE;
  erase.NbPages = (RECORD_LIMIT - RECORD_BASE) / FLASH_PAGE_SIZE;
  RECORD_CHECK(HAL_FLASH_Erase(&erase, &erase_error) == HAL_OK);
  memset(sg_page, 0xFF, sizeof(sg_page));
  memset(sg_first_page, 0xFF, sizeof(sg_first_page));
  header[0] = RECORD_MAGIC;
  header[1] = RECORD_RATE;
  header[2] = 0U;
  header[3] = 1U;
  sg_produced = 0U;
  sg_consumed = 0U;
  sg_fault = 0U;
  sg_capture_limit = RECORD_COUNT;
  sg_limit_reached = 0U;
  g_record_error = 0U;
  g_record_samples = 0U;
  RECORD_CHECK(HAL_ADC_Start_DMA(&sg_adc, (uint32_t *)sg_capture,
                                RECORD_HALF * 2U) == HAL_OK);
  __HAL_TIM_SET_COUNTER(&sg_timer, 0U);
  __HAL_TIM_CLEAR_FLAG(&sg_timer, TIM_FLAG_UPDATE);
  g_record_state = 2U;
  RECORD_CHECK(HAL_TIM_Base_Start(&sg_timer) == HAL_OK);
  wait_start = xTaskGetTickCount();
  for (;;) {
    if (stopped == 0U) {
      if (record_key_down() != 0U) {
        released = 0U;
      } else if (released == 0U) {
        released = 1U;
        release_start = xTaskGetTickCount();
      }
      if (sg_limit_reached != 0U ||
          (released != 0U && xTaskGetTickCount() - release_start >=
                            pdMS_TO_TICKS(FAQIUJI_RECORD_DEBOUNCE_MS))) {
        RECORD_CHECK(HAL_TIM_Base_Stop(&sg_timer) == HAL_OK);
        /* Let an in-flight conversion finish before freezing the DMA cursor. */
        vTaskDelay(pdMS_TO_TICKS(2U));
        HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
        HAL_DMA_IRQHandler(&sg_dma);
        remaining = __HAL_DMA_GET_COUNTER(&sg_dma);
        captured = sg_produced * RECORD_HALF +
                   ((RECORD_HALF * 2U - remaining) % RECORD_HALF);
        if (captured > RECORD_COUNT) {
          captured = RECORD_COUNT;
        }
        RECORD_CHECK(HAL_ADC_Stop_DMA(&sg_adc) == HAL_OK);
        stopped = 1U;
      }
    }
    RECORD_CHECK(sg_fault == 0U);
    if (stopped != 0U && g_record_samples >= captured) {
      break;
    }
    if (stopped == 0U && sg_produced == sg_consumed) {
      RECORD_CHECK(xTaskGetTickCount() - wait_start < pdMS_TO_TICKS(500U));
      vTaskDelay(1U);
      continue;
    }
    wait_start = xTaskGetTickCount();
    half = sg_consumed & 1U;
    /* Copy only the completed half; DMA is filling the other half. */
    remaining = __HAL_DMA_GET_COUNTER(&sg_dma);
    RECORD_CHECK(stopped != 0U ||
                 (half == 0U ? remaining <= RECORD_HALF && remaining != 0U :
                               remaining > RECORD_HALF));
    n = (stopped != 0U ? captured : RECORD_COUNT) - g_record_samples;
    if (n > RECORD_HALF) {
      n = RECORD_HALF;
    }
    for (i = 0U; i < n; i++) {
      sg_pcm[i] = (uint8_t)(sg_capture[half * RECORD_HALF + i] >> 4);
    }
    __DMB();
    remaining = __HAL_DMA_GET_COUNTER(&sg_dma);
    RECORD_CHECK(sg_fault == 0U &&
                 (stopped != 0U ||
                  (half == 0U ? remaining <= RECORD_HALF && remaining != 0U :
                                remaining > RECORD_HALF)));
    sg_consumed++;
    for (i = 0U; i < n; i++) {
      ((uint8_t *)sg_page)[used++] = sg_pcm[i];
      if (used == FLASH_PAGE_SIZE) {
        if (address == RECORD_BASE) {
          /* Keep the header erased until the entire recording is saved. */
          memcpy((uint8_t *)sg_first_page + RECORD_HEADER,
                 (uint8_t *)sg_page + RECORD_HEADER,
                 FLASH_PAGE_SIZE - RECORD_HEADER);
        } else {
          RECORD_CHECK(record_page(address, sg_page) == HAL_OK);
        }
        address += FLASH_PAGE_SIZE;
        used = 0U;
        memset(sg_page, 0xFF, sizeof(sg_page));
      }
    }
    g_record_samples += n;
    RECORD_CHECK(sg_fault == 0U &&
                 (ADC1->SR & ADC_FLAG_OVR) == 0U);
  }
  RECORD_CHECK(g_record_samples != 0U);
  if (used != 0U) {
    if (address == RECORD_BASE) {
      memcpy((uint8_t *)sg_first_page + RECORD_HEADER,
             (uint8_t *)sg_page + RECORD_HEADER, used - RECORD_HEADER);
    } else {
      RECORD_CHECK(record_page(address, sg_page) == HAL_OK);
    }
  }
  header[2] = g_record_samples;
  RECORD_CHECK(record_page(RECORD_BASE, sg_first_page) == HAL_OK);
  RECORD_CHECK(HAL_FLASH_Lock() == HAL_OK);
  HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
  return 1U;

failed:
  if (sg_timer.Instance != NULL) {
    HAL_TIM_Base_Stop(&sg_timer);
  }
  if (sg_adc.Instance != NULL && sg_dma.Instance != NULL &&
      sg_dma.State == HAL_DMA_STATE_BUSY) {
    HAL_ADC_Stop_DMA(&sg_adc);
  }
  HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
  HAL_FLASH_Lock();
  return 0U;
#undef RECORD_CHECK
}

void faqiuji_record_task(void *argument)
{
  GPIO_InitTypeDef key = {0};
  (void)argument;
  __HAL_RCC_GPIOC_CLK_ENABLE();
  key.Pin = FAQIUJI_RECORD_KEY_PIN;
  key.Mode = GPIO_MODE_INPUT;
  key.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(FAQIUJI_RECORD_KEY_PORT, &key);
  for (;;) {
    if (record_key_down() == 0U) {
      vTaskDelay(pdMS_TO_TICKS(5U));
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(FAQIUJI_RECORD_DEBOUNCE_MS));
    if (record_key_down() == 0U) {
      continue;
    }
    faqiuji_audio_record_begin();
    g_record_error = 0U;
    g_record_samples = 0U;
    if (record_save() != 0U) {
      g_record_state = 3U;
      faqiuji_audio_record_end(1U);
    } else {
      g_record_state = 4U;
      faqiuji_audio_record_end(0U);
    }
    /* Reaching the limit while held must not start another recording. */
    do {
      while (record_key_down() != 0U) {
        vTaskDelay(pdMS_TO_TICKS(5U));
      }
      vTaskDelay(pdMS_TO_TICKS(FAQIUJI_RECORD_DEBOUNCE_MS));
    } while (record_key_down() != 0U);
  }
}
