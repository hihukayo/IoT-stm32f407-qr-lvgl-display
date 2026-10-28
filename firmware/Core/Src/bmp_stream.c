/**
 * @file    bmp_stream.c
 * @brief   BMP 1bpp（top-down）文件头构造与流式读取实现。
 * @details
 *          不申请整张图片缓冲：只保存 62 字节文件头，像素区直接引用二维码位图
 *          `g_qr_bitmap`。`BMP_Read()` 把"文件绝对偏移"映射为
 *          "文件头内的偏移"或"位图内的偏移"，因此上位机可以按任意块大小拉取。
 *
 *          BMP 字节布局（小端）：
 *          @verbatim
 *          偏移  长度  内容
 *          0     2     'B','M'
 *          2     4     文件大小 = 7742
 *          6     4     保留 = 0
 *          10    4     bfOffBits = 62
 *          14    4     biSize = 40
 *          18    4     biWidth = 240
 *          22    4     biHeight = -240（top-down，像素行自上而下）
 *          26    2     biPlanes = 1
 *          28    2     biBitCount = 1
 *          30    4     biCompression = 0 (BI_RGB)
 *          34    4     biSizeImage = 7680
 *          38    4     biXPelsPerMeter = 2835
 *          42    4     biYPelsPerMeter = 2835
 *          46    4     biClrUsed = 2
 *          50    4     biClrImportant = 0
 *          54    4     调色板[0] = 黑 (0,0,0,0)
 *          58    4     调色板[1] = 白 (255,255,255,0)
 *          62    7680  像素数据（240 行 x 32 字节）
 *          @endverbatim
 */

#include "bmp_stream.h"
#include "uart_protocol.h"
#include <string.h>

/** @brief 62 字节文件头（含 8 字节调色板）。 */
static uint8_t s_header[BMP_HEADER_SIZE];

/** @brief 像素数据指针（指向 240x240 的 1bpp 位图）。 */
static const uint8_t *s_pixels = NULL;

/**
 * @brief  以小端格式写入 16 位整数。
 * @param  p 目标地址
 * @param  v 数值
 */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

/**
 * @brief  以小端格式写入 32 位整数。
 * @param  p 目标地址
 * @param  v 数值
 */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

void BMP_Init(const uint8_t *pixel_data)
{
    s_pixels = pixel_data;

    (void)memset(s_header, 0, sizeof(s_header));

    /* --- BITMAPFILEHEADER（14 字节） --- */
    s_header[0] = (uint8_t)'B';
    s_header[1] = (uint8_t)'M';
    put_u32(&s_header[2], (uint32_t)BMP_FILE_SIZE);     /* bfSize            */
    put_u16(&s_header[6], 0U);                          /* bfReserved1       */
    put_u16(&s_header[8], 0U);                          /* bfReserved2       */
    put_u32(&s_header[10], (uint32_t)BMP_HEADER_SIZE);  /* bfOffBits = 62    */

    /* --- BITMAPINFOHEADER（40 字节，从偏移 14 开始） --- */
    put_u32(&s_header[14], 40U);                        /* biSize            */
    put_u32(&s_header[18], (uint32_t)BMP_WIDTH);        /* biWidth = 240     */
    /* biHeight：top-down 使用负值（-240 的补码表示） */
    put_u32(&s_header[22], (uint32_t)(-(int32_t)BMP_HEIGHT));
    put_u16(&s_header[26], 1U);                         /* biPlanes          */
    put_u16(&s_header[28], 1U);                         /* biBitCount = 1    */
    put_u32(&s_header[30], 0U);                         /* biCompression=RGB */
    put_u32(&s_header[34], (uint32_t)BMP_DATA_SIZE);    /* biSizeImage=7680  */
    put_u32(&s_header[38], 2835U);                      /* 72 DPI            */
    put_u32(&s_header[42], 2835U);                      /* 72 DPI            */
    put_u32(&s_header[46], 2U);                         /* biClrUsed = 2     */
    put_u32(&s_header[50], 0U);                         /* biClrImportant    */

    /* --- 调色板（8 字节，从偏移 54 开始） --- */
    s_header[54] = 0x00U;   /* 索引 0：黑 B */
    s_header[55] = 0x00U;   /*            G */
    s_header[56] = 0x00U;   /*            R */
    s_header[57] = 0x00U;   /*            A */
    s_header[58] = 0xFFU;   /* 索引 1：白 B */
    s_header[59] = 0xFFU;   /*            G */
    s_header[60] = 0xFFU;   /*            R */
    s_header[61] = 0x00U;   /*            A */
}

uint32_t BMP_Read(uint32_t offset, uint8_t *dst, uint32_t len)
{
    uint32_t copied = 0U;

    if ((dst == NULL) || (len == 0U) || (s_pixels == NULL)) {
        return 0U;
    }
    if (offset >= (uint32_t)BMP_FILE_SIZE) {
        return 0U;
    }
    if (len > ((uint32_t)BMP_FILE_SIZE - offset)) {
        len = (uint32_t)BMP_FILE_SIZE - offset;
    }

    /* 先取文件头部分 */
    if (offset < (uint32_t)BMP_HEADER_SIZE) {
        uint32_t n = (uint32_t)BMP_HEADER_SIZE - offset;
        if (n > len) {
            n = len;
        }
        (void)memcpy(dst, &s_header[offset], n);
        dst      += n;
        offset   += n;
        len      -= n;
        copied   += n;
    }

    /* 再取像素数据部分 */
    if (len != 0U) {
        (void)memcpy(dst, s_pixels + (offset - (uint32_t)BMP_HEADER_SIZE), len);
        copied += len;
    }

    return copied;
}

uint32_t BMP_ComputeCrc32(void)
{
    uint32_t crc;

    if (s_pixels == NULL) {
        return 0U;
    }

    crc = proto_crc32_start();
    crc = proto_crc32_update(crc, s_header, (uint32_t)BMP_HEADER_SIZE);
    crc = proto_crc32_update(crc, s_pixels, (uint32_t)BMP_DATA_SIZE);

    return proto_crc32_finish(crc);
}

const uint8_t *BMP_GetPixelData(void)
{
    return s_pixels;
}
