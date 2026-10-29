/**
 * @file    lv_port_disp.c
 * @brief   LVGL v9.6.0 -> ST7789 显示移植层实现。
 * @details
 *          - 240x320 显示设备，颜色格式 `LV_COLOR_FORMAT_RGB565_SWAPPED`
 *            （SPI 需要高字节先发，交换放在格式层，驱动只需原样发送字节）；
 *          - `LV_DISPLAY_RENDER_MODE_PARTIAL`，单缓冲 240x20 = 9 600 字节；
 *          - flush_cb 阻塞发送完成后调用 `lv_display_flush_ready()`；
 *          - 时基由 `lv_tick_set_cb(HAL_GetTick)` 提供，SysTick 1 ms。
 *
 *          @warning 行跨度以 `lv_display_get_buf_active()->header.stride` 为准，
 *                   不假设它等于"区域宽度 x 2"，因此任意对齐宽度都能正确刷屏。
 */

#include "lv_port_disp.h"
#include "lvgl_ui.h"
#include "lcd_st7789.h"
#include "board_config.h"

/* ======================================================================
 * 静态缓冲区（.bss，普通 SRAM，DMA 可访问区域）
 * ==================================================================== */

/** @brief LVGL 局部绘图缓冲：240 x 20 像素 RGB565。 */
static uint16_t s_draw_buf[LV_PORT_DISP_BUF_PIXELS];

/** @brief 显示设备句柄。 */
static lv_display_t *s_disp = NULL;

/* ======================================================================
 * 时基
 * ==================================================================== */
/**
 * @brief  LVGL 时基回调。
 * @return 毫秒计数（来自 HAL SysTick，1 ms 递增）
 * @note   只是读取一个 32 位变量，可在任意上下文调用，不会运行 GUI。
 */
static uint32_t port_tick_get_cb(void)
{
    return (uint32_t)HAL_GetTick();
}

/* ======================================================================
 * flush 回调
 * ==================================================================== */
/**
 * @brief  LVGL 局部区域刷新回调。
 * @param  disp    显示设备
 * @param  area    要刷新的闭区间区域
 * @param  px_map  像素数据起始地址（RGB565_SWAPPED，字节序即 SPI 需要的顺序）
 */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w;
    uint32_t h;
    uint32_t stride;
    lv_draw_buf_t *buf;

    if ((area == NULL) || (px_map == NULL)) {
        lv_display_flush_ready(disp);
        return;
    }

    w = (uint32_t)lv_area_get_width(area);
    h = (uint32_t)lv_area_get_height(area);

    buf = lv_display_get_buf_active(disp);
    stride = (buf != NULL) ? buf->header.stride : (w * 2U);

    /* 二维码区域：按 1bpp 位图重新生成像素，绕开 LVGL 的 I1 源混色路径
     * （该路径在本工程配置下会把整张位图渲染成全黑，见 lvgl_ui.c 注释）。 */
    /* 二维码区域：按 1bpp 位图重新生成像素，绕开 LVGL 的 I1 源混色路径
     * （该路径在本工程配置下会把整张位图渲染成全黑，见 lvgl_ui.c 注释）。 */
    {
        lv_area_t qr_area = *area;
        (void)UI_QrOverlay(&qr_area, px_map, stride);
    }

    /* 阻塞式写入；返回时数据已全部进入 ST7789 的 GRAM */
    ST7789_BlitRGB565((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)w, (uint16_t)h, stride, px_map);

    /* 只有真正发送完成才允许 LVGL 复用该缓冲区 */
    lv_display_flush_ready(disp);
}

/* ======================================================================
 * 初始化
 * ==================================================================== */
void LV_Port_Init(void)
{
    /* 1. LVGL 内核 */
    lv_init();

    /* 2. 1 ms 时基（不依赖 lv_tick_inc 的中断调用，直接读 SysTick 计数） */
    lv_tick_set_cb(port_tick_get_cb);

    /* 3. 创建显示设备 */
    s_disp = lv_display_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    if (s_disp == NULL) {
        return;
    }

    /* 4. 颜色格式：字节序已交换，SPI 层无需再处理 */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    /* 5. 刷新回调 */
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    /* 6. 局部渲染模式 + 单缓冲（第二缓冲传 NULL） */
    lv_display_set_buffers(s_disp, s_draw_buf, NULL,
                           (uint32_t)sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
}

uint32_t LV_Port_GetBufferBytes(void)
{
    return (uint32_t)sizeof(s_draw_buf);
}
