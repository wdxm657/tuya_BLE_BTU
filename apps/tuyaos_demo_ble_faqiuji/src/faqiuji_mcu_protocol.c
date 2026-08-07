#include "string.h"
#include "tal_uart.h"
#include "tal_log.h"
#include "faqiuji_mcu_protocol.h"
#include "tuya_sdk_callback.h"

#define FAQIUJI_MCU_PORT TUYA_UART_NUM_0

typedef struct {
    UINT8_T state;
    UINT8_T cmd;
    UINT8_T seq;
    UINT8_T len;
    UINT8_T pos;
    UINT8_T crc_l;
    UINT8_T payload[FAQIUJI_MCU_MAX_PAYLOAD];
    FAQIUJI_MCU_FRAME_CB cb;
} FAQIUJI_MCU_CTX_T;

STATIC FAQIUJI_MCU_CTX_T sg_mcu = {0};
STATIC UINT8_T sg_mcu_seq = 0;

STATIC UINT16_T faqiuji_crc16(CONST UINT8_T *data, UINT16_T len)
{
    UINT16_T crc = 0xFFFF;
    UINT16_T i;
    UINT8_T bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}

STATIC VOID_T faqiuji_mcu_reset_parser(VOID_T)
{
    sg_mcu.state = 0;
    sg_mcu.pos = 0;
}

STATIC VOID_T faqiuji_mcu_frame_done(UINT8_T crc_l, UINT8_T crc_h)
{
    UINT8_T crc_buf[4 + FAQIUJI_MCU_MAX_PAYLOAD];
    UINT16_T crc;

    crc_buf[0] = FAQIUJI_MCU_FRAME_VERSION;
    crc_buf[1] = sg_mcu.cmd;
    crc_buf[2] = sg_mcu.seq;
    crc_buf[3] = sg_mcu.len;
    memcpy(&crc_buf[4], sg_mcu.payload, sg_mcu.len);
    crc = faqiuji_crc16(crc_buf, (UINT16_T)(4 + sg_mcu.len));
    if ((UINT8_T)crc == crc_l && (UINT8_T)(crc >> 8) == crc_h) {
        FAQIUJI_MCU_FRAME_T frame;
        frame.cmd = sg_mcu.cmd;
        frame.seq = sg_mcu.seq;
        frame.len = sg_mcu.len;
        memcpy(frame.payload, sg_mcu.payload, sg_mcu.len);
        if (sg_mcu.cb != NULL) {
            sg_mcu.cb(&frame);
        }
    } else {
        TAL_PR_WARN("faqiuji mcu crc error");
    }
    faqiuji_mcu_reset_parser();
}

OPERATE_RET faqiuji_mcu_protocol_init(FAQIUJI_MCU_FRAME_CB cb)
{
    memset(&sg_mcu, 0, sizeof(sg_mcu));
    sg_mcu.cb = cb;
    return OPRT_OK;
}

VOID_T faqiuji_mcu_protocol_input(CONST UINT8_T *data, UINT16_T len)
{
    UINT16_T i;
    UINT8_T value;

    if (data == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        value = data[i];
        switch (sg_mcu.state) {
            case 0:
                if (value == FAQIUJI_MCU_FRAME_HEAD) sg_mcu.state = 1;
                break;
            case 1:
                if (value == FAQIUJI_MCU_FRAME_VERSION) sg_mcu.state = 2;
                else faqiuji_mcu_reset_parser();
                break;
            case 2: sg_mcu.cmd = value; sg_mcu.state = 3; break;
            case 3: sg_mcu.seq = value; sg_mcu.state = 4; break;
            case 4:
                sg_mcu.len = value;
                if (sg_mcu.len > FAQIUJI_MCU_MAX_PAYLOAD) faqiuji_mcu_reset_parser();
                else if (sg_mcu.len == 0) sg_mcu.state = 6;
                else sg_mcu.state = 5;
                break;
            case 5:
                sg_mcu.payload[sg_mcu.pos++] = value;
                if (sg_mcu.pos >= sg_mcu.len) sg_mcu.state = 6;
                break;
            case 6: sg_mcu.crc_l = value; sg_mcu.state = 7; break;
            case 7: faqiuji_mcu_frame_done(sg_mcu.crc_l, value); break;
            default: faqiuji_mcu_reset_parser(); break;
        }
    }
}

OPERATE_RET faqiuji_mcu_send(UINT8_T cmd, CONST UINT8_T *payload, UINT8_T len)
{
    UINT8_T frame[7 + FAQIUJI_MCU_MAX_PAYLOAD];
    UINT16_T crc;
    UINT8_T seq;

    if (len > FAQIUJI_MCU_MAX_PAYLOAD || (len != 0 && payload == NULL)) {
        return OPRT_INVALID_PARM;
    }
    seq = ++sg_mcu_seq;
    frame[0] = FAQIUJI_MCU_FRAME_HEAD;
    frame[1] = FAQIUJI_MCU_FRAME_VERSION;
    frame[2] = cmd;
    frame[3] = seq;
    frame[4] = len;
    if (len != 0) memcpy(&frame[5], payload, len);
    frame[5 + len] = 0;
    frame[6 + len] = 0;
    crc = faqiuji_crc16(&frame[1], (UINT16_T)(4 + len));
    frame[5 + len] = (UINT8_T)crc;
    frame[6 + len] = (UINT8_T)(crc >> 8);
    return (tal_uart_write(FAQIUJI_MCU_PORT, frame, (UINT32_T)(7 + len)) < 0) ?
           OPRT_COM_ERROR : OPRT_OK;
}

OPERATE_RET faqiuji_mcu_ping(VOID_T) { return faqiuji_mcu_send(FAQIUJI_MCU_CMD_PING, NULL, 0); }
OPERATE_RET faqiuji_mcu_stop(VOID_T) { return faqiuji_mcu_send(FAQIUJI_MCU_CMD_STOP, NULL, 0); }
OPERATE_RET faqiuji_mcu_status_get(VOID_T) { return faqiuji_mcu_send(FAQIUJI_MCU_CMD_STATUS_GET, NULL, 0); }

OPERATE_RET faqiuji_mcu_launch(UINT8_T speed, UINT8_T angle, UINT16_T interval_ms)
{
    UINT8_T payload[4] = {speed, angle, (UINT8_T)interval_ms, (UINT8_T)(interval_ms >> 8)};
    return faqiuji_mcu_send(FAQIUJI_MCU_CMD_LAUNCH, payload, sizeof(payload));
}
