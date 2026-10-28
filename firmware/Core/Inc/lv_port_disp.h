/**
 * @file    lv_port_disp.h
 * @brief   LVGL v9.6.0 显示移植层：240x320 / RGB565 / PARTIAL 局部渲染 / ST7789 flush。
 * @details
 *          内存策略（需求 NFR-008、6.2 节）：
 *          - 单缓冲，240 x 20 个 RGB565 像素 = 9 600 字节，静态分配于 .bss；
 *          - `LV_DISPLAY_RENDER_MODE_PARTIAL`，**绝不**分配 240x320x2 = 153 600 字节全屏缓冲；
 *          - flush_cb 在**阻塞发送真正完成之后**才调用 `lv_display_flush_ready()`；
 *          - 时基通过 `lv_tick_set_cb()` 直接读取 `HAL_GetTick()`（SysTick 1 ms），
 *            不在中断里运行 GUI。
 */

#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief LVGL 绘图缓冲区像素个数（240 x 20）。 */
#define LV_PORT_DISP_BUF_LINES  20U
/** @brief LVGL 绘图缓冲区像素总数。 */
#define LV_PORT_DISP_BUF_PIXELS (240U * LV_PORT_DISP_BUF_LINES)
/** @brief LVGL 绘图缓冲区字节数（RGB565）。 */
#define LV_PORT_DISP_BUF_BYTES  (LV_PORT_DISP_BUF_PIXELS * 2U)

/**
 * @brief  初始化 LVGL 并注册 ST7789 显示设备。
 * @note   调用前必须先完成 ST7789_Init() 与 lv_init()；本函数内部负责
 *         `lv_init()` 之后的显示注册、缓冲区绑定与 flush 回调注册。
 *         实际顺序请使用 @ref LV_Port_Init()。
 */
void LV_Port_Init(void);

/**
 * @brief  获取 LVGL 绘图缓冲区字节数（用于内存预算自检/日志）。
 * @return 缓冲区字节数
 */
uint32_t LV_Port_GetBufferBytes(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_DISP_H */
