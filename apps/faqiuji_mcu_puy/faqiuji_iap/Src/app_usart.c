#include "app_usart.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define APP_USART_BAUDRATE        115200U
#define APP_USART_MAX_PAYLOAD     64U
#define APP_USART_DEBUG_TEXT_SIZE (APP_USART_MAX_PAYLOAD + 1U)
#define APP_USART_FRAME_HEAD      0xAAU
#define APP_USART_FRAME_VERSION   0x01U
#define APP_USART_CMD_DEBUG       0x22U

UART_HandleTypeDef g_app_uart;
static uint8_t sg_sequence;

static uint16_t APP_USART_Crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFFU;
  uint16_t i;
  uint8_t bit;

  for (i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; ++bit) {
      crc = (crc & 1U) != 0U ?
            (uint16_t)((crc >> 1U) ^ 0xA001U) :
            (uint16_t)(crc >> 1U);
    }
  }
  return crc;
}

HAL_StatusTypeDef APP_USART_Init(void)
{
  memset(&g_app_uart, 0, sizeof(g_app_uart));
  g_app_uart.Instance = USART1;
  g_app_uart.Init.BaudRate = APP_USART_BAUDRATE;
  g_app_uart.Init.WordLength = UART_WORDLENGTH_8B;
  g_app_uart.Init.StopBits = UART_STOPBITS_1;
  g_app_uart.Init.Parity = UART_PARITY_NONE;
  g_app_uart.Init.Mode = UART_MODE_TX_RX;
  g_app_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  g_app_uart.Init.OverSampling = UART_OVERSAMPLING_16;
  g_app_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  return HAL_UART_Init(&g_app_uart);
}

HAL_StatusTypeDef APP_USART_Send(const uint8_t *data, uint16_t size)
{
  if (data == NULL || size == 0U) {
    return HAL_OK;
  }
  return HAL_UART_Transmit(&g_app_uart, (uint8_t *)data, size, 1000U);
}

void APP_USART_Debug(const char *format, ...)
{
  uint8_t frame[7U + APP_USART_MAX_PAYLOAD];
  char text[APP_USART_DEBUG_TEXT_SIZE];
  va_list args;
  int length;
  uint16_t crc;
  uint8_t payload_length;

  if (format == NULL || g_app_uart.Instance == NULL) {
    return;
  }

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length < 0) {
    return;
  }
  if (length >= (int)sizeof(text)) {
    length = (int)sizeof(text) - 1;
  }

  payload_length = (uint8_t)length;
  frame[0] = APP_USART_FRAME_HEAD;
  frame[1] = APP_USART_FRAME_VERSION;
  frame[2] = APP_USART_CMD_DEBUG;
  frame[3] = ++sg_sequence;
  frame[4] = payload_length;
  if (payload_length != 0U) {
    memcpy(&frame[5], text, payload_length);
  }

  crc = APP_USART_Crc16(&frame[1], (uint16_t)(4U + payload_length));
  frame[5U + payload_length] = (uint8_t)crc;
  frame[6U + payload_length] = (uint8_t)(crc >> 8U);
  (void)APP_USART_Send(frame, (uint16_t)(7U + payload_length));
}

void APP_USART_DeInit(void)
{
  if (g_app_uart.Instance != NULL) {
    (void)HAL_UART_DeInit(&g_app_uart);
  }
}
