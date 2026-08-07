#ifndef FAQIUJI_EXT_FLASH_H
#define FAQIUJI_EXT_FLASH_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_FLASH_SIZE       0x200000UL
#define FAQIUJI_FLASH_SECTOR     0x1000UL
#define FAQIUJI_FLASH_PAGE       0x100UL
#define FAQIUJI_AUDIO_BASE       0x000000UL
#define FAQIUJI_AUDIO_SLOT_SIZE  0x040000UL
#define FAQIUJI_AUDIO_SLOT_COUNT 8

OPERATE_RET faqiuji_ext_flash_init(VOID_T);
OPERATE_RET faqiuji_ext_flash_read(UINT32_T addr, UINT8_T *buf, UINT32_T len);
OPERATE_RET faqiuji_ext_flash_write(UINT32_T addr, CONST UINT8_T *buf, UINT32_T len);
OPERATE_RET faqiuji_ext_flash_erase(UINT32_T addr, UINT32_T len);
OPERATE_RET faqiuji_ext_flash_audio_erase(UINT8_T file_id);
OPERATE_RET faqiuji_ext_flash_audio_read(UINT8_T file_id, UINT32_T offset, UINT8_T *buf, UINT32_T len);
OPERATE_RET faqiuji_ext_flash_audio_write(UINT8_T file_id, UINT32_T offset, CONST UINT8_T *buf, UINT32_T len);

#ifdef __cplusplus
}
#endif

#endif
