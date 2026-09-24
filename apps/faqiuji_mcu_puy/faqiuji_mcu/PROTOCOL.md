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
| `0x10` LAUNCH | `speed, angle, interval_lo, interval_hi` | `0x90` | `STATUS` |
| `0x11` STOP | 空 | `0x91` | `STATUS` |
| `0x12` STATUS_GET | 空 | `0x92` | `STATUS, work_state, speed, angle, interval_lo, interval_hi` |
| `0x13` PARAM_SET | `speed, angle, interval_lo, interval_hi` | `0x93` | `STATUS` |
| `0x14` CONFIG_SET | `mode, max_count_lo, max_count_hi, standby_min_lo, standby_min_hi` | `0x94` | `STATUS` |
| `0x15` CONTROL_SET | `enabled` | `0x95` | `STATUS` |
| `0x20` KEY_EVENT | `pressed` | 无 | 按键状态变化主动上报 |
| `0x21` STATUS_EVENT | `event_type, event_data...` | 无 | 传感器/工作状态主动上报 |

应答命令为请求命令加 `0x80`，应答中的 `SEQ` 原样返回。

`KEY_EVENT` 的 `PAYLOAD` 长度为 1：`0x00` 表示松开，`0x01` 表示按下；
该事件没有应答，仅在按键状态稳定变化时由控制单片机主动发送。

`CONFIG_SET` 的模式值为：`0=固定10米`、`1=固定20米`、`2=随机3-20米`。
随机距离由 PUYA 控制单片机本地计算，TUYA 端只下发模式。

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

当前 `LAUNCH` 和 `STOP` 只更新通信层工作状态，尚未驱动电机和发球机构。
