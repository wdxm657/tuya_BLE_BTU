#ifndef __APP_BOOTLOADER_H
#define __APP_BOOTLOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t APP_Bootloader_Select(void);
void APP_Bootloader_Go(uint32_t address);

#ifdef __cplusplus
}
#endif

#endif
