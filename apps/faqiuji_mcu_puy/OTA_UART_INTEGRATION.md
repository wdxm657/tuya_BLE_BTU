# PUYA UART OTA Host Contract

The PUYA application OTA engine is implemented in
`faqiuji_mcu/Src/faqiuji_ota.c`. IAP only reports boot diagnostics over the same
UART; it does not receive an update session. TUYA attachment-OTA data forwarding
is a separate integration step and is not yet implemented by this change.

## UART / Frame

UART1 is PB8 TX / PB9 RX, AF9, 115200 baud, 8 data bits, no parity, one stop
bit. Both IAP debug strings and APP protocol frames use the existing framed
protocol documented in `faqiuji_mcu/PROTOCOL.md`:

```text
AA 01 CMD SEQ LEN PAYLOAD CRC_L CRC_H
```

CRC is Modbus CRC16 over version through payload. Request replies set bit 7 of
CMD and echo SEQ. IAP emits CMD `0x22` DEBUG_STRING logs only; no IAP OTA
commands exist.

## Transfer

Query `0x30` INFO before starting. Its 24-byte response tells the running base,
inactive base, capacity, reported running version, 60-byte maximum data chunk,
busy state, and current received offset. Send `0x31` BEGIN with four little
endian 32-bit values: target base, image size, image CRC32, and new version.
Send `0x32` DATA as little-endian byte offset followed by 1..60 image bytes.
Send DATA in strict stop-and-wait order and await each response before sending
the next one. After all bytes, send `0x33` END. Only after its success response
send `0x35` REBOOT. An optional `0x34` ABORT cancels an incomplete session or
removes the new commit before reboot.

Retries of an identical BEGIN during an active session and byte-identical DATA
already received are accepted to tolerate a lost UART reply. A DATA packet at
any other offset, or a duplicate with changed contents, is rejected. Sessions
expire after 30 seconds without a successful BEGIN or DATA request; a timed-out
slot is not committed.

No UART transfer routine may enqueue OTA requests faster than the previous
response. Flash operations are synchronous and can stall application tasks;
the APP intentionally stops the launcher and rejects power/control commands
during the update. Keep UART flow control at the application protocol layer.

## Image / Package Requirements

The PY32F040xB layout is:

| Region | Start | Size |
| --- | ---: | ---: |
| IAP | `0x08000000` | 8 KB |
| APP A | `0x08002000` | 48 KB |
| APP B | `0x0800E000` | 48 KB |
| Slot metadata | `0x0801A000` | 16 KB |
| Reserved config | `0x0801E000` | 8 KB |

APP A and B are separate linked images, not a single position-independent BIN.
Firmware distribution must match image base to target slot. INFO chooses the
inactive slot at runtime; host must possess an image linked for that exact
base. Validate package base, size, and CRC32 before sending. CRC32 is reflected
IEEE with polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`.

The app commits metadata only after image CRC and vector validation. The IAP
chooses the newest valid committed generation and falls back to the other valid
slot on corruption. This is not runtime health rollback. The first factory APP
A image may start without a metadata record when its vectors are valid and its
metadata guard remains erased. Before field-updating A, APP permanently writes
the guard at metadata page 2; never bulk erase the metadata sector.

## TUYA Attachment OTA Work Remaining

The project includes Tuya's attachment-OTA framework and its port currently
uses the Tuya chip's own Flash path. To use the PUYA protocol, implement a
dedicated external-MCU port that:

1. Uses the supported external-MCU OTA route/channel from the product/platform
   configuration, or an enabled attachment channel if the project chooses that
   route. Do not assume that channel 9 is accepted by this SDK without checking
   its feature configuration; the included attachment framework documents
   channels 10-19.
2. Obtains PUYA INFO and selects the firmware artifact whose link base matches
   its inactive slot. The OTA file metadata exposed to the Tuya framework must
   describe the selected artifact consistently.
3. Translates start/data/end callbacks into BEGIN/DATA/END UART commands, waits
   for matching replies, handles offset and CRC failures, and requests REBOOT
   only after END succeeds.
4. Reports the running PUYA version through the appropriate Tuya version API
   and tests reconnect, timeout, lost-response retry, power-loss recovery, and
   APP A/B image selection.

Current firmware does not yet bridge BLE OTA callbacks to these PUYA commands,
so Tuya app OTA cannot be considered end-to-end until this adapter and its
product package selection are added.
