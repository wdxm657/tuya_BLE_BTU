#include "main.h"
#include "app_bootloader.h"
#include "app_usart.h"
#include "../../common/faqiuji_image.h"
#include <stddef.h>
#include <string.h>

typedef FAQIUJI_IMAGE_T FAQIUJI_IMAGE_RECORD_T;

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

static uint8_t APP_Bootloader_VectorValidAt(uint32_t storage,
                                            uint32_t execution, uint32_t size)
{
  const uint32_t *vectors = (const uint32_t *)storage;
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
         (reset_handler & ~1UL) >= execution &&
         (reset_handler & ~1UL) < execution + size;
}

static uint8_t APP_Bootloader_VectorValid(uint32_t base, uint32_t size)
{
  return APP_Bootloader_VectorValidAt(base, base, size);
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
         (record->magic == FAQIUJI_IMAGE_MAGIC ||
          (base == FAQIUJI_FLASH_APP_B_BASE &&
           record->magic == FAQIUJI_STAGED_IMAGE_MAGIC)) &&
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
         record->magic == FAQIUJI_IMAGE_MAGIC &&
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

static uint8_t APP_Bootloader_StagedValid(FAQIUJI_IMAGE_RECORD_T *record)
{
  return APP_Bootloader_RecordValid(FAQIUJI_FLASH_APP_B_BASE, record) &&
         record->magic == FAQIUJI_STAGED_IMAGE_MAGIC &&
         APP_Bootloader_VectorValidAt(FAQIUJI_FLASH_APP_B_BASE,
                                      FAQIUJI_FLASH_APP_A_BASE, record->size) &&
         record->crc32 == APP_Bootloader_Crc32(
             (const void *)FAQIUJI_FLASH_APP_B_BASE, record->size);
}

static uint8_t APP_Bootloader_DirectBValid(void)
{
  uint32_t offset;

  /*
   * A direct programmer load has no staged descriptor. Keep it distinct from
   * an OTA transfer, which writes the B marker before modifying the image.
   */
  for (offset = 0U; offset < 2U * FAQIUJI_FLASH_PAGE_SIZE; offset += 4U) {
    if (*(const volatile uint32_t *)(FAQIUJI_META_B_BASE + offset) !=
        0xFFFFFFFFUL) {
      return 0U;
    }
  }
  return APP_Bootloader_VectorValidAt(FAQIUJI_FLASH_APP_B_BASE,
                                      FAQIUJI_FLASH_APP_A_BASE,
                                      FAQIUJI_FLASH_APP_B_SIZE);
}

static uint8_t APP_Bootloader_ErasePage(uint32_t address)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t error;
  HAL_StatusTypeDef result;
  erase.TypeErase = FLASH_TYPEERASE_PAGEERASE;
  erase.PageAddress = address;
  erase.NbPages = 1U;
  if (HAL_FLASH_Unlock() != HAL_OK) {
    return 0U;
  }
  result = HAL_FLASH_Erase(&erase, &error);
  (void)HAL_FLASH_Lock();
  return result == HAL_OK;
}

static uint8_t APP_Bootloader_ProgramPage(uint32_t address, uint32_t *page)
{
  HAL_StatusTypeDef result;
  if (HAL_FLASH_Unlock() != HAL_OK) {
    return 0U;
  }
  result = HAL_FLASH_PageProgram(address, page);
  (void)HAL_FLASH_Lock();
  return result == HAL_OK &&
         memcmp((const void *)address, page, FAQIUJI_FLASH_PAGE_SIZE) == 0;
}

static uint8_t APP_Bootloader_Install(const FAQIUJI_IMAGE_RECORD_T *staged)
{
  uint32_t page[FAQIUJI_FLASH_PAGE_SIZE / 4U];
  FAQIUJI_IMAGE_RECORD_T installed;
  uint32_t offset, count;
  uint32_t guard = FAQIUJI_META_A_BASE + FAQIUJI_META_FACTORY_GUARD_OFFSET;

  /* Never modify the verified B image or its commit while installing A. */
  for (offset = 0U; offset < FAQIUJI_FLASH_PAGE_SIZE; offset += 4U) {
    if (*(const volatile uint32_t *)(guard + offset) != 0xFFFFFFFFUL) {
      break;
    }
  }
  if (offset == FAQIUJI_FLASH_PAGE_SIZE) {
    memset(page, 0xFF, sizeof(page));
    page[0] = FAQIUJI_UPDATE_MARKER;
    if (!APP_Bootloader_ProgramPage(guard, page)) {
      return 0U;
    }
  }
  if (!APP_Bootloader_ErasePage(FAQIUJI_META_A_BASE +
                               FAQIUJI_META_RECORD_OFFSET) ||
      !APP_Bootloader_ErasePage(FAQIUJI_META_A_BASE)) {
    return 0U;
  }
  memset(page, 0xFF, sizeof(page));
  page[0] = FAQIUJI_UPDATE_MARKER;
  if (!APP_Bootloader_ProgramPage(FAQIUJI_META_A_BASE, page)) {
    return 0U;
  }
  for (offset = 0U; offset < staged->size; offset += sizeof(page)) {
    count = staged->size - offset;
    if (count > sizeof(page)) {
      count = sizeof(page);
    }
    memset(page, 0xFF, sizeof(page));
    memcpy(page, (const void *)(FAQIUJI_FLASH_APP_B_BASE + offset), count);
    if (!APP_Bootloader_ErasePage(FAQIUJI_FLASH_APP_A_BASE + offset) ||
        !APP_Bootloader_ProgramPage(FAQIUJI_FLASH_APP_A_BASE + offset, page)) {
      return 0U;
    }
  }
  if (!APP_Bootloader_VectorValid(FAQIUJI_FLASH_APP_A_BASE, staged->size) ||
      APP_Bootloader_Crc32((const void *)FAQIUJI_FLASH_APP_A_BASE,
                           staged->size) != staged->crc32) {
    return 0U;
  }
  installed = *staged;
  installed.magic = FAQIUJI_IMAGE_MAGIC;
  installed.base = FAQIUJI_FLASH_APP_A_BASE;
  installed.record_crc = APP_Bootloader_Crc32(
      &installed, offsetof(FAQIUJI_IMAGE_RECORD_T, record_crc));
  memset(page, 0xFF, sizeof(page));
  memcpy(page, &installed, sizeof(installed));
  return APP_Bootloader_ProgramPage(
      FAQIUJI_META_A_BASE + FAQIUJI_META_RECORD_OFFSET, page);
}

uint32_t APP_Bootloader_Select(void)
{
  FAQIUJI_IMAGE_RECORD_T record_a;
  FAQIUJI_IMAGE_RECORD_T record_b;
  uint8_t valid_a;
  uint32_t crc_a, crc_b;

  valid_a = APP_Bootloader_ImageValid(FAQIUJI_FLASH_APP_A_BASE, &record_a);
  if (APP_Bootloader_StagedValid(&record_b) &&
      (!valid_a || record_b.generation > record_a.generation)) {
    APP_USART_Debug("IAP install unified APP gen=%lu",
                    (unsigned long)record_b.generation);
    if (!APP_Bootloader_Install(&record_b)) {
      APP_USART_Debug("IAP install failed; staged image retained");
      return 0U;
    }
    APP_USART_Debug("IAP unified APP installed");
    return FAQIUJI_FLASH_APP_A_BASE;
  }
  if (APP_Bootloader_DirectBValid()) {
    crc_a = APP_Bootloader_Crc32((const void *)FAQIUJI_FLASH_APP_A_BASE,
                                 FAQIUJI_FLASH_APP_B_SIZE);
    crc_b = APP_Bootloader_Crc32((const void *)FAQIUJI_FLASH_APP_B_BASE,
                                 FAQIUJI_FLASH_APP_B_SIZE);
    if (!valid_a || crc_a != crc_b) {
      record_b.magic = FAQIUJI_STAGED_IMAGE_MAGIC;
      record_b.base = FAQIUJI_FLASH_APP_B_BASE;
      record_b.size = FAQIUJI_FLASH_APP_B_SIZE;
      record_b.crc32 = crc_b;
      record_b.version = FAQIUJI_APP_VERSION;
      record_b.generation = valid_a &&
                            record_a.generation < 0xFFFFFFFEUL ?
                            record_a.generation + 1U : 1U;
      record_b.record_crc = APP_Bootloader_Crc32(
          &record_b, offsetof(FAQIUJI_IMAGE_RECORD_T, record_crc));
      APP_USART_Debug("IAP install direct APP B crc=%08lX",
                      (unsigned long)crc_b);
      if (!APP_Bootloader_Install(&record_b)) {
        APP_USART_Debug("IAP direct APP install failed; B retained");
        return 0U;
      }
      APP_USART_Debug("IAP direct APP B installed");
      return FAQIUJI_FLASH_APP_A_BASE;
    }
  }
  if (valid_a != 0U) {
    APP_USART_Debug("IAP select APP A gen=%lu",
                    (unsigned long)record_a.generation);
    return FAQIUJI_FLASH_APP_A_BASE;
  }
  if (APP_Bootloader_FactoryAValid() != 0U) {
    APP_USART_Debug("IAP select factory APP A");
    return FAQIUJI_FLASH_APP_A_BASE;
  }

  APP_USART_Debug("IAP no valid APP SP=%08lX PC=%08lX",
      (unsigned long)*(const volatile uint32_t *)FAQIUJI_FLASH_APP_A_BASE,
      (unsigned long)*(const volatile uint32_t *)(FAQIUJI_FLASH_APP_A_BASE + 4U));
  APP_USART_Debug("IAP META A=%08lX guard=%08lX",
      (unsigned long)*(const volatile uint32_t *)FAQIUJI_META_A_BASE,
      (unsigned long)*(const volatile uint32_t *)(FAQIUJI_META_A_BASE +
                                               FAQIUJI_META_FACTORY_GUARD_OFFSET));
  return 0U;
}

void APP_Bootloader_Go(uint32_t address)
{
  uint32_t stack_pointer;
  uint32_t reset_handler;
  void (*application)(void);

  if (address != FAQIUJI_FLASH_APP_A_BASE) {
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
