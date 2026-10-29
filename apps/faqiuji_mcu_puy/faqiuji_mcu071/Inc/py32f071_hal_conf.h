#ifndef PY32F071_HAL_CONF_H
#define PY32F071_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_DAC_MODULE_ENABLED

#define HSI_VALUE              ((uint32_t)8000000U)
#define HSE_VALUE              ((uint32_t)24000000U)
#define LSI_VALUE              ((uint32_t)32768U)
#define LSE_VALUE              ((uint32_t)32768U)
#define HSE_STARTUP_TIMEOUT    200U
#define LSE_STARTUP_TIMEOUT    5000U
#define VDD_VALUE              3300U
#define TICK_INT_PRIORITY      3U
#define USE_RTOS               0U
#define PREFETCH_ENABLE        0U

#define assert_param(expr)     ((void)0U)

#ifdef HAL_MODULE_ENABLED
#include "py32f0xx_hal.h"
#endif

#ifdef HAL_RCC_MODULE_ENABLED
#include "py32f071_hal_rcc.h"
#endif
#ifdef HAL_GPIO_MODULE_ENABLED
#include "py32f071_hal_gpio.h"
#endif
#ifdef HAL_CORTEX_MODULE_ENABLED
#include "py32f071_hal_cortex.h"
#endif
#ifdef HAL_DMA_MODULE_ENABLED
#include "py32f071_hal_dma.h"
#endif
#ifdef HAL_FLASH_MODULE_ENABLED
#include "py32f071_hal_flash.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
#include "py32f071_hal_pwr.h"
#endif
#ifdef HAL_TIM_MODULE_ENABLED
#include "py32f071_hal_tim.h"
#endif
#ifdef HAL_DAC_MODULE_ENABLED
#include "py32f071_hal_dac.h"
#endif

#ifdef __cplusplus
}
#endif

#endif
