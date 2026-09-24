/**
  ******************************************************************************
  * @file    py32f040_hal_msp.c
  * @author  MCU Application Team
  * @brief   This file provides code for the MSP Initialization
  *          and de-Initialization codes.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2023 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Puya under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* External functions --------------------------------------------------------*/

/**
  * @brief Initialize the MSP
  */
void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();
}

/**
  * @brief Initialize USART related MSP
  * @param  huart：USART handle
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef  GPIO_InitStruct = {0};

  if (huart->Instance == USART1)
  {
    /* Enable USART1 clock */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    /* Initialize GPIO
    PB8     ------> USART1_TX
    PB9     ------> USART1_RX
    */
    GPIO_InitStruct.Pin       = GPIO_PIN_8;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF9_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Alternate = GPIO_AF9_USART1;
    GPIO_InitStruct.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    /* Enable USART1 interrupt */
    HAL_NVIC_SetPriority(USART1_IRQn, 0, 1);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
  }
}

void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
  GPIO_InitTypeDef gpio = {0};

  if (htim->Instance == TIM2) {
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = MOTOR_PWM;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF2_TIM2;
    HAL_GPIO_Init(GPIOA, &gpio);
  } else if (htim->Instance == TIM1) {
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = IE_PWM;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF2_TIM1;
    HAL_GPIO_Init(GPIOA, &gpio);
  }
}

void HAL_TIM_PWM_MspDeInit(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2) {
    HAL_GPIO_DeInit(GPIOA, MOTOR_PWM);
    __HAL_RCC_TIM2_CLK_DISABLE();
  } else if (htim->Instance == TIM1) {
    HAL_GPIO_DeInit(GPIOA, IE_PWM);
    __HAL_RCC_TIM1_CLK_DISABLE();
  }
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1) {
    RCC_PeriphCLKInitTypeDef clock_config = {0};

    __HAL_RCC_ADC_CLK_ENABLE();
    clock_config.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    clock_config.ADCClockSelection = RCC_ADCCLKSOURCE_PCLK_DIV4;
    if (HAL_RCCEx_PeriphCLKConfig(&clock_config) != HAL_OK) {
      APP_ErrorHandler();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    {
      GPIO_InitTypeDef gpio = {0};
      gpio.Mode = GPIO_MODE_ANALOG;
      gpio.Pull = GPIO_NOPULL;
      gpio.Pin = AD_BAT | AD_I_SHUNT;
      HAL_GPIO_Init(GPIOA, &gpio);
      gpio.Pin = AD_NTC;
      HAL_GPIO_Init(GPIOB, &gpio);
    }
  }
}

/************************ (C) COPYRIGHT Puya *****END OF FILE******************/
