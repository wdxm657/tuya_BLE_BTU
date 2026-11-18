#ifndef FAQIUJI_AUDIO_H
#define FAQIUJI_AUDIO_H

#include "py32f0xx_hal.h"

/* 1: built-in 1 kHz sine, 0: PCM audio stored in flash. */
#ifndef FAQIUJI_AUDIO_SINE_TEST
#define FAQIUJI_AUDIO_SINE_TEST 0U
#endif

#ifndef FAQIUJI_AUDIO_VOLUME_PERCENT
#define FAQIUJI_AUDIO_VOLUME_PERCENT 20U
#endif

/* Isolate the DAC output while the ADC and flash writer are active. */
#ifndef FAQIUJI_AUDIO_RECORD_MUTE_LOW
#define FAQIUJI_AUDIO_RECORD_MUTE_LOW 1U
#endif

void faqiuji_audio_init(void);
void faqiuji_audio_task(void *argument);
void faqiuji_audio_tim6_irq_handler(void);
void faqiuji_audio_dma_irq_handler(void);
void faqiuji_audio_record_begin(void);
void faqiuji_audio_record_end(uint8_t saved);

#endif
