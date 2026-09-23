#include "faqiuji_protocol.h"
#include "gpio_config.h"
#include "faqiuji_launcher.h"
#include "faqiuji_device.h"

#include <string.h>

typedef enum
{
  FAQIUJI_RX_WAIT_HEAD = 0,
  FAQIUJI_RX_WAIT_VERSION,
  FAQIUJI_RX_WAIT_CMD,
  FAQIUJI_RX_WAIT_SEQ,
  FAQIUJI_RX_WAIT_LEN,
  FAQIUJI_RX_PAYLOAD,
  FAQIUJI_RX_CRC_LOW,
  FAQIUJI_RX_CRC_HIGH,
} FAQIUJI_RX_STATE_E;

typedef struct
{
  UART_HandleTypeDef *huart;
  FAQIUJI_RX_STATE_E state;
  FAQIUJI_FRAME_T frame;
  uint8_t payload_pos;
  uint8_t crc_low;
  uint8_t work_state;
  uint8_t speed;
  uint8_t angle;
  uint16_t interval_ms;
} FAQIUJI_PROTOCOL_CTX_T;

static FAQIUJI_PROTOCOL_CTX_T sg_protocol;
static uint8_t sg_event_seq;

static uint16_t faqiuji_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFU;
  uint16_t i;
  uint8_t bit;

  for (i = 0; i < len; i++) {
    crc ^= data[i];
    for (bit = 0; bit < 8U; bit++) {
      crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0xA001U)
                       : (uint16_t)(crc >> 1U);
    }
  }
  return crc;
}

static void faqiuji_protocol_reset_parser(void)
{
  sg_protocol.state = FAQIUJI_RX_WAIT_HEAD;
  sg_protocol.payload_pos = 0U;
}

static void faqiuji_protocol_reply(uint8_t cmd, uint8_t seq,
                                   FAQIUJI_STATUS_E status)
{
  uint8_t payload[1] = {(uint8_t)status};
  faqiuji_protocol_send((uint8_t)(cmd | FAQIUJI_RESPONSE_FLAG),
                        seq, payload, sizeof(payload));
}

static void faqiuji_protocol_handle_frame(void)
{
  const FAQIUJI_FRAME_T *frame = &sg_protocol.frame;
  uint8_t status = FAQIUJI_STATUS_OK;
  static int flag = 0;
  switch (frame->cmd) {
    case FAQIUJI_CMD_PING:
      if (frame->len != 0U) {
        status = FAQIUJI_STATUS_BAD_LENGTH;
      }
      flag = !flag;
      HAL_GPIO_WritePin(GPIOA,  LED_MIDDLE, flag? GPIO_PIN_RESET : GPIO_PIN_SET);

      faqiuji_protocol_reply(frame->cmd, frame->seq, (FAQIUJI_STATUS_E)status);
      break;

    case FAQIUJI_CMD_LAUNCH:
      if (frame->len == 1U) {
        if (frame->payload[0] > FAQIUJI_LAUNCH_MODE_RANDOM) {
          status = FAQIUJI_STATUS_BAD_PAYLOAD;
        } else if (faqiuji_launcher_start(
                     (FAQIUJI_LAUNCH_MODE_E)frame->payload[0]) == 0U) {
          status = FAQIUJI_STATUS_BUSY;
        } else {
          sg_protocol.work_state = FAQIUJI_WORK_RUNNING;
        }
      } else if (frame->len != 4U) {
        status = FAQIUJI_STATUS_BAD_LENGTH;
      } else {
        sg_protocol.speed = frame->payload[0];
        sg_protocol.angle = frame->payload[1];
        sg_protocol.interval_ms = (uint16_t)frame->payload[2] |
                                  ((uint16_t)frame->payload[3] << 8U);
        sg_protocol.work_state = FAQIUJI_WORK_RUNNING;
      }
      faqiuji_protocol_reply(frame->cmd, frame->seq, (FAQIUJI_STATUS_E)status);
      break;

    case FAQIUJI_CMD_STOP:
      if (frame->len != 0U) {
        status = FAQIUJI_STATUS_BAD_LENGTH;
      } else {
        faqiuji_launcher_stop();
        sg_protocol.work_state = FAQIUJI_WORK_STOPPED;
      }
      faqiuji_protocol_reply(frame->cmd, frame->seq, (FAQIUJI_STATUS_E)status);
      break;

    case FAQIUJI_CMD_STATUS_GET:
      if (frame->len != 0U) {
        faqiuji_protocol_reply(frame->cmd, frame->seq,
                               FAQIUJI_STATUS_BAD_LENGTH);
      } else {
        uint8_t payload[6];
        payload[0] = FAQIUJI_STATUS_OK;
        payload[1] = sg_protocol.work_state;
        payload[2] = sg_protocol.speed;
        payload[3] = sg_protocol.angle;
        payload[4] = (uint8_t)sg_protocol.interval_ms;
        payload[5] = (uint8_t)(sg_protocol.interval_ms >> 8U);
        faqiuji_protocol_send((uint8_t)(frame->cmd | FAQIUJI_RESPONSE_FLAG),
                              frame->seq, payload, sizeof(payload));
      }
      break;

    case FAQIUJI_CMD_PARAM_SET:
      if (frame->len != 4U) {
        status = FAQIUJI_STATUS_BAD_LENGTH;
      } else {
        sg_protocol.speed = frame->payload[0];
        sg_protocol.angle = frame->payload[1];
        sg_protocol.interval_ms = (uint16_t)frame->payload[2] |
                                  ((uint16_t)frame->payload[3] << 8U);
      }
      faqiuji_protocol_reply(frame->cmd, frame->seq, (FAQIUJI_STATUS_E)status);
      break;

    case FAQIUJI_CMD_CONFIG_SET:
      if (frame->len != 5U || frame->payload[0] > FAQIUJI_LAUNCH_MODE_RANDOM) {
        status = FAQIUJI_STATUS_BAD_PAYLOAD;
      } else {
        uint16_t max_count = (uint16_t)frame->payload[1] |
                             ((uint16_t)frame->payload[2] << 8U);
        uint16_t standby_min = (uint16_t)frame->payload[3] |
                               ((uint16_t)frame->payload[4] << 8U);
        faqiuji_device_set_config(frame->payload[0], max_count, standby_min);
      }
      faqiuji_protocol_reply(frame->cmd, frame->seq, (FAQIUJI_STATUS_E)status);
      break;

    case FAQIUJI_CMD_CONTROL_SET:
      if (frame->len != 1U) {
        status = FAQIUJI_STATUS_BAD_LENGTH;
      } else {
        faqiuji_device_set_enabled(frame->payload[0] != 0U);
      }
      faqiuji_protocol_reply(frame->cmd, frame->seq, (FAQIUJI_STATUS_E)status);
      break;

    default:
      faqiuji_protocol_reply(frame->cmd, frame->seq,
                             FAQIUJI_STATUS_UNKNOWN_CMD);
      break;
  }
}

static void faqiuji_protocol_frame_done(uint8_t crc_high)
{
  uint8_t crc_data[4U + FAQIUJI_MAX_PAYLOAD];
  uint16_t expected_crc;
  uint16_t received_crc;

  crc_data[0] = FAQIUJI_FRAME_VERSION;
  crc_data[1] = sg_protocol.frame.cmd;
  crc_data[2] = sg_protocol.frame.seq;
  crc_data[3] = sg_protocol.frame.len;
  memcpy(&crc_data[4], sg_protocol.frame.payload, sg_protocol.frame.len);

  expected_crc = faqiuji_crc16(crc_data,
                               (uint16_t)(4U + sg_protocol.frame.len));
  received_crc = (uint16_t)sg_protocol.crc_low |
                 ((uint16_t)crc_high << 8U);

  if (expected_crc == received_crc) {
    faqiuji_protocol_handle_frame();
  }
  faqiuji_protocol_reset_parser();
}

void faqiuji_protocol_init(UART_HandleTypeDef *huart)
{
  memset(&sg_protocol, 0, sizeof(sg_protocol));
  sg_protocol.huart = huart;
  sg_protocol.state = FAQIUJI_RX_WAIT_HEAD;
  sg_protocol.work_state = FAQIUJI_WORK_STOPPED;
  sg_event_seq = 0U;
}

void faqiuji_protocol_input(uint8_t value)
{
  switch (sg_protocol.state) {
    case FAQIUJI_RX_WAIT_HEAD:
      if (value == FAQIUJI_FRAME_HEAD) {
        sg_protocol.state = FAQIUJI_RX_WAIT_VERSION;
      }
      break;

    case FAQIUJI_RX_WAIT_VERSION:
      if (value == FAQIUJI_FRAME_VERSION) {
        sg_protocol.state = FAQIUJI_RX_WAIT_CMD;
      } else {
        faqiuji_protocol_reset_parser();
      }
      break;

    case FAQIUJI_RX_WAIT_CMD:
      sg_protocol.frame.cmd = value;
      sg_protocol.state = FAQIUJI_RX_WAIT_SEQ;
      break;

    case FAQIUJI_RX_WAIT_SEQ:
      sg_protocol.frame.seq = value;
      sg_protocol.state = FAQIUJI_RX_WAIT_LEN;
      break;

    case FAQIUJI_RX_WAIT_LEN:
      sg_protocol.frame.len = value;
      sg_protocol.payload_pos = 0U;
      if (value > FAQIUJI_MAX_PAYLOAD) {
        faqiuji_protocol_reset_parser();
      } else if (value == 0U) {
        sg_protocol.state = FAQIUJI_RX_CRC_LOW;
      } else {
        sg_protocol.state = FAQIUJI_RX_PAYLOAD;
      }
      break;

    case FAQIUJI_RX_PAYLOAD:
      sg_protocol.frame.payload[sg_protocol.payload_pos++] = value;
      if (sg_protocol.payload_pos >= sg_protocol.frame.len) {
        sg_protocol.state = FAQIUJI_RX_CRC_LOW;
      }
      break;

    case FAQIUJI_RX_CRC_LOW:
      sg_protocol.crc_low = value;
      sg_protocol.state = FAQIUJI_RX_CRC_HIGH;
      break;

    case FAQIUJI_RX_CRC_HIGH:
      faqiuji_protocol_frame_done(value);
      break;

    default:
      faqiuji_protocol_reset_parser();
      break;
  }
}

void faqiuji_protocol_send(uint8_t cmd, uint8_t seq,
                           const uint8_t *payload, uint8_t len)
{
  uint8_t tx[7U + FAQIUJI_MAX_PAYLOAD];
  uint16_t crc;

  if (sg_protocol.huart == NULL || len > FAQIUJI_MAX_PAYLOAD ||
      (len != 0U && payload == NULL)) {
    return;
  }

  tx[0] = FAQIUJI_FRAME_HEAD;
  tx[1] = FAQIUJI_FRAME_VERSION;
  tx[2] = cmd;
  tx[3] = seq;
  tx[4] = len;
  if (len != 0U) {
    memcpy(&tx[5], payload, len);
  }

  crc = faqiuji_crc16(&tx[1], (uint16_t)(4U + len));
  tx[5U + len] = (uint8_t)crc;
  tx[6U + len] = (uint8_t)(crc >> 8U);

  /* All protocol sends are serialized in the main loop. */
  (void)HAL_UART_Transmit(sg_protocol.huart, tx, (uint16_t)(7U + len),
                          100U);
}

void faqiuji_protocol_key_event(uint8_t pressed)
{
  uint8_t payload = pressed ? 1U : 0U;
  if (sg_protocol.huart == NULL) {
    return;
  }
  HAL_GPIO_WritePin(GPIOA,  LED_HIGH, pressed? GPIO_PIN_RESET : GPIO_PIN_SET);

  faqiuji_protocol_send(FAQIUJI_CMD_KEY_EVENT, ++sg_event_seq, &payload, 1U);
}

void faqiuji_protocol_status_event(FAQIUJI_EVENT_TYPE_E type,
                                   const uint8_t *data, uint8_t len)
{
  uint8_t payload[FAQIUJI_MAX_PAYLOAD];

  if (len > (FAQIUJI_MAX_PAYLOAD - 1U)) {
    return;
  }
  payload[0] = (uint8_t)type;
  if (len != 0U && data != NULL) {
    memcpy(&payload[1], data, len);
  }
  faqiuji_protocol_send(FAQIUJI_CMD_STATUS_EVENT, ++sg_event_seq,
                        payload, (uint8_t)(len + 1U));
}
