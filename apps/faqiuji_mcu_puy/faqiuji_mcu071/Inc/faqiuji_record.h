#ifndef FAQIUJI_RECORD_H
#define FAQIUJI_RECORD_H

#include "main.h"

#define FAQIUJI_RECORD_SECONDS 5U
#define FAQIUJI_RECORD_KEY_PORT GPIOC
#define FAQIUJI_RECORD_KEY_PIN GPIO_PIN_15
#define FAQIUJI_RECORD_KEY_ACTIVE GPIO_PIN_RESET
#define FAQIUJI_RECORD_DEBOUNCE_MS 20U

/* Inspect these symbols in Keil: 0 waiting, 1 erasing, 2 recording,
 * 3 saved, 4 failed. Error is the failing source line. */
extern volatile uint32_t g_record_state;
extern volatile uint32_t g_record_samples;
extern volatile uint32_t g_record_error;

void faqiuji_record_task(void *argument);
void faqiuji_record_dma_irq_handler(void);

#endif
