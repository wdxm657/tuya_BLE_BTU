#include "main.h"
#include "faqiuji_audio.h"
#include "faqiuji_record.h"
#include "FreeRTOS.h"
#include "task.h"

static void APP_SystemClockConfig(void);
static void APP_DebugUartInit(void);
static UART_HandleTypeDef sg_debug_uart;

void APP_LogTimestamp(void)
{
  uint32_t milliseconds = HAL_GetTick();
  uint32_t seconds = milliseconds / 1000U;

  printf("[%02lu:%02lu:%02lu.%03lu] ",
         (unsigned long)(seconds / 3600U),
         (unsigned long)((seconds / 60U) % 60U),
         (unsigned long)(seconds % 60U),
         (unsigned long)(milliseconds % 1000U));
}

/* Blocking debug output for task context only, not ADC/DAC interrupts. */
int fputc(int ch, FILE *stream)
{
  uint8_t byte = (uint8_t)ch;
  (void)stream;
  return HAL_UART_Transmit(&sg_debug_uart, &byte, 1U, 20U) == HAL_OK
             ? ch : EOF;
}

static void APP_DebugUartInit(void)
{
  sg_debug_uart.Instance = USART1;
  sg_debug_uart.Init.BaudRate = FAQIUJI_DEBUG_BAUDRATE;
  sg_debug_uart.Init.WordLength = UART_WORDLENGTH_8B;
  sg_debug_uart.Init.StopBits = UART_STOPBITS_1;
  sg_debug_uart.Init.Parity = UART_PARITY_NONE;
  sg_debug_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  sg_debug_uart.Init.Mode = UART_MODE_TX_RX;
  sg_debug_uart.Init.OverSampling = UART_OVERSAMPLING_16;
  sg_debug_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&sg_debug_uart) != HAL_OK) {
    APP_ErrorHandler();
  }
}

int main(void)
{
  HAL_Init();
  APP_SystemClockConfig();
  APP_DebugUartInit();
  APP_LOG("F071 audio/record ready; USART1 PB8/PB9 AF9 115200\r\n");
  faqiuji_audio_init();

  if (xTaskCreate(faqiuji_record_task, "record", 256, NULL, 2, NULL) !=
      pdPASS) {
    APP_ErrorHandler();
  }
  if (xTaskCreate(faqiuji_audio_task, "audio", 192, NULL, 1, NULL) !=
      pdPASS) {
    APP_ErrorHandler();
  }

  vTaskStartScheduler();
  APP_ErrorHandler();
  return 0;
}

static void APP_SystemClockConfig(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clock = {0};

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState = RCC_HSI_ON;
  osc.HSIDiv = RCC_HSI_DIV1;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_8MHz;
  osc.HSEState = RCC_HSE_OFF;
  osc.LSIState = RCC_LSI_OFF;
  osc.LSEState = RCC_LSE_OFF;
  osc.PLL.PLLState = RCC_PLL_OFF;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    APP_ErrorHandler();
  }

  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSISYS;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_0) != HAL_OK) {
    APP_ErrorHandler();
  }
}

void APP_ErrorHandler(void)
{
  __disable_irq();
  for (;;) {
  }
}
