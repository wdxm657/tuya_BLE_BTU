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
} FAQIUJI_MCU_CMD_E;

typedef struct {
    UINT8_T cmd;
    UINT8_T seq;
    UINT8_T len;
    UINT8_T payload[FAQIUJI_MCU_MAX_PAYLOAD];
} FAQIUJI_MCU_FRAME_T;

typedef VOID_T (*FAQIUJI_MCU_FRAME_CB)(CONST FAQIUJI_MCU_FRAME_T *frame);

OPERATE_RET faqiuji_mcu_protocol_init(FAQIUJI_MCU_FRAME_CB cb);
VOID_T faqiuji_mcu_protocol_input(CONST UINT8_T *data, UINT16_T len);
OPERATE_RET faqiuji_mcu_send(UINT8_T cmd, CONST UINT8_T *payload, UINT8_T len);
OPERATE_RET faqiuji_mcu_ping(VOID_T);
OPERATE_RET faqiuji_mcu_launch(UINT8_T speed, UINT8_T angle, UINT16_T interval_ms);
OPERATE_RET faqiuji_mcu_stop(VOID_T);
OPERATE_RET faqiuji_mcu_status_get(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
