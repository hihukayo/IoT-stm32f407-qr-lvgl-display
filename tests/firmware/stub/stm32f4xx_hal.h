/**
 * @file    stm32f4xx_hal.h
 * @brief   主机端测试用的 HAL 桩（stub）。
 * @details
 *          为了让 `uart_protocol.c` / `bmp_stream.c` / `qr_service.c` / `qr_bitmap.c`
 *          这些纯逻辑模块能在 PC 上用 gcc 编译并做单元测试，这里提供 HAL 的最小替身：
 *          - 句柄类型（只有占位字段）；
 *          - `HAL_StatusTypeDef`；
 *          - `HAL_GetTick()` 与 `HAL_UART_Transmit()`（由 test_main.c 实现）。
 *
 *          @warning 本文件**只用于主机端测试**，绝不可加入 Keil 固件工程。
 */

#ifndef STM32F4XX_HAL_H
#define STM32F4XX_HAL_H

#include <stdint.h>
#include <string.h>

/** @brief UART 句柄替身。 */
typedef struct {
    int dummy;
} UART_HandleTypeDef;

/** @brief SPI 句柄替身。 */
typedef struct {
    int dummy;
} SPI_HandleTypeDef;

/** @brief HAL 返回状态。 */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/** @brief 获取毫秒时基（测试中可被手动推进）。 */
uint32_t HAL_GetTick(void);

/** @brief 模拟串口发送（测试中把字节捕获到缓冲区）。 */
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout);

#endif /* STM32F4XX_HAL_H */
