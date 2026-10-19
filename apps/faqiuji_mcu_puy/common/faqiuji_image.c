#include "faqiuji_image.h"
#include <stddef.h>
#include <string.h>

uint32_t faqiuji_crc32(const void *data, uint32_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t i;
  uint8_t bit;
  for (i = 0U; i < length; ++i) {
    crc ^= bytes[i];
    for (bit = 0U; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

uint32_t faqiuji_image_meta(uint32_t base)
{
  return base == FAQIUJI_FLASH_APP_A_BASE ? FAQIUJI_META_A_BASE :
         base == FAQIUJI_FLASH_APP_B_BASE ? FAQIUJI_META_B_BASE : 0U;
}

uint8_t faqiuji_image_vectors_valid(uint32_t base, uint32_t size)
{
  const uint32_t *vectors;
  uint32_t entry;
  if (faqiuji_image_meta(base) == 0U || size < 8U ||
      size > FAQIUJI_FLASH_APP_A_SIZE) {
    return 0U;
  }
  vectors = (const uint32_t *)base;
  entry = vectors[1] & ~1UL;
  return vectors[0] >= 0x20000000UL && vectors[0] <= 0x20004000UL &&
         (vectors[0] & 7U) == 0U && (vectors[1] & 1U) != 0U &&
         entry >= base && entry < base + size;
}

uint8_t faqiuji_image_record(uint32_t base, FAQIUJI_IMAGE_T *record)
{
  uint32_t meta = faqiuji_image_meta(base);
  if (meta == 0U || record == NULL) {
    return 0U;
  }
  memcpy(record, (const void *)(meta + FAQIUJI_META_RECORD_OFFSET),
         sizeof(*record));
  return *(const volatile uint32_t *)meta == FAQIUJI_UPDATE_MARKER &&
         record->magic == FAQIUJI_IMAGE_MAGIC && record->base == base &&
         record->generation != 0U && record->generation != 0xFFFFFFFFUL &&
         record->size >= 8U && record->size <= FAQIUJI_FLASH_APP_A_SIZE &&
         record->record_crc == faqiuji_crc32(record,
                                      offsetof(FAQIUJI_IMAGE_T, record_crc));
}

uint8_t faqiuji_image_valid(uint32_t base, FAQIUJI_IMAGE_T *record)
{
  return faqiuji_image_record(base, record) &&
         faqiuji_image_vectors_valid(base, record->size) &&
         record->crc32 == faqiuji_crc32((const void *)base, record->size);
}

uint8_t faqiuji_image_factory_a(void)
{
  uint32_t offset;
  /* The third page is a permanent guard, never erased by field OTA. */
  for (offset = 0U; offset < 3U * FAQIUJI_FLASH_PAGE_SIZE; offset += 4U) {
    if (*(const volatile uint32_t *)(FAQIUJI_META_A_BASE + offset) !=
        0xFFFFFFFFUL) {
      return 0U;
    }
  }
  return faqiuji_image_vectors_valid(FAQIUJI_FLASH_APP_A_BASE,
                                      FAQIUJI_FLASH_APP_A_SIZE);
}
