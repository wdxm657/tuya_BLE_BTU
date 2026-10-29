#include "main.h"
#include "faqiuji_audio.h"
#include "FreeRTOS.h"
#include "task.h"

static void APP_SystemClockConfig(void);

int main(void)
{
  HAL_Init();
  APP_SystemClockConfig();
  faqiuji_audio_init();

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
