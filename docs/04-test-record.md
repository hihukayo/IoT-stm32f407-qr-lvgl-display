# 测试记录

> **重要声明**
> 本记录分两个阶段：
> - 第 1–4 节：**软件验证**（编译器 + 主机端单元测试 + 真实图片解码），**不需要硬件**；
> - 第 5 节：**上板验证**，2026-10-29 在实物上完成一轮，逐条标注通过 / 部分通过 / 未测试。
>
> 标 ⚠️ 的条目只完成了其中一部分，说明里写清了通过了哪部分；
> 标 ⬜ 的条目仍**未测试**，**不得**视为已通过。
>
> 记录格式按需求 8.3 节：编号 / 环境 / 输入 / 步骤 / 预期 / 实际 / 结论 / 证据。

## 0. 被测版本

| 项 | 值 |
|:--|:--|
| 固件版本 | `QR_LVGL_F407` v1.0（协议 VER = 0x01，文档 v1.0） |
| LVGL | v9.6.0（tag `v9.6.0`，commit `80ca777`） |
| qrcodegen | commit `3c6d0b3` |
| STM32F4 HAL | commit `1f6451c` |
| 编译器（固件） | Keil MDK 5.24a + ARM Compiler 6.7（AC6），C11，-O2 |
| 编译器（主机测试） | MinGW-w64 gcc 8.1.0，-Wall -Wextra -O2 |
| Python | 3.12.14（主机测试） |
| 硬件 | **未连接**（无板卡、无屏幕、无 USB-TTL） |
| 记录日期 | 2026-10-28 |

---

## 1. 构建验证

### B-001 Keil 全量编译

| 项 | 内容 |
|:--|:--|
| 输入 | `firmware/MDK-ARM/QR_LVGL_F407.uvprojx`（512 个源文件） |
| 命令 | `UV4.exe -b QR_LVGL_F407.uvprojx -j0 -o build.log` |
| 预期 | 0 error / 0 warning，产出 .axf 与 .hex |
| 实际 | `0 Error(s), 0 Warning(s)`；`Code=316444 RO-data=21792 RW-data=124 ZI-data=74260`；生成 `QR_LVGL_F407.hex`（951 796 字节） |
| 结论 | ✅ **通过** |
| 证据 | `firmware/MDK-ARM/build.log`、`Listings/QR_LVGL_F407.map` |

### B-002 主机端 gcc 编译（逻辑模块）

| 项 | 内容 |
|:--|:--|
| 命令 | `gcc -std=c11 -Wall -Wextra -O2 …`（见 `tests/firmware/run_tests.ps1`） |
| 预期 | 无 error、无 warning |
| 实际 | 编译成功，0 warning |
| 结论 | ✅ **通过** |

---

## 2. 固件逻辑单元测试（主机端，104 项断言）

运行：`powershell -ExecutionPolicy Bypass -File tests\firmware\run_tests.ps1`
产物：`tests/firmware/out/test_fw.exe`

| 编号 | 测试内容 | 输入 | 预期 | 实际 | 结论 |
|:--|:--|:--|:--|:--|:--|
| F-001 | CRC16/MODBUS 标准向量（T-001） | `"123456789"` | `0x4B37` | `0x4B37` | ✅ |
| F-002 | CRC32/ISO-HDLC 标准向量 | `"123456789"` | `0xCBF43926` | `0xCBF43926` | ✅ |
| F-003 | CRC32 分段与整体一致 | 4+5 字节分段 | 同上 | 一致 | ✅ |
| F-004 | 帧发送字段布局 | cmd=0x01 seq=0x07 pl=4B | SOF/VER/CMD/SEQ/LEN/PAYLOAD/CRC 位置正确 | 全部正确 | ✅ |
| F-005 | CRC 覆盖范围与字节序 | 同上 | 覆盖 VER..PAYLOAD，低字节先 | 正确（`s_uart_tx[11]`=低） | ✅ |
| F-006 | 帧解析往返 | SET_QR + 6B payload | 1 帧，字段一致 | 一致 | ✅ |
| F-007 | payload 含 `A5 5A`（T-002） | `A5 5A 00 A5 5A FF` | 原样保留 | 原样保留 | ✅ |
| F-008 | 两帧粘连 | 2 帧连写 | 解析出 2 帧 | 2 帧 | ✅ |
| F-009 | 坏 CRC 丢弃并计数 | 末字节取反 | 0 帧，`crc_errors=1` | 0 帧，计数 1 | ✅ |
| F-010 | LEN=513 拒绝 | 伪造头部 | 丢弃 | 丢弃 | ✅ |
| F-011 | 未知 VER 拒绝 | VER=0x02 | 丢弃 | 丢弃 | ✅ |
| F-012 | 截断帧 + 1 s 超时重同步 | 少 3 字节 + `poll(+1001ms)` | 不产生帧；之后正常 | 符合 | ✅ |
| F-013 | 垃圾字节后重同步 | `00 FF A5` + 完整帧 | 只解析出 1 帧且 SEQ 正确 | 符合 | ✅ |
| F-014 | LEN=0 合法 | GET_STATUS | 1 帧，len=0 | 符合 | ✅ |
| F-015 | ACK 布局 | SET_QR→ACK | `CMD\|0x80`、4 字节、字段顺序 | 符合 | ✅ |
| F-016 | `Hello 123` 编码 | ECC=M | OK，version 与 modules 自洽 | V3 / 29×29 | ✅ |
| F-017 | scale/origin 公式 | — | `scale=min(6,240/(mod+8))`，居中 | 6 / 33 | ✅ |
| F-018 | ECC L/M/Q/H 全通 | `Hello 123` | 4 次均 OK，ecc 记录正确 | 符合 | ✅ |
| F-019 | 错误分支 | 空指针/空文本/ECC=4/DataMatrix/非法码制/含 NUL/257B | 对应错误码 | PARAM/PARAM/PARAM/UNSUPPORTED/PARAM/PARAM/TOO_LONG | ✅ |
| F-020 | 失败保留旧二维码（FR-007） | 失败调用后查 `IsValid()` | 仍为 true | true | ✅ |
| F-021 | 位图尺寸 | — | 7680 B | 7680 | ✅ |
| F-022 | 矩阵外全白（静区 + 留白） | 逐像素 240×240 | 全为 1 | 全为 1 | ✅ |
| F-023 | 定位图形首模块为黑 | (origin+4s, origin+4s) | 0（黑） | 0 | ✅ |
| F-024 | 四模块静区纯白（NFR-004） | 四周 4·scale 环带 | 全白 | 全白 | ✅ |
| F-025 | 行补齐字节 | 每行 30/31 字节 | 0xFF（BMP 忽略） | 0xFF | ✅ |
| F-026 | `QR_BitmapClear` 全白 | — | 全 1 | 全 1 | ✅ |
| F-027 | BMP 总长度（T-004） | 完整读取 | 7742 | 7742 | ✅ |
| F-028 | BMP 头字段 | — | BM / 7742 / 62 / 40 / 240 / -240 / 1 / 1 / 0 / 7680 / 2 / 调色板 | 全部正确 | ✅ |
| F-029 | 像素区 == 位图 | `memcmp` | 相同 | 相同 | ✅ |
| F-030 | BMP CRC32 == 整块 CRC32 | — | 相同 | 相同 | ✅ |
| F-031 | 448 B 分块读与整块一致 | 18 块 | 全部一致，累计 7742 | 一致 | ✅ |
| F-032 | 越界读 | offset=7742 / 7740+10 | 0 / 2 字节 | 符合 | ✅ |
| F-033 | 导出样例 BMP | 3 组文本 | 生成 3 个 7742 B 文件 | 已生成 | ✅ |

**小计：通过 104 项，失败 0 项。**

---

## 3. 导出 BMP 的外部校验（真实解码）

运行：`python tests/firmware/validate_bmp.py tests/firmware/out`

### T-004 BMP 结构（外部工具）

| 项 | 期望 | 实际（Pillow + 手工解析） | 结论 |
|:--|:--|:--|:--|
| 文件大小 | 7742 | 7742 | ✅ |
| 尺寸 / 模式 | 240×240 / 1bpp | `(240, 240)` / `'1'` | ✅ |
| bfOffBits | 62 | 62 | ✅ |
| biHeight | -240（top-down） | -240 | ✅ |
| 调色板 | 0=黑 / 1=白 | 一致 | ✅ |

### T-005 二维码真实解码（OpenCV `QRCodeDetector`）

这是本项目最有力的验证：**固件 C 代码生成的 1bpp 位图 + BMP 封装，被独立解码器成功识别**。

| 编号 | 输入文本 | 解码结果 | 结论 |
|:--|:--|:--|:--|
| D-001 | `Hello 123` | `Hello 123` | ✅ 完全一致 |
| D-002 | `https://example.com/qr-test` | `https://example.com/qr-test` | ✅ 完全一致 |
| D-003 | `二维码测试-中文 UTF-8` | 解码字节 `e4 ba 8c e7 bb b4 …` 与期望 UTF-8 逐字节相同 | ✅ 完全一致 |

> ⚠️ D-003 只能说明"OpenCV 能按 UTF-8 正确解码"，**不能代表手机扫码 App**。
> 手机端两种扫码软件的结果仍需上板实测（见第 5 节 T-109）。

---

## 4. 上位机与跨实现一致性

### 4.1 上位机单元测试（64 项）

运行：`cd host && python -m unittest discover -s tests -v`

```
Ran 64 tests in 0.291s
OK
```

| 文件 | 覆盖 |
|:--|:--|
| `test_protocol.py` | CRC 向量、帧封装/解析、A5 5A payload、坏 CRC、LEN>512、错 VER、截断+超时、粘连帧、payload 编解码 |
| `test_image_receiver.py` | T-004 BMP 头、T-005 offset 缺口/重复/总长错/CRC32 错 → 不生成 .bmp |
| `test_serial_worker.py` | 假串口 ACK 匹配、超时、重试、缺 pyserial 时的降级 |
| `test_main_module.py` | 模块导入、默认文件名 |

结论：✅ **通过**

### 4.2 MCU(C) ↔ PC(Python) 协议一致性

运行：`python tests/firmware/check_interop.py` → **22 项，0 失败**

方法：由固件真实函数 `proto_send_frame()` / `proto_send_ack()` 生成帧并落盘
（`out/f_*.bin`），再用**上位机实际使用的** `host/protocol.py` 重新构造同样帧，逐字节比对；
反向再用上位机解析固件产出的帧，校验字段。

| 编号 | 帧 | 结果 |
|:--|:--|:--|
| I-001 | PING（SEQ=7） | 字节完全相同 |
| I-002 | SET_QR（SEQ=0x42, `Hello 123`, M） | 字节完全相同；解析出文本一致 |
| I-003 | ACK（status/err/ver=3/mod=29） | 字节完全相同；字段一致 |
| I-004 | GET_STATUS ACK（5 字节 payload） | 字节完全相同；字段一致 |
| I-005 | CRC16 向量与线上字节序 | `0x4B37`，线上 `37 4B` |

---

## 5. 上板验证（实测结果）

> 2026-10-29 在实物上完成一轮验证：STM32F407VET6 核心板 + 2.4" 240×320 ST7789
> （RGB 顺序、不用 INVON）+ CH340 USB-TTL + ST-Link。
> ⬜ = 仍未测试，**不得**视为通过；⚠️ = 部分通过，说明里写清通过了哪部分。

| 编号 | 内容 | 判定标准 | 状态 |
|:--|:--|:--|:--|
| T-101 | 上电 LCD 初始化，白底/标题/等待状态，无花屏 | 目视 | ✅ 通过（`MADCTL_BGR=0`、`INVERT_COLOR=0`） |
| T-102 | 发送 PING，2 s 内收到同 SEQ 的 ACK | 上位机日志 | ✅ 通过（`status=0`，CRC 正确） |
| T-103 | `Hello 123` → ACK 成功、LCD 显示、**独立扫码器**识别为 `Hello 123` | 手机/扫码枪 | ⚠️ 部分：ACK 成功、LCD 显示正常、设备导出的 BMP 反解为 `Hello 123`；**手机实扫未做** |
| T-104 | URL、标点、长度边界文本；ECC L/M/Q/H 分别实测 | 逐个扫码 | ⚠️ 部分：URL（微信名片 48 B）与 ECC=M 实测通过；L/Q/H 未逐个扫 |
| T-105 | 空文本/错 ECC/错码制/超长输入 → 正确错误码且不死机 | 上位机日志 + 屏幕 | ⬜ 未测试（逻辑层已测） |
| T-106 | 注入 CRC 错帧 → 丢弃，后续命令仍可用 | 上位机日志 | ⬜ 未测试（解析器已测） |
| T-107 | 导出 BMP → 收到 7742 B、CRC32 一致、图片可打开 | 文件大小/校验/预览 | ✅ 通过（7742/7742 B，CRC32 `0xDB95AC43`，BMP 头校验通过并被解码） |
| T-108 | 传图过程中发 SET_QR → 返回 BUSY | 上位机日志 | ⬜ 未测试 |
| T-109 | 中文二维码在**两种不同扫码软件**的解码结果 | 截图/文字记录 | ⬜ 未测试（OpenCV 已通过） |
| T-110 | 连续生成并导出 50 次，内存/解析稳定 | 无异常 | ⬜ 未测试 |
| T-111 | LVGL 显示标题/状态/二维码，无花屏/闪烁 | 目视 | ✅ 通过（界面、状态栏、二维码刷新均正常） |
| T-112 | map 中无 240×320×2 framebuffer；`flush_ready` 在实际传输后调用 | 静态检查 | ✅ **静态检查已通过**（见内存报告） |
| T-113 | 连续刷新状态 + 不同二维码，对象/内存稳定 | 长时间观察 | ⬜ 未测试 |
| T-114 | 若用 SPI DMA，缓冲在可访问 SRAM、flush_ready 时机正确 | — | ➖ **本版本未使用 DMA** |
| T-115 | 掉电/复位后恢复上一次的二维码（W25Q16） | 复位后自动显示 | ✅ 通过（复位后自动恢复，未重发 `SET_QR` 时 `last_qr_valid=1`） |
| T-116 | W25Q16 双槽轮换（连续写入不同内容） | 复位后显示最新一条 | ✅ 通过（写入 v3/29×29 后复位显示新码） |
| AC-001 | 上位机连接串口，Ping 成功 | 界面提示 | ✅ 通过 |
| AC-007 | 导出 BMP 后 PC 能打开，尺寸 240×240、1bpp、7742 B、CRC32 正确 | — | ✅ 通过（串口实传 + 文件侧双重校验） |

### 5.0 上板过程中发现并修复的缺陷

| 现象 | 根因 | 修复 |
|:--|:--|:--|
| 二维码区域整块纯黑（导出 BMP 却正常） | LVGL v9.6 的 I1 源在 `RGB565_SWAPPED` 目标下被当作全 0 读取 | `disp_flush_cb()` 中按位图重算像素（`UI_QrOverlay`），见 [已知限制 3.1](06-known-limitations.md) |
| 整屏负片（白底变深蓝）、标题栏发棕 | `BOARD_LCD_INVERT_COLOR` 与 `BOARD_LCD_MADCTL_BGR` 沿用了"多数 IPS 模块"的默认值 1 | 实测校正为 `0 / 0` |

### 5.1 上板测试建议顺序

1. 纯色/四角像素测试（确认坐标与颜色顺序）
2. PING（确认串口链路）
3. `Hello 123` + 手机扫码
4. ECC 四档 + 边界文本
5. 坏帧注入
6. BMP 导出（先用小文本，看 7742 B 与 CRC32）
7. 中文 + 两种扫码软件记录
8. 50 次连续生成/导出
9. 长时间（≥30 min）运行观察

---

## 6. 复现方式汇总

```powershell
# 1. 固件编译（需要 Keil MDK）
"D:\Keil5\UV4\UV4.exe" -b firmware\MDK-ARM\QR_LVGL_F407.uvprojx -j0 -o build.log

# 2. 固件逻辑单元测试 + BMP 校验 + 协议一致性（需要 gcc、Python）
powershell -ExecutionPolicy Bypass -File tests\firmware\run_tests.ps1

# 3. 上位机单元测试（只需 Python 标准库）
cd host
python -m unittest discover -s tests -v
```
