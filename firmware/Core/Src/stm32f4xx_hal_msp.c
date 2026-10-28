/**
 * @file    stm32f4xx_hal_msp.c
 * @brief   HAL 外设底层初始化（MCU Support Package）：时钟使能、GPIO 复用、中断优先级。
 * @details
 *          本文件负责"把外设接到引脚上"，所有引脚号来自 board_config.h。
 *          业务层（app.c）与驱动层（lcd_st7789.c）都不直接配置 GPIO。
 */

#include "main.h"

/**
 * @brief  SPI 底层初始化：使能 SPI2 时钟，配置 PB13/PB14/PB15 为 AF5。
 * @param  hspi HAL SPI 句柄
 */
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    GPIO_InitTypeDef gpio = {0};

    if (hspi->Instance != SPI2) {
        return;
    }

    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* SCK / MOSI / MISO 复用推挽 */
    gpio.Pin       = BOARD_LCD_SCK_PIN | BOARD_LCD_MOSI_PIN | BOARD_LCD_MISO_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = BOARD_LCD_SPI_AF;
    HAL_GPIO_Init(BOARD_LCD_SPI_GPIO_PORT, &gpio);
}

/**
 * @brief  USART 底层初始化：使能 USART1 时钟，配置 PA9/PA10 为 AF7，开中断。
 * @param  huart HAL UART 句柄
 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};

    if (huart->Instance != USART1) {
        return;
    }

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin       = BOARD_UART_TX_PIN | BOARD_UART_RX_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;   /* 未接 USB-TTL 时 RX 不会浮空 */
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = BOARD_UART_AF;
    HAL_GPIO_Init(BOARD_UART_TX_PORT, &gpio);

    HAL_NVIC_SetPriority(BOARD_UART_IRQn, BOARD_UART_IRQ_PRIO, 0U);
    HAL_NVIC_EnableIRQ(BOARD_UART_IRQn);
}
