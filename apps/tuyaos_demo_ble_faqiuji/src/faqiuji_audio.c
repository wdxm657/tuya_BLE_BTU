#include "string.h"
#include "drivers.h"
#include "audio.h"
#include "dfifo.h"
#include "board.h"
#include "tal_log.h"
#include "tal_pwm.h"
#include "tkl_timer.h"
#include "faqiuji_audio.h"
#include "faqiuji_ext_flash.h"

#define AUDIO_MAGIC 0x55415146UL
#define AUDIO_HEADER_SIZE 8
#define AUDIO_BUFFER_SAMPLES 2048
#define AUDIO_IO_BUFFER_SIZE 1024
#define AUDIO_PLAY_BUFFER_SIZE 256
#define AUDIO_PWM_CH TUYA_PWM_NUM_0
#define AUDIO_TIMER_CH TUYA_TIMER_NUM_0
#define AUDIO_SAMPLE_PERIOD_US 125

typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_RECORD,
    AUDIO_STATE_PLAY,
} FAQIUJI_AUDIO_STATE_E;

STATIC UINT16_T sg_capture_buffer[AUDIO_BUFFER_SAMPLES];
STATIC UINT8_T sg_io_buffer[AUDIO_IO_BUFFER_SIZE];
STATIC UINT8_T sg_play_buffer[2][AUDIO_PLAY_BUFFER_SIZE];
STATIC volatile UINT16_T sg_play_buffer_len[2];
STATIC volatile UINT16_T sg_play_sample_pos;
STATIC volatile UINT8_T sg_play_buffer_index;
STATIC volatile UINT8_T sg_play_refill_index;
STATIC volatile BOOL_T sg_play_refill_pending;
STATIC volatile BOOL_T sg_play_finished;
STATIC FAQIUJI_AUDIO_STATE_E sg_audio_state = AUDIO_STATE_IDLE;
STATIC UINT8_T sg_file_id = 0;
STATIC UINT32_T sg_record_bytes = 0;
STATIC UINT32_T sg_play_offset = AUDIO_HEADER_SIZE;
STATIC UINT32_T sg_play_size = 0;
STATIC UINT16_T sg_capture_read = 0;

STATIC VOID_T faqiuji_audio_play_task(VOID_T);

STATIC VOID_T faqiuji_audio_timer_cb(VOID_T *args)
{
    UINT8_T buffer_index;
    UINT16_T sample;
    UINT32_T duty;

    (VOID_T)args;
    if (sg_audio_state != AUDIO_STATE_PLAY) {
        reg_tmr_ctrl &= ~FLD_TMR0_EN;
        return;
    }

    buffer_index = sg_play_buffer_index;
    if (sg_play_sample_pos >= sg_play_buffer_len[buffer_index]) {
        if (sg_play_buffer_len[buffer_index ^ 1] == 0) {
            sg_play_finished = TRUE;
            sg_audio_state = AUDIO_STATE_IDLE;
            reg_tmr_ctrl &= ~FLD_TMR0_EN;
            tal_pwm_duty_set(AUDIO_PWM_CH, 0);
            return;
        }
        sg_play_refill_index = buffer_index;
        sg_play_refill_pending = TRUE;
        sg_play_buffer_index = buffer_index ^ 1;
        sg_play_sample_pos = 0;
        buffer_index ^= 1;
    }

    sample = (UINT16_T)sg_play_buffer[buffer_index][sg_play_sample_pos];
    sample |= (UINT16_T)sg_play_buffer[buffer_index][sg_play_sample_pos + 1] << 8;
    duty = (((INT32_T)(INT16_T)sample + 32768L) * 1000000UL) / 65535UL;
    tal_pwm_duty_set(AUDIO_PWM_CH, duty);
    sg_play_sample_pos += 2;
}

STATIC VOID_T put_u32(UINT8_T *p, UINT32_T value)
{
    p[0] = (UINT8_T)value;
    p[1] = (UINT8_T)(value >> 8);
    p[2] = (UINT8_T)(value >> 16);
    p[3] = (UINT8_T)(value >> 24);
}

STATIC UINT32_T get_u32(CONST UINT8_T *p)
{
    return ((UINT32_T)p[0]) | ((UINT32_T)p[1] << 8) |
           ((UINT32_T)p[2] << 16) | ((UINT32_T)p[3] << 24);
}

OPERATE_RET faqiuji_audio_init(VOID_T)
{
    OPERATE_RET ret = faqiuji_ext_flash_init();
    TUYA_PWM_BASE_CFG_T pwm_cfg = {
        .frequency = 62500,
        .duty = 50,
        .polarity = TUYA_PWM_POSITIVE,
    };
    if (ret != OPRT_OK) return ret;

    audio_amic_init(AUDIO_8K);
    audio_config_mic_buf(sg_capture_buffer, sizeof(sg_capture_buffer));
    ret = tal_pwm_init(AUDIO_PWM_CH, &pwm_cfg);
    if (ret != OPRT_OK) return ret;
    ret = tkl_timer_init(AUDIO_TIMER_CH, &(TUYA_TIMER_BASE_CFG_T) {
        .mode = TUYA_TIMER_MODE_PERIOD,
        .cb = faqiuji_audio_timer_cb,
        .args = NULL,
    });
    if (ret != OPRT_OK) return ret;
    sg_capture_read = 0;
    return OPRT_OK;
}

OPERATE_RET faqiuji_audio_record_start(UINT8_T file_id)
{
    UINT8_T header[AUDIO_HEADER_SIZE] = {0};
    OPERATE_RET ret;
    if (file_id >= FAQIUJI_AUDIO_SLOT_COUNT || sg_audio_state != AUDIO_STATE_IDLE) return OPRT_INVALID_PARM;

    ret = faqiuji_ext_flash_audio_erase(file_id);
    if (ret != OPRT_OK) return ret;
    put_u32(header, AUDIO_MAGIC);
    put_u32(&header[4], 0xFFFFFFFFUL);
    ret = faqiuji_ext_flash_audio_write(file_id, 0, header, sizeof(header));
    if (ret == OPRT_OK) {
        sg_file_id = file_id;
        sg_record_bytes = 0;
        sg_capture_read = get_mic_wr_ptr() % AUDIO_BUFFER_SAMPLES;
        sg_audio_state = AUDIO_STATE_RECORD;
    }
    return ret;
}

OPERATE_RET faqiuji_audio_record_stop(VOID_T)
{
    UINT8_T size_buf[4];
    OPERATE_RET ret;
    if (sg_audio_state != AUDIO_STATE_RECORD) return OPRT_INVALID_PARM;

    sg_audio_state = AUDIO_STATE_IDLE;
    put_u32(size_buf, sg_record_bytes);
    ret = faqiuji_ext_flash_audio_write(sg_file_id, 4, size_buf, sizeof(size_buf));
    return ret;
}

OPERATE_RET faqiuji_audio_play_start(UINT8_T file_id)
{
    UINT8_T header[AUDIO_HEADER_SIZE];
    OPERATE_RET ret;
    if (file_id >= FAQIUJI_AUDIO_SLOT_COUNT || sg_audio_state != AUDIO_STATE_IDLE) return OPRT_INVALID_PARM;

    ret = faqiuji_ext_flash_audio_read(file_id, 0, header, sizeof(header));
    if (ret != OPRT_OK || get_u32(header) != AUDIO_MAGIC ||
        get_u32(&header[4]) > FAQIUJI_AUDIO_SLOT_SIZE - AUDIO_HEADER_SIZE) {
        return OPRT_NOT_FOUND;
    }
    sg_file_id = file_id;
    sg_play_size = get_u32(&header[4]);
    sg_play_offset = AUDIO_HEADER_SIZE;
    sg_play_buffer_len[0] = 0;
    sg_play_buffer_len[1] = 0;
    sg_play_buffer_index = 0;
    sg_play_sample_pos = 0;
    sg_play_refill_pending = FALSE;
    sg_play_finished = FALSE;
    faqiuji_audio_play_task();
    if (sg_play_buffer_len[0] == 0) return OPRT_COM_ERROR;
    if (sg_play_size > AUDIO_PLAY_BUFFER_SIZE && sg_play_buffer_len[1] == 0) return OPRT_COM_ERROR;
    sg_audio_state = AUDIO_STATE_PLAY;
    ret = tkl_timer_start(AUDIO_TIMER_CH, AUDIO_SAMPLE_PERIOD_US);
    if (ret != OPRT_OK) {
        sg_audio_state = AUDIO_STATE_IDLE;
        return ret;
    }
    tal_pwm_start(AUDIO_PWM_CH);
    return OPRT_OK;
}

OPERATE_RET faqiuji_audio_stop(VOID_T)
{
    if (sg_audio_state == AUDIO_STATE_PLAY) {
        tkl_timer_stop(AUDIO_TIMER_CH);
        tal_pwm_duty_set(AUDIO_PWM_CH, 0);
        tal_pwm_stop(AUDIO_PWM_CH);
    }
    sg_audio_state = AUDIO_STATE_IDLE;
    return OPRT_OK;
}

STATIC VOID_T faqiuji_audio_record_task(VOID_T)
{
    UINT16_T write_ptr = get_mic_wr_ptr() % AUDIO_BUFFER_SAMPLES;
    UINT16_T available = (write_ptr >= sg_capture_read) ?
                         (write_ptr - sg_capture_read) :
                         (AUDIO_BUFFER_SAMPLES - sg_capture_read + write_ptr);
    UINT16_T count = available;
    UINT32_T max_bytes = FAQIUJI_AUDIO_SLOT_SIZE - AUDIO_HEADER_SIZE - sg_record_bytes;
    if (count > AUDIO_IO_BUFFER_SIZE / 2) count = AUDIO_IO_BUFFER_SIZE / 2;
    if ((UINT32_T)count * 2 > max_bytes) count = (UINT16_T)(max_bytes / 2);
    if (count == 0) return;

    memcpy(sg_io_buffer, &sg_capture_buffer[sg_capture_read], count * 2);
    if (faqiuji_ext_flash_audio_write(sg_file_id, AUDIO_HEADER_SIZE + sg_record_bytes,
                                       sg_io_buffer, count * 2) == OPRT_OK) {
        sg_record_bytes += count * 2;
        sg_capture_read = (sg_capture_read + count) % AUDIO_BUFFER_SAMPLES;
    }
}

STATIC VOID_T faqiuji_audio_play_task(VOID_T)
{
    UINT8_T buffer_index;
    UINT32_T remain;
    UINT32_T count;

    if (sg_play_finished) {
        sg_play_finished = FALSE;
        sg_play_refill_pending = FALSE;
        tal_pwm_stop(AUDIO_PWM_CH);
        return;
    }

    if (!sg_play_refill_pending && sg_play_buffer_len[0] != 0 && sg_play_buffer_len[1] != 0) return;
    buffer_index = sg_play_refill_pending ? sg_play_refill_index :
                   (sg_play_buffer_len[0] == 0 ? 0 : 1);
    if (sg_play_offset - AUDIO_HEADER_SIZE >= sg_play_size) {
        sg_play_buffer_len[buffer_index] = 0;
        sg_play_refill_pending = FALSE;
        return;
    }

    remain = sg_play_size - (sg_play_offset - AUDIO_HEADER_SIZE);
    count = remain > AUDIO_PLAY_BUFFER_SIZE ? AUDIO_PLAY_BUFFER_SIZE : remain;
    count &= ~1UL;
    if (count == 0) {
        sg_play_buffer_len[buffer_index] = 0;
        sg_play_refill_pending = FALSE;
        return;
    }
    if (faqiuji_ext_flash_audio_read(sg_file_id, sg_play_offset,
                                     sg_play_buffer[buffer_index], count) == OPRT_OK) {
        /*
         * PWM audio principle:
         * Timer0 interrupts every 125 us. The ISR consumes one 16-bit PCM
         * sample and converts it to PWM duty, while this task only refills
         * the inactive buffer from external Flash.
         */
        sg_play_buffer_len[buffer_index] = (UINT16_T)count;
        sg_play_offset += count;
        sg_play_refill_pending = FALSE;
    } else {
        sg_play_buffer_len[buffer_index] = 0;
    }
}

VOID_T faqiuji_audio_task(VOID_T)
{
    if (sg_audio_state == AUDIO_STATE_RECORD) faqiuji_audio_record_task();
    else if (sg_audio_state == AUDIO_STATE_PLAY) faqiuji_audio_play_task();
}

BOOL_T faqiuji_audio_is_recording(VOID_T) { return sg_audio_state == AUDIO_STATE_RECORD; }
BOOL_T faqiuji_audio_is_playing(VOID_T) { return sg_audio_state == AUDIO_STATE_PLAY; }
