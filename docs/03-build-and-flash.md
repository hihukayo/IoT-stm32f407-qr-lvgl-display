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
若全白：检查是否发了 `INVON`（`BOARD_LCD_INVERT_COLOR`）。

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

上位机单元测试（52 项，不需要 pyserial 与硬件）。

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
