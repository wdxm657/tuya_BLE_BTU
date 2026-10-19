# 发球机控制串口协议

串口参数：`115200 8N1`，控制单片机使用 `PB8/USART1_TX` 和
`PB9/USART1_RX`。

## 帧格式

```text
AA 01 CMD SEQ LEN PAYLOAD... CRC_L CRC_H
```

CRC 为 Modbus/IBM CRC16，初值 `0xFFFF`，多项式 `0xA001`，校验范围是
`VERSION` 到 `PAYLOAD`，低字节先发送。

## 命令

| CMD | 请求数据 | 应答 CMD | 应答数据 |
| --- | --- | --- | --- |
| `0x01` PING | 空 | `0x81` | `STATUS` |
| `0x14` CONFIG_SET | `mode, max_count_lo, max_count_hi, standby_min_lo, standby_min_hi` | `0x94` | `STATUS` |
| `0x15` CONTROL_SET | `enabled` | `0x95` | `STATUS` |
| `0x16` CONFIG_SYNC | `version, enabled, mode, max_count_lo, max_count_hi, standby_min_lo, standby_min_hi` | `0x96` | `STATUS` |
| `0x20` KEY_EVENT | `pressed` | 无 | 按键状态变化主动上报 |
| `0x21` STATUS_EVENT | `event_type, event_data...` | 无 | 传感器/工作状态主动上报 |
| `0x22` DEBUG_STRING | UTF-8/ASCII 字符串字节 | 无 | PUYA 端调试文本主动上报 |
| `0x30` OTA_INFO | 空 | `0xB0` | 查询运行槽、备用槽和 OTA 状态 |
| `0x31` OTA_BEGIN | `base:u32, size:u32, crc32:u32, version:u32` | `0xB1` | 开始备用槽更新 |
| `0x32` OTA_DATA | `offset:u32, bytes...` | `0xB2` | 顺序写入数据，最多 60 字节 |
| `0x33` OTA_END | 空 | `0xB3` | 校验并提交新镜像 |
| `0x34` OTA_ABORT | 空 | `0xB4` | 取消会话/撤销尚未启动的新镜像 |
| `0x35` OTA_REBOOT | 空 | `0xB5` | 收到应答后延时重启 |

应答命令为请求命令加 `0x80`，应答中的 `SEQ` 原样返回。

`KEY_EVENT` 的 `PAYLOAD` 长度为 1：`0x00` 表示松开，`0x01` 表示按下；
该事件没有应答，仅在按键状态稳定变化时由控制单片机主动发送。

`CONFIG_SET` 的模式值为：`0=固定10米`、`1=固定20米`、`2=随机3-20米`。
随机距离由 PUYA 控制单片机本地计算，TUYA 端只下发模式。

`CONFIG_SYNC` 用于TUYA与PUYA建立通信后的全量配置同步。当前版本字段为
`0x01`，后续新增连接初始化配置时可在该版本协议中继续扩展。

`DEBUG_STRING` 由 PUYA 端调用 `faqiuji_protocol_debug_string()` 发送，支持
`%d`、`%u`、`%x`、`%s`、`%f` 和精度控制等 `printf` 格式，例如：

```c
faqiuji_protocol_debug_string("battery=%dmV voltage=%.2fV", battery_mv,
                              voltage);
```

TUYA 端使用 `TAL_PR_INFO` 输出为 `MCU DEBUG: ...`。格式化后的字符串单次最多
保留 255 个字符，超过 64 字节时会自动拆分为多个调试帧；每个调试帧沿用正常
协议的 CRC 校验。

`STATUS_EVENT` 的事件定义如下：

```text
0x01 KEY         byte 1: 0=released, 1=pressed
0x02 USB         byte 1: 0=removed, 1=inserted
0x03 CHARGE      byte 1: 0=not charging, 1=charging
0x04 BATTERY     byte 1: battery percentage
0x05 BALL        byte 1: 0=absent, 1=present
0x06 RADAR       byte 1: 0=not detected, 1=detected
0x07 TEMPERATURE byte 1-2: NTC ADC raw, little endian
0x08 WORK        byte 1: 0=work, 1=standby, 2=charging,
                  3=charge_done, 4=stopped
0x09 COUNT       byte 1-2: automatic launch count, little endian
```

## PUYA APP OTA

OTA 命令仅由运行中的 PUYA APP 处理，IAP 不接收或写入 OTA 数据。TUYA
附件 OTA 转发尚未接入；本节定义 PUYA UART 侧的传输合同，供后续 TUYA port
实现使用。

多字节整数均为小端。OTA DATA 使用 stop-and-wait：下一包的 offset 必须等于
已接收字节数；完整重复的包仅在内容与已接收数据完全相同才视为成功，便于
发送端在 ACK 丢失后重试。每包最多 60 字节图像数据，因此请求 payload 不超过
64 字节。图像大小最多 48 KB，APP A/B 镜像必须分别链接到各自槽地址。

`OTA_INFO` 成功响应的 payload 长 24 字节：

```text
0 status (0=OK)
1 protocol version (1)
2..5 running slot base
6..9 inactive slot base
10..13 slot capacity
14..17 running APP version
18 max OTA_DATA image bytes (60)
19 busy (0/1)
20..23 received offset
```

其余 OTA 响应 payload 长 5 字节：`status, received_offset:u32`。状态码
`0x05` Flash error、`0x06` bad offset、`0x07` image CRC/vector error、
`0x08` invalid OTA state。BEGIN 可使用与当前会话完全相同的参数重试；
不同参数的 BEGIN 在会话期间返回 BUSY。活动传输 30 秒无成功 BEGIN/DATA
后超时，未提交槽不参与启动。

推荐主机流程：查询 INFO，确认目标为 inactive slot 并核对固件链接地址；
发送 BEGIN；按 60 字节以内严格连续下发 DATA，每包等待相同 SEQ 的应答；
发送 END 并确认成功；再单独发送 REBOOT。ACK 丢失时使用原 SEQ 和原 payload
重试。ABORT 撤销已提交但尚未重启的新槽。写入、CRC 校验和入口向量检查通过
后才提交启动记录。旧槽不会因下载失败而擦除。

APP A 的 OTA metadata 第三页是永久 factory-fallback guard。第一次把 A 作为
备用槽更新时会置位；不可擦除这页作为常规清理操作，否则空 metadata 下的
工厂向量回退判断可能再次生效。

状态码：

```text
0x00 OK
0x01 BAD_LENGTH
0x02 BAD_PAYLOAD
0x03 UNKNOWN_CMD
0x04 BUSY
0x05 FLASH_ERROR (OTA)
0x06 BAD_OFFSET (OTA)
0x07 IMAGE_CRC_OR_VECTOR_ERROR (OTA)
0x08 BAD_OTA_STATE (OTA)
```

仅TUYA端每2秒主动发送一次`PING`，PUYA端收到请求后回复`0x81`。
TUYA端收到有效的`0x81`后将通信心跳标志置1；连续两个PING周期未收到有效回复
时清零。心跳标志由0变1时，TUYA端将保存的全部配置同步给PUYA端；其他时间
配置修改只发送对应的配置命令。
