#ifndef FAQIUJI_AUDIO_H
#define FAQIUJI_AUDIO_H

#include "py32f0xx_hal.h"

#ifndef FAQIUJI_AUDIO_VOLUME_PERCENT
#define FAQIUJI_AUDIO_VOLUME_PERCENT 20U
#endif

void faqiuji_audio_init(void);
void faqiuji_audio_task(void *argument);
void faqiuji_audio_tim6_irq_handler(void);

#endif
