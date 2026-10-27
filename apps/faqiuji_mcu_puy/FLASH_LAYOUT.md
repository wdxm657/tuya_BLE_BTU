# PY32F040 IAP and APP Flash Layout

All end addresses are exclusive. This map assumes the PY32F040xB's 128 KB
Flash, 256-byte program pages, and 8 KB erase sectors.

| Region | Start | End | Size |
| --- | ---: | ---: | ---: |
| IAP | `0x08000000` | `0x08004000` | 16 KB |
| APP A (execution) | `0x08004000` | `0x08010000` | 48 KB |
| APP B (download) | `0x08010000` | `0x0801C000` | 48 KB |
| OTA metadata | `0x0801C000` | `0x0801E000` | 8 KB |
| Reserved configuration | `0x0801E000` | `0x08020000` | 8 KB |

The canonical definitions and compile-time boundary checks are in
`faqiuji_flash_layout.h`.

## Image Layout

All released firmware is linked for APP A. Keil projects are:

- `faqiuji_mcu/MDK-ARM/Project.uvprojx`: APP, link base `0x08004000`.
- `faqiuji_iap/MDK-ARM/Project.uvprojx`: IAP, link base `0x08000000`.

Publish `FAQIUJI_APP_A.bin` for every device. B stores the downloaded A-linked
image and is never executed. The old APP B project is no longer a release
target. Update the major/minor/patch macros for each release.

## Metadata and Selection

Each area owns one 4 KB half of the 8 KB metadata region. The first three pages
are reserved:

| Page | Use |
| --- | --- |
| 0 | Update marker. A committed image is considered only when this is present. |
| 1 | CRC-protected committed image descriptor. |
| 2 | Permanent APP A factory-fallback guard; written before the first field update to A and never erased. |

The descriptor contains magic, slot base, image size, image CRC32, version,
generation, and a CRC32 over the descriptor fields. Image CRC32 is reflected
IEEE CRC32 (`0xEDB88320`, initial and final XOR `0xFFFFFFFF`).

The APP updater writes only B. It invalidates B's descriptor, writes pages,
verifies CRC and A-linked vectors, and commits a staged descriptor
(`FAQIUJI_STAGED_IMAGE_MAGIC`). At reset IAP validates B and, if newer than A,
sets A's permanent guard, invalidates A's descriptor, and copies B to A. It
checks every programmed page and the complete A CRC, then commits A's normal
descriptor last. B and its descriptor remain intact throughout installation;
power loss causes installation to restart on the next boot. An A record of
the same generation prevents unnecessary copying. IAP only jumps to A.
An untouched factory APP A with sane vectors may
be used only while its metadata pages, including the permanent guard, are
erased. This supports first programming from a factory APP A image without
pretending that it has an OTA CRC descriptor.

Download failure leaves A intact. Once installation starts, B is the recovery
source; the previous A image is overwritten. This provides installation
recovery, not runtime health rollback.

The last 8 KB is reserved for future PUYA-side configuration storage; this
implementation does not write it. TUYA's own saved DP configuration remains
separate.

## UART

IAP initializes only USART1 on PB8 TX / PB9 RX, AF9, 115200 8N1. It transmits
the existing framed `DEBUG_STRING` (`CMD 0x22`) messages for boot, slot
validation, and jump diagnostics. IAP does not receive OTA commands over UART.
APP OTA data is handled by the APP protocol; TUYA attachment channel 10 forwards
these commands while APP A is running. INFO protocol version 2 identifies the
unified-image receiver.

If IAP cannot find an eligible image, it repeats a debug log and waits. It has
no serial recovery/update mode; recovery then requires SWD or another
programming method.

## Build and Flashing

Use the Keil projects listed above. Flash the new IAP and an initial APP A
through SWD/programmer before enabling this OTA flow. An old IAP cannot install
staged descriptors. Subsequent releases use the same A-linked image: download
to B through APP OTA, then install to A at reset.

Do not download an APP A HEX as if it were an APP B image. Do not erase the
entire chip during an APP-only update because that also destroys IAP, both
slots, and OTA metadata.
