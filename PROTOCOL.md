# 协议说明

两端使用同一套小端二进制协议，版本号为 1。这是未加密的学习原型，文件内容没有加密或认证。

## 标识

- Service UUID：`6b8a1f40-2c7d-4e15-9a33-0d5e7c1b4a86`
- RX：`6b8a1f41-2c7d-4e15-9a33-0d5e7c1b4a86`，Windows 写入，Android 接收
- TX：`6b8a1f42-2c7d-4e15-9a33-0d5e7c1b4a86`，Android Notify，Windows 订阅
- 角色：Android = 1，Windows = 2

蓝牙广播里的 128-bit UUID 使用标准线序。Android 另在 scan response 的 Service Data 中放入设备 ID 的 8 个字符，完整 ID 在 HELLO 里确认。

## BLE 分片

每个 GATT 包：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | u8 | 版本，固定 1 |
| 1 | u8 | 消息类型 |
| 2 | u16 | 消息 ID |
| 4 | u16 | 分片序号，从 0 开始 |
| 6 | u16 | 分片总数 |
| 8 | u16 | 本片载荷长度 |
| 10 | u16 | 本片载荷的 CRC-16/CCITT-FALSE |
| 12 | 字节 | 载荷 |

CRC 多项式 `0x1021`，初始值 `0xFFFF`。单包长度不超过当前 ATT 载荷，默认按 20 字节处理。重组超时 8 秒。重复的完整消息会被丢弃，冲突分片会报错。

信令类型：`HELLO=1`，`HELLO_ACK=2`，`SDP=3`，`CANDIDATE=4`，`GATHERING_DONE=5`，`BYE=6`，`ERROR=7`。

HELLO / HELLO_ACK 载荷：版本 u8、角色 u8、设备 ID 字符串、设备名称字符串。字符串是 u16 长度加 UTF-8。SDP 和 Candidate 是一条字符串。远端 SDP 还没设置时，Candidate 先排队。

## 文件数据报

只在 libjuice 的 UDP 通道上发送。整包不超过 1200 字节。

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | 4 字节 | `AFS1` |
| 4 | u8 | 版本 1 |
| 5 | u8 | 消息类型 |
| 6 | u8 | 标志，必须为 0 |
| 7 | u8 | 头长度，当前为 24 |
| 8 | 16 字节 | 传输 ID |
| 24 | 字节 | 消息体 |

类型：`PING=1`，`PONG=2`，`FILE_OFFER=3`，`FILE_ACCEPT=4`，`FILE_REJECT=5`，`FILE_START=6`，`FILE_CHUNK=7`，`FILE_ACK=8`，`FILE_END=9`，`FILE_FINISH=10`，`FILE_CANCEL=11`，`FILE_ERROR=12`。

- FILE_OFFER：文件大小 u64，文件名字符串。
- FILE_CHUNK：序号 u32、偏移 u64、长度 u16、数据。数据不超过 1000 字节。
- FILE_ACK：下一个需要的序号 u32。小于该序号的分块都已连续收到。
- FILE_END：文件大小 u64、分块数 u32、SHA-256 32 字节。
- FILE_FINISH：接收端算出的 SHA-256。

发送窗口为 16，400 毫秒超时，最多重传 8 次。乱序分块最多缓存 64 个，按顺序写入。发送和接收各自独立，但同一方向同时只有一个文件。文件名只保留最后一段，拒绝空名、`.`、`..` 和盘符。

长度和 SHA-256 都一致才算接收完成。失败时会删除未完成文件。

## 状态

`Idle`、`BleScanning`、`BleConnecting`、`BleConnected`、`IceGathering`、`IceConnecting`、`P2PConnected`、`IceFailed`、`Disconnecting`、`Disconnected`。

只有 libjuice 进入 `JUICE_STATE_CONNECTED` 或 `JUICE_STATE_COMPLETED` 才显示 P2P 已连接。不配置 STUN/TURN。
