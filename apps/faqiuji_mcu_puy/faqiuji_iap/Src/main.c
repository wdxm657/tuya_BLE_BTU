#include "main.h"
#include "app_bootloader.h"
#include "app_usart.h"
#include "app_wdg.h"

static void APP_SystemClockConfig(void)
{
  RCC_OscInitTypeDef oscillator = {0};
  RCC_ClkInitTypeDef clock = {0};

  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSIDiv = RCC_HSI_DIV1;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_24MHz;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  oscillator.PLL.PLLMUL = RCC_PLL_MUL2;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
    NVIC_SystemReset();
  }

  clock.ClockType = RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_1) != HAL_OK) {
    NVIC_SystemReset();
  }
}

int main(void)
{
  uint32_t application;
  uint32_t elapsed;

  HAL_Init();
  APP_WDG_Init();
  APP_SystemClockConfig();
  if (APP_USART_Init() != HAL_OK) {
    while (1) {
    }
  }

  APP_USART_Debug("IAP start");
  application = APP_Bootloader_Select();
  if (application != 0U) {
    APP_Bootloader_Go(application);
  }

  while (1) {
    APP_USART_Debug("IAP waiting for valid APP");
    for (elapsed = 0U; elapsed < 2000U; elapsed += 100U) {
      APP_WDG_Refresh();
      HAL_Delay(100U);
    }
  }
}
