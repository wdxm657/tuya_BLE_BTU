# PUYA UART OTA Host Contract

The PUYA application OTA engine is implemented in
`faqiuji_mcu/Src/faqiuji_ota.c`. IAP only reports boot diagnostics over the same
UART; it does not receive an update session. TUYA attachment channel 10 forwards
the image to APP A, which stores it in B for installation by IAP.

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
download base, capacity, reported running version, 60-byte maximum data chunk,
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
| IAP | `0x08000000` | 16 KB |
| APP A (execution) | `0x08004000` | 48 KB |
| APP B (download) | `0x08010000` | 48 KB |
| Area metadata | `0x0801C000` | 8 KB |
| Reserved config | `0x0801E000` | 8 KB |

Publish one image linked for A (`0x08004000`) to every device. INFO protocol
version 2 reports A as the running region and B as download storage. The host
sends the unchanged image to B; IAP copies it to A before execution. Validate
image size, A-linked vectors, and CRC32 before committing. CRC32 is reflected
IEEE with polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`.

APP commits B's staged descriptor only after CRC and vector validation. IAP
copies a newer staged image to A and commits A's descriptor only after another
CRC check. B remains intact for recovery after interrupted installation.
The first factory A may start without a descriptor while its guard remains
erased. IAP sets the permanent guard before installation; never bulk erase
metadata during an update. This is installation recovery, not runtime rollback.

## TUYA Attachment OTA

The component port implements INFO, BEGIN, DATA, END, ABORT and REBOOT over the
existing UART protocol. It requires INFO protocol version 2 before accepting
an upgrade, splits BLE packets into at most 60 image bytes per UART command,
and verifies reply status and offsets. It reports the actual running version
after boot and after OTA. Deploy the updated IAP and APP together; updating only
TUYA cannot enable staged installation in an old PUYA IAP.
