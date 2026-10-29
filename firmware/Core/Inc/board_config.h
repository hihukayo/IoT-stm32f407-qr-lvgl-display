/**
 * @file    board_config.h
 * @brief   板级配置中心：LCD（ST7789 / SPI2）、串口、状态灯、屏幕几何与颜色参数。
 * @details
 *          本文件是"引脚与硬件参数"的唯一事实来源，业务逻辑（协议、二维码、BMP）
 *          不得直接出现任何 GPIO 端口/引脚宏。更换开发板或屏幕时，只改本文件即可。
 *
 *          **引脚来源**：全部取自随项目提交的原理图
 *          `docs/schematic/原理图.pdf`（STM32F407VET6 核心板，mcudev 出品）。
 *          原理图第 1 页 "TFT液晶屏（SPI）接口" J4（SIP8-2.54mm）网络标号：
 *
 *          | J4 引脚 | 网络名   | MCU 引脚 | 说明                       |
 *          |:-------:|:---------|:---------|:---------------------------|
 *          | 1       | 3V3      | -        | 屏幕供电（3.3 V）          |
 *          | 2       | GND      | -        | 地                         |
 *          | 3       | SDI      | PB15     | SPI2_MOSI                  |
 *          | 4       | SCL      | PB13     | SPI2_SCK                   |
 *          | 5       | CS       | PB12     | 片选（软件控制）           |
 *          | 6       | SDO      | PB14     | SPI2_MISO（只写，可不用）  |
 *          | 7       | RS       | PC5      | 数据/命令（DC）            |
 *          | 8       | BLK      | PB1      | 背光（高电平点亮）         |
 *
 *          @warning 该核心板的 J4 接口**没有 RESET 引脚**（第 6 脚是 SDO）。
 *                   因此默认使用 ST7789 的软件复位命令（0x01），并把
 *                   `BOARD_LCD_USE_HW_RESET` 置 0。若你的屏幕模块带 RST 脚，
 *                   把它接到一个空闲 GPIO（例如 PB0），把该宏改为 1 并修改
 *                   `BOARD_LCD_RST_PIN` / `BOARD_LCD_RST_PORT` 即可。
 *
 * @note    本文件中的引脚不是"猜测"值，而是由原理图网络表逐点核对得到；
 *          但屏幕模块自身的行列偏移、RGB/BGR 顺序、是否反色仍需上板确认，
 *          相关开关集中在文件末尾的 "屏幕时序/颜色可调项" 区域。
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "stm32f4xx_hal.h"

/* ======================================================================
 * 1. 时钟：核心板板载 8 MHz 晶振（原理图 Y2 = 8M，C10/C11 = 20 pF）
 * ==================================================================== */
#define BOARD_HSE_VALUE         8000000U   /**< 外部高速晶振频率，Hz（原理图 Y2） */
#define BOARD_SYSCLK_HZ         168000000U /**< 系统主频，STM32F407 上限 168 MHz    */
#define BOARD_APB1_HZ           42000000U  /**< APB1 = 42 MHz（SPI2/USART2 所在总线）*/
#define BOARD_APB2_HZ           84000000U  /**< APB2 = 84 MHz（USART1/SPI1 所在总线）*/

/* ======================================================================
 * 2. 串口：USART1，PA9 = TX，PA10 = RX（原理图网络 PA9/TXD1、PA10/RXD1）
 * ==================================================================== */
extern UART_HandleTypeDef huart1;          /**< USART1 句柄，定义于 main.c */

#define BOARD_UART_HANDLE       (&huart1)  /**< 应用层使用的串口句柄            */
#define BOARD_UART_BAUDRATE     115200U    /**< 默认波特率，8N1，无流控         */
#define BOARD_UART_TX_PORT      GPIOA      /**< PA9  = USART1_TX（AF7）         */
#define BOARD_UART_TX_PIN       GPIO_PIN_9
#define BOARD_UART_RX_PORT      GPIOA      /**< PA10 = USART1_RX（AF7）         */
#define BOARD_UART_RX_PIN       GPIO_PIN_10
#define BOARD_UART_AF           GPIO_AF7_USART1
#define BOARD_UART_IRQn         USART1_IRQn

/* ======================================================================
 * 3. LCD：ST7789，4 线 SPI，挂在 SPI2
 * ==================================================================== */
extern SPI_HandleTypeDef hspi2;            /**< SPI2 句柄，定义于 main.c */

#define BOARD_LCD_SPI_HANDLE    (&hspi2)   /**< LCD 使用的 SPI 外设              */
/** SPI2 时钟分频：APB1 42 MHz / 4 = 10.5 MHz（稳妥默认值）。
 *  上板确认稳定后可改为 SPI_BAUDRATEPRESCALER_2（21 MHz）以提升刷新率。 */
#define BOARD_LCD_SPI_PRESCALER SPI_BAUDRATEPRESCALER_4
/** SPI 单次传输超时（ms）。用于 HALL 阻塞发送，防止屏幕异常时死等。 */
#define BOARD_LCD_SPI_TIMEOUT   100U

/* --- SPI 总线引脚（PB13/PB14/PB15，AF5） --- */
#define BOARD_LCD_SPI_GPIO_PORT GPIOB
#define BOARD_LCD_SCK_PIN       GPIO_PIN_13  /**< PB13 = SPI2_SCK  = SCL  */
#define BOARD_LCD_MOSI_PIN      GPIO_PIN_15  /**< PB15 = SPI2_MOSI = SDI  */
#define BOARD_LCD_MISO_PIN      GPIO_PIN_14  /**< PB14 = SPI2_MISO = SDO（只写时不用）*/
#define BOARD_LCD_SPI_AF        GPIO_AF5_SPI2

/* --- 片选 CS：PB12，软件控制 --- */
#define BOARD_LCD_CS_PORT       GPIOB
#define BOARD_LCD_CS_PIN        GPIO_PIN_12

/* --- 数据/命令 DC（原理图网络名 RS）：PC5 --- */
#define BOARD_LCD_DC_PORT       GPIOC
#define BOARD_LCD_DC_PIN        GPIO_PIN_5

/* --- 背光 BLK：PB1，高电平点亮 --- */
#define BOARD_LCD_BL_PORT       GPIOB
#define BOARD_LCD_BL_PIN        GPIO_PIN_1
#define BOARD_LCD_BL_ACTIVE_HIGH 1           /**< 1 = 高电平点亮；若模块相反改为 0 */

/* --- 硬件复位：该核心板 J4 无 RST 脚，默认关闭，使用软件复位 --- */
#define BOARD_LCD_USE_HW_RESET  0           /**< 1 = 使用独立 GPIO 复位屏幕 */
#if BOARD_LCD_USE_HW_RESET
  #define BOARD_LCD_RST_PORT    GPIOB
  #define BOARD_LCD_RST_PIN     GPIO_PIN_0
#endif

/* ======================================================================
 * 4. 屏幕几何与可调参数（上板确认项）
 * ==================================================================== */
#define BOARD_LCD_WIDTH         240         /**< 逻辑宽度（竖屏）              */
#define BOARD_LCD_HEIGHT        320         /**< 逻辑高度（竖屏）              */
#define BOARD_LCD_X_OFFSET      0           /**< 列偏移，部分 240x320 模块为 0 */
#define BOARD_LCD_Y_OFFSET      0           /**< 行偏移，部分 240x320 模块为 0 */

/** 1 = MADCTL 使用 BGR 顺序（IPS 屏常见），0 = RGB。颜色不对时取反。
 *  本项目实测的 2.4" 模块为 **RGB 顺序**：置 1 时深蓝标题栏会变成棕色
 *  （`#1B2A41` 交换红蓝即 `#412A1B`），故默认取 0。 */
#define BOARD_LCD_MADCTL_BGR    0
/** 1 = 上电发送 INVON（0x21）开启反色，0 = 不发送。
 *  本项目实测的 2.4" 240x320 ST7789 模块**不需要**反色：置 1 会出现"白底变深蓝、
 *  黑色模块变白"的整屏负片效果（详见 docs/06-known-limitations.md 第 3.1 节后记）。
 *  若你的模块颜色恰好相反（白底变黑底），再把它改回 1。 */
#define BOARD_LCD_INVERT_COLOR  0

/** 固件启动时的屏幕旋转：0=竖屏，1=横屏，2=竖屏翻转，3=横屏翻转。 */
#define BOARD_LCD_ROTATION      0

/* ======================================================================
 * 5. 状态指示灯：D2 接 PA1，低电平点亮（原理图 "LED指示灯" 段）
 *       3V3 -> D2(LED) -> R14(510R) -> PA1
 * ==================================================================== */
#define BOARD_LED_ENABLE        1           /**< 0 = 不使用状态灯                */
#define BOARD_LED_PORT          GPIOA
#define BOARD_LED_PIN           GPIO_PIN_1
#define BOARD_LED_ACTIVE_LOW    1           /**< 1 = 低电平点亮                  */

/* ======================================================================
 * 6. 中断优先级（数值越小优先级越高）
 * ==================================================================== */
#define BOARD_UART_IRQ_PRIO     1U
#define BOARD_SPI_IRQ_PRIO      2U

/* ======================================================================
 * 7. 外部 SPI Flash：W25Q16（2 MiB），用于掉电保存上一次的二维码文本
 *       原理图「SPI Flash」段：SCK=PB3  MISO=PB4  MOSI=PB5  CS=PA15
 *       与 LCD 的 SPI2 是两条独立总线，互不影响。
 * ==================================================================== */
extern SPI_HandleTypeDef hspi1;

#define BOARD_FLASH_ENABLE              1                       /**< 0 = 关闭持久化        */
#define BOARD_FLASH_SPI_HANDLE          (&hspi1)
#define BOARD_FLASH_SPI_GPIO_PORT       GPIOB
#define BOARD_FLASH_SCK_PIN             GPIO_PIN_3              /**< PB3  = SPI1_SCK  */
#define BOARD_FLASH_MISO_PIN            GPIO_PIN_4              /**< PB4  = SPI1_MISO */
#define BOARD_FLASH_MOSI_PIN            GPIO_PIN_5              /**< PB5  = SPI1_MOSI */
#define BOARD_FLASH_SPI_AF              GPIO_AF5_SPI1
/** SPI1 挂在 APB2(84 MHz)，4 分频 = 21 MHz；W25Q16 支持到 104 MHz，余量充足。 */
#define BOARD_FLASH_SPI_PRESCALER       SPI_BAUDRATEPRESCALER_4
/** 片选：PA15，低有效；上电默认拉高（不选中）。 */
#define BOARD_FLASH_CS_PORT             GPIOA
#define BOARD_FLASH_CS_PIN              GPIO_PIN_15
/** W25Q16 的 JEDEC ID（读 0x9F 应得 EF 40 15）。 */
#define BOARD_FLASH_JEDEC_ID            0xEF4015U

/* ======================================================================
 * 8. 常用颜色（RGB565）
 * ==================================================================== */
#define BOARD_COLOR_BLACK       0x0000U
#define BOARD_COLOR_WHITE       0xFFFFU
#define BOARD_COLOR_RED         0xF800U
#define BOARD_COLOR_GREEN       0x07E0U
#define BOARD_COLOR_BLUE        0x001FU
#define BOARD_COLOR_GRAY        0x8410U

#endif /* BOARD_CONFIG_H */
