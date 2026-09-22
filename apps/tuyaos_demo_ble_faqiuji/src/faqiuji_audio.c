#include "string.h"
#include "drivers.h"
#include "board.h"
#include "tal_log.h"
#include "tal_adc.h"
#include "tal_gpio.h"
#include "tal_pwm.h"
#include "tal_sw_timer.h"
#include "tkl_timer.h"
#include "tkl_system.h"
#include "pwm.h"
#include "faqiuji_audio.h"
#include "faqiuji_ext_flash.h"

#define AUDIO_MAGIC 0x55415146UL
#define AUDIO_HEADER_SIZE 8
#define AUDIO_PLAY_BUFFER_SIZE 2048
#define AUDIO_CAPTURE_BUFFER_SIZE 4096
#define AUDIO_CAPTURE_BUFFER_SAMPLES (AUDIO_CAPTURE_BUFFER_SIZE / sizeof(UINT16_T))
#define AUDIO_CAPTURE_CHUNK_SIZE 256
#define AUDIO_CAPTURE_INPUT_CHUNK_SAMPLES (AUDIO_CAPTURE_CHUNK_SIZE / sizeof(INT16_T))
#define AUDIO_ADC_CHANNEL 8 /* ADC channel 8 is GPIO_PC4 on TLSR825x */
#define AUDIO_ADC_WIDTH 14
#define AUDIO_ADC_VREF_MV 1200UL
#define AUDIO_ADC_PCM_GAIN 64L
#define AUDIO_ADC_CALIBRATION_SAMPLES 32
#define AUDIO_CAPTURE_LOG_INTERVAL_MS 1000
#define AUDIO_PWM_CH TUYA_PWM_NUM_0
#define AUDIO_TIMER_CH TUYA_TIMER_NUM_0
/*
 * 48 MHz system clock gives a 4 MHz PWM clock after the SDK /12 divider.
 * 31.25 kHz keeps the carrier above audible range while preserving 128 PWM
 * duty steps, which is the best quality/resolution tradeoff for 8 kHz PCM on
 * this plain-PWM output path.
 */
#define AUDIO_PWM_FREQUENCY 31250UL
#define AUDIO_PWM_CLOCK (CLOCK_SYS_CLOCK_HZ / 12UL)
#define AUDIO_RECORD_SAMPLE_PERIOD_US 125
/* Factory PCM files are also 8 kHz, so they must use the same output rate. */
#define AUDIO_FACTORY_SAMPLE_PERIOD_US 125
#define AUDIO_PREVIEW_MAX_MS 20000UL

typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAY,
    AUDIO_STATE_RECORD,
} FAQIUJI_AUDIO_STATE_E;

typedef enum {
    AUDIO_PLAY_MODE_FULL = 0,
    AUDIO_PLAY_MODE_PREVIEW,
} FAQIUJI_AUDIO_PLAY_MODE_E;

STATIC UINT8_T sg_play_buffer[2][AUDIO_PLAY_BUFFER_SIZE];
STATIC volatile UINT16_T sg_play_buffer_len[2];
STATIC volatile UINT16_T sg_play_sample_pos;
STATIC volatile UINT8_T sg_play_buffer_index;
STATIC volatile UINT8_T sg_play_refill_index;
STATIC volatile BOOL_T sg_play_refill_pending;
STATIC volatile BOOL_T sg_play_finished;
STATIC volatile UINT32_T sg_play_isr_count;
STATIC volatile UINT32_T sg_play_isr_error_count;
STATIC UINT32_T sg_play_last_log_count;
STATIC FAQIUJI_AUDIO_PLAY_MODE_E sg_play_mode = AUDIO_PLAY_MODE_FULL;
STATIC UINT8_T sg_volume = 80;
STATIC TIMER_ID sg_preview_timer = NULL;
STATIC BOOL_T sg_play_finished_report = FALSE;
STATIC volatile FAQIUJI_AUDIO_STATE_E sg_audio_state = AUDIO_STATE_IDLE;
STATIC UINT8_T sg_file_id = 0;
STATIC UINT32_T sg_play_offset = AUDIO_HEADER_SIZE;
STATIC UINT32_T sg_play_data_start = AUDIO_HEADER_SIZE;
STATIC UINT32_T sg_play_size = 0;
STATIC UINT16_T sg_play_sample_period_us = AUDIO_RECORD_SAMPLE_PERIOD_US;
STATIC UINT16_T sg_pwm_period_ticks = 0;
STATIC volatile UINT32_T sg_pwm_quant_error;
/*
 * ADC samples are collected by the 8 kHz timer callback and drained by the
 * application task. Flash writes never run from the timer callback.
 */
STATIC INT16_T sg_capture_buffer[AUDIO_CAPTURE_BUFFER_SAMPLES];
STATIC UINT8_T sg_capture_write_buffer[AUDIO_CAPTURE_CHUNK_SIZE];
STATIC volatile UINT16_T sg_capture_write_sample;
STATIC volatile UINT16_T sg_capture_read_sample;
STATIC volatile UINT16_T sg_capture_adc_pending;
STATIC volatile UINT32_T sg_capture_adc_error_count;
STATIC INT32_T sg_capture_adc_center_mv;
STATIC INT32_T sg_capture_adc_last_mv;
STATIC INT32_T sg_capture_adc_min_mv;
STATIC INT32_T sg_capture_adc_max_mv;
STATIC INT32_T sg_capture_adc_sum_mv;
STATIC UINT32_T sg_capture_flash_offset;
STATIC UINT32_T sg_capture_pcm_bytes;
STATIC UINT32_T sg_capture_sample_count;
STATIC UINT32_T sg_capture_input_sample_count;
STATIC UINT32_T sg_capture_flash_bytes;
STATIC UINT32_T sg_capture_overruns;
STATIC UINT32_T sg_capture_last_log_ms;
STATIC UINT32_T sg_capture_last_log_samples;
STATIC UINT32_T sg_capture_last_log_input_samples;
STATIC UINT32_T sg_capture_last_log_flash_bytes;
STATIC INT32_T sg_capture_last_log_adc_sum_mv;
STATIC UINT32_T sg_capture_last_log_adc_count;

STATIC VOID_T faqiuji_audio_play_task(VOID_T);
STATIC VOID_T faqiuji_audio_record_task(VOID_T);
STATIC _attribute_ram_code_ VOID_T faqiuji_audio_adc_tick_cb(VOID_T);
STATIC VOID_T faqiuji_audio_adc_sample(VOID_T);
OPERATE_RET faqiuji_audio_stop(VOID_T);

STATIC VOID_T faqiuji_audio_amp_on(VOID_T)
{
    /*
     * Keep the playback power-up sequence identical to the known-good
     * hardware path. The recording path does not exercise these pins.
     */
    TAL_PR_INFO("AUDIO AMP: power on pin=%d", SPK_POWER_CON);
    tal_gpio_write(SPK_POWER_CON, TUYA_GPIO_LEVEL_HIGH);
    tkl_system_delay(2);
    TAL_PR_INFO("AUDIO AMP: ctrl on pin=%d", SPK_CTRL);
    tal_gpio_write(SPK_CTRL, TUYA_GPIO_LEVEL_HIGH);
}

STATIC VOID_T faqiuji_audio_amp_off(VOID_T)
{
    TAL_PR_INFO("AUDIO AMP: ctrl off pin=%d", SPK_CTRL);
    tal_gpio_write(SPK_CTRL, TUYA_GPIO_LEVEL_LOW);
    TAL_PR_INFO("AUDIO AMP: power off pin=%d", SPK_POWER_CON);
    tal_gpio_write(SPK_POWER_CON, TUYA_GPIO_LEVEL_LOW);
}

STATIC VOID_T faqiuji_audio_preview_timeout(TIMER_ID timer_id, VOID_T *arg)
{
    (VOID_T)timer_id;
    (VOID_T)arg;
    if (sg_audio_state == AUDIO_STATE_PLAY &&
        sg_play_mode == AUDIO_PLAY_MODE_PREVIEW) {
        faqiuji_audio_stop();
    }
}

STATIC _attribute_ram_code_ VOID_T faqiuji_audio_timer_cb(VOID_T *args)
{
    UINT8_T buffer_index;
    UINT16_T sample;
    UINT32_T duty_ticks;

    (VOID_T)args;
    sg_play_isr_count++;
    if (sg_audio_state == AUDIO_STATE_RECORD) {
        /*
         * ADC conversion is deliberately deferred out of the interrupt.
         * tal_adc_read_voltage() resets/configures DFIFO and busy-waits for
         * several conversions, which is not safe in this timer ISR.
         */
        faqiuji_audio_adc_tick_cb();
        return;
    }
    if (sg_audio_state != AUDIO_STATE_PLAY) {
        reg_tmr_ctrl &= ~FLD_TMR0_EN;
        return;
    }

    buffer_index = sg_play_buffer_index;
    if (buffer_index > 1) {
        sg_play_isr_error_count++;
        sg_play_finished = TRUE;
        sg_audio_state = AUDIO_STATE_IDLE;
        reg_tmr_ctrl &= ~FLD_TMR0_EN;
        pwm_set_cmp((pwm_id)AUDIO_PWM_CH, 0);
        return;
    }
    if (sg_play_sample_pos >= sg_play_buffer_len[buffer_index]) {
        if (sg_play_buffer_len[buffer_index ^ 1] == 0) {
            sg_play_finished = TRUE;
            sg_audio_state = AUDIO_STATE_IDLE;
            reg_tmr_ctrl &= ~FLD_TMR0_EN;
            pwm_set_cmp((pwm_id)AUDIO_PWM_CH, 0);
            return;
        }
        sg_play_refill_index = buffer_index;
        sg_play_refill_pending = TRUE;
        sg_play_buffer_index = buffer_index ^ 1;
        sg_play_sample_pos = 0;
        buffer_index ^= 1;
    }

    if (sg_play_sample_pos >= AUDIO_PLAY_BUFFER_SIZE ||
        sg_play_sample_pos + 1 >= sg_play_buffer_len[buffer_index]) {
        sg_play_isr_error_count++;
        sg_play_finished = TRUE;
        sg_audio_state = AUDIO_STATE_IDLE;
        reg_tmr_ctrl &= ~FLD_TMR0_EN;
        pwm_set_cmp((pwm_id)AUDIO_PWM_CH, 0);
        return;
    }

    sample = (UINT16_T)sg_play_buffer[buffer_index][sg_play_sample_pos];
    sample |= (UINT16_T)sg_play_buffer[buffer_index][sg_play_sample_pos + 1] << 8;
    sample = (UINT16_T)(32768L + (((INT32_T)(INT16_T)sample * sg_volume) / 100));
    duty_ticks = (UINT32_T)sample * sg_pwm_period_ticks + sg_pwm_quant_error;
    sg_pwm_quant_error = duty_ticks % 65535UL;
    duty_ticks /= 65535UL;
    pwm_set_cmp((pwm_id)AUDIO_PWM_CH, (UINT16_T)duty_ticks);
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
    TAL_PR_INFO("AUDIO INIT: begin, flash_ret=%d", ret);
    if (ret != OPRT_OK) return ret;
    TUYA_PWM_BASE_CFG_T pwm_cfg = {
        .frequency = AUDIO_PWM_FREQUENCY,
        .duty = 0,
        .polarity = TUYA_PWM_POSITIVE,
    };
    ret = tal_pwm_init(AUDIO_PWM_CH, &pwm_cfg);
    TAL_PR_INFO("AUDIO INIT: PWM init ch=%d freq=%d ret=%d",
                AUDIO_PWM_CH, pwm_cfg.frequency, ret);
    if (ret != OPRT_OK) return ret;
    sg_pwm_period_ticks = (UINT16_T)(AUDIO_PWM_CLOCK / AUDIO_PWM_FREQUENCY);
    TAL_PR_INFO("AUDIO INIT: playback only, pwm_period_ticks=%u sample_period_us=%u",
                sg_pwm_period_ticks, AUDIO_RECORD_SAMPLE_PERIOD_US);
    ret = tkl_timer_init(AUDIO_TIMER_CH, &(TUYA_TIMER_BASE_CFG_T) {
        .mode = TUYA_TIMER_MODE_PERIOD,
        .cb = faqiuji_audio_timer_cb,
        .args = NULL,
    });
    TAL_PR_INFO("AUDIO INIT: timer init ch=%d ret=%d", AUDIO_TIMER_CH, ret);
    if (ret != OPRT_OK) return ret;
    ret = tal_sw_timer_create(faqiuji_audio_preview_timeout, NULL, &sg_preview_timer);
    TAL_PR_INFO("AUDIO INIT: preview timer=%p ret=%d",
                sg_preview_timer, ret);
    if (ret != OPRT_OK) return ret;
    faqiuji_audio_amp_off();
    sg_play_isr_count = 0;
    sg_play_isr_error_count = 0;
    sg_play_last_log_count = 0;
    sg_pwm_quant_error = 0;
    TAL_PR_INFO("AUDIO INIT: done, amp pins power=%d ctrl=%d",
                SPK_POWER_CON, SPK_CTRL);
    return OPRT_OK;
}

STATIC UINT16_T faqiuji_audio_capture_write_ptr(VOID_T)
{
    return sg_capture_write_sample;
}

STATIC UINT16_T faqiuji_audio_capture_read_ptr(VOID_T)
{
    return sg_capture_read_sample;
}

STATIC UINT16_T faqiuji_audio_capture_available(VOID_T)
{
    return (UINT16_T)((faqiuji_audio_capture_write_ptr() -
                       faqiuji_audio_capture_read_ptr()) &
                      (AUDIO_CAPTURE_BUFFER_SAMPLES - 1));
}

STATIC _attribute_ram_code_ VOID_T faqiuji_audio_adc_tick_cb(VOID_T)
{
    if (sg_audio_state != AUDIO_STATE_RECORD) {
        return;
    }
    if (sg_capture_adc_pending < AUDIO_CAPTURE_BUFFER_SAMPLES) {
        sg_capture_adc_pending++;
    } else {
        sg_capture_overruns++;
    }
}

STATIC VOID_T faqiuji_audio_adc_sample(VOID_T)
{
    INT32_T adc_mv;
    INT32_T pcm_value;
    UINT16_T next_write;

    if (tal_adc_read_voltage(TUYA_ADC_NUM_0, &adc_mv, 1) != OPRT_OK) {
        sg_capture_adc_error_count++;
        return;
    }

    sg_capture_adc_last_mv = adc_mv;
    if (adc_mv < sg_capture_adc_min_mv) sg_capture_adc_min_mv = adc_mv;
    if (adc_mv > sg_capture_adc_max_mv) sg_capture_adc_max_mv = adc_mv;
    sg_capture_adc_sum_mv += adc_mv;
    sg_capture_input_sample_count++;

    pcm_value = (adc_mv - sg_capture_adc_center_mv) * AUDIO_ADC_PCM_GAIN;
    if (pcm_value > 32767L) pcm_value = 32767L;
    if (pcm_value < -32768L) pcm_value = -32768L;

    next_write = (UINT16_T)((sg_capture_write_sample + 1) &
                            (AUDIO_CAPTURE_BUFFER_SAMPLES - 1));
    if (next_write == sg_capture_read_sample) {
        sg_capture_overruns++;
        return;
    }
    sg_capture_buffer[sg_capture_write_sample] = (INT16_T)pcm_value;
    sg_capture_write_sample = next_write;
}

STATIC OPERATE_RET faqiuji_audio_capture_flush(BOOL_T force)
{
    UINT16_T available;
    UINT16_T read_ptr;
    UINT16_T count_samples;
    UINT16_T count_bytes;
    UINT16_T input_index;
    UINT32_T write_addr;
    OPERATE_RET ret;

    available = faqiuji_audio_capture_available();
    if (!force && available < AUDIO_CAPTURE_INPUT_CHUNK_SAMPLES) {
        return OPRT_OK;
    }
    if (available == 0) return OPRT_OK;

    count_samples = available;
    if (count_samples > AUDIO_CAPTURE_INPUT_CHUNK_SAMPLES) {
        count_samples = AUDIO_CAPTURE_INPUT_CHUNK_SAMPLES;
    }

    read_ptr = faqiuji_audio_capture_read_ptr();
    for (input_index = 0; input_index < count_samples; input_index++) {
        UINT16_T sample_index = (UINT16_T)(
            (read_ptr + input_index) & (AUDIO_CAPTURE_BUFFER_SAMPLES - 1));
        UINT16_T pcm_sample = (UINT16_T)sg_capture_buffer[sample_index];
        sg_capture_write_buffer[input_index * 2] = (UINT8_T)pcm_sample;
        sg_capture_write_buffer[input_index * 2 + 1] =
            (UINT8_T)(pcm_sample >> 8);
    }
    count_bytes = (UINT16_T)(count_samples * sizeof(INT16_T));

    write_addr = AUDIO_HEADER_SIZE + sg_capture_flash_offset;
    if (write_addr + count_bytes > FAQIUJI_AUDIO_SLOT_SIZE) {
        TAL_PR_ERR("AUDIO RECORD: flash full offset=%u count=%u",
                   sg_capture_flash_offset, count_bytes);
        return OPRT_COM_ERROR;
    }

    ret = faqiuji_ext_flash_audio_write(FAQIUJI_AUDIO_USER_FILE_ID,
                                        write_addr, sg_capture_write_buffer,
                                        count_bytes);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("AUDIO RECORD: flash write failed offset=%u count=%u ret=%d",
                   sg_capture_flash_offset, count_bytes, ret);
        return ret;
    }

    sg_capture_read_sample = (UINT16_T)(
        (read_ptr + count_samples) &
        (AUDIO_CAPTURE_BUFFER_SAMPLES - 1));
    sg_capture_flash_offset += count_bytes;
    sg_capture_pcm_bytes += count_bytes;
    sg_capture_flash_bytes += count_bytes;
    sg_capture_sample_count += count_samples;
    return OPRT_OK;
}

STATIC VOID_T faqiuji_audio_record_log(BOOL_T final)
{
    UINT32_T now = tkl_system_get_millisecond();
    UINT32_T elapsed = now - sg_capture_last_log_ms;
    UINT32_T samples = sg_capture_sample_count - sg_capture_last_log_samples;
    UINT32_T input_samples =
        sg_capture_input_sample_count - sg_capture_last_log_input_samples;
    UINT32_T flash_bytes = sg_capture_flash_bytes - sg_capture_last_log_flash_bytes;
    UINT32_T adc_count = sg_capture_input_sample_count -
                         sg_capture_last_log_adc_count;
    INT32_T adc_sum = sg_capture_adc_sum_mv -
                      sg_capture_last_log_adc_sum_mv;
    UINT16_T available = faqiuji_audio_capture_available();

    if (!final && elapsed < AUDIO_CAPTURE_LOG_INTERVAL_MS) return;
    if (elapsed == 0) elapsed = 1;
    TAL_PR_INFO("AUDIO RECORD: %s elapsed=%ums sample=%u/s input=%u/s "
                "flash=%uB/s backlog=%u/%u total_samples=%u total_bytes=%u "
                "overruns=%u wr_sample=%u rd_sample=%u adc_mv=%d "
                "adc_min=%d adc_max=%d adc_avg=%u center=%d adc_errors=%u",
                final ? "STOP_RATE" : "RATE", elapsed,
                (samples * 1000UL) / elapsed,
                (input_samples * 1000UL) / elapsed,
                (flash_bytes * 1000UL) / elapsed,
                available, AUDIO_CAPTURE_BUFFER_SAMPLES,
                sg_capture_sample_count, sg_capture_pcm_bytes,
                sg_capture_overruns,
                faqiuji_audio_capture_write_ptr(),
                faqiuji_audio_capture_read_ptr(),
                sg_capture_adc_last_mv, sg_capture_adc_min_mv,
                sg_capture_adc_max_mv,
                adc_count ? (UINT32_T)(adc_sum / adc_count) : 0,
                sg_capture_adc_center_mv, sg_capture_adc_error_count);
    sg_capture_last_log_ms = now;
    sg_capture_last_log_samples = sg_capture_sample_count;
    sg_capture_last_log_input_samples = sg_capture_input_sample_count;
    sg_capture_last_log_flash_bytes = sg_capture_flash_bytes;
    sg_capture_last_log_adc_sum_mv = sg_capture_adc_sum_mv;
    sg_capture_last_log_adc_count = sg_capture_input_sample_count;
}

OPERATE_RET faqiuji_audio_record_start(UINT8_T file_id)
{
    UINT8_T header[AUDIO_HEADER_SIZE];
    TUYA_ADC_BASE_CFG_T adc_cfg;
    INT32_T adc_mv;
    UINT32_T adc_sum = 0;
    UINT16_T calibration_count = 0;
    OPERATE_RET ret;

    if (file_id != FAQIUJI_AUDIO_USER_FILE_ID ||
        sg_audio_state != AUDIO_STATE_IDLE) {
        return OPRT_INVALID_PARM;
    }
    ret = faqiuji_ext_flash_audio_erase(file_id);
    if (ret != OPRT_OK) return ret;

    put_u32(header, AUDIO_MAGIC);
    /*
     * NOR Flash can only change bits from 1 to 0 without another erase.
     * Leave the size field erased while recording so the final size can be
     * committed at stop time.
     */
    put_u32(&header[4], 0xFFFFFFFFUL);
    ret = faqiuji_ext_flash_audio_write(file_id, 0, header, sizeof(header));
    if (ret != OPRT_OK) return ret;

    adc_cfg.ch_nums = 1;
    adc_cfg.ch_list.data = (1UL << AUDIO_ADC_CHANNEL);
    adc_cfg.width = AUDIO_ADC_WIDTH;
    adc_cfg.freq = FAQIUJI_AUDIO_SAMPLE_RATE;
    adc_cfg.type = TUYA_ADC_EXTERNAL_SAMPLE_VOL;
    adc_cfg.mode = TUYA_ADC_SINGLE;
    adc_cfg.conv_cnt = 1;
    adc_cfg.ref_vol = AUDIO_ADC_VREF_MV;
    ret = tal_adc_init(TUYA_ADC_NUM_0, &adc_cfg);
    TAL_PR_INFO("AUDIO RECORD: ADC init pin=PC4 channel=%u width=%u vref=%umV ret=%d",
                AUDIO_ADC_CHANNEL, AUDIO_ADC_WIDTH, AUDIO_ADC_VREF_MV, ret);
    if (ret != OPRT_OK) return ret;

    while (calibration_count < AUDIO_ADC_CALIBRATION_SAMPLES) {
        if (tal_adc_read_voltage(TUYA_ADC_NUM_0, &adc_mv, 1) == OPRT_OK) {
            adc_sum += adc_mv;
            calibration_count++;
        }
    }
    TAL_PR_INFO("AUDIO RECORD: ADC calibration samples=%u center=%dmV",
                calibration_count, adc_sum / calibration_count);

    sg_capture_flash_offset = 0;
    sg_capture_pcm_bytes = 0;
    sg_capture_sample_count = 0;
    sg_capture_input_sample_count = 0;
    sg_capture_flash_bytes = 0;
    sg_capture_overruns = 0;
    sg_capture_write_sample = 0;
    sg_capture_read_sample = 0;
    sg_capture_adc_pending = 0;
    sg_capture_adc_error_count = 0;
    sg_capture_adc_center_mv = adc_sum / calibration_count;
    sg_capture_adc_last_mv = sg_capture_adc_center_mv;
    sg_capture_adc_min_mv = sg_capture_adc_center_mv;
    sg_capture_adc_max_mv = sg_capture_adc_center_mv;
    sg_capture_adc_sum_mv = 0;
    sg_capture_last_log_ms = tkl_system_get_millisecond();
    sg_capture_last_log_samples = 0;
    sg_capture_last_log_input_samples = 0;
    sg_capture_last_log_flash_bytes = 0;
    sg_capture_last_log_adc_sum_mv = 0;
    sg_capture_last_log_adc_count = 0;
    sg_audio_state = AUDIO_STATE_RECORD;

    TAL_PR_INFO("AUDIO RECORD: START file=%u format=%uk/%ubit/mono "
                "source=ADC/PC4 adc_rate=%uHz buffer=%uB/%usamples "
                "chunk=%uB flash_base=0x%06x center=%dmV gain=%d",
                file_id, FAQIUJI_AUDIO_SAMPLE_RATE / 1000,
                FAQIUJI_AUDIO_BITS, FAQIUJI_AUDIO_SAMPLE_RATE,
                AUDIO_CAPTURE_BUFFER_SIZE,
                AUDIO_CAPTURE_BUFFER_SAMPLES, AUDIO_CAPTURE_CHUNK_SIZE,
                FAQIUJI_AUDIO_USER_BASE, sg_capture_adc_center_mv,
                AUDIO_ADC_PCM_GAIN);
    ret = tkl_timer_start(AUDIO_TIMER_CH, AUDIO_RECORD_SAMPLE_PERIOD_US);
    TAL_PR_INFO("AUDIO RECORD: ADC timer start period_us=%u rate=%u ret=%d",
                AUDIO_RECORD_SAMPLE_PERIOD_US,
                1000000UL / AUDIO_RECORD_SAMPLE_PERIOD_US, ret);
    if (ret != OPRT_OK) {
        sg_audio_state = AUDIO_STATE_IDLE;
        tal_adc_deinit(TUYA_ADC_NUM_0);
        return ret;
    }
    return OPRT_OK;
}

OPERATE_RET faqiuji_audio_record_stop(VOID_T)
{
    UINT8_T header[AUDIO_HEADER_SIZE];
    UINT8_T header_bk[AUDIO_HEADER_SIZE];
    OPERATE_RET ret = OPRT_OK;
    OPERATE_RET verify_ret;

    if (sg_audio_state != AUDIO_STATE_RECORD) return OPRT_INVALID_PARM;

    tkl_timer_stop(AUDIO_TIMER_CH);
    while (sg_capture_adc_pending != 0) {
        faqiuji_audio_record_task();
    }
    faqiuji_audio_record_task();
    while (faqiuji_audio_capture_available() != 0) {
        ret = faqiuji_audio_capture_flush(TRUE);
        if (ret != OPRT_OK) break;
    }
    tal_adc_deinit(TUYA_ADC_NUM_0);

    put_u32(header, AUDIO_MAGIC);
    put_u32(&header[4], sg_capture_pcm_bytes);
    if (ret == OPRT_OK) {
        ret = faqiuji_ext_flash_audio_write(FAQIUJI_AUDIO_USER_FILE_ID,
                                            0, header, sizeof(header));
    }
    sg_audio_state = AUDIO_STATE_IDLE;
    faqiuji_audio_record_log(TRUE);
    TAL_PR_INFO("AUDIO RECORD: STOP ret=%d bytes=%u samples=%u",
                ret, sg_capture_pcm_bytes, sg_capture_sample_count);
    verify_ret = faqiuji_ext_flash_audio_read(FAQIUJI_AUDIO_USER_FILE_ID, 0,
                                              header_bk, sizeof(header_bk));
    TAL_PR_INFO("AUDIO RECORD: HEADER verify_ret=%d magic=0x%08x size=%u",
                verify_ret, get_u32(header_bk), get_u32(&header_bk[4]));
    if (ret == OPRT_OK && verify_ret != OPRT_OK) {
        ret = verify_ret;
    }
    return ret;
}

BOOL_T faqiuji_audio_is_recording(VOID_T)
{
    return sg_audio_state == AUDIO_STATE_RECORD;
}

/* Load one audio source into the double buffer and start the PCM stream. */
STATIC OPERATE_RET faqiuji_audio_play_begin(UINT8_T file_id,
                                            FAQIUJI_AUDIO_PLAY_MODE_E mode)
{
    // faqiuji_audio_amp_on();
    // return OPRT_OK;
    UINT8_T header[AUDIO_HEADER_SIZE];
    OPERATE_RET ret;
    TAL_PR_INFO("AUDIO PLAY: begin file=%d mode=%d state=%d",
                file_id, mode, sg_audio_state);
    if (file_id >= FAQIUJI_AUDIO_SLOT_COUNT || sg_audio_state != AUDIO_STATE_IDLE) {
        TAL_PR_ERR("AUDIO PLAY: invalid request file=%d state=%d",
                   file_id, sg_audio_state);
        return OPRT_INVALID_PARM;
    }

    if (file_id == FAQIUJI_AUDIO_FACTORY_SOUND_1 ||
        file_id == FAQIUJI_AUDIO_FACTORY_SOUND_2) {
        sg_play_data_start = 0;
        sg_play_size = (file_id == FAQIUJI_AUDIO_FACTORY_SOUND_1) ?
                       FAQIUJI_AUDIO_FACTORY_1_SIZE : FAQIUJI_AUDIO_FACTORY_2_SIZE;
        sg_play_sample_period_us = AUDIO_FACTORY_SAMPLE_PERIOD_US;
        TAL_PR_INFO("AUDIO PLAY: factory file=%d size=%u", file_id, sg_play_size);
    } else {
        ret = faqiuji_ext_flash_audio_read(file_id, 0, header, sizeof(header));
        TAL_PR_INFO("AUDIO PLAY: user header read ret=%d magic=0x%08x size=%u",
                    ret, get_u32(header), get_u32(&header[4]));
        if (ret != OPRT_OK || get_u32(header) != AUDIO_MAGIC ||
            get_u32(&header[4]) == 0 ||
            get_u32(&header[4]) > FAQIUJI_AUDIO_SLOT_SIZE - AUDIO_HEADER_SIZE) {
            return OPRT_NOT_FOUND;
        }
        sg_play_data_start = AUDIO_HEADER_SIZE;
        sg_play_size = get_u32(&header[4]);
        sg_play_sample_period_us = AUDIO_RECORD_SAMPLE_PERIOD_US;
    }
    sg_file_id = file_id;
    sg_play_mode = mode;
    sg_play_offset = sg_play_data_start;
    sg_play_buffer_len[0] = 0;
    sg_play_buffer_len[1] = 0;
    sg_play_buffer_index = 0;
    sg_play_sample_pos = 0;
    sg_play_refill_pending = FALSE;
    sg_play_finished = FALSE;
    sg_play_finished_report = FALSE;
    sg_play_isr_count = 0;
    sg_play_isr_error_count = 0;
    sg_play_last_log_count = 0;
    sg_pwm_quant_error = 0;
    TAL_PR_INFO("AUDIO PLAY: buffers reset offset=%u data_start=%u size=%u",
                sg_play_offset, sg_play_data_start, sg_play_size);
    faqiuji_audio_play_task();
    TAL_PR_INFO("AUDIO PLAY: buffer0 loaded len=%u next_offset=%u ret_pending=%d",
                sg_play_buffer_len[0], sg_play_offset, sg_play_refill_pending);
    if (sg_play_size > AUDIO_PLAY_BUFFER_SIZE) {
        faqiuji_audio_play_task();
        TAL_PR_INFO("AUDIO PLAY: buffer1 loaded len=%u next_offset=%u",
                    sg_play_buffer_len[1], sg_play_offset);
    }
    if (sg_play_buffer_len[0] == 0) {
        TAL_PR_ERR("AUDIO PLAY: buffer0 empty, abort");
        sg_play_buffer_len[1] = 0;
        return OPRT_COM_ERROR;
    }
    sg_audio_state = AUDIO_STATE_PLAY;
    TAL_PR_INFO("AUDIO PLAY: state PLAY, amp on begin");
    faqiuji_audio_amp_on();
    TAL_PR_INFO("AUDIO PLAY: amp on done, timer start period_us=%u rate=%u",
                sg_play_sample_period_us,
                1000000UL / sg_play_sample_period_us);
    ret = tkl_timer_start(AUDIO_TIMER_CH, sg_play_sample_period_us);
    TAL_PR_INFO("AUDIO PLAY: timer start ret=%d", ret);
    if (ret != OPRT_OK) {
        sg_audio_state = AUDIO_STATE_IDLE;
        sg_play_buffer_len[0] = 0;
        sg_play_buffer_len[1] = 0;
        faqiuji_audio_amp_off();
        return ret;
    }
    tal_pwm_start(AUDIO_PWM_CH);
    TAL_PR_INFO("AUDIO PLAY: PWM started ch=%d mode=%d", AUDIO_PWM_CH, mode);
    if (mode == AUDIO_PLAY_MODE_PREVIEW) {
        ret = tal_sw_timer_start(sg_preview_timer, AUDIO_PREVIEW_MAX_MS, TAL_TIMER_ONCE);
        TAL_PR_INFO("AUDIO PLAY: preview timer start ms=%u ret=%d",
                    AUDIO_PREVIEW_MAX_MS, ret);
        if (ret != OPRT_OK) {
            tkl_timer_stop(AUDIO_TIMER_CH);
            tal_pwm_stop(AUDIO_PWM_CH);
            faqiuji_audio_amp_off();
            sg_audio_state = AUDIO_STATE_IDLE;
            sg_play_buffer_len[0] = 0;
            sg_play_buffer_len[1] = 0;
            return ret;
        }
    }
    TAL_PR_INFO("AUDIO PLAY: start complete");
    return OPRT_OK;
}

/* Full playback has no preview timeout and runs until the source ends. */
STATIC OPERATE_RET faqiuji_audio_play_full_internal(UINT8_T file_id)
{
    return faqiuji_audio_play_begin(file_id, AUDIO_PLAY_MODE_FULL);
}

/* Preview playback is limited by the preview timer. */
STATIC OPERATE_RET faqiuji_audio_play_preview_internal(UINT8_T file_id)
{
    return faqiuji_audio_play_begin(file_id, AUDIO_PLAY_MODE_PREVIEW);
}

OPERATE_RET faqiuji_audio_play_start(UINT8_T file_id)
{
    return faqiuji_audio_play_full_internal(file_id);
}

OPERATE_RET faqiuji_audio_preview_start(UINT8_T file_id)
{
    return faqiuji_audio_play_preview_internal(file_id);
}

OPERATE_RET faqiuji_audio_stop(VOID_T)
{
    TAL_PR_INFO("AUDIO PLAY: stop state=%d mode=%d isr_count=%u isr_errors=%u",
                sg_audio_state, sg_play_mode, sg_play_isr_count,
                sg_play_isr_error_count);
    if (sg_audio_state == AUDIO_STATE_PLAY) {
        if (sg_preview_timer != NULL) tal_sw_timer_stop(sg_preview_timer);
        tkl_timer_stop(AUDIO_TIMER_CH);
        pwm_set_cmp((pwm_id)AUDIO_PWM_CH, 0);
        tal_pwm_stop(AUDIO_PWM_CH);
        sg_pwm_quant_error = 0;
        faqiuji_audio_amp_off();
        if (sg_play_mode == AUDIO_PLAY_MODE_FULL) {
            sg_play_finished_report = TRUE;
        }
    }
    sg_audio_state = AUDIO_STATE_IDLE;
    sg_play_mode = AUDIO_PLAY_MODE_FULL;
    return OPRT_OK;
}

OPERATE_RET faqiuji_audio_set_volume(UINT8_T volume)
{
    if (volume < 1) volume = 1;
    if (volume > 100) volume = 100;
    sg_volume = volume;
    return OPRT_OK;
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
        sg_pwm_quant_error = 0;
        faqiuji_audio_amp_off();
        sg_play_finished_report = TRUE;
        return;
    }

    if (!sg_play_refill_pending && sg_play_buffer_len[0] != 0 && sg_play_buffer_len[1] != 0) return;
    buffer_index = sg_play_refill_pending ? sg_play_refill_index :
                   (sg_play_buffer_len[0] == 0 ? 0 : 1);
    if (sg_play_offset - sg_play_data_start >= sg_play_size) {
        sg_play_buffer_len[buffer_index] = 0;
        sg_play_refill_pending = FALSE;
        return;
    }

    remain = sg_play_size - (sg_play_offset - sg_play_data_start);
    count = remain > AUDIO_PLAY_BUFFER_SIZE ? AUDIO_PLAY_BUFFER_SIZE : remain;
    count &= ~1UL;
    if (count == 0) {
        sg_play_buffer_len[buffer_index] = 0;
        sg_play_refill_pending = FALSE;
        return;
    }
    if (faqiuji_ext_flash_audio_read(sg_file_id, sg_play_offset,
                                     sg_play_buffer[buffer_index], count) == OPRT_OK) {
        /* The timer ISR consumes one PCM sample; this task refills Flash data. */
        sg_play_buffer_len[buffer_index] = (UINT16_T)count;
        sg_play_offset += count;
        sg_play_refill_pending = FALSE;
    } else {
        TAL_PR_ERR("AUDIO PLAY: flash refill failed file=%u buffer=%u offset=%u len=%u",
                   sg_file_id, buffer_index, sg_play_offset, count);
        sg_play_buffer_len[buffer_index] = 0;
    }
}

VOID_T faqiuji_audio_task(VOID_T)
{
    if (sg_audio_state == AUDIO_STATE_RECORD) {
        faqiuji_audio_record_task();
        return;
    }
    if (sg_audio_state == AUDIO_STATE_PLAY &&
        sg_play_isr_count - sg_play_last_log_count >= 8000) {
        sg_play_last_log_count = sg_play_isr_count;
        TAL_PR_INFO("AUDIO PLAY: running samples=%u buf=%u pos=%u/%u refill=%d errors=%u",
                    sg_play_isr_count, sg_play_buffer_index,
                    sg_play_sample_pos, sg_play_buffer_len[sg_play_buffer_index],
                    sg_play_refill_pending, sg_play_isr_error_count);
    }
    if (sg_play_finished) {
        TAL_PR_INFO("AUDIO PLAY: finished isr_count=%u errors=%u",
                    sg_play_isr_count, sg_play_isr_error_count);
        sg_play_finished = FALSE;
        sg_play_refill_pending = FALSE;
        tal_pwm_stop(AUDIO_PWM_CH);
        faqiuji_audio_amp_off();
        sg_play_finished_report = TRUE;
    }
    if (sg_audio_state == AUDIO_STATE_PLAY) faqiuji_audio_play_task();
}

STATIC VOID_T faqiuji_audio_record_task(VOID_T)
{
    UINT16_T available;
    UINT8_T sample_count = 0;

    /*
     * Keep each main-loop pass bounded. The ADC driver performs a blocking
     * multi-conversion internally, so draining a small batch avoids starving
     * BLE and other application work.
     */
    while (sg_capture_adc_pending != 0 &&
           sample_count < 8) {
        sg_capture_adc_pending--;
        faqiuji_audio_adc_sample();
        sample_count++;
    }

    available = faqiuji_audio_capture_available();
    if (available >= AUDIO_CAPTURE_BUFFER_SAMPLES -
                    AUDIO_CAPTURE_INPUT_CHUNK_SAMPLES) {
        sg_capture_overruns++;
        TAL_PR_ERR("AUDIO RECORD: capture backlog/overrun available=%u "
                   "wptr=%u rptr=%u overruns=%u",
                   available, faqiuji_audio_capture_write_ptr(),
                   faqiuji_audio_capture_read_ptr(), sg_capture_overruns);
    }

    if (faqiuji_audio_capture_flush(FALSE) != OPRT_OK) {
        return;
    }
    faqiuji_audio_record_log(FALSE);
}

BOOL_T faqiuji_audio_is_playing(VOID_T) { return sg_audio_state == AUDIO_STATE_PLAY; }
UINT8_T faqiuji_audio_get_volume(VOID_T) { return sg_volume; }

BOOL_T faqiuji_audio_take_play_finished(VOID_T)
{
    BOOL_T finished = sg_play_finished_report;
    sg_play_finished_report = FALSE;
    return finished;
}
