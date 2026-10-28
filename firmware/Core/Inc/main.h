/**
 * @file    main.h
 * @brief   工程总头文件：HAL 引入、错误处理与全局句柄声明。
 */

#ifndef MAIN_H
#define MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"
#include "board_config.h"

/**
 * @brief  致命错误处理：关闭中断并停在此处，等待调试器或看门狗复位。
 * @note   仅用于初始化失败等不可恢复场景。
 */
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
