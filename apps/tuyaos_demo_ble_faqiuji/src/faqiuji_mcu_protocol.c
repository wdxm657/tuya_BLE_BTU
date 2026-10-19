#include "string.h"
#include "tal_uart.h"
#include "tal_log.h"
#include "tkl_system.h"
#include "tuya_ble_api.h"
#include "tuya_sdk_callback.h"
#include "faqiuji_mcu_protocol.h"
#include "app_dp_parser.h"

#define FAQIUJI_MCU_PORT TUYA_UART_NUM_0
#define FAQIUJI_MCU_FRAME_QUEUE_SIZE 4U
#define FAQIUJI_KEY_LONG_PRESS_MS 3000UL

typedef struct {
    UINT8_T state;
    UINT8_T cmd;
    UINT8_T seq;
    UINT8_T len;
    UINT8_T pos;
    UINT8_T crc_l;
    UINT8_T payload[FAQIUJI_MCU_MAX_PAYLOAD];
} FAQIUJI_MCU_CTX_T;

STATIC FAQIUJI_MCU_CTX_T sg_mcu = {0};
STATIC UINT8_T sg_mcu_seq = 0;
STATIC FAQIUJI_MCU_FRAME_T sg_frame_queue[FAQIUJI_MCU_FRAME_QUEUE_SIZE];
STATIC volatile UINT8_T sg_frame_queue_read = 0;
STATIC volatile UINT8_T sg_frame_queue_write = 0;
STATIC volatile UINT8_T sg_frame_queue_count = 0;
STATIC volatile BOOL_T sg_mcu_heartbeat = FALSE;
STATIC UINT8_T sg_mcu_ping_age = 0U;
STATIC BOOL_T sg_mcu_sync_pending = FALSE;
STATIC BOOL_T sg_key_pressed = FALSE;
STATIC UINT32_T sg_key_press_ms;
STATIC BOOL_T sg_factory_reset_pending = FALSE;
STATIC volatile BOOL_T sg_response_valid = FALSE;
STATIC volatile FAQIUJI_MCU_FRAME_T sg_response_frame;

STATIC VOID_T faqiuji_mcu_key_event(UINT8_T pressed)
{
    UINT32_T now = tkl_system_get_millisecond();
    UINT32_T duration;

    if (pressed != 0U) {
        if (!sg_key_pressed) {
            sg_key_pressed = TRUE;
            sg_key_press_ms = now;
            TAL_PR_INFO("MCU KEY: pressed");
        }
        return;
    }
    if (!sg_key_pressed) {
        return;
    }
    sg_key_pressed = FALSE;
    duration = now - sg_key_press_ms;
    TAL_PR_INFO("MCU KEY: released after %d ms", (unsigned long)duration);
    if (duration >= FAQIUJI_KEY_LONG_PRESS_MS) {
        sg_factory_reset_pending = TRUE;
    }
}

STATIC VOID_T faqiuji_mcu_frame_cb(CONST FAQIUJI_MCU_FRAME_T *frame)
{
    CHAR_T debug_text[FAQIUJI_MCU_MAX_PAYLOAD + 1U];
    UINT8_T event_type;
    UINT8_T value;
    UINT16_T value16;
    UINT8_T work_state;

    if (frame == NULL) {
        return;
    }

    if (frame->cmd == (FAQIUJI_MCU_CMD_PING |
                       FAQIUJI_MCU_RESPONSE_FLAG)) {
        BOOL_T was_heartbeat = sg_mcu_heartbeat;
        sg_mcu_heartbeat = (frame->len == 1U &&
                            frame->payload[0] == 0U);
        sg_mcu_ping_age = 0U;
        // TAL_PR_INFO("MCU PING response: heartbeat=%u status=%u",
        //             sg_mcu_heartbeat,
        //             frame->len ? frame->payload[0] : 0xFFU);
        if (!was_heartbeat && sg_mcu_heartbeat) {
            sg_mcu_sync_pending = TRUE;
        }
        return;
    }

    if (frame->cmd == FAQIUJI_MCU_CMD_DEBUG_STRING) {
        memcpy(debug_text, frame->payload, frame->len);
        debug_text[frame->len] = '\0';
        TAL_PR_INFO("MCU DEBUG: %s", debug_text);
        // if (frame->len != 0U) {
        //     (VOID_T)app_dp_report(DP_ID_LOG_UPLOAD,
        //                          (UINT8_T *)debug_text, frame->len);
        // }
        return;
    }
    // TAL_PR_INFO("MCU FRAME: cmd=0x%02x seq=%u len=%u",
    //             frame->cmd, frame->seq, frame->len);

    if (frame->cmd == FAQIUJI_MCU_CMD_KEY_EVENT) {
        if (frame->len != 1U || frame->payload[0] > 1U) {
            TAL_PR_WARN("MCU KEY: invalid payload len=%u", frame->len);
            return;
        }
        faqiuji_mcu_key_event(frame->payload[0]);
        return;
    }

    if (frame->cmd != FAQIUJI_MCU_CMD_STATUS_EVENT) {
        return;
    }

    event_type = frame->payload[0];
    value = frame->len > 1U ? frame->payload[1] : 0U;
    switch (event_type) {
        case FAQIUJI_MCU_EVENT_USB:
            TAL_PR_INFO("MCU USB: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_CHARGE:
            TAL_PR_INFO("MCU CHARGE: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_BATTERY:
        {
            UINT32_T battery_value;
            UINT8_T battery_dp[4];
            OPERATE_RET report_ret;

            if (frame->len != 2U) {
                TAL_PR_WARN("MCU BATTERY invalid len=%u", frame->len);
                break;
            }
            value = frame->payload[1] > 100U ? 100U : frame->payload[1];
            battery_value = value;
            /* Tuya VALUE DP payloads use network byte order. */
            battery_dp[0] = (UINT8_T)(battery_value >> 24U);
            battery_dp[1] = (UINT8_T)(battery_value >> 16U);
            battery_dp[2] = (UINT8_T)(battery_value >> 8U);
            battery_dp[3] = (UINT8_T)battery_value;
            report_ret = app_dp_report(DP_ID_BATTERY, battery_dp, 4U);
            TAL_PR_INFO("MCU BATTERY: %u%% dp_report_ret=%d",
                        value, report_ret);
            break;
        }
        case FAQIUJI_MCU_EVENT_BALL:
            TAL_PR_INFO("MCU BALL: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_RADAR:
        {
            UINT8_T radar_dp = value ? 0U : 1U;
            TAL_PR_INFO("MCU RADAR: %d dp=%u", value, radar_dp);
            (VOID_T)app_dp_report(DP_ID_RADAR_SENSE, &radar_dp, 1U);
            break;
        }
        case FAQIUJI_MCU_EVENT_TEMPERATURE:
            value16 = frame->len >= 3U ?
                      ((UINT16_T)frame->payload[1] |
                       ((UINT16_T)frame->payload[2] << 8U)) : 0U;
            TAL_PR_INFO("MCU NTC RAW: %u", value16);
            break;
        case FAQIUJI_MCU_EVENT_WORK:
            work_state = value;
            TAL_PR_INFO("MCU WORK STATE: %d", work_state);
            if (work_state == 4U) {
                UINT8_T s_work_state = 1;
                UINT8_T switch_state = 0U;
                (VOID_T)app_dp_report(DP_ID_WORK_STATE, &s_work_state, 1U);
                (VOID_T)app_dp_report(DP_ID_SWITCH, &switch_state, 1U);
                app_dp_set_work_state(work_state);
            } else if (work_state <= 3U) {
                (VOID_T)app_dp_report(DP_ID_WORK_STATE, &work_state, 1U);
                app_dp_set_work_state(work_state);
            } else {
                TAL_PR_WARN("MCU WORK STATE invalid: %u", work_state);
            }
            break;
        case FAQIUJI_MCU_EVENT_COUNT:
            value16 = frame->len >= 3U ?
                      ((UINT16_T)frame->payload[1] |
                       ((UINT16_T)frame->payload[2] << 8U)) : 0U;
            TAL_PR_INFO("MCU LAUNCH COUNT: %u", value16);
            break;
        default:
            TAL_PR_WARN("MCU EVENT UNKNOWN: %u", event_type);
            break;
    }
}

OPERATE_RET faqiuji_mcu_protocol_init(VOID_T)
{
    memset(&sg_mcu, 0, sizeof(sg_mcu));
    sg_mcu_seq = 0U;
    sg_frame_queue_read = 0U;
    sg_frame_queue_write = 0U;
    sg_frame_queue_count = 0U;
    sg_mcu_heartbeat = FALSE;
    sg_mcu_ping_age = 0U;
    sg_mcu_sync_pending = FALSE;
    sg_key_pressed = FALSE;
    sg_key_press_ms = 0U;
    sg_factory_reset_pending = FALSE;
    sg_response_valid = FALSE;
    memset((VOID_T *)&sg_response_frame, 0, sizeof(sg_response_frame));
    return OPRT_OK;
}

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
        if ((sg_mcu.cmd & FAQIUJI_MCU_RESPONSE_FLAG) != 0U) {
            sg_response_frame.cmd = sg_mcu.cmd;
            sg_response_frame.seq = sg_mcu.seq;
            sg_response_frame.len = sg_mcu.len;
            memcpy((VOID_T *)sg_response_frame.payload,
                   sg_mcu.payload, sg_mcu.len);
            sg_response_valid = TRUE;
        }
        if (sg_frame_queue_count < FAQIUJI_MCU_FRAME_QUEUE_SIZE) {
            FAQIUJI_MCU_FRAME_T *frame =
                &sg_frame_queue[sg_frame_queue_write];
            frame->cmd = sg_mcu.cmd;
            frame->seq = sg_mcu.seq;
            frame->len = sg_mcu.len;
            memcpy(frame->payload, sg_mcu.payload, sg_mcu.len);
            sg_frame_queue_write++;
            if (sg_frame_queue_write >= FAQIUJI_MCU_FRAME_QUEUE_SIZE) {
                sg_frame_queue_write = 0;
            }
            sg_frame_queue_count++;
        }
    } else {
        TAL_PR_WARN("faqiuji mcu crc error");
    }
    faqiuji_mcu_reset_parser();
}

VOID_T faqiuji_mcu_protocol_process(VOID_T)
{
    FAQIUJI_MCU_FRAME_T frame;

    while (sg_frame_queue_count > 0U) {
        frame = sg_frame_queue[sg_frame_queue_read];
        sg_frame_queue_read++;
        if (sg_frame_queue_read >= FAQIUJI_MCU_FRAME_QUEUE_SIZE) {
            sg_frame_queue_read = 0;
        }
        sg_frame_queue_count--;
        faqiuji_mcu_frame_cb(&frame);
    }

    if (sg_factory_reset_pending) {
        OPERATE_RET reset_ret;
        tuya_ble_status_t unbind_ret;

        sg_factory_reset_pending = FALSE;
        sg_mcu_sync_pending = FALSE;
        sg_key_pressed = FALSE;
        TAL_PR_INFO("MCU KEY: long press release -> factory reset/pairing");
        reset_ret = app_dp_reset_to_defaults();
        if (reset_ret != OPRT_OK) {
            TAL_PR_ERR("FACTORY RESET: defaults/sync failed=%d", reset_ret);
            return;
        }
        if (tuya_app_get_conn_handle() != 0xFFFFU) {
            tuya_ble_gap_disconnect();
        }
        unbind_ret = tuya_ble_device_unbind();
        TAL_PR_INFO("FACTORY RESET: unbind request ret=%d", unbind_ret);
        return;
    }

    /*
     * Do not transmit CONFIG_SET/CONTROL_SET from inside the PING response
     * callback. Complete the current RX queue first, then start a new
     * protocol transaction from the main loop.
     */
    if (sg_mcu_sync_pending) {
        sg_mcu_sync_pending = FALSE;
        if (sg_mcu_heartbeat) {
            (VOID_T)app_dp_sync_to_mcu();
        }
    }
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
            case 7:
                faqiuji_mcu_frame_done(sg_mcu.crc_l, value);
                break;
            default: faqiuji_mcu_reset_parser(); break;
        }
    }
}

OPERATE_RET faqiuji_mcu_send(UINT8_T cmd, CONST UINT8_T *payload, UINT8_T len)
{
    return faqiuji_mcu_send_frame(cmd, ++sg_mcu_seq, payload, len);
}

OPERATE_RET faqiuji_mcu_send_frame(UINT8_T cmd, UINT8_T seq,
                                   CONST UINT8_T *payload, UINT8_T len)
{
    UINT8_T frame[7 + FAQIUJI_MCU_MAX_PAYLOAD];
    UINT16_T crc;

    if (len > FAQIUJI_MCU_MAX_PAYLOAD || (len != 0 && payload == NULL)) {
        return OPRT_INVALID_PARM;
    }
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

OPERATE_RET faqiuji_mcu_send_wait_response(UINT8_T cmd,
                                           CONST UINT8_T *payload,
                                           UINT8_T len,
                                           FAQIUJI_MCU_FRAME_T *response,
                                           UINT32_T timeout_ms)
{
    UINT8_T seq;
    UINT8_T attempt;
    UINT32_T start;
    FAQIUJI_MCU_FRAME_T received;

    if (response == NULL || timeout_ms == 0U) {
        return OPRT_INVALID_PARM;
    }

    seq = ++sg_mcu_seq;
    for (attempt = 0U; attempt < 3U; attempt++) {
        sg_response_valid = FALSE;
        if (faqiuji_mcu_send_frame(cmd, seq, payload, len) != OPRT_OK) {
            return OPRT_COM_ERROR;
        }

        start = tkl_system_get_millisecond();
        while ((tkl_system_get_millisecond() - start) < timeout_ms) {
            if (sg_response_valid) {
                received = sg_response_frame;
                sg_response_valid = FALSE;
                if (received.cmd == (UINT8_T)(cmd | FAQIUJI_MCU_RESPONSE_FLAG) &&
                    received.seq == seq) {
                    *response = received;
                    return OPRT_OK;
                }
            }
            tkl_system_delay(1U);
        }
    }
    return OPRT_TIMEOUT;
}

OPERATE_RET faqiuji_mcu_ping(VOID_T) { return faqiuji_mcu_send(FAQIUJI_MCU_CMD_PING, NULL, 0); }

OPERATE_RET faqiuji_mcu_ping_periodic(VOID_T)
{
    if (sg_mcu_ping_age < 2U) {
        sg_mcu_ping_age++;
    }
    if (sg_mcu_ping_age >= 2U) {
        sg_mcu_heartbeat = FALSE;
    }
    return faqiuji_mcu_ping();
}

OPERATE_RET faqiuji_mcu_config_set(UINT8_T mode, UINT16_T max_count,
                                   UINT16_T standby_min)
{
    UINT8_T payload[5] = {
        mode, (UINT8_T)max_count, (UINT8_T)(max_count >> 8),
        (UINT8_T)standby_min, (UINT8_T)(standby_min >> 8)
    };
    return faqiuji_mcu_send(FAQIUJI_MCU_CMD_CONFIG_SET, payload,
                            sizeof(payload));
}

OPERATE_RET faqiuji_mcu_control_set(BOOL_T enabled)
{
    UINT8_T payload = enabled ? 1U : 0U;
    return faqiuji_mcu_send(FAQIUJI_MCU_CMD_CONTROL_SET, &payload, 1);
}

OPERATE_RET faqiuji_mcu_config_sync(BOOL_T enabled, UINT8_T mode,
                                    UINT16_T max_count,
                                    UINT16_T standby_min,
                                    UINT8_T motor_speed)
{
    UINT8_T payload[8] = {
        FAQIUJI_MCU_CONFIG_SYNC_VERSION,
        enabled ? 1U : 0U,
        mode,
        (UINT8_T)max_count,
        (UINT8_T)(max_count >> 8),
        (UINT8_T)standby_min,
        (UINT8_T)(standby_min >> 8),
        motor_speed
    };

    if (motor_speed < 1U || motor_speed > 100U) {
        return OPRT_INVALID_PARM;
    }
    return faqiuji_mcu_send(FAQIUJI_MCU_CMD_CONFIG_SYNC, payload,
                            sizeof(payload));
}

OPERATE_RET faqiuji_mcu_duty_set(UINT8_T motor_speed)
{
    if (motor_speed < 1U || motor_speed > 100U) {
        return OPRT_INVALID_PARM;
    }
    return faqiuji_mcu_send(FAQIUJI_MCU_CMD_DUTY_SET, &motor_speed, 1U);
}

BOOL_T faqiuji_mcu_is_ping_ok(VOID_T)
{
    return 1;
}
