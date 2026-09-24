#include "py32f0xx_hal.h"
#include "py32f040_hal_tim.h"

static TIM_HandleTypeDef sg_timebase_tim;

HAL_StatusTypeDef HAL_InitTick(uint32_t tick_priority)
{
  RCC_ClkInitTypeDef clock_config = {0};
  uint32_t flash_latency = 0U;
  uint32_t timer_clock;
  uint32_t prescaler;

  HAL_NVIC_SetPriority(TIM14_IRQn, tick_priority, 0U);
  HAL_NVIC_EnableIRQ(TIM14_IRQn);
  __HAL_RCC_TIM14_CLK_ENABLE();
  HAL_RCC_GetClockConfig(&clock_config, &flash_latency);

  timer_clock = HAL_RCC_GetPCLK1Freq();
  if (clock_config.APB1CLKDivider != RCC_HCLK_DIV1) {
    timer_clock *= 2U;
  }
  prescaler = (timer_clock / 1000000U) - 1U;

  sg_timebase_tim.Instance = TIM14;
  sg_timebase_tim.Init.Prescaler = prescaler;
  sg_timebase_tim.Init.Period = 999U;
  sg_timebase_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  sg_timebase_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  sg_timebase_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  if (HAL_TIM_Base_Init(&sg_timebase_tim) != HAL_OK) {
    return HAL_ERROR;
  }
  return HAL_TIM_Base_Start_IT(&sg_timebase_tim);
}

void HAL_SuspendTick(void)
{
  __HAL_TIM_DISABLE_IT(&sg_timebase_tim, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
  __HAL_TIM_ENABLE_IT(&sg_timebase_tim, TIM_IT_UPDATE);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM14) {
    HAL_IncTick();
  }
}

void TIM14_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&sg_timebase_tim);
}
