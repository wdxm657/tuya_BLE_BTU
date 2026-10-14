# 发球机控制串口协议

串口参数：`9600 8N1`，控制单片机使用 `PB8/USART1_TX` 和
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

状态码：

```text
0x00 OK
0x01 BAD_LENGTH
0x02 BAD_PAYLOAD
0x03 UNKNOWN_CMD
0x04 BUSY
```

仅TUYA端每2秒主动发送一次`PING`，PUYA端收到请求后回复`0x81`。
TUYA端收到有效的`0x81`后将通信心跳标志置1；连续两个PING周期未收到有效回复
时清零。心跳标志由0变1时，TUYA端将保存的全部配置同步给PUYA端；其他时间
配置修改只发送对应的配置命令。
