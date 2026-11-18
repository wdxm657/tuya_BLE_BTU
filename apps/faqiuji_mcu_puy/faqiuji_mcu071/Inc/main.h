#ifndef FAQIUJI_MCU071_MAIN_H
#define FAQIUJI_MCU071_MAIN_H

#include "py32f0xx_hal.h"
#include <stdio.h>

#define FAQIUJI_DEBUG_BAUDRATE 115200U

void APP_ErrorHandler(void);
void APP_LogTimestamp(void);
/* Task context only; callers include the trailing CRLF. */
#define APP_LOG(...) do { APP_LogTimestamp(); printf(__VA_ARGS__); } while (0)

#endif
