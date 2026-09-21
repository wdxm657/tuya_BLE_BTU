#include "string.h"
#include "drivers.h"
#include "board.h"
#include "tal_log.h"
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
#define AUDIO_PWM_CH TUYA_PWM_NUM_0
#define AUDIO_TIMER_CH TUYA_TIMER_NUM_0
#define AUDIO_PWM_FREQUENCY 62500UL
#define AUDIO_PWM_CLOCK (CLOCK_SYS_CLOCK_HZ / 12UL)
#define AUDIO_SAMPLE_PERIOD_US 62
#define AUDIO_PREVIEW_MAX_MS 20000UL

typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAY,
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
STATIC UINT8_T sg_volume = 100;
STATIC TIMER_ID sg_preview_timer = NULL;
STATIC BOOL_T sg_play_finished_report = FALSE;
STATIC FAQIUJI_AUDIO_STATE_E sg_audio_state = AUDIO_STATE_IDLE;
STATIC UINT8_T sg_file_id = 0;
STATIC UINT32_T sg_play_offset = AUDIO_HEADER_SIZE;
STATIC UINT32_T sg_play_data_start = AUDIO_HEADER_SIZE;
STATIC UINT32_T sg_play_size = 0;
STATIC UINT16_T sg_pwm_period_ticks = 0;

STATIC VOID_T faqiuji_audio_play_task(VOID_T);
OPERATE_RET faqiuji_audio_stop(VOID_T);

STATIC VOID_T faqiuji_audio_amp_on(VOID_T)
{
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
    duty_ticks = ((UINT32_T)sample * sg_pwm_period_ticks) / 65535UL;
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
        .duty = 50,
        .polarity = TUYA_PWM_POSITIVE,
    };
    /*
     * Playback-only path. AMIC and DFIFO are intentionally not initialized:
     * audio data comes from the external Flash.
     */
    ret = tal_pwm_init(AUDIO_PWM_CH, &pwm_cfg);
    TAL_PR_INFO("AUDIO INIT: PWM init ch=%d freq=%d ret=%d",
                AUDIO_PWM_CH, pwm_cfg.frequency, ret);
    if (ret != OPRT_OK) return ret;
    sg_pwm_period_ticks = (UINT16_T)(AUDIO_PWM_CLOCK / AUDIO_PWM_FREQUENCY);
    TAL_PR_INFO("AUDIO INIT: playback only, pwm_period_ticks=%u sample_period_us=%u",
                sg_pwm_period_ticks, AUDIO_SAMPLE_PERIOD_US);
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
    TAL_PR_INFO("AUDIO INIT: done, amp pins power=%d ctrl=%d",
                SPK_POWER_CON, SPK_CTRL);
    return OPRT_OK;
}

/* Load one audio source into the double buffer and start the PCM stream. */
STATIC OPERATE_RET faqiuji_audio_play_begin(UINT8_T file_id,
                                            FAQIUJI_AUDIO_PLAY_MODE_E mode)
{
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
    sg_play_isr_count = 0;
    sg_play_isr_error_count = 0;
    sg_play_last_log_count = 0;
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
        return OPRT_COM_ERROR;
    }
    sg_audio_state = AUDIO_STATE_PLAY;
    TAL_PR_INFO("AUDIO PLAY: state PLAY, amp on begin");
    faqiuji_audio_amp_on();
    TAL_PR_INFO("AUDIO PLAY: amp on done, timer start period_us=%d",
                AUDIO_SAMPLE_PERIOD_US);
    ret = tkl_timer_start(AUDIO_TIMER_CH, AUDIO_SAMPLE_PERIOD_US);
    TAL_PR_INFO("AUDIO PLAY: timer start ret=%d", ret);
    if (ret != OPRT_OK) {
        sg_audio_state = AUDIO_STATE_IDLE;
        faqiuji_audio_amp_off();
        return ret;
    }
    tal_pwm_start(AUDIO_PWM_CH);
    TAL_PR_INFO("AUDIO PLAY: PWM started ch=%d mode=%d", AUDIO_PWM_CH, mode);
    if (mode == AUDIO_PLAY_MODE_PREVIEW) {
        ret = tal_sw_timer_start(sg_preview_timer, AUDIO_PREVIEW_MAX_MS, TAL_TIMER_ONCE);
        TAL_PR_INFO("AUDIO PLAY: preview timer start ms=%u ret=%d",
                    AUDIO_PREVIEW_MAX_MS, ret);
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

BOOL_T faqiuji_audio_is_playing(VOID_T) { return sg_audio_state == AUDIO_STATE_PLAY; }
UINT8_T faqiuji_audio_get_volume(VOID_T) { return sg_volume; }

BOOL_T faqiuji_audio_take_play_finished(VOID_T)
{
    BOOL_T finished = sg_play_finished_report;
    sg_play_finished_report = FALSE;
    return finished;
}
