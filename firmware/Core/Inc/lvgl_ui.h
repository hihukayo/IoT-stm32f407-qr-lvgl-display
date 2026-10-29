/**
 * @file    lvgl_ui.h
 * @brief   LVGL 界面层：标题、状态、二维码图像控件。
 * @details
 *          布局（240x320 竖屏，需求 6.3 节）：
 *          @verbatim
 *          y =   0..39   顶部：标题 "QR Generator" + 右侧信息（版本/ECC）
 *          y =  40..279  二维码画布 240x240（1bpp I1 图像控件）
 *          y = 280..319  底部：状态文本（等待数据 / 生成成功 / 导出中 / 错误）
 *          @endverbatim
 *
 *          二维码**不会**为每个黑色模块创建一个 lv_obj，而是渲染到一块
 *          240x240 的 1bpp 位图（见 qr_bitmap.h），由单个 lv_image 控件显示。
 */

#ifndef LVGL_UI_H
#define LVGL_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "qr_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  创建全部界面对象。
 * @note   必须在 lv_init() 与 LV_Port_Init() 之后调用，且只在主循环上下文中调用。
 */
void UI_Init(void);

/**
 * @brief  设置底部状态文本（自动截断，不分配新对象）。
 * @param  text UTF-8 文本，NULL 表示清空
 * @note   内部使用静态 label，复用同一控件，不会每次创建新对象。
 */
void UI_SetStatus(const char *text);

/**
 * @brief  设置右上角信息文本（例如 "V3 M 29x29"）。
 * @param  text UTF-8 文本，NULL 表示清空
 */
void UI_SetInfo(const char *text);

/**
 * @brief  二维码位图更新后调用：显示图像控件并触发重绘。
 * @param  meta 当前二维码元数据（用于判断是否需要按新尺寸居中）
 */
void UI_ShowQr(const qr_meta_t *meta);

/**
 * @brief  隐藏二维码图像（例如尚未生成任何二维码时）。画布区域保持纯白。
 */
void UI_HideQr(void);

/**
 * @brief  强制刷新二维码图像（数据在 g_qr_bitmap 中被原地修改后调用）。
 */
void UI_RefreshQr(void);

/**
 * @brief  以醒目颜色显示一条错误状态。
 * @param  text UTF-8 文本
 */
void UI_SetError(const char *text);

/**
 * @brief  在 LVGL flush 阶段用 g_qr_bitmap 强制覆盖二维码区域的像素。
 * @param  area      本次要刷新的闭区间区域（屏幕坐标）
 * @param  px_map    该区域的像素起始地址（RGB565_SWAPPED）
 * @param  stride    行跨度（字节）
 * @return true 表示本次 flush 落在二维码区域内且已被覆盖，false 表示未处理
 *
 * @note   存在原因：LVGL v9.6 在本工程的配置下（RGB565_SWAPPED 目标格式 +
 *         静态 lv_image_dsc_t 的 I1 源）会把整张位图渲染成全黑（源位图被当作
 *         全 0 读取）。真实位图 `g_qr_bitmap` 本身是正确的——BMP 导出即由它
 *         原样流出，扫码可解。为保证"屏幕所见 = 导出所得"，在数据真正送上屏
 *         之前按位重新生成该区域的像素。黑白两色在字节序交换下数值不变
 *         （0x0000 / 0xFFFF），因此这里无需区分 RGB565 与 RGB565_SWAPPED。
 */
bool UI_QrOverlay(lv_area_t *area, uint8_t *px_map, uint32_t stride);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_H */
