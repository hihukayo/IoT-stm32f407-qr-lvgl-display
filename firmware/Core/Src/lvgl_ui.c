/**
 * @file    lvgl_ui.c
 * @brief   LVGL 界面实现：顶部标题栏、中部 240x240 二维码图像、底部状态栏。
 * @details
 *          二维码使用**单个** `lv_image` 控件显示 240x240 的 I1 位图，
 *          不为每个黑色模块创建对象（否则高版本二维码会产生数千个控件）。
 *
 *          @note 界面文本使用 ASCII 英文：LVGL 点阵字体只包含 Latin 字符集，
 *                为避免引入体积巨大的 CJK 字库（会浪费上百 KiB Flash），
 *                屏幕状态提示使用英文；PC 上位机界面为中文。
 */

#include "lvgl_ui.h"
#include "qr_bitmap.h"
#include <string.h>

/* ======================================================================
 * 颜色
 * ==================================================================== */
#define UI_COLOR_HEADER     0x1B2A41U   /**< 顶部标题栏深蓝 */
#define UI_COLOR_FOOTER     0xEDEFF2U   /**< 底部状态栏浅灰 */
#define UI_COLOR_TEXT_W     0xFFFFFFU   /**< 标题栏文字（白）*/
#define UI_COLOR_TEXT_D     0x101418U   /**< 状态栏文字（近黑）*/
#define UI_COLOR_ACCENT     0x0F9D58U   /**< 正常状态绿 */
#define UI_COLOR_ERROR      0xD93025U   /**< 错误状态红 */

/* ======================================================================
 * 对象与图像描述
 * ==================================================================== */
static lv_obj_t *s_header = NULL;   /**< 顶部色块 */
static lv_obj_t *s_footer = NULL;   /**< 底部色块 */
static lv_obj_t *s_title  = NULL;   /**< 标题文本 */
static lv_obj_t *s_info   = NULL;   /**< 右上角信息（版本/ECC/模块数）*/
static lv_obj_t *s_status = NULL;   /**< 底部状态文本 */
static lv_obj_t *s_qr     = NULL;   /**< 二维码图像控件 */
static bool      s_qr_shown = false;/**< 二维码是否应显示（供 flush 覆盖判断）*/

/** @brief 二维码图像在本屏中的左上角 y 坐标（标题栏高度）。 */
#define UI_QR_Y   40

/**
 * @brief 二维码图像描述符：直接引用 `g_qr_bitmap`，不复制、不申请内存。
 * @note  I1 的位语义：位 = 1 -> 白，位 = 0 -> 黑；MSB 为最左像素。
 *        该语义与 BMP 1bpp 完全一致，故同一缓冲区同时用于显示与导出。
 */
static lv_image_dsc_t s_qr_dsc = {
    .header.magic  = LV_IMAGE_HEADER_MAGIC,
    .header.cf     = LV_COLOR_FORMAT_I1,
    .header.flags  = 0,
    .header.w      = QR_BITMAP_WIDTH,
    .header.h      = QR_BITMAP_HEIGHT,
    .header.stride = QR_BITMAP_STRIDE,
    .data_size     = QR_BITMAP_SIZE,
    .data          = g_qr_bitmap,
    .reserved      = NULL,
    .reserved_2    = NULL,
};

/* ======================================================================
 * 初始化
 * ==================================================================== */
void UI_Init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* --- 屏幕底色：纯白 --- */
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- 顶部标题栏：240 x 40 --- */
    s_header = lv_obj_create(scr);
    lv_obj_remove_style_all(s_header);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_size(s_header, 240, 40);
    lv_obj_set_style_bg_color(s_header, lv_color_hex(UI_COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, LV_PART_MAIN);

    s_title = lv_label_create(s_header);
    lv_label_set_text(s_title, "QR Generator");
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COLOR_TEXT_W), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 8, 0);

    s_info = lv_label_create(s_header);
    lv_label_set_text(s_info, "INIT");
    lv_obj_set_style_text_color(s_info, lv_color_hex(0xB9C4D2U), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_info, LV_ALIGN_RIGHT_MID, -8, 0);

    /* --- 中部二维码图像：240 x 240，y = 40 --- */
    s_qr = lv_image_create(scr);
    lv_image_set_src(s_qr, &s_qr_dsc);
    lv_obj_set_pos(s_qr, 0, 40);
    lv_obj_set_size(s_qr, QR_BITMAP_WIDTH, QR_BITMAP_HEIGHT);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    /* 图像与背景之间不要任何边框/内边距，保证静区纯白 */
    lv_obj_set_style_pad_all(s_qr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_qr, 0, LV_PART_MAIN);

    /* --- 底部状态栏：240 x 40，y = 280 --- */
    s_footer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_footer);
    lv_obj_set_pos(s_footer, 0, 280);
    lv_obj_set_size(s_footer, 240, 40);
    lv_obj_set_style_bg_color(s_footer, lv_color_hex(UI_COLOR_FOOTER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_COVER, LV_PART_MAIN);

    s_status = lv_label_create(s_footer);
    lv_label_set_text(s_status, "Waiting for data...");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_status, 224);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_TEXT_D), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 0);
}

/* ======================================================================
 * 文本更新
 * ==================================================================== */
void UI_SetStatus(const char *text)
{
    if ((s_status == NULL) || (text == NULL)) {
        return;
    }
    /* 仅在内容变化时重绘，避免无谓刷新 */
    if (strcmp(lv_label_get_text(s_status), text) != 0) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_TEXT_D), LV_PART_MAIN);
    }
}

void UI_SetError(const char *text)
{
    if ((s_status == NULL) || (text == NULL)) {
        return;
    }
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_ERROR), LV_PART_MAIN);
}

void UI_SetInfo(const char *text)
{
    if ((s_info == NULL) || (text == NULL)) {
        return;
    }
    if (strcmp(lv_label_get_text(s_info), text) != 0) {
        lv_label_set_text(s_info, text);
    }
}

/* ======================================================================
 * 二维码显示
 * ==================================================================== */
void UI_ShowQr(const qr_meta_t *meta)
{
    (void)meta; /* 图像尺寸固定 240x240，无需按元数据调整 */

    if (s_qr == NULL) {
        return;
    }

    /* 重新设置源，确保解码/缓存状态刷新后再重绘 */
    lv_image_set_src(s_qr, &s_qr_dsc);
    s_qr_shown = true;
    lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_qr);
}

void UI_HideQr(void)
{
    if (s_qr == NULL) {
        return;
    }
    s_qr_shown = false;
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_qr);
}

void UI_RefreshQr(void)
{
    if (s_qr == NULL) {
        return;
    }
    lv_image_set_src(s_qr, &s_qr_dsc);
    s_qr_shown = true;
    lv_obj_invalidate(s_qr);
}

/* ======================================================================
 * flush 阶段的位图覆盖（保证屏幕所见 = 导出所得）
 * ====================================================================== */
bool UI_QrOverlay(lv_area_t *area, uint8_t *px_map, uint32_t stride)
{
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    int32_t x;
    int32_t y;

    if ((!s_qr_shown) || (area == NULL) || (px_map == NULL)) {
        return false;
    }

    /* 与本屏二维码矩形求交集：x 0..239，y 40..279 */
    x1 = (area->x1 > 0) ? area->x1 : 0;
    y1 = (area->y1 > UI_QR_Y) ? area->y1 : UI_QR_Y;
    x2 = (area->x2 < (int32_t)(QR_BITMAP_WIDTH - 1U)) ? area->x2 : (int32_t)(QR_BITMAP_WIDTH - 1U);
    y2 = (area->y2 < (UI_QR_Y + (int32_t)(QR_BITMAP_HEIGHT - 1U))) ?
         area->y2 : (UI_QR_Y + (int32_t)(QR_BITMAP_HEIGHT - 1U));

    if ((x1 > x2) || (y1 > y2)) {
        return false;
    }

    for (y = y1; y <= y2; y++) {
        const uint8_t *src_row = &g_qr_bitmap[(uint32_t)(y - UI_QR_Y) * QR_BITMAP_STRIDE];
        uint8_t *dst = px_map
                     + ((uint32_t)(y - area->y1) * stride)
                     + ((uint32_t)(x1 - area->x1) * 2U);

        for (x = x1; x <= x2; x++) {
            uint8_t bit = (uint8_t)((src_row[(uint32_t)x >> 3] >> (7U - ((uint32_t)x & 7U))) & 1U);
            /* 黑 0x0000 / 白 0xFFFF：字节序两种排法数值相同，故无需交换 */
            uint16_t color = (bit != 0U) ? 0xFFFFU : 0x0000U;
            dst[0] = (uint8_t)(color & 0xFFU);
            dst[1] = (uint8_t)(color >> 8);
            dst += 2;
        }
    }

    return true;
}
