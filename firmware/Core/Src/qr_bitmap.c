/**
 * @file    qr_bitmap.c
 * @brief   将 QR 模块矩阵渲染为 240x240 / 1bpp / 32 字节行跨度的黑白位图。
 * @details
 *          位图语义（必须与 LVGL v9 I1 和 BMP 1bpp 同时兼容）：
 *          - 每个字节的最高位对应最左边的像素；
 *          - 位 = 1 -> 白色，位 = 0 -> 黑色；
 *          - 每行 32 字节，其中第 30、31 字节是补齐字节（BMP 规范要求补齐，值为 0）。
 *
 *          渲染规则（需求 6.4 节）：
 *          - 整个 240x240 画布先清成纯白；
 *          - 模块边长 scale = min(6, floor(240 / (modules + 8)))，整数倍，不插值；
 *          - 二维码块（矩阵 + 四周各 4 模块静区）在画布中居中；
 *          - 静区以外区域保持纯白，禁止任何花纹或文字覆盖。
 */

#include "qr_bitmap.h"
#include <string.h>

/** @brief 全局唯一的二维码位图（7680 字节，静态分配于普通 SRAM）。 */
uint8_t g_qr_bitmap[QR_BITMAP_SIZE];

/**
 * @brief  把某个像素写成黑色（位清零）。
 * @param  x 列坐标
 * @param  y 行坐标
 */
static inline void bitmap_set_black(uint16_t x, uint16_t y)
{
    uint32_t idx = ((uint32_t)y * QR_BITMAP_STRIDE) + ((uint32_t)x >> 3);
    uint8_t  mask = (uint8_t)(0x80U >> (x & 0x07U));
    g_qr_bitmap[idx] &= (uint8_t)(~mask);
}

/**
 * @brief  把整张位图清为纯白（0xFF）。
 * @note   I1 语义为"位=1 白、位=0 黑"，因此 0xFF 即全白。
 */
void QR_BitmapClear(void)
{
    (void)memset(g_qr_bitmap, 0xFF, sizeof(g_qr_bitmap));
}

/**
 * @brief  按元数据把 QR 模块矩阵渲染成 240x240 1bpp 位图。
 * @param  meta 当前有效二维码的元数据（QR_ServiceEncode 填充）
 * @note   内部先清屏再画：整数倍缩放（最多 6 像素/模块）、居中、四周保留 4 模块静区；
 * **同一份位图同时供 LCD 显示与 BMP 导出**，保证"屏幕所见 = 导出所得"。
 */
void QR_BitmapRender(const qr_meta_t *meta)
{
    uint8_t  modules;
    uint8_t  scale;
    uint16_t origin_x;
    uint16_t origin_y;
    uint8_t  mx;
    uint8_t  my;
    uint8_t  dx;
    uint8_t  dy;

    QR_BitmapClear();

    if ((meta == NULL) || (!meta->valid)) {
        return;
    }

    modules  = meta->modules;
    scale    = meta->scale;
    origin_x = meta->origin_x;
    origin_y = meta->origin_y;

    if ((modules == 0U) || (scale == 0U)) {
        return;
    }

    for (my = 0U; my < modules; my++) {
        for (mx = 0U; mx < modules; mx++) {
            uint16_t px;
            uint16_t py;

            if (!QR_ServiceGetModule(mx, my)) {
                continue;   /* 浅色模块：保持白色 */
            }

            px = (uint16_t)(origin_x + ((uint16_t)(mx + QR_QUIET_MODULES) * scale));
            py = (uint16_t)(origin_y + ((uint16_t)(my + QR_QUIET_MODULES) * scale));

            for (dy = 0U; dy < scale; dy++) {
                for (dx = 0U; dx < scale; dx++) {
                    bitmap_set_black((uint16_t)(px + dx), (uint16_t)(py + dy));
                }
            }
        }
    }
}

/**
 * @brief  读取位图中某像素的位值（1=白，0=黑）。
 * @param  x 列坐标 0..239
 * @param  y 行坐标 0..239
 * @return 1 或 0；越界返回 0
 * @note   仅供测试与自检使用，绘制路径不经过它（避免逐像素函数调用开销）。
 */
uint8_t QR_BitmapGetPixel(uint16_t x, uint16_t y)
{
    uint32_t idx;
    uint8_t  bit;

    if ((x >= QR_BITMAP_WIDTH) || (y >= QR_BITMAP_HEIGHT)) {
        return 0U;
    }

    idx = ((uint32_t)y * QR_BITMAP_STRIDE) + ((uint32_t)x >> 3);
    bit = (uint8_t)((g_qr_bitmap[idx] >> (7U - (x & 0x07U))) & 0x01U);
    return bit;
}
