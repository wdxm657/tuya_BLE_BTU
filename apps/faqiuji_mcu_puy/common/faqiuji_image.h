#ifndef FAQIUJI_IMAGE_H
#define FAQIUJI_IMAGE_H

#include <stdint.h>
#include "../faqiuji_flash_layout.h"

#define FAQIUJI_IMAGE_MAGIC 0x464A4931UL
#define FAQIUJI_UPDATE_MARKER 0x55504431UL

typedef struct {
  uint32_t magic;
  uint32_t base;
  uint32_t size;
  uint32_t crc32;
  uint32_t version;
  uint32_t generation;
  uint32_t record_crc;
} FAQIUJI_IMAGE_T;

uint32_t faqiuji_crc32(const void *data, uint32_t length);
uint32_t faqiuji_image_meta(uint32_t base);
uint8_t faqiuji_image_vectors_valid(uint32_t base, uint32_t size);
uint8_t faqiuji_image_record(uint32_t base, FAQIUJI_IMAGE_T *record);
uint8_t faqiuji_image_valid(uint32_t base, FAQIUJI_IMAGE_T *record);
uint8_t faqiuji_image_factory_a(void);

#endif
