#ifndef FAQIUJI_DEVICE_H
#define FAQIUJI_DEVICE_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

void faqiuji_device_init(void);
void faqiuji_device_sensor_task(void *argument);
void faqiuji_device_power_task(void *argument);
void faqiuji_device_led_task(void *argument);
void faqiuji_device_ir_task(void *argument);
void faqiuji_device_control_task(void *argument);
void faqiuji_device_set_mode(uint8_t mode);
void faqiuji_device_set_config(uint8_t mode, uint16_t max_count,
                               uint16_t standby_min);
void faqiuji_device_set_enabled(uint8_t enabled);
uint8_t faqiuji_device_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
