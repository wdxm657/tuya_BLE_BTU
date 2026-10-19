#ifndef FAQIUJI_OTA_H
#define FAQIUJI_OTA_H

#include "faqiuji_protocol.h"

#define FAQIUJI_CMD_OTA_INFO   0x30U
#define FAQIUJI_CMD_OTA_BEGIN  0x31U
#define FAQIUJI_CMD_OTA_DATA   0x32U
#define FAQIUJI_CMD_OTA_END    0x33U
#define FAQIUJI_CMD_OTA_ABORT  0x34U
#define FAQIUJI_CMD_OTA_REBOOT 0x35U

void faqiuji_ota_handle(const FAQIUJI_FRAME_T *frame);
void faqiuji_ota_poll(void);
uint8_t faqiuji_ota_busy(void);

#endif
