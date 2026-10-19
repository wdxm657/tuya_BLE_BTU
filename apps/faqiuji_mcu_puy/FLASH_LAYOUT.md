# PY32F040 IAP and APP Flash Layout

All end addresses are exclusive. This map assumes the PY32F040xB's 128 KB
Flash, 256-byte program pages, and 8 KB erase sectors.

| Region | Start | End | Size |
| --- | ---: | ---: | ---: |
| IAP | `0x08000000` | `0x08002000` | 8 KB |
| APP A | `0x08002000` | `0x0800E000` | 48 KB |
| APP B | `0x0800E000` | `0x0801A000` | 48 KB |
| OTA metadata | `0x0801A000` | `0x0801E000` | 16 KB |
| Reserved configuration | `0x0801E000` | `0x08020000` | 8 KB |

The canonical definitions and compile-time boundary checks are in
`faqiuji_flash_layout.h`.

## Image Layout

APP A and APP B are linked separately because their vector tables and code use
absolute addresses. Keil projects are:

- `faqiuji_mcu/MDK-ARM/Project.uvprojx`: APP A, link base `0x08002000`.
- `faqiuji_mcu/MDK-ARM/Project_APP_B.uvprojx`: APP B, link base `0x0800E000`.
- `faqiuji_bootloader/MDK-ARM/Project.uvprojx`: IAP, link base `0x08000000`.

Build and package the matching slot image; an A-linked binary cannot be
installed into B (or vice versa). `FAQIUJI_APP_VERSION` is the version reported
by the currently running build and should be updated for releases.

## Metadata and Selection

Each slot owns one 8 KB metadata sector. The first three pages are reserved:

| Page | Use |
| --- | --- |
| 0 | Update marker. A committed image is considered only when this is present. |
| 1 | CRC-protected committed image descriptor. |
| 2 | Permanent APP A factory-fallback guard; written before the first field update to A and never erased. |

The descriptor contains magic, slot base, image size, image CRC32, version,
generation, and a CRC32 over the descriptor fields. Image CRC32 is reflected
IEEE CRC32 (`0xEDB88320`, initial and final XOR `0xFFFFFFFF`).

The APP updater writes only the inactive slot. It first invalidates that slot's
old descriptor, writes image pages, verifies image CRC and vectors, and writes
the new descriptor last. IAP performs no OTA receive or Flash writes. At reset
it validates committed descriptors, chooses the valid slot with the newest
generation, and jumps to it. An untouched factory APP A with sane vectors may
be used only while its metadata pages, including the permanent guard, are
erased. This supports first programming from a factory APP A image without
pretending that it has an OTA CRC descriptor.

Power loss or a failed update leaves the other slot's committed image intact.
This is integrity fallback, not an application-health rollback: once an image
has passed CRC/vector checks and is committed, a later runtime fault does not
automatically restore the older version.

The last 8 KB is reserved for future PUYA-side configuration storage; this
implementation does not write it. TUYA's own saved DP configuration remains
separate.

## UART

IAP initializes only USART1 on PB8 TX / PB9 RX, AF9, 115200 8N1. It transmits
the existing framed `DEBUG_STRING` (`CMD 0x22`) messages for boot, slot
validation, and jump diagnostics. IAP does not receive OTA commands over UART.
APP OTA data is handled by the APP protocol; the future TUYA attachment-OTA
port must forward those commands over this UART while the APP is running.

If IAP cannot find an eligible image, it repeats a debug log and waits. It has
no serial recovery/update mode; recovery then requires SWD or another
programming method.

## Build and Flashing

Use the Keil projects listed above. The helper
`tools/update_keil_projects.ps1` verifies project source paths and Flash
regions; it does not compile firmware. Flash IAP and an initial APP A image
through SWD/programmer. Subsequent APP images are installed into the inactive
slot through APP OTA.

Do not download an APP A HEX as if it were an APP B image. Do not erase the
entire chip during an APP-only update because that also destroys IAP, both
slots, and OTA metadata.
