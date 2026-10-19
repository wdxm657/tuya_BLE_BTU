#include "faqiuji_ota.h"
#include "faqiuji_device.h"
#include "faqiuji_launcher.h"
#include "../../common/faqiuji_image.h"
#include <stddef.h>
#include <string.h>

#define OTA_TIMEOUT_MS 30000UL
#define OTA_FLASH_ERROR 0x05U
#define OTA_BAD_OFFSET  0x06U
#define OTA_CRC_ERROR   0x07U
#define OTA_BAD_STATE   0x08U

static struct {
  volatile uint8_t active;
  volatile uint8_t ready;
  uint32_t base;
  uint32_t size;
  uint32_t crc;
  uint32_t version;
  uint32_t generation;
  uint32_t received;
  uint32_t flushed;
  uint32_t tick;
  uint32_t reset_tick;
  uint8_t reset_pending;
  /* HAL page programming requires an aligned full 256-byte RAM buffer. */
  uint32_t page[FAQIUJI_FLASH_PAGE_SIZE / 4U];
} sg_ota;

static uint32_t read32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
         ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void write32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8U);
  p[2] = (uint8_t)(value >> 16U);
  p[3] = (uint8_t)(value >> 24U);
}

uint8_t faqiuji_ota_busy(void)
{
  return sg_ota.active || sg_ota.ready;
}

static HAL_StatusTypeDef erase_page(uint32_t address)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t error;
  HAL_StatusTypeDef result;
  erase.TypeErase = FLASH_TYPEERASE_PAGEERASE;
  erase.PageAddress = address;
  erase.NbPages = 1U;
  if (HAL_FLASH_Unlock() != HAL_OK) {
    return HAL_ERROR;
  }
  result = HAL_FLASH_Erase(&erase, &error);
  (void)HAL_FLASH_Lock();
  return result;
}

static HAL_StatusTypeDef program_page(uint32_t address)
{
  HAL_StatusTypeDef result;
  if (HAL_FLASH_Unlock() != HAL_OK) {
    return HAL_ERROR;
  }
  result = HAL_FLASH_PageProgram(address, sg_ota.page);
  (void)HAL_FLASH_Lock();
  if (result == HAL_OK &&
      memcmp((const void *)address, sg_ota.page, sizeof(sg_ota.page)) != 0) {
    result = HAL_ERROR;
  }
  return result;
}

static uint8_t flush_page(void)
{
  uint32_t address = sg_ota.base + sg_ota.flushed;
  if (sg_ota.flushed >= FAQIUJI_FLASH_APP_A_SIZE ||
      erase_page(address) != HAL_OK || program_page(address) != HAL_OK) {
    return OTA_FLASH_ERROR;
  }
  sg_ota.flushed += FAQIUJI_FLASH_PAGE_SIZE;
  memset(sg_ota.page, 0xFF, sizeof(sg_ota.page));
  return FAQIUJI_STATUS_OK;
}

static uint8_t begin(const FAQIUJI_FRAME_T *frame)
{
  FAQIUJI_IMAGE_T current;
  uint32_t base, size, meta, i;
  if (frame->len != 16U) {
    return FAQIUJI_STATUS_BAD_LENGTH;
  }
  base = read32(frame->payload);
  size = read32(frame->payload + 4U);
  if (faqiuji_ota_busy()) {
    if (sg_ota.active && base == sg_ota.base && size == sg_ota.size &&
        read32(frame->payload + 8U) == sg_ota.crc &&
        read32(frame->payload + 12U) == sg_ota.version) {
      return FAQIUJI_STATUS_OK;
    }
    return FAQIUJI_STATUS_BUSY;
  }
  if (base != (FAQIUJI_APP_BASE == FAQIUJI_FLASH_APP_A_BASE ?
              FAQIUJI_FLASH_APP_B_BASE : FAQIUJI_FLASH_APP_A_BASE) ||
      size < 8U || size > FAQIUJI_FLASH_APP_A_SIZE) {
    return FAQIUJI_STATUS_BAD_PAYLOAD;
  }
  memset(&sg_ota, 0, sizeof(sg_ota));
  sg_ota.base = base;
  sg_ota.size = size;
  sg_ota.crc = read32(frame->payload + 8U);
  sg_ota.version = read32(frame->payload + 12U);
  sg_ota.generation = faqiuji_image_record(FAQIUJI_APP_BASE, &current) ?
                       current.generation + 1U : 1U;
  if (sg_ota.generation == 0xFFFFFFFFUL) {
    return OTA_BAD_STATE;
  }
  sg_ota.active = 1U;
  sg_ota.tick = HAL_GetTick();
  faqiuji_device_set_enabled(0U);
  faqiuji_launcher_stop();
  meta = faqiuji_image_meta(base);
  if (base == FAQIUJI_FLASH_APP_A_BASE) {
    /* Keep factory fallback disabled across repeated interrupted A updates. */
    for (i = 0U; i < FAQIUJI_FLASH_PAGE_SIZE; i += 4U) {
      if (*(const volatile uint32_t *)(meta +
          FAQIUJI_META_FACTORY_GUARD_OFFSET + i) != 0xFFFFFFFFUL) {
        break;
      }
    }
    if (i == FAQIUJI_FLASH_PAGE_SIZE) {
      memset(sg_ota.page, 0xFF, sizeof(sg_ota.page));
      sg_ota.page[0] = FAQIUJI_UPDATE_MARKER;
      if (program_page(meta + FAQIUJI_META_FACTORY_GUARD_OFFSET) != HAL_OK) {
        sg_ota.active = 0U;
        return OTA_FLASH_ERROR;
      }
    }
  }
  /*
   * Remove the old commit before invalidating the slot. The image is still
   * untouched if power fails here. Never erase the running slot's metadata.
   */
  if (erase_page(meta + FAQIUJI_META_RECORD_OFFSET) != HAL_OK ||
      erase_page(meta) != HAL_OK) {
    sg_ota.active = 0U;
    return OTA_FLASH_ERROR;
  }
  memset(sg_ota.page, 0xFF, sizeof(sg_ota.page));
  sg_ota.page[0] = FAQIUJI_UPDATE_MARKER;
  if (program_page(meta) != HAL_OK) {
    sg_ota.active = 0U;
    return OTA_FLASH_ERROR;
  }
  memset(sg_ota.page, 0xFF, sizeof(sg_ota.page));
  faqiuji_protocol_debug_string("OTA begin base=%08lX size=%lu",
                                (unsigned long)base, (unsigned long)size);
  return FAQIUJI_STATUS_OK;
}

static uint8_t data(const FAQIUJI_FRAME_T *frame)
{
  uint32_t offset, count, i, buffered;
  uint8_t result;
  if (frame->len < 5U) {
    return FAQIUJI_STATUS_BAD_LENGTH;
  }
  if (!sg_ota.active) {
    return OTA_BAD_STATE;
  }
  offset = read32(frame->payload);
  count = frame->len - 4U;
  if (offset > sg_ota.size || count > sg_ota.size - offset) {
    return FAQIUJI_STATUS_BAD_PAYLOAD;
  }
  /* Lost ACK retries are accepted only when the bytes match exactly. */
  if (offset < sg_ota.received && count <= sg_ota.received - offset) {
    for (i = 0U; i < count; ++i) {
      uint32_t pos = offset + i;
      uint8_t actual = pos < sg_ota.flushed ?
          *(const uint8_t *)(sg_ota.base + pos) :
          ((const uint8_t *)sg_ota.page)[pos - sg_ota.flushed];
      if (actual != frame->payload[4U + i]) {
        return OTA_BAD_OFFSET;
      }
    }
    return FAQIUJI_STATUS_OK;
  }
  if (offset != sg_ota.received) {
    return OTA_BAD_OFFSET;
  }
  for (i = 0U; i < count; ++i) {
    buffered = sg_ota.received - sg_ota.flushed;
    ((uint8_t *)sg_ota.page)[buffered] = frame->payload[4U + i];
    ++sg_ota.received;
    if (sg_ota.received - sg_ota.flushed == FAQIUJI_FLASH_PAGE_SIZE) {
      result = flush_page();
      if (result != FAQIUJI_STATUS_OK) {
        sg_ota.active = 0U;
        return result;
      }
    }
  }
  return FAQIUJI_STATUS_OK;
}

static uint8_t finish(void)
{
  FAQIUJI_IMAGE_T record;
  if (sg_ota.ready) {
    return FAQIUJI_STATUS_OK;
  }
  if (!sg_ota.active || sg_ota.received != sg_ota.size) {
    return OTA_BAD_STATE;
  }
  if (sg_ota.received > sg_ota.flushed &&
      flush_page() != FAQIUJI_STATUS_OK) {
    sg_ota.active = 0U;
    return OTA_FLASH_ERROR;
  }
  if (!faqiuji_image_vectors_valid(sg_ota.base, sg_ota.size) ||
      faqiuji_crc32((const void *)sg_ota.base, sg_ota.size) != sg_ota.crc) {
    sg_ota.active = 0U;
    return OTA_CRC_ERROR;
  }
  record.magic = FAQIUJI_IMAGE_MAGIC;
  record.base = sg_ota.base;
  record.size = sg_ota.size;
  record.crc32 = sg_ota.crc;
  record.version = sg_ota.version;
  record.generation = sg_ota.generation;
  record.record_crc = faqiuji_crc32(&record,
                                      offsetof(FAQIUJI_IMAGE_T, record_crc));
  memset(sg_ota.page, 0xFF, sizeof(sg_ota.page));
  memcpy(sg_ota.page, &record, sizeof(record));
  if (program_page(faqiuji_image_meta(sg_ota.base) +
                   FAQIUJI_META_RECORD_OFFSET) != HAL_OK) {
    sg_ota.active = 0U;
    return OTA_FLASH_ERROR;
  }
  sg_ota.active = 0U;
  sg_ota.ready = 1U;
  faqiuji_protocol_debug_string("OTA verified; reboot command required");
  return FAQIUJI_STATUS_OK;
}

void faqiuji_ota_handle(const FAQIUJI_FRAME_T *frame)
{
  uint8_t reply[24] = {0};
  uint8_t length = 5U;
  if (frame->cmd == FAQIUJI_CMD_OTA_INFO) {
    if (frame->len != 0U) {
      reply[0] = FAQIUJI_STATUS_BAD_LENGTH;
    } else {
      FAQIUJI_IMAGE_T current;
      uint32_t version = FAQIUJI_APP_VERSION;
      if (faqiuji_image_record(FAQIUJI_APP_BASE, &current)) {
        version = current.version;
      }
      reply[1] = 1U;
      write32(reply + 2U, FAQIUJI_APP_BASE);
      write32(reply + 6U, FAQIUJI_APP_BASE == FAQIUJI_FLASH_APP_A_BASE ?
                           FAQIUJI_FLASH_APP_B_BASE : FAQIUJI_FLASH_APP_A_BASE);
      write32(reply + 10U, FAQIUJI_FLASH_APP_A_SIZE);
      write32(reply + 14U, version);
      reply[18] = FAQIUJI_MAX_PAYLOAD - 4U;
      reply[19] = faqiuji_ota_busy();
      write32(reply + 20U, sg_ota.received);
      length = sizeof(reply);
    }
  } else if (frame->cmd == FAQIUJI_CMD_OTA_BEGIN) {
    reply[0] = begin(frame);
  } else if (frame->cmd == FAQIUJI_CMD_OTA_DATA) {
    reply[0] = data(frame);
  } else if (frame->len != 0U) {
    reply[0] = FAQIUJI_STATUS_BAD_LENGTH;
  } else if (frame->cmd == FAQIUJI_CMD_OTA_END) {
    reply[0] = finish();
  } else if (frame->cmd == FAQIUJI_CMD_OTA_ABORT) {
    /* A committed slot cannot be cancelled without erasing its commit. */
    if (sg_ota.ready &&
        erase_page(faqiuji_image_meta(sg_ota.base) +
                    FAQIUJI_META_RECORD_OFFSET) != HAL_OK) {
      reply[0] = OTA_FLASH_ERROR;
    } else {
      sg_ota.active = 0U;
      sg_ota.ready = 0U;
      sg_ota.reset_pending = 0U;
    }
  } else if (frame->cmd == FAQIUJI_CMD_OTA_REBOOT) {
    if (!sg_ota.ready) {
      reply[0] = OTA_BAD_STATE;
    } else {
      sg_ota.reset_pending = 1U;
      sg_ota.reset_tick = HAL_GetTick();
    }
  }
  if (frame->cmd != FAQIUJI_CMD_OTA_INFO) {
    write32(reply + 1U, sg_ota.received);
  }
  if (reply[0] == FAQIUJI_STATUS_OK &&
      (frame->cmd == FAQIUJI_CMD_OTA_BEGIN ||
       frame->cmd == FAQIUJI_CMD_OTA_DATA)) {
    sg_ota.tick = HAL_GetTick();
  }
  faqiuji_protocol_send(frame->cmd | FAQIUJI_RESPONSE_FLAG, frame->seq,
                         reply, length);
}

void faqiuji_ota_poll(void)
{
  if (sg_ota.reset_pending &&
      HAL_GetTick() - sg_ota.reset_tick >= 200U) {
    NVIC_SystemReset();
  }
  if (sg_ota.active && HAL_GetTick() - sg_ota.tick >= OTA_TIMEOUT_MS) {
    sg_ota.active = 0U;
    faqiuji_protocol_debug_string("OTA timeout; old application retained");
  }
}
