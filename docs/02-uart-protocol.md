# 串口二进制协议规格（v1.0）

> 本文是 MCU 与 PC 之间的**唯一权威协议说明**。
> 固件实现：`firmware/Core/Src/uart_protocol.c` + `app.c`
> 上位机实现：`host/protocol.py`
> 两端一致性由 `tests/firmware/check_interop.py` 逐字节比对（22 项，全部通过）。

## 1. 物理层

| 参数 | 值 |
|:--|:--|
| 外设 | USART1（PA9 = TX，PA10 = RX） |
| 波特率 | **115200**（上位机可改，固件需同步修改 `BOARD_UART_BAUDRATE`） |
| 数据位 / 校验 / 停止位 | 8 / None / 1（8N1） |
| 流控 | None |
| 帧内字节间隔超时 | **1000 ms**（接收中途超过 1 s 无新字节 → 丢弃半帧并重新找帧头） |

## 2. 通用帧格式

```
偏移   0     1     2    3    4     5       6     7 .. 7+LEN-1   7+LEN  8+LEN
     A5    5A   VER  CMD  SEQ  LEN_L   LEN_H   PAYLOAD ...      CRC_L  CRC_H
```

| 字段 | 长度 | 说明 |
|:--|:--:|:--|
| SOF0 / SOF1 | 1 + 1 | 固定 `0xA5 0x5A` |
| VER | 1 | 协议版本，**固定 `0x01`** |
| CMD | 1 | 命令字（见第 3 节） |
| SEQ | 1 | 请求序号，PC 每个请求递增（0–255 回绕）；**回应沿用该序号** |
| LEN | 2 | payload 长度，**uint16 小端**，范围 0–512 |
| PAYLOAD | LEN | 任意字节，**可以包含 0xA5 0x5A** |
| CRC | 2 | CRC-16/MODBUS，**uint16 小端（低字节先）** |

**整帧长度 = 9 + LEN。**

### 2.1 CRC-16/MODBUS

| 项 | 值 |
|:--|:--|
| 算法 | CRC-16/MODBUS（反射） |
| 初值 | `0xFFFF` |
| 多项式 | `0xA001`（即反射形式） |
| 输入/输出反射 | 是 / 是 |
| 覆盖范围 | **从 VER 到 PAYLOAD 末尾**（不含 SOF0/SOF1，不含 CRC 自身） |
| 线上顺序 | 低字节先 |

**标准测试向量**：ASCII `"123456789"` → `0x4B37` → 线上字节 `37 4B`。

### 2.2 解析要求（实现已满足）

1. 不假设一次 UART 中断能收到完整帧；按字节推进状态机。
2. PAYLOAD 内出现 `0xA5 0x5A` 时不得误判帧头（只按 LEN 计数）。
3. 两帧粘连在同一缓冲区时都要能解析。
4. 校验失败（VER 非 0x01 / LEN > 512 / CRC 不符）→ **丢弃并重新找帧头**，不影响后续帧。
5. 接收中途超时 1 s → 清空半帧。
6. 发送必须串行，不允许两个上下文交错写串口。

## 3. 命令与事件 ID

| 值 | 名称 | 方向 | 说明 |
|:--:|:--|:--|:--|
| `0x01` | `CMD_PING` | PC → MCU | 连通性测试，无 payload |
| `0x02` | `CMD_SET_QR` | PC → MCU | 设置文本、编码、显示 |
| `0x03` | `CMD_GET_STATUS` | PC → MCU | 读取设备状态 |
| `0x04` | `CMD_GET_IMAGE` | PC → MCU | 请求导出当前二维码 BMP |
| `0x30` | `EVT_IMAGE_BEGIN` | MCU → PC | 图像传输开始 |
| `0x31` | `EVT_IMAGE_DATA` | MCU → PC | 图像数据块 |
| `0x32` | `EVT_IMAGE_END` | MCU → PC | 图像传输结束（含 CRC32） |
| `0x81`–`0x84` | ACK | MCU → PC | 请求命令 `\| 0x80` |

> 未知命令：返回 `cmd|0x80`，`status=1`，`error_code=UNSUPPORTED(7)`。

## 4. `CMD_SET_QR`（0x02）

### 4.1 请求 payload

```
[0] symbology (u8)   0x01 = QR Code；0x02 = Data Matrix（本版本返回 UNSUPPORTED）
[1] ecc       (u8)   0 = L，1 = M，2 = Q，3 = H（默认 M）
[2..] text    (u8[]) UTF-8 原始字节，长度 = LEN - 2
```

合法条件（任一不满足即报错，**绝不静默截断**）：

| 条件 | 失败返回 |
|:--|:--|
| `LEN >= 3`（至少 1 字节文本） | `PARAM_ERR(1)` |
| `text` 长度 1–256 | 超过 → `TOO_LONG(2)` |
| `text` 不含 `0x00` | 含 NUL → `PARAM_ERR(1)` |
| `symbology == 0x01` | `0x02` → `UNSUPPORTED(7)`；其他 → `PARAM_ERR(1)` |
| `ecc <= 3` | 越界 → `PARAM_ERR(1)` |
| 内容能在版本 1–20 内编码 | 失败 → `ENCODE_FAIL(3)` |
| 设备不在传图状态 | 正在传图 → `BUSY(5)` |

### 4.2 回应

普通 ACK（4 字节，规则同下）。

## 5. ACK payload

所有非 `GET_STATUS` 的回应都是 4 字节：

```
[0] status       (u8)   0 = OK；非 0 = 失败
[1] error_code   (u8)   见下表
[2] qr_version   (u8)   QR 版本 1–20；不适用或失败时为 0
[3] module_count (u8)   矩阵每边模块数（= 17 + 4*version）；不适用或失败时为 0
```

| error_code | 名称 | 含义 |
|:--:|:--|:--|
| 0 | `NONE` | 无错误 |
| 1 | `PARAM_ERR` | 参数非法（空文本 / 含 NUL / ECC 越界 / 码制非法 / LEN < 3） |
| 2 | `TOO_LONG` | 文本超过 256 字节 |
| 3 | `ENCODE_FAIL` | 编码失败（容量不足等） |
| 4 | `NOT_READY` | 尚无可用二维码，不能导出 |
| 5 | `BUSY` | 设备忙（正在发送图片流） |
| 6 | `CRC_ERR` | 校验错误（保留，当前实现直接丢弃坏帧） |
| 7 | `UNSUPPORTED` | 不支持的码制或未知命令 |
| 8 | `INTERNAL_ERR` | 内部错误 |

> `status` 只有 0 / 1 两种取值，**具体原因看 `error_code`**。

## 6. `CMD_GET_STATUS`（0x03）

请求 payload 为空。

**回应 payload 是 5 字节**（注意：与普通 ACK 的 4 字节不同）：

```
[0] status         (u8)  0 = OK
[1] last_qr_valid  (u8)  1 = 存在上一张成功生成的二维码
[2] symbology      (u8)  固定 0x01（本版本只有 QR）
[3] qr_version     (u8)  0 = 从未成功生成
[4] module_count   (u8)  0 = 从未成功生成
```

上位机据此决定「导出 BMP」按钮是否可用。

## 7. BMP 图片流

### 7.1 流程

```
PC                          MCU
 |--- CMD_GET_IMAGE (0x04) --->|
 |<-- ACK 0x84 (status/err) ---|    失败（NOT_READY/BUSY）→ 流程结束
 |<-- EVT_IMAGE_BEGIN (0x30) --|
 |<-- EVT_IMAGE_DATA  (0x31) --|    offset=0
 |<-- EVT_IMAGE_DATA  (0x31) --|    offset=448
 |            ...              |
 |<-- EVT_IMAGE_DATA  (0x31) --|    最后一块
 |<-- EVT_IMAGE_END   (0x32) --|    file_size + crc32
```

所有事件沿用 `CMD_GET_IMAGE` 请求的 SEQ。

### 7.2 `EVT_IMAGE_BEGIN` payload（15 字节）

| 偏移 | 类型 | 字段 | 固定值 |
|:--:|:--|:--|:--|
| 0 | u8 | format | **1** = BMP 1bpp 无压缩 top-down |
| 1 | u16 LE | width | **240** |
| 3 | u16 LE | height | **240** |
| 5 | u32 LE | file_size | **7742** |
| 9 | u32 LE | data_size | **7680** |
| 13 | u16 LE | row_stride | **32** |

### 7.3 `EVT_IMAGE_DATA` payload

```
[0..3] offset (u32 LE)   该块在**整个 BMP 文件**中的绝对偏移（文件头从 0 开始算）
[4..]  chunk             最多 448 字节（payload 总长 452 ≤ 512）
```

- 块必须按 offset **严格递增、连续无缺口**；
- PC 必须拒绝：offset 不匹配、重复块、缺口、越界数据、总长度不符。

### 7.4 `EVT_IMAGE_END` payload（8 字节）

```
[0..3] file_size (u32 LE)  7742
[4..7] crc32     (u32 LE)  整份 BMP 文件的 CRC-32/ISO-HDLC
```

CRC-32/ISO-HDLC：反射多项式 `0xEDB88320`，初值 `0xFFFFFFFF`，结果再异或 `0xFFFFFFFF`，
覆盖文件偏移 0 到末尾的全部字节。测试向量：`"123456789"` → `0xCBF43926`。

### 7.5 PC 端文件处理

1. 先写 `xxx.bmp.part` 临时文件；
2. 校验：每帧 CRC16、offset 连续性、总长度 7742、整文件 CRC32；
3. **全部通过**才 `os.replace()` 成 `.bmp`；
4. 任一失败：保留 `.part`（或按配置删除），**绝不**把损坏文件标记为成功。

## 8. 时序与超时建议

| 项 | 建议值 | 说明 |
|:--|:--|:--|
| 普通请求等 ACK | 2 s | `PROTO_ACK_TIMEOUT_MS` |
| 整条图片流 | 10 s | 7742 B + 帧开销 ≈ 8 KB，115200 下约 0.8 s |
| 帧内字节超时 | 1 s | `PROTO_FRAME_TIMEOUT_MS` |
| `SET_QR` 重试 | 最多 2 次（可选） | 必须处理"重复 SEQ 导致重复副作用"；本工程默认不自动重试 |

## 9. 完整示例

### 9.1 `CMD_PING`，SEQ = 0x07

```
A5 5A 01 01 07 00 00 <CRC_L> <CRC_H>
```

由固件 `proto_send_frame(0x01, 0x07, NULL, 0)` 生成的实测值：

```
A5 5A 01 01 07 00 00 A9 FD
```

### 9.2 `CMD_SET_QR`，SEQ = 0x42，文本 `Hello 123`，ECC = M

payload = `01 01 48 65 6C 6C 6F 20 31 32 33`（11 字节）

```
A5 5A 01 02 42 0B 00 01 01 48 65 6C 6C 6F 20 31 32 33 1E 9C
```

### 9.3 回应 ACK，SEQ = 0x42，成功，版本 3，29 模块

payload = `00 00 03 1D`

```
A5 5A 01 82 42 04 00 00 00 03 1D 2D A0
```

### 9.4 `GET_STATUS` 回应，SEQ = 0x10

payload = `00 01 01 03 1D`（OK / 有旧图 / QR / V3 / 29 模块）

```
A5 5A 01 83 10 05 00 00 01 01 03 1D 90 3D
```

> 以上帧的**逐字节内容**已由 `tests/firmware/check_interop.py` 与 Python 实现比对通过；
> 上面四组帧的 CRC 均已实测，C 固件与 Python 上位机两端逐字节一致。

## 10. 变更策略

- 新增命令：只追加 CMD，不改动既有字段布局。
- 修改字段：必须提升 `VER`，并在本文与 `host/protocol.py` 的 `PROTOCOL_VERSION` 同步。
- 本文档版本：**v1.0**，对应 `VER = 0x01`。
