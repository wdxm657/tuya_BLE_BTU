#ifndef FAQIUJI_MCU_PROTOCOL_H
#define FAQIUJI_MCU_PROTOCOL_H

#include "tuya_cloud_types.h"
#include "tuya_ble_mutli_tsf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_MCU_FRAME_HEAD       0xAA
#define FAQIUJI_MCU_FRAME_VERSION    0x01
#define FAQIUJI_MCU_MAX_PAYLOAD      64
#define FAQIUJI_MCU_RESPONSE_FLAG    0x80
#define FAQIUJI_MCU_CONFIG_SYNC_VERSION 0x01

typedef enum {
    FAQIUJI_MCU_CMD_PING       = 0x01,
    FAQIUJI_MCU_CMD_CONFIG_SET  = 0x14,
    FAQIUJI_MCU_CMD_CONTROL_SET = 0x15,
    FAQIUJI_MCU_CMD_CONFIG_SYNC = 0x16,
    FAQIUJI_MCU_CMD_KEY_EVENT  = 0x20,
    FAQIUJI_MCU_CMD_STATUS_EVENT = 0x21,
    FAQIUJI_MCU_CMD_DEBUG_STRING = 0x22,
} FAQIUJI_MCU_CMD_E;

typedef enum {
    FAQIUJI_MCU_EVENT_KEY = 0x01,
    FAQIUJI_MCU_EVENT_USB = 0x02,
    FAQIUJI_MCU_EVENT_CHARGE = 0x03,
    FAQIUJI_MCU_EVENT_BATTERY = 0x04,
    FAQIUJI_MCU_EVENT_BALL = 0x05,
    FAQIUJI_MCU_EVENT_RADAR = 0x06,
    FAQIUJI_MCU_EVENT_TEMPERATURE = 0x07,
    FAQIUJI_MCU_EVENT_WORK = 0x08,
    FAQIUJI_MCU_EVENT_COUNT = 0x09,
} FAQIUJI_MCU_EVENT_TYPE_E;

typedef struct {
    UINT8_T cmd;
    UINT8_T seq;
    UINT8_T len;
    UINT8_T payload[FAQIUJI_MCU_MAX_PAYLOAD];
} FAQIUJI_MCU_FRAME_T;

OPERATE_RET faqiuji_mcu_protocol_init(VOID_T);
VOID_T faqiuji_mcu_protocol_input(CONST UINT8_T *data, UINT16_T len);
VOID_T faqiuji_mcu_protocol_process(VOID_T);
OPERATE_RET faqiuji_mcu_send(UINT8_T cmd, CONST UINT8_T *payload, UINT8_T len);
OPERATE_RET faqiuji_mcu_send_frame(UINT8_T cmd, UINT8_T seq,
                                   CONST UINT8_T *payload, UINT8_T len);
OPERATE_RET faqiuji_mcu_ping(VOID_T);
OPERATE_RET faqiuji_mcu_ping_periodic(VOID_T);
OPERATE_RET faqiuji_mcu_config_set(UINT8_T mode, UINT16_T max_count,
                                    UINT16_T standby_min);
OPERATE_RET faqiuji_mcu_control_set(BOOL_T enabled);
OPERATE_RET faqiuji_mcu_config_sync(BOOL_T enabled, UINT8_T mode,
                                    UINT16_T max_count,
                                    UINT16_T standby_min);
BOOL_T faqiuji_mcu_is_ping_ok(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
