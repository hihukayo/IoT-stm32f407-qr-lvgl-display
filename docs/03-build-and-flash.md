# 编译、烧录与环境安装

## 1. 工具链要求

| 组件 | 本项目验证环境 | 说明 |
|:--|:--|:--|
| Keil MDK-ARM | **5.24a**（µVision 5） | 更高版本同样可用 |
| ARM Compiler | **6.7**（AC6 / armclang） | 工程已设 `uAC6=1`，C 语言标准 C11 |
| 器件支持包 | `Keil.STM32F4xx_DFP` | **只用于烧录算法与器件识别**，编译不依赖 |
| 调试器（可选） | ST-Link V2 / DAP-Link | 也可用串口 ISP |
| Python | 3.10+ | 上位机与测试脚本 |
| gcc（可选） | MinGW-w64 8.1 | 仅用于跑 `tests/firmware` 的主机端测试 |

> **关于 DFP**：本工程把 CMSIS 器件头文件、启动文件、HAL 驱动全部随仓库提交，
> 因此**即使没装 STM32F4 器件包也能编译通过**（已实测：Keil 5.24a + AC6.7，未装 F4 DFP，0 error）。
> 但要 **Flash → Download** 就必须安装 DFP（需要其中的 `STM32F4xx_512.FLM` 烧录算法）。
> 安装方式：Keil 菜单 `Pack Installer` → 搜索 `STM32F4xx_DFP` → Install。

## 2. 编译

### 2.1 图形界面

1. 双击 `firmware/MDK-ARM/QR_LVGL_F407.uvprojx`；
2. 工具栏选择 target `QR_LVGL_F407`；
3. `Project → Build Target`（**F7**）。

### 2.2 命令行（可用于 CI）

```bat
"D:\Keil5\UV4\UV4.exe" -b firmware\MDK-ARM\QR_LVGL_F407.uvprojx -j0 -o build.log
```

退出码：`0` = 成功；`1` = 有警告；`≥2` = 有错误。日志写入 `build.log`。

**本项目实测输出**：

```
*** Using Compiler 'V6.7', folder: 'D:\Keil5\ARM\ARMCLANG\Bin'
Build target 'QR_LVGL_F407'
compiling ...
linking...
Program Size: Code=316444 RO-data=21792 RW-data=124 ZI-data=74260
FromELF: creating hex file...
".\Objects\QR_LVGL_F407.axf" - 0 Error(s), 0 Warning(s).
Build Time Elapsed:  00:00:28
```

产物：

| 文件 | 用途 |
|:--|:--|
| `firmware/MDK-ARM/Objects/QR_LVGL_F407.axf` | 调试用 ELF |
| `firmware/MDK-ARM/Objects/QR_LVGL_F407.hex` | 烧录用 Intel HEX |
| `firmware/MDK-ARM/Listings/QR_LVGL_F407.map` | 内存分析 |

### 2.3 重新生成工程（源码增删时）

工程 XML 由脚本生成，**不要手改**（512 个文件）：

```bat
python tools\gen_keil_project.py
```

脚本会扫描 `firmware/Core/Src/*.c`、HAL `Src/*.c`、`Middlewares/qrcodegen/*.c`、
`Middlewares/lvgl/src/**/*.c` 并写入 `QR_LVGL_F407.uvprojx`，同时打印每个分组的文件数。

## 2.4 让 Keil 正确显示中文注释

本工程源码是 **UTF-8（无 BOM）**，注释为中文。Keil uVision 默认按 ANSI/GBK 打开，
会显示成乱码（**不影响编译**，armclang 按 UTF-8 处理）。设为 UTF-8 即可正常阅读：

`Edit → Configuration → Editor → Encoding` 选择 **UTF-8**，然后重新打开文件。

> 若你的 Keil 版本没有该选项，可以改装 `Keil MDK 5.3x` 以上，或用 VS Code 阅读源码。
> 不建议给 .c 文件加 BOM（部分工具链/脚本会误判文件类型）。

## 2.5 与 STM32CubeMX 的关系（**不要在此目录生成代码**）

### 2.5.1 本工程不是 CubeMX 生成的

代码**沿用了 CubeMX 的命名与骨架**（`MX_xxx_Init`、`SystemClock_Config`、`Error_Handler`），
但全部是手写的。判断依据：

| 判断依据 | 真 CubeMX 工程 | 本工程 |
|:--|:--|:--|
| `.mxproject`（工程缓存） | 有 | **无** |
| `/* USER CODE BEGIN/END */` 标记 | 每个生成文件都有 | **一个都没有** |
| 文件头 | ST 版权 + `@brief : Main program body` | 自写 Doxygen、中文注释 |
| 源文件构成 | 只有 CubeMX 生成的那几个 | 另有 `board_config.h`、`app.c`、`storage.c`、`lcd_st7789.c`、`lvgl_ui.c` 等业务模块 |

`firmware/QR_LVGL_F407.ioc` 是随工程提供的**参考配置**（引脚 / 时钟 / 外设），
供阅读与核对，不是"生成源"。

### 2.5.2 点 `GENERATE CODE` 会发生什么

CubeMX 只认识它自己生成的文件清单：

| 动作 | 文件 |
|:--|:--|
| **整文件覆盖** | `Core/Src/main.c`、`stm32f4xx_hal_msp.c`、`stm32f4xx_it.c`、`system_stm32f4xx.c`、启动文件、`Drivers/` |
| **重新生成** | `MDK-ARM/QR_LVGL_F407.uvprojx`、`.mxproject` |
| **不碰**（磁盘上仍在） | `app.c`、`storage.c`、`lcd_st7789.c`、`lvgl_ui.c`、`qr_*.c`、`uart_protocol.c`… |

后果：

1. `main.c` 里没有 `USER CODE` 块 → `ST7789_Init()` / `LV_Port_Init()` / `APP_Init()`
   全部丢失，只剩空 `while(1)`；
2. `.uvprojx` 被重新生成 → 工程里 **512 个源文件**（LVGL + qrcodegen + 业务模块）
   从项目树消失，编译立即报大量 `undefined symbol`；
3. `.ioc` 里没有 `MX_SPI1_Init` 的原始配置时，SPI1（W25Q16）的 GPIO 复用会丢失，
   掉电保存静默失效（本仓库的 `.ioc` 已补上 SPI1，但仍不建议原地生成）。

### 2.5.3 想改引脚 / 时钟时的安全做法

```bat
:: 1) 把 .ioc 复制到一个空目录
mkdir D:\tmp\mxcheck && copy firmware\QR_LVGL_F407.ioc D:\tmp\mxcheck\
:: 2) 在 CubeMX 里改完，生成到那个空目录
:: 3) 只比对三个文件，手工把差异合并回来
fc D:\tmp\mxcheck\Core\Src\main.c            firmware\Core\Src\main.c
fc D:\tmp\mxcheck\Core\Src\stm32f4xx_hal_msp.c firmware\Core\Src\stm32f4xx_hal_msp.c
```

即"**把 CubeMX 当配置计算器，而不是代码生成器**"：它算出的时钟/引脚参数落到
`board_config.h` + `main.c` + `stm32f4xx_hal_msp.c`，其余代码保持不动。
万一误操作，先 `git checkout .` 回滚，再重新编译。

## 3. 烧录

### 3.1 ST-Link / DAP-Link（推荐）

1. 按 [接线表](01-hardware-wiring.md) 连接 J1：`3V3 / GND / SWDIO(PA13) / SWCLK(PA14)`；
2. `Options for Target → Debug` 选择 `ST-Link Debugger`（或 CMSIS-DAP）；
3. `Settings → Flash Download` 勾选 `Reset and Run`；
4. `Flash → Download`（**F8**）。

### 3.2 串口 ISP（没有调试器时）

1. 把 **BOOT0 接 3V3**（原理图上 BOOT0 默认经 R7 接地）；
2. 复位（按 K1 或重新上电），芯片进入系统存储器 ISP 模式；
3. 用 STM32CubeProgrammer（UART 模式）或 FlyMcu，选择 USART1 对应 COM 口，
   115200 8N1，下载 `QR_LVGL_F407.hex`；
4. 下载完成后把 **BOOT0 接回 GND**，再复位一次，运行用户程序。

### 3.3 上电自检

正常现象：

1. 背光亮、屏幕先黑后显示；
2. 顶部深蓝色标题栏「QR Generator」，右上角 `READY`；
3. 中间纯白（尚未生成二维码）；
4. 底部浅灰状态栏「Waiting for data...」；
5. 板载 LED D2（PA1）1 Hz 闪烁。

若黑屏但背光亮：检查 SPI/CS/DC 接线与 `BOARD_LCD_*` 开关（见接线文档第 6 节）。
若全白或整屏像负片（白底变深蓝、标题栏发棕）：检查 `BOARD_LCD_INVERT_COLOR` 与
`BOARD_LCD_MADCTL_BGR`——**本项目实屏两者都是 `0`**，照抄别的模块的 `1` 会出现负片效果。

## 4. 上位机环境

```bat
cd host
python -m pip install -r requirements.txt
python main.py
```

`requirements.txt` 只有一项 `pyserial>=3.5`；Tkinter 随 Python 自带。

> Windows 官方 Python 自带 Tkinter；若使用精简版/嵌入式 Python 需要单独安装 Tk。
> 若把 `python` 指向了 LibreOffice 自带的 Python（本机就是如此），它**没有 Tkinter**，
> 请改用 `py -3` 或官方安装包。

## 5. 主机端测试（不需要硬件）

```powershell
powershell -ExecutionPolicy Bypass -File tests\firmware\run_tests.ps1
```

四步：gcc 编译 → 104 项断言 → BMP 结构/二维码解码校验 → MCU/PC 协议一致性比对。

```bat
cd host
python -m unittest discover -s tests -v
```

上位机单元测试（64 项，不需要 pyserial 与硬件）。

## 6. 常见问题

| 现象 | 原因 | 处理 |
|:--|:--|:--|
| `error: 'core_cm4.h' file not found` | CMSIS 头缺失 | 仓库应含 `firmware/Drivers/CMSIS/Include/core_cm4.h`；若被删请重新克隆 |
| `L6218E: Undefined symbol __clz` | AC6 把 `__builtin_clz` 下降为运行库符号 | 已由 `Core/Src/platform_compat.c` 提供，勿删 |
| 编译很慢（首次约 30 s） | LVGL 有 483 个 .c | 正常；增量编译只重编改动文件 |
| `Device not found` 但能编译 | 未装 STM32F4 DFP | 装 DFP 才能烧录，编译不受影响 |
| 烧录报 "Flash Download failed" | 缺 `STM32F4xx_512.FLM` | 安装 DFP 后重启 Keil |
| 串口打不开 | 被其他软件占用 / 驱动未装 | 关闭串口助手、装 CH340/CP210x 驱动 |
| 屏幕花屏 | SPI 太快 / 线太长 | `BOARD_LCD_SPI_PRESCALER` 改为 `/_8` |
| 颜色错乱 | BGR/RGB 或反色设置 | 见接线文档第 6 节 |
