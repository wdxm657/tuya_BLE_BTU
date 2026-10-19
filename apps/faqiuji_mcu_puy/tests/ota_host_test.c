/* Linux host tests of the actual OTA/image code; no MCU toolchain required. */
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../common/faqiuji_image.h"

/* Replace only the hardware/RTOS headers, keeping the production OTA code. */
#define FAQIUJI_OTA_H
#define FAQIUJI_DEVICE_H
#define FAQIUJI_LAUNCHER_H
#define FAQIUJI_MAX_PAYLOAD 64U
#define FAQIUJI_RESPONSE_FLAG 0x80U
#define FAQIUJI_STATUS_OK 0U
#define FAQIUJI_STATUS_BAD_LENGTH 1U
#define FAQIUJI_STATUS_BAD_PAYLOAD 2U
#define FAQIUJI_STATUS_BUSY 4U
#define FAQIUJI_CMD_OTA_INFO 0x30U
#define FAQIUJI_CMD_OTA_BEGIN 0x31U
#define FAQIUJI_CMD_OTA_DATA 0x32U
#define FAQIUJI_CMD_OTA_END 0x33U
#define FAQIUJI_CMD_OTA_ABORT 0x34U
#define FAQIUJI_CMD_OTA_REBOOT 0x35U
#define FLASH_TYPEERASE_PAGEERASE 1U
typedef int HAL_StatusTypeDef;
#define HAL_OK 0
#define HAL_ERROR 1
typedef struct {
  uint32_t TypeErase, PageAddress, NbPages;
} FLASH_EraseInitTypeDef;
typedef struct {
  uint8_t cmd, seq, len, payload[64];
} FAQIUJI_FRAME_T;

static uint32_t tick, resets, stops, mutations, cut_at;
static int locked = 1;
static uint8_t response[64], response_len;
static uint8_t image[600];
static jmp_buf power_cut;

static uint32_t target_base(void)
{
  return FAQIUJI_APP_BASE == FAQIUJI_FLASH_APP_A_BASE ?
             FAQIUJI_FLASH_APP_B_BASE : FAQIUJI_FLASH_APP_A_BASE;
}

static void mutation(uint32_t address)
{
  uint32_t meta = faqiuji_image_meta(target_base());
  assert(!locked);
  assert((address & 255U) == 0U);
  assert((address >= target_base() &&
          address + 256U <= target_base() + FAQIUJI_FLASH_APP_A_SIZE) ||
         (address >= meta && address + 256U <= meta + 3U * 256U));
  if (++mutations == cut_at) {
    longjmp(power_cut, 1);
  }
}

static HAL_StatusTypeDef HAL_FLASH_Unlock(void) { locked = 0; return HAL_OK; }
static HAL_StatusTypeDef HAL_FLASH_Lock(void) { locked = 1; return HAL_OK; }
static HAL_StatusTypeDef HAL_FLASH_Erase(FLASH_EraseInitTypeDef *erase,
                                        uint32_t *error)
{
  (void)error;
  assert(erase->NbPages == 1U);
  mutation(erase->PageAddress);
  memset((void *)(uintptr_t)erase->PageAddress, 255, 256);
  return HAL_OK;
}
static HAL_StatusTypeDef HAL_FLASH_PageProgram(uint32_t address, uint32_t *page)
{
  uint32_t i;
  uint8_t *flash = (uint8_t *)(uintptr_t)address;
  mutation(address);
  for (i = 0; i < 256; ++i) {
    assert((flash[i] & ((uint8_t *)page)[i]) == ((uint8_t *)page)[i]);
    flash[i] &= ((uint8_t *)page)[i];
  }
  return HAL_OK;
}
static uint32_t HAL_GetTick(void) { return tick; }
static void NVIC_SystemReset(void) { ++resets; }
static void faqiuji_device_set_enabled(uint8_t value) { assert(value == 0); }
static void faqiuji_launcher_stop(void) { ++stops; }
static void faqiuji_protocol_debug_string(const char *format, ...) { (void)format; }
static void faqiuji_protocol_send(uint8_t cmd, uint8_t seq,
                                  const uint8_t *payload, uint8_t len)
{
  assert((cmd & 0x80U) && seq == 7U);
  response_len = len;
  memcpy(response, payload, len);
}

#include "../faqiuji_mcu/Src/faqiuji_ota.c"

static uint8_t request(uint8_t cmd, const void *payload, uint8_t len)
{
  FAQIUJI_FRAME_T frame = {0};
  frame.cmd = cmd;
  frame.seq = 7U;
  frame.len = len;
  if (len) { memcpy(frame.payload, payload, len); }
  faqiuji_ota_handle(&frame);
  return response[0];
}

static void vectors(uint32_t base, uint8_t *bytes)
{
  write32(bytes, 0x20004000U);
  write32(bytes + 4, base + 0x101U);
}

static void committed(uint32_t base, uint32_t generation)
{
  FAQIUJI_IMAGE_T record;
  uint8_t *flash = (uint8_t *)(uintptr_t)base;
  memset(flash, 0x5A, 600);
  vectors(base, flash);
  record.magic = FAQIUJI_IMAGE_MAGIC;
  record.base = base;
  record.size = 600;
  record.crc32 = faqiuji_crc32(flash, 600);
  record.version = 0x10000U;
  record.generation = generation;
  record.record_crc = faqiuji_crc32(&record, 24);
  *(uint32_t *)(uintptr_t)faqiuji_image_meta(base) = FAQIUJI_UPDATE_MARKER;
  memcpy((void *)(uintptr_t)(faqiuji_image_meta(base) + 256), &record, sizeof(record));
}

static void setup(void)
{
  memset((void *)(uintptr_t)FAQIUJI_FLASH_BASE, 255, FAQIUJI_FLASH_SIZE);
  memset(&sg_ota, 0, sizeof(sg_ota));
  tick = resets = stops = mutations = cut_at = 0;
  locked = 1;
  committed(FAQIUJI_APP_BASE, 4);
  memset(image, 0xA5, sizeof(image));
  vectors(target_base(), image);
}

static uint8_t start(uint32_t crc)
{
  uint8_t payload[16];
  write32(payload, target_base());
  write32(payload + 4, sizeof(image));
  write32(payload + 8, crc);
  write32(payload + 12, 0x20000U);
  return request(FAQIUJI_CMD_OTA_BEGIN, payload, sizeof(payload));
}

static void transfer(void)
{
  uint32_t offset;
  uint8_t payload[64];
  for (offset = 0; offset < sizeof(image); offset += 60) {
    write32(payload, offset);
    memcpy(payload + 4, image + offset, 60);
    assert(request(FAQIUJI_CMD_OTA_DATA, payload, 64) == 0);
    assert(request(FAQIUJI_CMD_OTA_DATA, payload, 64) == 0);
  }
}

int main(void)
{
  FAQIUJI_IMAGE_T record;
  uint8_t bad[16] = {0};
  uint32_t cut;
  void *flash = mmap((void *)(uintptr_t)FAQIUJI_FLASH_BASE,
      FAQIUJI_FLASH_SIZE, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  assert(flash == (void *)(uintptr_t)FAQIUJI_FLASH_BASE);
  assert(faqiuji_crc32("123456789", 9) == 0xCBF43926U);
  setup();
  assert(request(FAQIUJI_CMD_OTA_INFO, NULL, 0) == 0 && response_len == 24);
  assert(read32(response + 2) == FAQIUJI_APP_BASE);
  assert(read32(response + 6) == target_base());
  assert(start(faqiuji_crc32(image, sizeof(image))) == 0);
  assert(stops == 1 && faqiuji_ota_busy());
  assert(start(faqiuji_crc32(image, sizeof(image))) == 0); /* lost BEGIN ACK */
  write32(bad, 1);
  assert(request(FAQIUJI_CMD_OTA_DATA, bad, 5) == OTA_BAD_OFFSET);
  write32(bad, 600);
  assert(request(FAQIUJI_CMD_OTA_DATA, bad, 5) == FAQIUJI_STATUS_BAD_PAYLOAD);
  assert(request(FAQIUJI_CMD_OTA_END, NULL, 0) == OTA_BAD_STATE);
  assert(request(FAQIUJI_CMD_OTA_ABORT, NULL, 0) == 0);
  assert(!faqiuji_ota_busy());
  assert(faqiuji_image_valid(FAQIUJI_APP_BASE, &record));
  assert(!faqiuji_image_valid(target_base(), &record));
  assert(start(faqiuji_crc32(image, sizeof(image))) == 0);
  transfer();
  assert(request(FAQIUJI_CMD_OTA_END, NULL, 0) == 0);
  assert(request(FAQIUJI_CMD_OTA_END, NULL, 0) == 0); /* lost END ACK */
  assert(faqiuji_image_valid(target_base(), &record) && record.generation == 5);
  assert(faqiuji_image_valid(FAQIUJI_APP_BASE, &record));
  assert(request(FAQIUJI_CMD_OTA_REBOOT, NULL, 0) == 0);
  tick = 199; faqiuji_ota_poll(); assert(resets == 0);
  tick = 200; faqiuji_ota_poll(); assert(resets == 1);
  assert(request(FAQIUJI_CMD_OTA_ABORT, NULL, 0) == 0);
  assert(!faqiuji_image_valid(target_base(), &record) && !faqiuji_ota_busy());
  setup();
  assert(start(0) == 0); transfer();
  assert(request(FAQIUJI_CMD_OTA_END, NULL, 0) == OTA_CRC_ERROR);
  assert(!faqiuji_image_valid(target_base(), &record));
  setup();
  image[3] = 0x00U; /* MSP outside the PY32 SRAM range. */
  assert(start(faqiuji_crc32(image, sizeof(image))) == 0);
  transfer();
  assert(request(FAQIUJI_CMD_OTA_END, NULL, 0) == OTA_CRC_ERROR);
  assert(!faqiuji_image_valid(target_base(), &record));
  setup();
  assert(start(faqiuji_crc32(image, sizeof(image))) == 0);
  tick = 30001; request(FAQIUJI_CMD_OTA_INFO, NULL, 0);
  faqiuji_ota_poll(); assert(!faqiuji_ota_busy()); /* INFO doesn't extend timeout */
  /* Inject power loss before every erase/program, including commit. */
  for (cut = 1; cut <= 13; ++cut) {
    setup();
    cut_at = cut;
    if (setjmp(power_cut) == 0) {
      assert(start(faqiuji_crc32(image, sizeof(image))) == 0);
      transfer();
      assert(request(FAQIUJI_CMD_OTA_END, NULL, 0) == 0);
    }
    assert(faqiuji_image_valid(FAQIUJI_APP_BASE, &record));
    assert(!faqiuji_image_valid(target_base(), &record) ||
           memcmp((void *)(uintptr_t)target_base(), image, sizeof(image)) == 0);
    if (target_base() == FAQIUJI_FLASH_APP_A_BASE && cut > 1) {
      assert(!faqiuji_image_factory_a());
    }
  }
  setup();
  memset((void *)(uintptr_t)FAQIUJI_META_A_BASE, 255, 768);
  vectors(FAQIUJI_FLASH_APP_A_BASE, (uint8_t *)(uintptr_t)FAQIUJI_FLASH_APP_A_BASE);
  assert(faqiuji_image_factory_a());
  *(uint32_t *)(uintptr_t)(FAQIUJI_META_A_BASE + 512) = FAQIUJI_UPDATE_MARKER;
  assert(!faqiuji_image_factory_a());
  printf("slot %c: OTA success/retry/bounds/CRC/timeout/reset/power-cut tests OK\n",
         FAQIUJI_APP_BASE == FAQIUJI_FLASH_APP_A_BASE ? 'A' : 'B');
  return 0;
}
