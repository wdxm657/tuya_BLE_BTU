#ifndef __APP_USART_H
#define __APP_USART_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

extern UART_HandleTypeDef g_app_uart;

HAL_StatusTypeDef APP_USART_Init(void);
HAL_StatusTypeDef APP_USART_Send(const uint8_t *data, uint16_t size);
void APP_USART_Debug(const char *format, ...);
void APP_USART_DeInit(void);

#ifdef __cplusplus
}
#endif

#endif
