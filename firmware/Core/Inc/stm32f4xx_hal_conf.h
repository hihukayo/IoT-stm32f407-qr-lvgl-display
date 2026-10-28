/**
 * @file    stm32f4xx_hal_conf.h
 * @brief   STM32F4 HAL 配置：只启用本工程真正用到的外设模块。
 * @details
 *          启用：RCC / GPIO / CORTEX / DMA / USART / SPI / PWR / FLASH / EXTI。
 *          其余模块（ADC、I2C、TIM、SD、USB、ETH…）全部关闭以减少 Flash 占用
 *          与编译时间。
 *
 *          @warning 若后续增加新外设（例如改用 DMA 发送 SPI），必须在这里
 *                   打开对应模块并加入对应的 `stm32f4xx_hal_xxx.c` 文件。
 */

#ifndef STM32F4xx_HAL_CONF_H
#define STM32F4xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * 模块选择
 * ==================================================================== */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ======================================================================
 * 时钟源参数（与 board_config.h 保持一致）
 * ==================================================================== */
#if !defined(HSE_VALUE)
  #define HSE_VALUE             8000000U   /**< 外部晶振 8 MHz（原理图 Y2） */
#endif

#if !defined(HSE_STARTUP_TIMEOUT)
  #define HSE_STARTUP_TIMEOUT   100U       /**< HSE 起振超时（ms）          */
#endif

#if !defined(HSI_VALUE)
  #define HSI_VALUE             16000000U  /**< 内部 RC 16 MHz              */
#endif

#if !defined(LSI_VALUE)
  #define LSI_VALUE             32000U     /**< 内部低速 RC 32 kHz          */
#endif

#if !defined(LSE_VALUE)
  #define LSE_VALUE             32768U     /**< 外部低速晶振 32.768 kHz（Y1）*/
#endif

#if !defined(LSE_STARTUP_TIMEOUT)
  #define LSE_STARTUP_TIMEOUT   5000U
#endif

#if !defined(EXTERNAL_CLOCK_VALUE)
  #define EXTERNAL_CLOCK_VALUE  12288000U
#endif

/* ======================================================================
 * 系统配置
 * ==================================================================== */
#define  VDD_VALUE                    3300U   /**< 供电电压（mV）        */
#define  TICK_INT_PRIORITY            0x0FU   /**< SysTick 中断优先级     */
#define  USE_RTOS                     0U
#define  PREFETCH_ENABLE              1U
#define  INSTRUCTION_CACHE_ENABLE     1U
#define  DATA_CACHE_ENABLE            1U

#define  USE_HAL_UART_REGISTER_CALLBACKS  0U
#define  USE_HAL_SPI_REGISTER_CALLBACKS   0U

/* ======================================================================
 * 断言
 * ==================================================================== */
/* #define USE_FULL_ASSERT    1U */

#ifdef USE_FULL_ASSERT
  #define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
  void assert_failed(uint8_t *file, uint32_t line);
#else
  #define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

/* ======================================================================
 * 模块头文件
 * ==================================================================== */
#ifdef HAL_RCC_MODULE_ENABLED
  #include "stm32f4xx_hal_rcc.h"
#endif

#ifdef HAL_GPIO_MODULE_ENABLED
  #include "stm32f4xx_hal_gpio.h"
#endif

#ifdef HAL_DMA_MODULE_ENABLED
  #include "stm32f4xx_hal_dma.h"
#endif

#ifdef HAL_CORTEX_MODULE_ENABLED
  #include "stm32f4xx_hal_cortex.h"
#endif

#ifdef HAL_EXTI_MODULE_ENABLED
  #include "stm32f4xx_hal_exti.h"
#endif

#ifdef HAL_FLASH_MODULE_ENABLED
  #include "stm32f4xx_hal_flash.h"
#endif

#ifdef HAL_PWR_MODULE_ENABLED
  #include "stm32f4xx_hal_pwr.h"
#endif

#ifdef HAL_SPI_MODULE_ENABLED
  #include "stm32f4xx_hal_spi.h"
#endif

#ifdef HAL_UART_MODULE_ENABLED
  #include "stm32f4xx_hal_uart.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32F4xx_HAL_CONF_H */
