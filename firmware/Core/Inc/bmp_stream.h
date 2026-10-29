/**
 * @file    bmp_stream.h
 * @brief   将当前二维码位图当作标准 BMP 文件流式读出（不申请整张图片缓冲）。
 * @details
 *          导出规格（需求 5.8 节，PC 端必须完全一致）：
 *          - 240 x 240、1 bit/pixel、无压缩、top-down（biHeight = -240）；
 *          - `bfOffBits` = 62（14 字节 BITMAPFILEHEADER + 40 字节 BITMAPINFOHEADER
 *            + 8 字节双色调色板）；
 *          - 调色板：索引 0 = 黑 (0,0,0,0)，索引 1 = 白 (255,255,255,0)；
 *          - 像素区 = 240 行 x 32 字节 = 7 680 字节；
 *          - 文件总长 = 62 + 7 680 = 7 742 字节。
 *
 *          实现方式：**不保存整张 BMP**，只保存 62 字节文件头，
 *          像素区直接引用 `g_qr_bitmap`（其位序与 BMP 规范一致，见 qr_bitmap.h）。
 *          `BMP_Read()` 把"文件偏移"映射到"文件头或位图"，从而支持按任意块流式发送。
 */

#ifndef BMP_STREAM_H
#define BMP_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief BMP 画布宽度（像素）。 */
#define BMP_WIDTH           240U
/** @brief BMP 画布高度（像素）。 */
#define BMP_HEIGHT          240U
/** @brief 每行像素字节数（240 bit = 30 字节）。 */
#define BMP_ROW_BYTES       30U
/** @brief 每行跨度（补齐到 4 字节倍数）= 32。 */
#define BMP_ROW_STRIDE      32U
/** @brief 文件头长度 = 14 + 40 + 8。 */
#define BMP_HEADER_SIZE     62U
/** @brief 像素数据长度 = 32 x 240。 */
#define BMP_DATA_SIZE       (BMP_ROW_STRIDE * BMP_HEIGHT)   /* 7680 */
/** @brief 整个 BMP 文件长度 = 7742。 */
#define BMP_FILE_SIZE       (BMP_HEADER_SIZE + BMP_DATA_SIZE)

/** @brief EVT_IMAGE_BEGIN 中的 format 字段取值：BMP 1bpp 无压缩 top-down。 */
#define BMP_FORMAT_CODE     1U

/**
 * @brief 初始化 BMP 流：构造 62 字节文件头并绑定像素数据。
 * @param pixel_data 1bpp 像素数据（240 行 x 32 字节），生命周期必须覆盖整个导出过程
 * @note  传入的通常是 @ref g_qr_bitmap 这个静态数组。
 */
void BMP_Init(const uint8_t *pixel_data);

/**
 * @brief  读取 BMP 文件的任意一段字节。
 * @param  offset 文件内绝对偏移（0 起）
 * @param  dst    输出缓冲区
 * @param  len    期望读取字节数
 * @return 实际拷贝的字节数（到达文件末尾时可能小于 len）
 */
uint32_t BMP_Read(uint32_t offset, uint8_t *dst, uint32_t len);

/**
 * @brief  计算整份 BMP 文件的 CRC-32/ISO-HDLC。
 * @return 32 位校验值
 * @note   仅在开始传输前调用一次，结果随 EVT_IMAGE_END 发送。
 */
uint32_t BMP_ComputeCrc32(void);

/**
 * @brief  读取当前绑定的像素数据指针。
 * @return 像素数据首地址
 */
const uint8_t *BMP_GetPixelData(void);

#ifdef __cplusplus
}
#endif

#endif /* BMP_STREAM_H */
