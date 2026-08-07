#ifndef FAQIUJI_AUDIO_H
#define FAQIUJI_AUDIO_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_AUDIO_SAMPLE_RATE 8000
#define FAQIUJI_AUDIO_CHANNELS    1
#define FAQIUJI_AUDIO_BITS       16
#define FAQIUJI_AUDIO_FACTORY_SOUND_1 0
#define FAQIUJI_AUDIO_FACTORY_SOUND_2 1
#define FAQIUJI_AUDIO_USER_RECORDING  2

OPERATE_RET faqiuji_audio_init(VOID_T);
OPERATE_RET faqiuji_audio_record_start(UINT8_T file_id);
OPERATE_RET faqiuji_audio_record_stop(VOID_T);
OPERATE_RET faqiuji_audio_play_start(UINT8_T file_id);
OPERATE_RET faqiuji_audio_stop(VOID_T);
VOID_T faqiuji_audio_task(VOID_T);
BOOL_T faqiuji_audio_is_recording(VOID_T);
BOOL_T faqiuji_audio_is_playing(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
