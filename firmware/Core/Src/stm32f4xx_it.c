/**
 * @file    stm32f4xx_it.c
 * @brief   中断服务程序：SysTick 与 USART1。
 * @details
 *          **只做最少的必要工作**：SysTick 递增 HAL 时基，USART1 交给 HAL 的
 *          中断处理函数（其回调里仅做"压入环形缓冲"）。GUI、二维码编码、
 *          Flash 操作等耗时任务一律在主循环完成。
 */

#include "main.h"

/* 外部句柄 */
extern UART_HandleTypeDef huart1;

/* ======================================================================
 * Cortex-M4 内核异常
 * ==================================================================== */
/**
 * @brief  不可屏蔽中断。
 */
void NMI_Handler(void)
{
    for (;;) {
    }
}

/**
 * @brief  硬件错误：停在原地，便于调试器定位。
 */
void HardFault_Handler(void)
{
    for (;;) {
    }
}

/**
 * @brief  存储器管理错误。
 */
void MemManage_Handler(void)
{
    for (;;) {
    }
}

/**
 * @brief  总线错误。
 */
void BusFault_Handler(void)
{
    for (;;) {
    }
}

/**
 * @brief  用法错误。
 */
void UsageFault_Handler(void)
{
    for (;;) {
    }
}

/**
 * @brief  调试监视器异常。
 */
void DebugMon_Handler(void)
{
}

/**
 * @brief  系统节拍中断：递增 HAL 毫秒时基（LVGL 通过 HAL_GetTick 取值）。
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ======================================================================
 * 外设中断
 * ==================================================================== */
/**
 * @brief  USART1 中断：交给 HAL 处理（其内部会调用 RxCplt / Error 回调）。
 */
void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}
