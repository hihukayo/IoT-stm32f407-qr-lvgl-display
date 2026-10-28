/**
 * @file    lcd_st7789.h
 * @brief   ST7789（240x320，4 线 SPI）驱动接口：初始化、窗口、像素/矩形填充、背光、旋转。
 * @details
 *          驱动只理解"屏幕"，不理解串口协议、二维码或 LVGL。
 *          ST7789 自带 GRAM，因此**不需要也不允许**分配 240x320x2 = 150 KiB 的全屏缓冲；
 *          本驱动所有接口都是"设定窗口 + 流式写入像素"。
 *
 *          引脚与 SPI 实例全部来自 @ref board_config.h。
 */

#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#include <stdint.h>
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 屏幕旋转方向。 */
typedef enum {
    LCD_ROTATION_0   = 0,   /**< 竖屏 240x320            */
    LCD_ROTATION_90  = 1,   /**< 横屏 320x240            */
    LCD_ROTATION_180 = 2,   /**< 竖屏翻转                */
    LCD_ROTATION_270 = 3    /**< 横屏翻转                */
} lcd_rotation_t;

/**
 * @brief  初始化 ST7789：GPIO 已由 MSP 配好，本函数发送复位与初始化序列并点亮背光。
 * @note   初始化序列、偏移、颜色顺序均来自 board_config.h 的可调宏。
 */
void ST7789_Init(void);

/**
 * @brief  设置当前旋转方向（会重新写入 MADCTL，并更新逻辑宽高）。
 * @param  rotation 旋转方向
 */
void ST7789_SetRotation(lcd_rotation_t rotation);

/**
 * @brief  读取当前逻辑宽度（受旋转影响）。
 * @return 像素宽度
 */
uint16_t ST7789_GetWidth(void);

/**
 * @brief  读取当前逻辑高度（受旋转影响）。
 * @return 像素高度
 */
uint16_t ST7789_GetHeight(void);

/**
 * @brief  设置写入窗口（闭区间），后续像素按窗口内从左到右、从上到下填充。
 * @param  x0 左列坐标（含）
 * @param  y0 上行坐标（含）
 * @param  x1 右列坐标（含）
 * @param  y1 下行坐标（含）
 * @note   内部自动加上 board_config.h 中的偏移量，并做边界裁剪。
 */
void ST7789_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief  向当前窗口连续写入 RGB565 像素。
 * @param  pixels 像素数组（主机字节序 uint16，驱动内部按高位先发）
 * @param  count  像素个数
 * @note   调用前需已调用 ST7789_SetWindow()。
 */
void ST7789_WritePixels(const uint16_t *pixels, uint32_t count);

/**
 * @brief  把一张已在主机内存中排好序的 RGB565 位图写入指定矩形区域。
 * @param  x      目标左列
 * @param  y      目标上行
 * @param  w      位图宽度（像素）
 * @param  h      位图高度（像素）
 * @param  stride 位图行跨度（字节），允许大于 w*2
 * @param  data   位图首地址
 * @note   用于 LVGL 的局部 flush：当 row_stride == w*2 时走整块快速路径，
 *         否则逐行发送，保证任意对齐方式都正确。
 */
void ST7789_BlitRGB565(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint32_t stride, const uint8_t *data);

/**
 * @brief  用单一颜色填充矩形。
 * @param  x 左列坐标
 * @param  y 上行坐标
 * @param  w 宽度（像素，0 表示无操作）
 * @param  h 高度（像素，0 表示无操作）
 * @param  color RGB565 颜色
 */
void ST7789_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief  整屏填充。
 * @param  color RGB565 颜色
 */
void ST7789_FillScreen(uint16_t color);

/**
 * @brief  绘制单个像素。
 * @param  x 列坐标
 * @param  y 行坐标
 * @param  color RGB565 颜色
 */
void ST7789_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief  控制背光。
 * @param  on true 点亮，false 熄灭
 */
void ST7789_SetBacklight(bool on);

/**
 * @brief  屏幕是否已完成初始化（供应用层判断，不做硬件探测）。
 * @return true 已初始化
 */
bool ST7789_IsReady(void);

#ifdef __cplusplus
}
#endif

#endif /* LCD_ST7789_H */
