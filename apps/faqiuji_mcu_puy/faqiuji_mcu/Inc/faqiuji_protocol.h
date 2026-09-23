#ifndef FAQIUJI_PROTOCOL_H
#define FAQIUJI_PROTOCOL_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_FRAME_HEAD          0xAAU
#define FAQIUJI_FRAME_VERSION       0x01U
#define FAQIUJI_MAX_PAYLOAD         64U
#define FAQIUJI_RESPONSE_FLAG       0x80U

typedef enum
{
  FAQIUJI_CMD_PING       = 0x01,
  FAQIUJI_CMD_LAUNCH     = 0x10,
  FAQIUJI_CMD_STOP       = 0x11,
  FAQIUJI_CMD_STATUS_GET = 0x12,
  FAQIUJI_CMD_PARAM_SET  = 0x13,
  FAQIUJI_CMD_KEY_EVENT  = 0x20,
} FAQIUJI_CMD_E;

typedef enum
{
  FAQIUJI_STATUS_OK           = 0x00,
  FAQIUJI_STATUS_BAD_LENGTH   = 0x01,
  FAQIUJI_STATUS_BAD_PAYLOAD  = 0x02,
  FAQIUJI_STATUS_UNKNOWN_CMD  = 0x03,
  FAQIUJI_STATUS_BUSY         = 0x04,
} FAQIUJI_STATUS_E;

typedef enum
{
  FAQIUJI_WORK_STOPPED = 0x00,
  FAQIUJI_WORK_RUNNING = 0x01,
} FAQIUJI_WORK_STATE_E;

typedef struct
{
  uint8_t cmd;
  uint8_t seq;
  uint8_t len;
  uint8_t payload[FAQIUJI_MAX_PAYLOAD];
} FAQIUJI_FRAME_T;

void faqiuji_protocol_init(UART_HandleTypeDef *huart);
void faqiuji_protocol_input(uint8_t value);
void faqiuji_protocol_send(uint8_t cmd, uint8_t seq,
                           const uint8_t *payload, uint8_t len);
void faqiuji_protocol_key_event(uint8_t pressed);

#ifdef __cplusplus
}
#endif

#endif
