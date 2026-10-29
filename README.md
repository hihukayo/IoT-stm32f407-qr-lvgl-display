# 基于 STM32F407VET6 的二维码生成与 LCD 显示系统（LVGL 版）

> 上位机串口发送文本 → STM32F407 本地生成 QR Code → 240×320 ST7789 屏显示 → 可反向上传 BMP 图片。
>
> 课设交付物：可编译固件（Keil MDK）、PC 上位机（Python/Tkinter）、二进制通信协议、测试记录与使用说明。

| 项目 | 内容 |
|:--|:--|
| 目标芯片 | STM32F407VET6（LQFP100，512 KiB Flash / 128 KiB SRAM + 64 KiB CCM） |
| 开发板 | 嵌入式开发网（mcudev）STM32F407 核心板，板载 8 针 SPI TFT 接口 J4 |
| 显示屏 | 2.4 英寸 240×320 ST7789，4 线 SPI，8 PIN |
| GUI | LVGL **v9.6.0**（固定版本），PARTIAL 局部渲染，RGB565 |
| 二维码库 | Nayuki **qrcodegen**（C 语言，MIT） |
| 串口 | USART1，PA9/PA10，默认 115200 8N1 |
| 工具链 | Keil MDK-ARM（原验证 5.24a + AC6.7；本机复验 5.43a + AC6.24） |
| 固件体积 | Flash **427.4 KiB** / 512 KiB；RAM 72.6 KiB / 128 KiB（**无全屏 framebuffer**）<br>AC6.7 构建为 330.4 KiB；AC6.24 对 LVGL 生成更大代码，仍余约 85 KiB |
| 构建状态 | ✅ `0 Error(s), 0 Warning(s)`（AC6.24：`Code=415664 RO=21836 RW=120 ZI=74264`） |
| 软件测试 | ✅ 固件逻辑 104 项断言；上位机 62 项单测（含 10 项图片解码） |
| 上板状态 | ✅ 已实测：屏幕显示、串口收发、BMP 导出、微信名片码复刻、图片导入 |

---

## 0. 效果示例

固件生成的 240×240 二维码（`Hello 123`，QR 版本 3，29×29 模块，每模块 6 像素，
四周 33 像素留白 + 4 模块静区），由 `tests/firmware/out/qr_hello123.bmp` 放大 2 倍：

<p align="center">
  <img src="docs/images/qr-example-hello123.png" alt="示例二维码：Hello 123">
</p>

该 BMP 文件大小为 **7 742 字节**、1bpp、无压缩，已用 OpenCV `QRCodeDetector`
真实解码为 `Hello 123`。

**上板实拍**（STM32F407VET6 核心板 + 2.4" 240×320 ST7789，串口发送 `Hello 123`，
ECC=M，固件回 `V1 M 21x21`，状态栏 `QR code generated`）：

<p align="center">
  <img src="docs/images/screenshot.jpg" width="360" alt="上板实拍：核心板与 ST7789 屏幕显示二维码">
</p>

---

## 1. 功能一览

### 1.1 必做功能

| 需求 | 实现方式 | 状态 |
|:--|:--|:--|
| FR-001 串口连接 | USART1 @115200 8N1，中断收 / 阻塞发 | ✅ 已实现 |
| FR-002 帧协议 | `A5 5A` 帧头 + VER + CMD + SEQ + LEN + PAYLOAD + CRC16/MODBUS | ✅ 已实现 |
| FR-003 输入内容 | 1–256 字节 UTF-8，长度由帧决定，禁止嵌入 NUL | ✅ 已实现 |
| FR-004 QR 编码 | qrcodegen，QR Model 2，版本 1–20，ECC L/M/Q/H | ✅ 已实现 |
| FR-005 LCD 输出 | ST7789 240×320，黑模块 / 白底 / 四模块静区 / 整数倍缩放 | ✅ 已实现 |
| FR-006 状态反馈 | 4 字节 ACK（status + error_code + version + modules） | ✅ 已实现 |
| FR-007 错误恢复 | 坏帧丢弃重同步；编码失败保留上一张二维码 | ✅ 已实现 |
| FR-008 工程可维护 | QR / LCD / 协议 / 应用 / 界面分模块 | ✅ 已实现 |
| FR-009 图形界面 | LVGL v9.6.0，PARTIAL，240×20 单缓冲 9 600 B | ✅ 已实现 |

### 1.2 发挥项

| 需求 | 实现方式 | 状态 |
|:--|:--|:--|
| FR-101 BMP 导出 | 240×240 / 1bpp / 无压缩 / top-down，流式分块发送 | ✅ 已实现 |
| FR-102 文件校验 | 每帧 CRC16 + 整文件 CRC32 + offset 连续性 + 长度校验 | ✅ 已实现 |
| FR-103 文件预览 | 上位机预览 + 另存为 | ✅ 已实现 |
| FR-104 多码制扩展 | 协议预留 `symbology` 枚举，Data Matrix 返回 `UNSUPPORTED` | ⚠️ **未实现**（如实标注） |
| FR-105 图片导入 | 上位机解码图片中的二维码（含 Micro QR / Data Matrix / 一维码）→ 文本 → `SET_QR` | ✅ 已实现 |

---

## 2. 目录结构

```
IoT-stm32f407-qr-lvgl-display/
├── README.md                          本文件
├── LICENSE                            MIT
├── .gitignore
├── docs/
│   ├── 01-hardware-wiring.md          接线表（逐脚来自原理图）
│   ├── 02-uart-protocol.md            串口协议完整规格
│   ├── 03-build-and-flash.md          编译、烧录、环境安装
│   ├── 04-test-record.md              测试记录（含"已软件验证/待上板"区分）
│   ├── 05-memory-budget.md            内存预算与 map 实测
│   ├── 06-known-limitations.md        已知限制
│   └── schematic/
│       └── STM32F407VET6-coreboard-schematic.pdf   （用户提供的原理图）
├── firmware/
│   ├── QR_LVGL_F407.ioc               CubeMX 引脚/时钟参考配置
│   ├── Core/
│   │   ├── Inc/
│   │   │   ├── main.h
│   │   │   ├── board_config.h         ★ 引脚/时钟/屏幕参数的唯一来源
│   │   │   ├── uart_protocol.h        帧结构、命令、状态码、CRC
│   │   │   ├── lcd_st7789.h           ST7789 驱动接口
│   │   │   ├── qr_service.h           二维码编码接口
│   │   │   ├── qr_bitmap.h            240×240 1bpp 位图
│   │   │   ├── bmp_stream.h           BMP 头 / 流式读取
│   │   │   ├── lvgl_ui.h              界面对象
│   │   │   ├── lv_port_disp.h         LVGL → ST7789 移植层
│   │   │   ├── app.h                  命令分派 / 图片流状态机
│   │   │   └── stm32f4xx_hal_conf.h   HAL 模块裁剪
│   │   └── Src/                       对应 .c 实现（11 个文件）
│   ├── Drivers/                       CMSIS + STM32F4 HAL（裁剪后 15 个 .c）
│   ├── Middlewares/
│   │   ├── lvgl/                      LVGL v9.6.0 源码 + lv_conf.h
│   │   └── qrcodegen/                 qrcodegen.c/.h + 许可证说明
│   └── MDK-ARM/
│       ├── QR_LVGL_F407.uvprojx       Keil 工程（512 个源文件，脚本生成）
│       ├── Objects/                   编译输出（.axf/.hex，已 gitignore）
│       └── Listings/                  map 文件（已 gitignore）
├── host/                              PC 上位机（Python 3 + Tkinter + pyserial）
│   ├── main.py                        主窗口
│   ├── protocol.py                    帧封装/解析、CRC16、命令定义
│   ├── crc.py                         CRC-32/ISO-HDLC
│   ├── serial_worker.py               串口线程 + 队列
│   ├── image_receiver.py              BMP 接收状态机
│   ├── qr_decode.py                   图片二维码解码（Pillow + zxing-cpp）
│   ├── tests/                         unittest 测试（62 项）
│   ├── requirements.txt
│   ├── run.bat
│   └── README.md
├── tests/
│   └── firmware/                      ★ 固件逻辑的主机端单元测试（gcc，无需硬件）
│       ├── stub/stm32f4xx_hal.h       HAL 桩
│       ├── test_main.c                104 项断言
│       ├── validate_bmp.py            BMP 结构校验 + OpenCV 真实解码
│       ├── run_tests.ps1              一键编译并运行
│       └── out/                       导出样例 BMP
└── tools/
    └── gen_keil_project.py            扫描源码树重新生成 .uvprojx
```

---

## 3. 硬件接线

> 全部引脚取自 `docs/schematic/STM32F407VET6-coreboard-schematic.pdf` 第 1 页的
> "TFT液晶屏（SPI）接口" J4 网络标号，**不是猜测值**。详见 [接线表](docs/01-hardware-wiring.md)。

### 3.1 LCD（板载 J4，8 针 SIP）

| J4 | 网络名 | STM32 引脚 | 外设/功能 |
|:--:|:--|:--|:--|
| 1 | 3V3 | — | 屏幕供电 3.3 V |
| 2 | GND | — | 地 |
| 3 | SDI | **PB15** | SPI2_MOSI（AF5） |
| 4 | SCL | **PB13** | SPI2_SCK（AF5） |
| 5 | CS | **PB12** | 片选（GPIO 输出） |
| 6 | SDO | **PB14** | SPI2_MISO（只写场景可不用） |
| 7 | RS | **PC5** | 数据/命令 DC（GPIO 输出） |
| 8 | BLK | **PB1** | 背光（高电平点亮） |

> ⚠️ **该核心板 J4 没有 RESET 引脚**（第 6 脚是 SDO）。固件默认用 ST7789 软件复位（0x01）。
> 若你的屏幕模块带 RST，把它接到空闲 GPIO（如 PB0），然后在 `board_config.h` 中：
> `#define BOARD_LCD_USE_HW_RESET 1` 并修改 `BOARD_LCD_RST_PORT/PIN`。

### 3.2 串口（外部 USB-TTL 模块）

核心板**没有板载 USB 转串口芯片**（Micro USB 直连 PA11/PA12，是 USB Device）。因此需要外接
3.3 V 电平的 USB-TTL 模块：

| USB-TTL | STM32 | 核心板排针 |
|:--|:--|:--|
| TXD | PA10（USART1_RX） | J2 第 21 脚 |
| RXD | PA9（USART1_TX） | J2 第 22 脚 |
| GND | GND | J2 第 5/6 脚 |
| 3V3（可选） | 3V3 | J2 第 1/2 脚 |

> ⚠️ 必须共地；**不要**把 RS-232 电平（±12 V）直接接到 MCU。

### 3.3 状态灯（可选）

D2 接 **PA1**，低电平点亮（原理图：3V3 → D2 → R14 510 Ω → PA1）。固件用它做心跳：
空闲 1 Hz 闪烁，导出 BMP 时常亮。

---

## 4. 快速开始

### 4.1 编译固件（Keil MDK）

1. 安装 **Keil MDK 5**（本工程用 MDK 5.24a + ARM Compiler 6.7 验证）。
2. 安装器件支持包 `Keil.STM32F4xx_DFP`（用于**烧录算法**；编译本身不依赖它）。
3. 打开 `firmware/MDK-ARM/QR_LVGL_F407.uvprojx`。
4. `Project → Build Target`（F7）。

命令行编译：

```bat
"D:\Keil5\UV4\UV4.exe" -b firmware\MDK-ARM\QR_LVGL_F407.uvprojx -j0 -o build.log
```

预期输出：

```
Program Size: Code=316444 RO-data=21792 RW-data=124 ZI-data=74260
".\Objects\QR_LVGL_F407.axf" - 0 Error(s), 0 Warning(s).
```

源码文件有增减时，重新生成工程（不用手改 XML）：

```bat
python tools\gen_keil_project.py
```

详见 [编译与烧录](docs/03-build-and-flash.md)。

### 4.2 烧录

| 方式 | 步骤 |
|:--|:--|
| **ST-Link / DAP-Link（推荐）** | J1 调试口（3V3 / GND / SWDIO=PA13 / SWCLK=PA14）→ Keil `Options → Debug` 选择 ST-Link → `Flash → Download`（F8） |
| **串口 ISP** | BOOT0 接 3V3 → 上电复位进入 ISP → 用 FlyMcu / STM32CubeProgrammer 通过 USART1 下载 → 下载完把 BOOT0 接回 GND 再复位 |

### 4.3 上位机

```bat
cd host
pip install -r requirements.txt
python main.py
```

或直接双击 `host\run.bat`。

<p align="center">
  <img src="docs/images/GUI.png" width="820" alt="上位机界面">
</p>

操作：选择串口 → 连接 → 输入文本（实时显示 UTF-8 字节数）→ 选 ECC → **生成并显示**
→ 需要存档时点 **导出 BMP**，校验通过后可预览、另存为。

**导入图片**（可选功能，需要 `Pillow` + `zxing-cpp`）：点 **导入图片**，选一张带二维码
的图片（截图、微信名片、文档插图都行），上位机会把图中的码解成文本填入输入框；
若已连接串口，会**自动接着发送** `SET_QR`，屏幕立即换成新的码。支持 QR / Micro QR /
Data Matrix / Aztec / PDF417 / 常见一维码的**解码**，但设备最终只会生成 QR（见
[host/README.md](host/README.md#43-从图片导入二维码图片--文本)）。

### 4.4 一键跑固件逻辑测试（无需硬件）

```powershell
powershell -ExecutionPolicy Bypass -File tests\firmware\run_tests.ps1
```

需要 `gcc`（如 MinGW-w64）。该脚本会用 gcc 编译 `uart_protocol.c` / `qr_service.c` /
`qr_bitmap.c` / `bmp_stream.c`（原样编译），跑 104 项断言，再用 Pillow/OpenCV 校验导出的 BMP。

---

## 5. 串口协议（摘要）

```
SOF0 | SOF1 | VER | CMD | SEQ | LEN_L | LEN_H | PAYLOAD... | CRC_L | CRC_H
0xA5 | 0x5A |0x01 |     |     |   uint16 LE    |            | uint16 LE
```

- CRC = **CRC-16/MODBUS**（初值 0xFFFF，反射多项式 0xA001），覆盖 `VER … PAYLOAD`，低字节先发；
- 整帧长度 = 9 + LEN，LEN ≤ 512；
- 命令：`0x01 PING` / `0x02 SET_QR` / `0x03 GET_STATUS` / `0x04 GET_IMAGE`；
  事件：`0x30 IMAGE_BEGIN` / `0x31 IMAGE_DATA` / `0x32 IMAGE_END`；
- 回应命令 = 请求命令 `| 0x80`，SEQ 原样返回。

测试向量：ASCII `"123456789"` → CRC16 = `0x4B37`（线上字节 `37 4B`）。

完整字段、边界条件与错误码见 **[协议规格](docs/02-uart-protocol.md)**。

---

## 6. 关键设计

### 6.1 没有全屏帧缓冲

240×320×2 = **153 600 字节**，占 SRAM 一半以上，方案被明确排除。实际做法：

| 缓冲 | 大小 | 用途 |
|:--|--:|:--|
| LVGL 局部绘图缓冲 | 9 600 B | `LV_DISPLAY_RENDER_MODE_PARTIAL`，240×20 RGB565 |
| 二维码 1bpp 位图 | 7 680 B | 同时用于 LCD 显示**和** BMP 像素区 |
| LVGL 内存池 | 40 960 B | 对象/样式/字体 |
| qrcodegen 工作区 | 3 × 1 177 B | 编码矩阵 + 暂存 + temp |
| 协议缓冲 | 2 × 521 B | 收帧装配 + 发送装配 |
| 主栈 / 堆 | 8 192 B / 512 B | — |

### 6.2 一份位图，两处复用

二维码渲染成 240×240、1bpp、32 字节/行的位图，位序与颜色语义同时满足两个消费者：

| 消费者 | 位序 | 含义 |
|:--|:--|:--|
| LVGL v9 `LV_COLOR_FORMAT_I1` | 最高位 = 最左像素 | 位=1 → 白，位=0 → 黑 |
| BMP 1bpp | 最高位 = 最左像素 | 位=1 → 调色板索引 1（白） |

所以 BMP 的像素区**就是** `g_qr_bitmap`，导出时只需拼上 62 字节文件头，无需重新光栅化，
也保证"屏幕看到什么，导出就是什么"。

### 6.3 二维码显示不做"每模块一个控件"

二维码用**单个** `lv_image` 控件 + 静态 `lv_image_dsc_t` 显示，而不是为每个黑色模块创建
`lv_obj`（版本 20 有 97×97 = 9 409 个模块，其中约一半是黑的）。缩放为整数倍：

```
scale = min(6, floor(240 / (modules + 8)))      // 8 = 两侧各 4 模块静区
origin = (240 - (modules + 8) * scale) / 2      // 居中
```

例：`Hello 123` → 版本 3 → 29×29 模块 → scale = 6 → 174×174 像素，四周各 33 px 留白 + 静区。

### 6.4 中断只搬字节

UART 中断里只做 `ring_push(byte)` 并重新挂起接收；帧解析、二维码编码、屏幕绘制、BMP 流
全部在主循环。图片流每轮只发一块（448 B），块间继续 `lv_timer_handler()`，因此导出期间
界面不假死。

---

## 7. 测试结果

### 7.1 已通过（软件验证，无需硬件）

| 编号 | 内容 | 结果 |
|:--|:--|:--|
| 构建 | Keil AC6 全量编译链接 | ✅ 0 error / 0 warning |
| 构建 | gcc `-Wall -Wextra` 编译 4 个逻辑模块 | ✅ 0 warning |
| T-001 | CRC16("123456789") = 0x4B37 | ✅ |
| — | CRC32/ISO-HDLC("123456789") = 0xCBF43926 | ✅ |
| T-002 | payload 含 `A5 5A` 仍正确解析 | ✅ |
| T-003 | 坏 CRC / LEN=513 / 错 VER / 截断帧超时 / 粘连帧 / 垃圾字节重同步 | ✅ |
| T-004 | BMP 头：7742 B、240×240、1bpp、offBits=62、dataSize=7680 | ✅ |
| — | BMP 像素区 == `g_qr_bitmap`；分块读与整块读一致 | ✅ |
| — | 四模块静区纯白、整数倍缩放、居中、定位图形首模块为黑 | ✅ |
| — | ECC L/M/Q/H 均编码成功且元数据自洽 | ✅ |
| — | 空文本 / 257 字节 / 含 NUL / ECC=4 / DataMatrix 错误码正确 | ✅ |
| — | **OpenCV 真实解码导出的 BMP**：`Hello 123`、URL、中文 UTF-8 逐字节一致 | ✅ |
| 上位机 | `host/tests` 协议、CRC、BMP、图片接收异常用例 | ✅ |
| 上位机 | 图片导入：示例图解出 `Hello 123`、缩小一半、双码计数、3 类异常提示 | ✅ 62 项全部通过 |

### 7.2 上板实测结果

以下均在实物（STM32F407VET6 核心板 + 2.4" 240×320 ST7789 + CH340 USB-TTL + ST-Link）上完成：

| 编号 | 内容 | 结果 |
|:--|:--|:--|
| T-101 | 上电 LCD 初始化、白底、标题栏、无花屏 | ✅ 校正 `MADCTL_BGR=0` / `INVERT_COLOR=0` 后：白底、深蓝标题栏、浅灰状态栏，无花屏 |
| T-103 | 屏幕二维码被扫码器识别 | ⚠️ 部分：设备导出的 BMP 反解逐字节一致、微信名片码上板复刻成功；**手机实扫未实测** |
| T-107 | 串口真实传输 7742 B + CRC32 校验 | ✅ `BEGIN/END` 一致，CRC32 `0xDB95AC43` 校验通过 |
| T-111 | LVGL 刷新无闪烁、对象/内存稳定 | ✅ 界面、状态栏刷新正常 |

修复过程中发现并解决的上板缺陷见 [已知限制 3.1](docs/06-known-limitations.md)
（LVGL I1 位图在 `RGB565_SWAPPED` 目标下渲染为全黑）。

### 7.3 仍需实机确认（不得视为通过）

| 编号 | 内容 | 说明 |
|:--|:--|:--|
| T-109 | 中文二维码在**两种不同扫码软件**中的解码结果 | 待测；qrcodegen 不写 ECI，手机 App 是否按 UTF-8 解释不保证 |
| T-110 | 连续生成并导出 50 次 | 待测（长时间稳定性） |
| T-113 | 长时间运行的内存/句柄稳定性 | 待测 |

完整记录见 [测试记录](docs/04-test-record.md)。

---

## 8. 上板需要确认的 4 个开关

`firmware/Core/Inc/board_config.h` 中。**下表的默认值已按实屏校正并验证**
（2.4" 240×320 ST7789：RGB 顺序、不需要 INVON）：

| 宏 | 默认 | 现象不对时怎么改 |
|:--|:--|:--|
| `BOARD_LCD_MADCTL_BGR` | `0` | 红色显示成蓝色、深蓝标题栏发棕 → 改成 `1` |
| `BOARD_LCD_INVERT_COLOR` | `0` | 白底变深蓝、整体像负片 → 改成 `1` |
| `BOARD_LCD_X_OFFSET` / `BOARD_LCD_Y_OFFSET` | `0` / `0` | 图像整体偏移/边缘花屏 → 试 0/80 等常见值 |
| `BOARD_LCD_SPI_PRESCALER` | `/_4`（10.5 MHz） | 花屏/闪烁 → 降到 `/_8`；稳定后可提到 `/_2` |

> 建议先用 `ST7789_FillScreen()` 纯色测试 + `ST7789_DrawPixel()` 四角测试确认坐标，
> 再接二维码（风险 A 的对策）。

---

## 9. 已知限制

1. **Data Matrix 未实现**：`symbology = 0x02` 返回 `UNSUPPORTED`，上位机该选项置灰。
2. **无 ECI 26 标记**：qrcodegen 的 byte 模式不写 ECI(UTF-8)。中文能否被正确识别取决于
   扫码软件是否默认按 UTF-8 解释。OpenCV 可正确解码，但**不保证所有手机 App**。详见
   [已知限制](docs/06-known-limitations.md)。
3. **无中文界面字库**：LCD 上的状态文本为英文（引入 CJK 字库会多占上百 KiB Flash）；
   上位机界面为中文。
4. **无触摸/按键交互**：屏幕仅显示；唯一的板载按键（PA0/WK_UP）本版本未使用。
5. **SPI 阻塞发送**：未启用 DMA，`flush_cb` 阻塞到发送完成才 `lv_display_flush_ready()`；
   10.5 MHz 下一次 240×20 刷新约 7 ms，静态界面足够。
6. **USB 未使用**：板载 Micro USB 是 USB Device（PA11/PA12），本版本未实现 USB CDC，
   串口走外部 USB-TTL。
7. **图片流期间不响应 SET_QR**：返回 `BUSY`（协议允许的推荐行为）。
8. **二维码显示经过 flush 覆盖层**：LVGL v9.6 的 I1 源在 `RGB565_SWAPPED` 目标格式下
   会把位图渲染成全黑（上板实测），因此在 `disp_flush_cb()` 中按 `g_qr_bitmap` 重算该
   区域像素。原因、取舍与验证见 [已知限制 3.1](docs/06-known-limitations.md)。

---

## 10. 依赖与许可证

| 组件 | 版本/来源 | 许可证 |
|:--|:--|:--|
| LVGL | **v9.6.0**（tag `v9.6.0`，commit `80ca777`） | MIT（`firmware/Middlewares/lvgl/LICENCE.txt`） |
| qrcodegen | Nayuki QR-Code-generator，commit `3c6d0b3` | MIT（`firmware/Middlewares/qrcodegen/`） |
| STM32F4 HAL | `STMicroelectronics/stm32f4xx_hal_driver` commit `1f6451c` | BSD-3-Clause（`LICENSE.md`） |
| CMSIS Device F4 | `STMicroelectronics/cmsis_device_f4` commit `9192c7b` | Apache-2.0 |
| pyserial | ≥ 3.5 | BSD-3-Clause |
| Pillow | ≥ 10.0（仅上位机「导入图片」） | HPND |
| zxing-cpp | ≥ 2.3（仅上位机「导入图片」） | Apache-2.0 |

> LVGL 版本已固定为 v9.6.0 并随仓库提交源码，**不依赖网络**，也不跟随 master。

---

## 11. 参考资料

1. STM32F407xx 数据手册 — <https://www.st.com/resource/en/datasheet/stm32f407ve.pdf>
2. Nayuki QR Code generator — <https://www.nayuki.io/page/qr-code-generator-library>
3. LVGL v9.6.0 STM32 SPI LCD 移植指南 — <https://github.com/lvgl/lvgl/blob/v9.6.0/docs/src/integration/chip_vendors/stm32/lcd_stm32_guide.mdx>
4. LVGL 图像颜色格式（含 I1） — <https://lvgl.io/docs/open/main-modules/images/color_formats>
5. ST7789 数据手册（初始化序列与 MADCTL/CASET/RASET 定义）

---

## 12. 给后续维护者的提示

- **改引脚**：只改 `board_config.h`，业务代码里不应出现任何 `GPIO_PIN_x`。
- **改屏幕型号**：替换 `lcd_st7789.c` 的初始化序列，保持 4 个对外接口不变。
- **加码制**：在 `qr_service.c` 里按 `qr_symbology_t` 分派，BMP/协议/界面无需改动。
- **改协议**：`uart_protocol.h` + `host/protocol.py` 必须同步，且更新
  `docs/02-uart-protocol.md` 的版本号。
- **改界面布局**：二维码矩形的位置由 `lvgl_ui.c` 的 `UI_QR_Y` 与 `UI_QrOverlay()`
  共同决定；**移动图像控件时必须同步改这两处**，否则屏幕上的码会被画到错误位置。
- **改 LVGL 版本**：升级后先验证 I1 位图能否正常渲染；若已修复，可删掉 flush 覆盖层
  （`lv_port_disp.c` 里的 `UI_QrOverlay()` 调用）以及 `lvgl_ui.c` 中的实现。
