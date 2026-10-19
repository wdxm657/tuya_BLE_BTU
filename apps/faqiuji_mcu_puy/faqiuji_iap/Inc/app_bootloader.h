#ifndef __APP_BOOTLOADER_H
#define __APP_BOOTLOADER_H

#include <stdint.h>
#include "../../faqiuji_flash_layout.h"

#define APP_ADDR   FAQIUJI_FLASH_APP_A_BASE
#define ACK_BYTE   0x79U

#ifdef __cplusplus
extern "C" {
#endif

uint32_t APP_Bootloader_Select(void);
void APP_Bootloader_Go(uint32_t address);

/*
 * Compatibility entry points retained for the vendor sample source files
 * that remain in this directory. The IAP main path does not use them.
 */
void APP_Bootloader_SendByte(uint8_t data);
void APP_Bootloader_SendData(uint8_t *data, uint16_t size);
uint8_t APP_Bootloader_ReadByte(void);
void APP_Bootloader_ReadData(uint8_t *data, uint16_t size);

#ifdef __cplusplus
}
#endif

#endif
