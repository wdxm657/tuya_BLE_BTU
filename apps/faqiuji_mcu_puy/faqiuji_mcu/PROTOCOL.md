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

应答命令为请求命令加 `0x80`，应答中的 `SEQ` 原样返回。

状态码：

```text
0x00 OK
0x01 BAD_LENGTH
0x02 BAD_PAYLOAD
0x03 UNKNOWN_CMD
0x04 BUSY
```

当前 `LAUNCH` 和 `STOP` 只更新通信层工作状态，尚未驱动电机和发球机构。
