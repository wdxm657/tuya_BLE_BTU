#ifndef FAQIUJI_AUDIO_H
#define FAQIUJI_AUDIO_H

#include "tuya_cloud_types.h"
#include "faqiuji_ext_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAQIUJI_AUDIO_SAMPLE_RATE 8000
#define FAQIUJI_AUDIO_CHANNELS    1
#define FAQIUJI_AUDIO_BITS       16
OPERATE_RET faqiuji_audio_init(VOID_T);
OPERATE_RET faqiuji_audio_play_start(UINT8_T file_id);
OPERATE_RET faqiuji_audio_preview_start(UINT8_T file_id);
OPERATE_RET faqiuji_audio_stop(VOID_T);
OPERATE_RET faqiuji_audio_set_volume(UINT8_T volume);
UINT8_T faqiuji_audio_get_volume(VOID_T);
BOOL_T faqiuji_audio_take_play_finished(VOID_T);
VOID_T faqiuji_audio_task(VOID_T);
BOOL_T faqiuji_audio_is_playing(VOID_T);
OPERATE_RET faqiuji_audio_record_start(UINT8_T file_id);
OPERATE_RET faqiuji_audio_record_stop(VOID_T);
BOOL_T faqiuji_audio_is_recording(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
