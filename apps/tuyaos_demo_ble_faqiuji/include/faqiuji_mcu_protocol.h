#ifndef FAQIUJI_MCU_PROTOCOL_H
#define FAQIUJI_MCU_PROTOCOL_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_MCU_FRAME_HEAD       0xAA
#define FAQIUJI_MCU_FRAME_VERSION    0x01
#define FAQIUJI_MCU_MAX_PAYLOAD      64

typedef enum {
    FAQIUJI_MCU_CMD_PING       = 0x01,
    FAQIUJI_MCU_CMD_LAUNCH     = 0x10,
    FAQIUJI_MCU_CMD_STOP       = 0x11,
    FAQIUJI_MCU_CMD_STATUS_GET = 0x12,
    FAQIUJI_MCU_CMD_PARAM_SET  = 0x13,
    FAQIUJI_MCU_CMD_CONFIG_SET  = 0x14,
    FAQIUJI_MCU_CMD_CONTROL_SET = 0x15,
    FAQIUJI_MCU_CMD_KEY_EVENT  = 0x20,
    FAQIUJI_MCU_CMD_STATUS_EVENT = 0x21,
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
OPERATE_RET faqiuji_mcu_ping(VOID_T);
OPERATE_RET faqiuji_mcu_launch(UINT8_T speed, UINT8_T angle, UINT16_T interval_ms);
OPERATE_RET faqiuji_mcu_launch_mode(UINT8_T mode);
OPERATE_RET faqiuji_mcu_config_set(UINT8_T mode, UINT16_T max_count,
                                    UINT16_T standby_min);
OPERATE_RET faqiuji_mcu_control_set(BOOL_T enabled);
OPERATE_RET faqiuji_mcu_stop(VOID_T);
OPERATE_RET faqiuji_mcu_status_get(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
