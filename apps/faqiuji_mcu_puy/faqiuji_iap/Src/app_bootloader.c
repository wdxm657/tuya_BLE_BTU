#include "main.h"
#include "app_bootloader.h"
#include "app_usart.h"
#include "../../faqiuji_flash_layout.h"
#include <stddef.h>
#include <string.h>

#define FAQIUJI_IMAGE_MAGIC   0x464A4931UL
#define FAQIUJI_UPDATE_MARKER 0x55504431UL

typedef struct {
  uint32_t magic;
  uint32_t base;
  uint32_t size;
  uint32_t crc32;
  uint32_t version;
  uint32_t generation;
  uint32_t record_crc;
} FAQIUJI_IMAGE_RECORD_T;

static uint32_t APP_Bootloader_Crc32(const void *data, uint32_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t i;
  uint8_t bit;

  for (i = 0U; i < length; ++i) {
    crc ^= bytes[i];
    for (bit = 0U; bit < 8U; ++bit) {
      crc = (crc & 1U) != 0U ?
            (crc >> 1U) ^ 0xEDB88320UL :
            crc >> 1U;
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

static uint8_t APP_Bootloader_VectorValid(uint32_t base, uint32_t size)
{
  const uint32_t *vectors = (const uint32_t *)base;
  uint32_t stack_pointer;
  uint32_t reset_handler;

  if (size < 8U || size > FAQIUJI_FLASH_APP_A_SIZE) {
    return 0U;
  }

  stack_pointer = vectors[0];
  reset_handler = vectors[1];
  return stack_pointer >= 0x20000000UL &&
         stack_pointer <= 0x20004000UL &&
         (stack_pointer & 7U) == 0U &&
         (reset_handler & 1U) != 0U &&
         (reset_handler & ~1UL) >= base &&
         (reset_handler & ~1UL) < base + size;
}

static uint32_t APP_Bootloader_Metadata(uint32_t base)
{
  if (base == FAQIUJI_FLASH_APP_A_BASE) {
    return FAQIUJI_META_A_BASE;
  }
  if (base == FAQIUJI_FLASH_APP_B_BASE) {
    return FAQIUJI_META_B_BASE;
  }
  return 0U;
}

static uint8_t APP_Bootloader_RecordValid(uint32_t base,
                                           FAQIUJI_IMAGE_RECORD_T *record)
{
  uint32_t metadata = APP_Bootloader_Metadata(base);

  if (metadata == 0U || record == NULL) {
    return 0U;
  }

  memcpy(record, (const void *)(metadata + FAQIUJI_META_RECORD_OFFSET),
         sizeof(*record));
  return *(const volatile uint32_t *)metadata == FAQIUJI_UPDATE_MARKER &&
         record->magic == FAQIUJI_IMAGE_MAGIC &&
         record->base == base &&
         record->size >= 8U &&
         record->size <= FAQIUJI_FLASH_APP_A_SIZE &&
         record->generation != 0U &&
         record->generation != 0xFFFFFFFFUL &&
         record->record_crc ==
             APP_Bootloader_Crc32(record,
                                  offsetof(FAQIUJI_IMAGE_RECORD_T,
                                           record_crc));
}

static uint8_t APP_Bootloader_ImageValid(uint32_t base,
                                          FAQIUJI_IMAGE_RECORD_T *record)
{
  return APP_Bootloader_RecordValid(base, record) != 0U &&
         APP_Bootloader_VectorValid(base, record->size) != 0U &&
         record->crc32 == APP_Bootloader_Crc32((const void *)base,
                                                record->size);
}

static uint8_t APP_Bootloader_FactoryAValid(void)
{
  uint32_t offset;

  for (offset = 0U;
       offset < 3U * FAQIUJI_FLASH_PAGE_SIZE;
       offset += 4U) {
    if (*(const volatile uint32_t *)(FAQIUJI_META_A_BASE + offset) !=
        0xFFFFFFFFUL) {
      return 0U;
    }
  }
  return APP_Bootloader_VectorValid(FAQIUJI_FLASH_APP_A_BASE,
                                    FAQIUJI_FLASH_APP_A_SIZE);
}

uint32_t APP_Bootloader_Select(void)
{
  FAQIUJI_IMAGE_RECORD_T record_a;
  FAQIUJI_IMAGE_RECORD_T record_b;
  uint8_t valid_a;
  uint8_t valid_b;

  valid_a = APP_Bootloader_ImageValid(FAQIUJI_FLASH_APP_A_BASE, &record_a);
  valid_b = APP_Bootloader_ImageValid(FAQIUJI_FLASH_APP_B_BASE, &record_b);

  if (valid_a != 0U && valid_b != 0U) {
    if (record_b.generation > record_a.generation) {
      APP_USART_Debug("IAP select APP B gen=%lu",
                      (unsigned long)record_b.generation);
      return FAQIUJI_FLASH_APP_B_BASE;
    }
    APP_USART_Debug("IAP select APP A gen=%lu",
                    (unsigned long)record_a.generation);
    return FAQIUJI_FLASH_APP_A_BASE;
  }
  if (valid_a != 0U) {
    APP_USART_Debug("IAP select APP A gen=%lu",
                    (unsigned long)record_a.generation);
    return FAQIUJI_FLASH_APP_A_BASE;
  }
  if (valid_b != 0U) {
    APP_USART_Debug("IAP select APP B gen=%lu",
                    (unsigned long)record_b.generation);
    return FAQIUJI_FLASH_APP_B_BASE;
  }
  if (APP_Bootloader_FactoryAValid() != 0U) {
    APP_USART_Debug("IAP select factory APP A");
    return FAQIUJI_FLASH_APP_A_BASE;
  }

  APP_USART_Debug("IAP no valid APP");
  return 0U;
}

void APP_Bootloader_Go(uint32_t address)
{
  uint32_t stack_pointer;
  uint32_t reset_handler;
  void (*application)(void);

  if (address != FAQIUJI_FLASH_APP_A_BASE &&
      address != FAQIUJI_FLASH_APP_B_BASE) {
    return;
  }
  if (APP_Bootloader_VectorValid(address, FAQIUJI_FLASH_APP_A_SIZE) == 0U) {
    APP_USART_Debug("IAP reject APP vector base=%08lX",
                    (unsigned long)address);
    return;
  }

  stack_pointer = *(const volatile uint32_t *)address;
  reset_handler = *(const volatile uint32_t *)(address + 4U);
  application = (void (*)(void))reset_handler;

  APP_USART_Debug("IAP jump APP base=%08lX", (unsigned long)address);
  HAL_Delay(2U);
  APP_USART_DeInit();

  /*
   * Match the vendor IAP handoff sequence: return SYSCLK to HSI before
   * entering the application. The application startup code may then
   * configure its own oscillator and PLL state from a known clock source.
   */
  CLEAR_BIT(RCC->CFGR, RCC_CFGR_SW);
  while (READ_BIT(RCC->CFGR, RCC_CFGR_SWS) != 0U) {
  }
  CLEAR_BIT(RCC->CR, RCC_CR_PLLON);

  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;
  SCB->VTOR = address;
  __DSB();
  __set_MSP(stack_pointer);
  __DSB();
  __ISB();
  application();

  while (1) {
  }
}
