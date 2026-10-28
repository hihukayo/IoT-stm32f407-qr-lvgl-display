/**
 * @file    main.c
 * @brief   工程入口：时钟树、GPIO、SPI2、USART1 初始化与主循环。
 * @details
 *          启动顺序：
 *          1. `HAL_Init()`（SysTick 1 ms）；
 *          2. `SystemClock_Config()`（HSE 8 MHz -> PLL -> 168 MHz）；
 *          3. 板级 GPIO（LCD 的 CS/DC/BLK、状态灯）；
 *          4. SPI2（LCD 总线）与 USART1（上位机）；
 *          5. `ST7789_Init()` 点亮屏幕；
 *          6. `LV_Port_Init()` 注册 LVGL 显示；
 *          7. `APP_Init()` 建界面、绑定 BMP、启动串口中断。
 *
 *          主循环只有两件事：处理协议/图片流（`APP_Process`）与跑 LVGL 定时器
 *          （`lv_timer_handler`）。**没有**任何阻塞等待，也不在中断里跑 GUI。
 */

#include "main.h"
#include "lcd_st7789.h"
#include "lv_port_disp.h"
#include "lvgl_ui.h"
#include "app.h"

/* ======================================================================
 * 全局外设句柄（board_config.h 中已 extern 声明）
 * ==================================================================== */
UART_HandleTypeDef huart1;
SPI_HandleTypeDef  hspi2;

/* ======================================================================
 * 内部函数声明
 * ==================================================================== */
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI2_Init(void);
static void MX_USART1_UART_Init(void);

/* ======================================================================
 * 入口
 * ==================================================================== */
int main(void)
{
    /* 1. HAL 初始化：SysTick 1 ms、NVIC 优先级分组 4 */
    HAL_Init();

    /* 2. 时钟树：168 MHz */
    SystemClock_Config();

    /* 3. 板级 GPIO */
    MX_GPIO_Init();

    /* 4. 外设 */
    MX_SPI2_Init();
    MX_USART1_UART_Init();

    /* 5. 屏幕与 GUI */
    ST7789_Init();
    LV_Port_Init();

    /* 6. 应用层（内部会建界面并启动串口接收中断） */
    APP_Init();

    /* 7. 主循环 */
    for (;;) {
        APP_Process();          /* 协议解析、二维码生成、BMP 流推进 */
        (void)lv_timer_handler(); /* LVGL 定时器与界面刷新（主循环上下文） */
    }
}

/* ======================================================================
 * 时钟配置
 * ==================================================================== */
/**
 * @brief  配置系统时钟为 168 MHz（HSE 8 MHz，PLL M=8 N=336 P=2 Q=7）。
 * @note   APB1 = 42 MHz，APB2 = 84 MHz，USB/SDIO 时钟 48 MHz。
 *         Flash 等待周期 5 WS（168 MHz 要求）。
 */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM            = 8U;
    osc.PLL.PLLN            = 336U;
    osc.PLL.PLLP            = RCC_PLLP_DIV2;
    osc.PLL.PLLQ            = 7U;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }
}

/* ======================================================================
 * GPIO：LCD 控制线 + 状态灯
 * ==================================================================== */
/**
 * @brief  配置 LCD 的 CS / DC / 背光（以及可选复位）与状态指示灯。
 * @note   所有输出先写“安全电平”再切换为输出模式，避免上电瞬间误触发。
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* --- 片选 CS：默认拉高（不选中） --- */
    HAL_GPIO_WritePin(BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN, GPIO_PIN_SET);
    /* --- DC：默认高（数据） --- */
    HAL_GPIO_WritePin(BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN, GPIO_PIN_SET);
    /* --- 背光：默认关闭 --- */
#if BOARD_LCD_BL_ACTIVE_HIGH
    HAL_GPIO_WritePin(BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN, GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN, GPIO_PIN_SET);
#endif

    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    /* PB12 = CS */
    gpio.Pin = BOARD_LCD_CS_PIN;
    HAL_GPIO_Init(BOARD_LCD_CS_PORT, &gpio);

    /* PC5 = DC */
    gpio.Pin = BOARD_LCD_DC_PIN;
    HAL_GPIO_Init(BOARD_LCD_DC_PORT, &gpio);

    /* PB1 = 背光 */
    gpio.Pin = BOARD_LCD_BL_PIN;
    HAL_GPIO_Init(BOARD_LCD_BL_PORT, &gpio);

#if BOARD_LCD_USE_HW_RESET
    HAL_GPIO_WritePin(BOARD_LCD_RST_PORT, BOARD_LCD_RST_PIN, GPIO_PIN_SET);
    gpio.Pin = BOARD_LCD_RST_PIN;
    HAL_GPIO_Init(BOARD_LCD_RST_PORT, &gpio);
#endif

#if BOARD_LED_ENABLE
    /* 状态灯 PA1：低电平点亮，先写灭 */
#if BOARD_LED_ACTIVE_LOW
    HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN, GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN, GPIO_PIN_RESET);
#endif
    gpio.Pin  = BOARD_LED_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(BOARD_LED_PORT, &gpio);
#endif
}

/* ======================================================================
 * SPI2
 * ==================================================================== */
/**
 * @brief  初始化 SPI2：主机、8 位、Mode 0、MSB first、软件 NSS。
 * @note   APB1 = 42 MHz，默认 4 分频 -> 10.5 MHz SCK。
 */
static void MX_SPI2_Init(void)
{
    hspi2.Instance               = SPI2;
    hspi2.Init.Mode              = SPI_MODE_MASTER;
    hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi2.Init.NSS               = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = BOARD_LCD_SPI_PRESCALER;
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 10U;

    if (HAL_SPI_Init(&hspi2) != HAL_OK) {
        Error_Handler();
    }
}

/* ======================================================================
 * USART1
 * ==================================================================== */
/**
 * @brief  初始化 USART1：115200 8N1，收发模式，无流控。
 * @note   中断接收在 APP_Init() 中以单字节方式挂起。
 */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = BOARD_UART_BAUDRATE;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }
}

/* ======================================================================
 * 错误处理
 * ==================================================================== */
/**
 * @brief  不可恢复错误：关闭中断并停留在此，等待调试器或复位。
 */
void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        /* 停留点：可用调试器查看调用栈 */
    }
}

#ifdef USE_FULL_ASSERT
/**
 * @brief  HAL 断言失败回调。
 * @param  file 文件名
 * @param  line 行号
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif /* USE_FULL_ASSERT */
