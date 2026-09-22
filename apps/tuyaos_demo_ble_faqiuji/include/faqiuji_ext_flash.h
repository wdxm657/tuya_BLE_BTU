#ifndef FAQIUJI_EXT_FLASH_H
#define FAQIUJI_EXT_FLASH_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_FLASH_SIZE       0x200000UL
#define FAQIUJI_FLASH_SECTOR     0x1000UL
#define FAQIUJI_FLASH_PAGE       0x100UL
// 0x000000 ~ 0x04E187   音频1
// 0x04E188 ~ 0x0C6F07   音频2
// 0x0C6F08 ~ 0x18DFFF   预留区域
// 0x18E000 ~ 0x1E1FFF   用户录音区域
// 0x1E2000 ~ 0x1FFFFF   剩余区域
#define FAQIUJI_AUDIO_FACTORY_1_BASE 0x000000UL
#define FAQIUJI_AUDIO_FACTORY_1_SIZE 0x04E188UL
#define FAQIUJI_AUDIO_FACTORY_2_BASE 0x04E188UL
#define FAQIUJI_AUDIO_FACTORY_2_SIZE 0x078D80UL
#define FAQIUJI_AUDIO_USER_BASE      0x18E000UL
#define FAQIUJI_AUDIO_SLOT_SIZE  0x054000UL
#define FAQIUJI_AUDIO_SLOT_COUNT 3
#define FAQIUJI_AUDIO_FACTORY_SOUND_1 0
#define FAQIUJI_AUDIO_FACTORY_SOUND_2 1
#define FAQIUJI_AUDIO_USER_FILE_ID 2

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
