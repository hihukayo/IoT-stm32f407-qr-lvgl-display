/**
 * @file    qr_bitmap.h
 * @brief   把 QR 模块矩阵渲染成 240x240、1bpp、32 字节/行的黑白位图。
 * @details
 *          该位图**同时**服务于两个消费者，因此两者的字节语义必须完全一致：
 *
 *          1. LVGL v9.6 的 I1 图像格式：LVGL 的软件渲染器读取 I1 位图时使用
 *             `(buf[idx/8] >> (7 - idx%8)) & 1`，位 = 1 表示白色（亮度 255），
 *             位 = 0 表示黑色；行跨度由 image header 指定。
 *          2. BMP 1bpp 像素数据：每行按最高位对应最左像素，位 = 1 表示调色板
 *             索引 1（白色），位 = 0 表示索引 0（黑色），每行补齐到 4 字节倍数
 *             （240 像素 -> 30 字节 -> 补齐 32 字节）。
 *
 *          两者的位序与颜色语义**天然一致**，所以只保存一份位图即可：
 *          - LCD 显示：把 `g_qr_bitmap` 交给 lv_image 控件；
 *          - BMP 导出：把 62 字节文件头 + `g_qr_bitmap` 原样流出。
 *
 *          内存占用：240 x 32 = 7 680 字节（静态 .bss），远小于
 *          240x240 RGB565 画布所需的 115 200 字节。
 */

#ifndef QR_BITMAP_H
#define QR_BITMAP_H

#include <stdint.h>
#include "qr_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 位图宽度（像素）。 */
#define QR_BITMAP_WIDTH      240U
/** @brief 位图高度（像素）。 */
#define QR_BITMAP_HEIGHT     240U
/** @brief 行跨度（字节）：240 位 = 30 字节，按 BMP 规则补齐到 4 的倍数 = 32。 */
#define QR_BITMAP_STRIDE     32U
/** @brief 位图总字节数。 */
#define QR_BITMAP_SIZE       (QR_BITMAP_STRIDE * QR_BITMAP_HEIGHT)   /* 7680 */

/**
 * @brief 二维码位图缓冲区（全局唯一一份）。
 * @note  由 qr_bitmap.c 以静态数组形式定义，位于可在 DMA 之外访问的普通 SRAM 中。
 */
extern uint8_t g_qr_bitmap[QR_BITMAP_SIZE];

/** @brief 把整张位图清为纯白（0xFF）。 */
void QR_BitmapClear(void);

/**
 * @brief  按元数据把 QR 模块矩阵渲染到位图（白底 + 四模块静区 + 整数倍缩放 + 居中）。
 * @param  meta 当前有效二维码的元数据（由 QR_ServiceEncode 填充）
 * @note   必须先调用 QR_BitmapClear()（本函数内部会先清屏，因此外部无需重复调用）。
 *         静区位于 (module_count + 8) 的区域之外，保证纯白。
 */
void QR_BitmapRender(const qr_meta_t *meta);

/**
 * @brief  取出位图中某个像素的位值（1 = 白，0 = 黑），仅用于自检。
 * @param  x 列坐标 0..239
 * @param  y 行坐标 0..239
 * @return 1 或 0
 */
uint8_t QR_BitmapGetPixel(uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif

#endif /* QR_BITMAP_H */
