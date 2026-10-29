# 硬件与接线

> 本文所有引脚均来自用户提供的原理图
> [`schematic/STM32F407VET6-coreboard-schematic.pdf`](schematic/STM32F407VET6-coreboard-schematic.pdf)
> （mcudev / 嵌入式开发网 **STM32F407 核心板**，共 2 页），是逐网络核对结果，**不是猜测值**。
> 代码中所有引脚集中在 `firmware/Core/Inc/board_config.h`。

## 1. 核心板概况

| 项目 | 参数 | 原理图依据 |
|:--|:--|:--|
| MCU | STM32F407VET6（LQFP100，512 KiB Flash / 128 KiB SRAM + 64 KiB CCM） | U2 |
| 主晶振 | 8 MHz（HSE） | Y2 + C10/C11 20 pF |
| RTC 晶振 | 32.768 kHz（LSE） | Y1 + C8/C9 10 pF |
| 供电 | Micro USB 5 V → AMS1117-3.3（U1） | 「供电稳压」段 |
| 复位键 | K1 → NRST | 「复位按键」段 |
| 用户键 | K1 之外的另一按键接 PA0/WK_UP | 网络 `PA0/WK_UP` |
| 状态灯 D2 | 3V3 → D2 → R14 510 Ω → **PA1**（低电平点亮） | 「LED指示灯」段 |
| 电源灯 D1 | 3V3 → R5 510 Ω → D1 → GND（常亮，不可控） | 「供电指示灯」段 |
| 外部 Flash | W25Q16，SPI1：SCK=PB3、MISO=PB4、MOSI=PB5、CS=PA15 | U3 / 网络 `SPI1_*`、`F_CS` |
| TF 卡 | SDIO：PC8–PC12、PD2 | U5 |
| USB | Micro USB 直连 PA11(D-)/PA12(D+) | J5 |
| 调试口 | J1 五针：3V3 / GND / SWDIO=PA13 / SWCLK=PA14 | J1 |

> ⚠️ **本核心板没有 USB 转串口芯片**。USB 口是 USB Device（PA11/PA12），
> 本固件未实现 USB CDC，因此串口必须外接 3.3 V 的 USB-TTL 模块。

## 2. TFT 液晶屏接口 J4（8 针单排 2.54 mm）

原理图第 1 页「TFT液晶屏（SPI）接口」：`J4 = SIP8-2.54mm`，网络标号与 MCU 引脚的对应关系如下。

| J4 脚号 | 网络名 | MCU 引脚 | 外设复用 | 方向 | 说明 |
|:--:|:--|:--|:--|:--|:--|
| 1 | 3V3 | — | — | 电源 | 屏幕 3.3 V 供电 |
| 2 | GND | — | — | 电源 | 地 |
| 3 | SDI | **PB15** | SPI2_MOSI（AF5） | MCU→屏 | 数据输入 |
| 4 | SCL | **PB13** | SPI2_SCK（AF5） | MCU→屏 | 时钟 |
| 5 | CS | **PB12** | GPIO 推挽输出 | MCU→屏 | 片选，低有效 |
| 6 | SDO | **PB14** | SPI2_MISO（AF5） | 屏→MCU | 只写场景可不用（但仍复用配置，避免冲突） |
| 7 | RS | **PC5** | GPIO 推挽输出 | MCU→屏 | 数据/命令选择（DC）：0=命令，1=数据 |
| 8 | BLK | **PB1** | GPIO 推挽输出 | MCU→屏 | 背光，**高电平点亮** |

### 2.1 关于 RESET

**J4 第 6 脚是 SDO，不是 RST。** 该接口没有复位线，所以固件默认走 ST7789 的软件复位命令
（`0x01 SWRESET`，随后延迟 150 ms）。

若你手上的模块有独立 RST 脚（很多 8 针模块是 `VCC/GND/SCL/SDA/RES/DC/CS/BL` 排序），
需要**手动飞线**到一个空闲 GPIO，例如 PB0：

1. 把模块 RST 接到 PB0；
2. 修改 `board_config.h`：
   ```c
   #define BOARD_LCD_USE_HW_RESET  1
   #define BOARD_LCD_RST_PORT      GPIOB
   #define BOARD_LCD_RST_PIN       GPIO_PIN_0
   ```
3. `main.c` 的 `MX_GPIO_Init()` 会自动把该脚配成输出（代码已按宏条件编译）。

> 注意：模块丝印与 J4 网络名的顺序可能不同（J4 的 3/4 脚是 SDI/SCL，5/6 脚是 CS/SDO，
> 7/8 脚是 RS/BLK）。**接线前请以模块丝印为准逐脚核对自己的屏幕**，不要照抄"1 对 1"。

### 2.2 为什么用 SPI2

| SPI | 引脚 | 用途 | 结论 |
|:--|:--|:--|:--|
| SPI1 | PB3/PB4/PB5 + PA15 | 板载 W25Q16 Flash | 已占用 |
| SPI2 | PB13/PB15/PB12/PB14 | TFT | **空闲，选用** |

SPI2 挂在 APB1（42 MHz），默认 4 分频 → **10.5 MHz** SCK，稳定后可改 2 分频（21 MHz）。

## 3. 串口（USART1）接线

| USB-TTL 模块 | STM32F407 | 核心板排针（J2，22×2） |
|:--|:--|:--|
| TXD | PA10（USART1_RX，AF7） | J2 第 **21** 脚 |
| RXD | PA9（USART1_TX，AF7） | J2 第 **22** 脚 |
| GND | GND | J2 第 **5/6** 脚 |
| 3V3（可选，给模块供电时） | 3V3 | J2 第 **1/2** 脚 |

- 波特率 115200，8 数据位，无校验，1 停止位，无流控。
- **必须共地**；USB-TTL 必须选 3.3 V 电平（5 V TTL 也能被 5 V 容忍引脚识别，但 RX 侧请确认模块输出电平）。
- 不要把 RS-232 的 ±12 V 接到 MCU。

## 4. 烧录/调试口 J1（SWD，五针）

| J1 | 信号 | MCU 引脚 |
|:--:|:--|:--|
| 1 | 3V3 | — |
| 2 | BOOT0（ISP 用） | BOOT0 |
| 3 | GND | — |
| 4 | SWDIO | PA13 |
| 5 | SWCLK | PA14 |

## 5. 引脚汇总（写进 `board_config.h` 的那一份）

```c
/* LCD：SPI2 */
#define BOARD_LCD_SCK_PIN        GPIO_PIN_13   /* PB13 */
#define BOARD_LCD_MOSI_PIN       GPIO_PIN_15   /* PB15 */
#define BOARD_LCD_MISO_PIN       GPIO_PIN_14   /* PB14 */
#define BOARD_LCD_CS_PIN         GPIO_PIN_12   /* PB12 */
#define BOARD_LCD_DC_PIN         GPIO_PIN_5    /* PC5  */
#define BOARD_LCD_BL_PIN         GPIO_PIN_1    /* PB1  */
#define BOARD_LCD_USE_HW_RESET   0             /* J4 无 RST，软件复位 */

/* 串口 */
#define BOARD_UART_TX_PIN        GPIO_PIN_9    /* PA9  */
#define BOARD_UART_RX_PIN        GPIO_PIN_10   /* PA10 */

/* 状态灯 */
#define BOARD_LED_PIN            GPIO_PIN_1    /* PA1，低电平点亮 */
```

## 6. 上板前必须确认的 4 项

| 项目 | 现象 | 处理 |
|:--|:--|:--|
| 颜色顺序 | 红色显示成蓝色（R/B 互换），深蓝标题栏发棕 | 本项目实测模块为 **RGB**，默认 `BOARD_LCD_MADCTL_BGR = 0`；若颜色相反则改 `1` |
| 是否反色 | 白底变黑底、整体像负片 | 本项目实测模块**不需要**反色，默认 `BOARD_LCD_INVERT_COLOR = 0`；若白底变深蓝则改 `1` |
| 行列偏移 | 图像整体偏移 / 边缘有彩条 | 试 `BOARD_LCD_X_OFFSET/Y_OFFSET = 0` 或 `80`（240×320 常见 0） |
| SPI 速度 | 花屏、闪烁、偶发错色 | `BOARD_LCD_SPI_PRESCALER` 从 `/_4` 降到 `/_8` |

**建议的确认顺序**（不要一上来就看二维码）：

1. `ST7789_FillScreen(BOARD_COLOR_RED)` → 全屏纯红？纯色对不对。
2. 依次填红/绿/蓝三块，确认颜色顺序。
3. `ST7789_DrawPixel(0,0,WHITE)`、`(239,0)`、`(0,319)`、`(239,319)` → 四角位置对不对。
4. 再跑 LVGL：应看到顶部深蓝标题栏「QR Generator」、底部浅灰状态栏「Waiting for data...」。
5. 最后生成二维码。
