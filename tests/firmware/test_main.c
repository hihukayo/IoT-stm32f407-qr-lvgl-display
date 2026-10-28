/**
 * @file    test_main.c
 * @brief   固件纯逻辑模块的主机端单元测试（gcc + 桩 HAL，无需硬件）。
 *
 * @details
 *          被测模块（原样编译，不改一行代码）：
 *          - `Core/Src/uart_protocol.c`  帧协议、CRC16/CRC32、解析状态机
 *          - `Core/Src/qr_service.c`     qrcodegen 封装
 *          - `Core/Src/qr_bitmap.c`      1bpp 位图渲染（静区/整数缩放/居中）
 *          - `Core/Src/bmp_stream.c`     BMP 1bpp 文件头与流式读取
 *          - `Middlewares/qrcodegen/qrcodegen.c`
 *
 *          覆盖的验收点：
 *          - T-001 CRC16/MODBUS 测试向量 0x4B37
 *          - T-002 payload 内含 A5 5A 仍能正确解析
 *          - T-003 坏 CRC / LEN=513 / 错 VER / 截断帧 / 粘连帧
 *          - T-004 BMP 头字段与文件大小（7742 / 240x240 / 1bpp / 62 / 7680）
 *          - 二维码模块数、版本、缩放、静区和居中
 *
 * @note    运行方式见同目录 run_tests.ps1。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32f4xx_hal.h"   /* 桩：仅提供句柄类型与时基/发送声明 */
#include "uart_protocol.h"
#include "qr_service.h"
#include "qr_bitmap.h"
#include "bmp_stream.h"

/* ======================================================================
 * HAL 桩实现
 * ==================================================================== */
UART_HandleTypeDef huart1;

static uint32_t s_fake_tick = 1000U;

uint32_t HAL_GetTick(void)
{
    return s_fake_tick;
}

/** @brief 捕获最近一次 HAL_UART_Transmit 发出的字节。 */
static uint8_t  s_uart_tx[2048];
static uint16_t s_uart_tx_len;

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    (void)huart;
    (void)Timeout;
    if (Size > (uint16_t)sizeof(s_uart_tx)) {
        return HAL_ERROR;
    }
    (void)memcpy(s_uart_tx, pData, Size);
    s_uart_tx_len = Size;
    return HAL_OK;
}

/* ======================================================================
 * 测试框架
 * ==================================================================== */
static int g_pass;
static int g_fail;

/**
 * @brief  断言宏：失败时打印文件/行号但不中断，便于一次跑完全部用例。
 */
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            printf("  [FAIL] %-52s (%s:%d)\n", msg, __FILE__, __LINE__);       \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                   \
    do {                                                                      \
        unsigned long _a = (unsigned long)(a);                                 \
        unsigned long _b = (unsigned long)(b);                                 \
        if (_a == _b) {                                                       \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            printf("  [FAIL] %-40s got=0x%lX want=0x%lX (%s:%d)\n",           \
                   msg, _a, _b, __FILE__, __LINE__);                          \
        }                                                                     \
    } while (0)

/* ======================================================================
 * 工具
 * ==================================================================== */
/** @brief 向解析器喂入一整段字节，返回解析成功的帧数。 */
static int feed_bytes(const uint8_t *data, uint32_t len, proto_frame_t *last, int *parsed)
{
    uint32_t i;
    int count = 0;

    for (i = 0U; i < len; i++) {
        if (proto_parser_feed(data[i], last)) {
            if (parsed != NULL) {
                parsed[count] = 1;
            }
            count++;
        }
    }
    return count;
}

/** @brief 生成一个合法帧到 out，返回整帧长度。 */
static uint16_t make_frame(uint8_t cmd, uint8_t seq, const uint8_t *pl, uint16_t len, uint8_t *out)
{
    uint16_t crc;

    out[0] = 0xA5U;
    out[1] = 0x5AU;
    out[2] = 0x01U;
    out[3] = cmd;
    out[4] = seq;
    out[5] = (uint8_t)(len & 0xFFU);
    out[6] = (uint8_t)(len >> 8);
    if (len != 0U) {
        (void)memcpy(&out[7], pl, len);
    }
    crc = proto_crc16(&out[2], (uint32_t)(5U + len));
    out[7 + len]      = (uint8_t)(crc & 0xFFU);
    out[8 + len]      = (uint8_t)(crc >> 8);
    return (uint16_t)(9U + len);
}

/* ======================================================================
 * 用例：CRC
 * ==================================================================== */
static void test_crc(void)
{
    const uint8_t vec[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    printf("[1] CRC 测试向量\n");
    /* T-001：ASCII "123456789" -> 0x4B37（低字节在前：37 4B） */
    CHECK_EQ(proto_crc16(vec, sizeof(vec)), 0x4B37U, "CRC16/MODBUS 测试向量");
    /* CRC-32/ISO-HDLC 标准向量 */
    CHECK_EQ(proto_crc32(vec, sizeof(vec)), 0xCBF43926U, "CRC32/ISO-HDLC 测试向量");
    /* 分段 CRC32 必须与一次性计算一致 */
    {
        uint32_t c = proto_crc32_start();
        c = proto_crc32_update(c, vec, 4U);
        c = proto_crc32_update(c, vec + 4, 5U);
        CHECK_EQ(proto_crc32_finish(c), 0xCBF43926U, "分段 CRC32 一致性");
    }
}

/* ======================================================================
 * 用例：帧打包
 * ==================================================================== */
static void test_frame_build(void)
{
    uint8_t pl[4] = { 0x11U, 0x22U, 0x33U, 0x44U };

    printf("[2] 帧发送格式\n");
    CHECK(proto_send_frame(PROTO_CMD_PING, 0x07U, pl, 4U), "proto_send_frame 返回成功");
    CHECK_EQ(s_uart_tx_len, 13U, "整帧长度 = 9 + LEN");
    CHECK_EQ(s_uart_tx[0], 0xA5U, "SOF0");
    CHECK_EQ(s_uart_tx[1], 0x5AU, "SOF1");
    CHECK_EQ(s_uart_tx[2], 0x01U, "VER");
    CHECK_EQ(s_uart_tx[3], 0x01U, "CMD");
    CHECK_EQ(s_uart_tx[4], 0x07U, "SEQ");
    CHECK_EQ(s_uart_tx[5], 0x04U, "LEN_L");
    CHECK_EQ(s_uart_tx[6], 0x00U, "LEN_H");
    CHECK_EQ(s_uart_tx[7], 0x11U, "PAYLOAD[0]");
    CHECK_EQ(s_uart_tx[10], 0x44U, "PAYLOAD[3]");
    /* CRC 覆盖 VER..PAYLOAD，低字节在前 */
    {
        uint16_t crc = proto_crc16(&s_uart_tx[2], 9U);
        CHECK_EQ(s_uart_tx[11], (uint8_t)(crc & 0xFFU), "CRC 低字节在前");
        CHECK_EQ(s_uart_tx[12], (uint8_t)(crc >> 8), "CRC 高字节在后");
    }
}

/* ======================================================================
 * 用例：帧解析
 * ==================================================================== */
static void test_parser(void)
{
    uint8_t frame[600];
    uint16_t n;
    proto_frame_t got;
    uint8_t tricky[6] = { 0xA5U, 0x5AU, 0x00U, 0xA5U, 0x5AU, 0xFFU };

    printf("[3] 帧解析\n");

    /* 3.1 基本往返 */
    proto_parser_reset();
    n = make_frame(PROTO_CMD_SET_QR, 0x42U, tricky, 6U, frame);
    CHECK_EQ(feed_bytes(frame, n, &got, NULL), 1, "合法帧解析出 1 帧");
    CHECK_EQ(got.cmd, PROTO_CMD_SET_QR, "解析出的 CMD");
    CHECK_EQ(got.seq, 0x42U, "解析出的 SEQ");
    CHECK_EQ(got.len, 6U, "解析出的 LEN");
    CHECK(memcmp(got.payload, tricky, 6U) == 0, "payload 含 A5 5A 原样保留 (T-002)");

    /* 3.2 两帧粘连 */
    proto_parser_reset();
    {
        uint16_t n1 = make_frame(PROTO_CMD_PING, 1U, NULL, 0U, frame);
        uint16_t n2 = make_frame(PROTO_CMD_GET_STATUS, 2U, NULL, 0U, frame + n1);
        CHECK_EQ(feed_bytes(frame, (uint32_t)n1 + n2, &got, NULL), 2, "两帧粘连均解析成功");
    }

    /* 3.3 坏 CRC 被丢弃 */
    proto_parser_reset();
    n = make_frame(PROTO_CMD_PING, 3U, NULL, 0U, frame);
    frame[n - 1U] ^= 0xFFU;
    CHECK_EQ(feed_bytes(frame, n, &got, NULL), 0, "坏 CRC 帧被丢弃");
    {
        uint32_t crc_err = 0U;
        proto_parser_stats(NULL, &crc_err, NULL);
        CHECK_EQ(crc_err, 1U, "CRC 错误计数 +1");
    }

    /* 3.4 LEN=513 被拒绝 */
    proto_parser_reset();
    {
        uint8_t bad[9] = { 0xA5U, 0x5AU, 0x01U, 0x01U, 0x00U, 0x01U, 0x02U, 0x00U, 0x00U };
        CHECK_EQ(feed_bytes(bad, 9U, &got, NULL), 0, "LEN=513 被拒绝");
    }

    /* 3.5 VER 错误被拒绝 */
    proto_parser_reset();
    {
        uint8_t bad[9] = { 0xA5U, 0x5AU, 0x02U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
        CHECK_EQ(feed_bytes(bad, 9U, &got, NULL), 0, "未知 VER 被拒绝");
    }

    /* 3.6 截断帧 + 1 秒超时后重新同步 */
    proto_parser_reset();
    n = make_frame(PROTO_CMD_PING, 5U, NULL, 0U, frame);
    CHECK_EQ(feed_bytes(frame, (uint32_t)n - 3U, &got, NULL), 0, "截断帧不产生完整帧");
    s_fake_tick += 1001U;
    proto_parser_poll(s_fake_tick);
    n = make_frame(PROTO_CMD_PING, 6U, NULL, 0U, frame);
    CHECK_EQ(feed_bytes(frame, n, &got, NULL), 1, "超时清空后可解析后续帧 (T-003)");
    CHECK_EQ(got.seq, 6U, "重同步后 SEQ 正确");

    /* 3.7 帧中途的 0xA5 0xA5 5A 不会误判 */
    proto_parser_reset();
    {
        uint8_t pre[3] = { 0x00U, 0xFFU, 0xA5U };
        n = make_frame(PROTO_CMD_PING, 9U, NULL, 0U, frame);
        CHECK_EQ(feed_bytes(pre, 3U, &got, NULL), 0, "垃圾字节不产生帧");
        CHECK_EQ(feed_bytes(frame, n, &got, NULL), 1, "垃圾字节后仍可重新同步 (T-003)");
        CHECK_EQ(got.seq, 9U, "重新同步后 SEQ 正确");
    }

    /* 3.8 空 payload 合法 */
    proto_parser_reset();
    n = make_frame(PROTO_CMD_GET_STATUS, 0xAAU, NULL, 0U, frame);
    CHECK_EQ(feed_bytes(frame, n, &got, NULL), 1, "LEN=0 帧合法");
    CHECK_EQ(got.len, 0U, "LEN=0");
}

/* ======================================================================
 * 用例：ACK 结构
 * ==================================================================== */
static void test_ack(void)
{
    printf("[4] ACK 结构与命令映射\n");
    CHECK(proto_send_ack(PROTO_CMD_SET_QR, 0x10U, PROTO_STATUS_OK, PROTO_ERR_NONE, 3U, 29U),
          "发送 ACK");
    CHECK_EQ(s_uart_tx[3], (uint8_t)(PROTO_CMD_SET_QR | 0x80U), "ACK CMD = CMD | 0x80");
    CHECK_EQ(s_uart_tx[5], 4U, "ACK payload = 4 字节");
    CHECK_EQ(s_uart_tx[7], PROTO_STATUS_OK, "status");
    CHECK_EQ(s_uart_tx[8], PROTO_ERR_NONE, "error_code");
    CHECK_EQ(s_uart_tx[9], 3U, "qr_version");
    CHECK_EQ(s_uart_tx[10], 29U, "module_count");
}

/* ======================================================================
 * 用例：二维码编码
 * ==================================================================== */
static void test_qr(void)
{
    const uint8_t text[] = "Hello 123";
    qr_meta_t meta;
    qr_result_t r;
    uint8_t ecc;

    printf("[5] 二维码编码\n");

    QR_ServiceInit();
    CHECK(!QR_ServiceIsValid(), "初始状态无有效二维码");
    CHECK(!QR_ServiceGetModule(0U, 0U), "无二维码时模块查询返回 false");

    r = QR_ServiceEncode(text, (uint16_t)(sizeof(text) - 1U), QR_SYM_QRCODE, QR_ECC_M, &meta);
    CHECK_EQ(r, QR_OK, "\"Hello 123\" 编码成功");
    CHECK(QR_ServiceIsValid(), "编码后状态有效");
    CHECK_EQ(meta.version, (uint8_t)((meta.modules - 17U) / 4U), "版本与模块数自洽");
    CHECK_EQ(meta.modules, (uint8_t)(17U + 4U * meta.version), "modules = 17 + 4*version");
    CHECK((meta.version >= 1U) && (meta.version <= 20U), "版本在 1..20 范围");
    CHECK_EQ(meta.text_len, 9U, "text_len");
    {
        uint32_t span = (uint32_t)meta.modules + 8U;
        uint32_t scale = 240U / span;
        if (scale > 6U) {
            scale = 6U;
        }
        CHECK_EQ(meta.scale, scale, "scale = min(6, 240/(modules+8))");
        CHECK_EQ(meta.origin_x, (uint16_t)((240U - span * scale) / 2U), "水平居中");
        CHECK_EQ(meta.origin_y, meta.origin_x, "垂直居中（正方形画布）");
        CHECK(((uint32_t)meta.origin_x + span * scale) <= 240U, "整块不超出画布");
    }

    /* 四种纠错等级都能编码，且版本/模块自洽 */
    for (ecc = 0U; ecc < 4U; ecc++) {
        r = QR_ServiceEncode(text, 9U, QR_SYM_QRCODE, (qr_ecc_t)ecc, &meta);
        CHECK_EQ(r, QR_OK, "ECC L/M/Q/H 均可编码");
        CHECK_EQ(meta.ecc, ecc, "元数据记录请求的 ECC");
        CHECK_EQ(QR_ServiceGetVersion(), meta.version, "版本接口与元数据一致");
        CHECK_EQ(QR_ServiceGetModuleCount(), meta.modules, "模块数接口与元数据一致");
    }

    /* 错误分支 */
    CHECK_EQ(QR_ServiceEncode(NULL, 5U, QR_SYM_QRCODE, QR_ECC_M, &meta), QR_ERR_PARAM, "空指针 -> PARAM");
    CHECK_EQ(QR_ServiceEncode(text, 0U, QR_SYM_QRCODE, QR_ECC_M, &meta), QR_ERR_PARAM, "空文本 -> PARAM");
    CHECK_EQ(QR_ServiceEncode(text, 9U, QR_SYM_QRCODE, (qr_ecc_t)4, &meta), QR_ERR_PARAM, "ECC=4 -> PARAM");
    CHECK_EQ(QR_ServiceEncode(text, 9U, QR_SYM_DATAMATRIX, QR_ECC_M, &meta), QR_ERR_UNSUPPORTED, "DataMatrix -> UNSUPPORTED");
    CHECK_EQ(QR_ServiceEncode(text, 9U, (qr_symbology_t)9, QR_ECC_M, &meta), QR_ERR_PARAM, "非法码制 -> PARAM");

    {
        uint8_t nul[3] = { 'a', 0x00U, 'b' };
        CHECK_EQ(QR_ServiceEncode(nul, 3U, QR_SYM_QRCODE, QR_ECC_M, &meta), QR_ERR_PARAM, "含 NUL -> PARAM");
    }
    {
        uint8_t big[257];
        (void)memset(big, 'a', sizeof(big));
        CHECK_EQ(QR_ServiceEncode(big, 257U, QR_SYM_QRCODE, QR_ECC_M, &meta), QR_ERR_TOO_LONG, "257 字节 -> TOO_LONG");
    }
    /* 失败后仍保留上一张有效二维码（FR-007） */
    CHECK(QR_ServiceIsValid(), "编码失败后仍保留旧二维码");
}

/* ======================================================================
 * 用例：1bpp 位图
 * ==================================================================== */
static void test_bitmap(void)
{
    const uint8_t text[] = "Hello 123";
    qr_meta_t meta;
    uint32_t scale;
    uint32_t span;
    uint32_t x;
    uint32_t y;
    int quiet_ok = 1;

    printf("[6] 二维码位图（静区/缩放/居中等）\n");

    CHECK_EQ(QR_ServiceEncode(text, 9U, QR_SYM_QRCODE, QR_ECC_M, &meta), QR_OK, "重新编码");
    CHECK_EQ(sizeof(g_qr_bitmap), 7680U, "位图 = 240 行 x 32 字节");

    QR_BitmapRender(&meta);
    scale = meta.scale;
    span  = (uint32_t)meta.modules + 8U;

    /* 6.1 矩阵以外必须是纯白（含四模块静区） */
    for (y = 0U; y < 240U; y++) {
        for (x = 0U; x < 240U; x++) {
            int inside = (x >= (uint32_t)meta.origin_x) && (x < (uint32_t)meta.origin_x + span * scale) &&
                         (y >= (uint32_t)meta.origin_y) && (y < (uint32_t)meta.origin_y + span * scale);
            if (!inside && QR_BitmapGetPixel((uint16_t)x, (uint16_t)y) != 1U) {
                quiet_ok = 0;
            }
        }
    }
    CHECK(quiet_ok, "矩阵外区域（静区+居中留白）全为白");

    /* 6.2 左上角定位图形第一个模块必须是黑色 */
    CHECK_EQ(QR_BitmapGetPixel((uint16_t)(meta.origin_x + 4U * scale),
                               (uint16_t)(meta.origin_y + 4U * scale)), 0U,
             "定位图形首模块为黑");

    /* 6.3 静区：矩阵四周 4*scale 像素必须全白 */
    {
        int ring_ok = 1;
        uint32_t i;
        uint32_t bx = (uint32_t)meta.origin_x;
        uint32_t by = (uint32_t)meta.origin_y;
        for (i = 0U; i < 4U * scale; i++) {
            uint32_t k;
            for (k = 0U; k < span * scale; k++) {
                if (QR_BitmapGetPixel((uint16_t)(bx + i), (uint16_t)(by + k)) != 1U) ring_ok = 0;
                if (QR_BitmapGetPixel((uint16_t)(bx + k), (uint16_t)(by + i)) != 1U) ring_ok = 0;
                if (QR_BitmapGetPixel((uint16_t)(bx + span * scale - 1U - i), (uint16_t)(by + k)) != 1U) ring_ok = 0;
                if (QR_BitmapGetPixel((uint16_t)(bx + k), (uint16_t)(by + span * scale - 1U - i)) != 1U) ring_ok = 0;
            }
        }
        CHECK(ring_ok, "四模块静区为纯白 (NFR-004)");
    }

    /* 6.4 每行末尾 2 字节为 BMP 补齐位，应为 0 */
    {
        int pad_ok = 1;
        for (y = 0U; y < QR_BITMAP_HEIGHT; y++) {
            if ((g_qr_bitmap[y * QR_BITMAP_STRIDE + 30U] != 0xFFU) ||
                (g_qr_bitmap[y * QR_BITMAP_STRIDE + 31U] != 0xFFU)) {
                pad_ok = 0;
            }
        }
        CHECK(pad_ok, "行补齐字节保持白色 0xFF（BMP 忽略）");
    }

    /* 6.5 清屏后全白 */
    QR_BitmapClear();
    {
        int white_ok = 1;
        for (y = 0U; y < QR_BITMAP_HEIGHT; y++) {
            if (QR_BitmapGetPixel(0U, (uint16_t)y) != 1U) white_ok = 0;
            if (QR_BitmapGetPixel(239U, (uint16_t)y) != 1U) white_ok = 0;
        }
        CHECK(white_ok, "QR_BitmapClear 后为纯白");
    }
}

/* ======================================================================
 * 用例：BMP 导出
 * ==================================================================== */
static uint8_t g_bmp[BMP_FILE_SIZE];

static void test_bmp(void)
{
    const uint8_t text[] = "Hello 123";
    qr_meta_t meta;
    uint32_t i;
    uint32_t got;

    printf("[7] BMP 导出\n");

    (void)QR_ServiceEncode(text, 9U, QR_SYM_QRCODE, QR_ECC_M, &meta);
    QR_BitmapRender(&meta);
    BMP_Init(g_qr_bitmap);

    /* 7.1 完整读出 7742 字节 */
    (void)memset(g_bmp, 0, sizeof(g_bmp));
    got = BMP_Read(0U, g_bmp, (uint32_t)sizeof(g_bmp));
    CHECK_EQ(got, 7742U, "BMP 文件长度 7742 (T-004/AC-007)");

    /* 7.2 文件头字段 */
    CHECK(g_bmp[0] == 'B' && g_bmp[1] == 'M', "bfType = 'BM'");
    CHECK_EQ(g_bmp[2] | (g_bmp[3] << 8) | (g_bmp[4] << 16) | ((uint32_t)g_bmp[5] << 24),
             7742U, "bfSize = 7742");
    CHECK_EQ(g_bmp[10], 62U, "bfOffBits = 62");
    CHECK_EQ(g_bmp[14], 40U, "biSize = 40");
    CHECK_EQ(g_bmp[18] | (g_bmp[19] << 8), 240U, "biWidth = 240");
    {
        int32_t h = (int32_t)((uint32_t)g_bmp[22] | ((uint32_t)g_bmp[23] << 8) |
                              ((uint32_t)g_bmp[24] << 16) | ((uint32_t)g_bmp[25] << 24));
        CHECK_EQ(h, (unsigned long)(int32_t)-240, "biHeight = -240 (top-down)");
    }
    CHECK_EQ(g_bmp[26] | (g_bmp[27] << 8), 1U, "biPlanes = 1");
    CHECK_EQ(g_bmp[28] | (g_bmp[29] << 8), 1U, "biBitCount = 1");
    CHECK_EQ(g_bmp[30] | (g_bmp[31] << 8), 0U, "biCompression = 0");
    CHECK_EQ(g_bmp[34] | (g_bmp[35] << 8), 7680U, "biSizeImage = 7680");
    CHECK_EQ(g_bmp[46] | (g_bmp[47] << 8), 2U, "biClrUsed = 2");
    CHECK(g_bmp[54] == 0 && g_bmp[55] == 0 && g_bmp[56] == 0, "调色板[0] = 黑");
    CHECK(g_bmp[58] == 0xFF && g_bmp[59] == 0xFF && g_bmp[60] == 0xFF, "调色板[1] = 白");

    /* 7.3 像素区与位图逐字节一致 */
    CHECK(memcmp(&g_bmp[62], g_qr_bitmap, 7680U) == 0, "像素区 == g_qr_bitmap");

    /* 7.4 整文件 CRC32 与逐段读取一致 */
    CHECK_EQ(BMP_ComputeCrc32(), proto_crc32(g_bmp, 7742U), "BMP CRC32 与整块计算一致");

    /* 7.5 分块读取与整块读取一致（模拟 UART 分片） */
    {
        uint8_t part[512];
        uint32_t off = 0U;
        int same = 1;
        while (off < 7742U) {
            uint32_t want = 7742U - off;
            if (want > 448U) want = 448U;
            got = BMP_Read(off, part, want);
            if (got != want) { same = 0; break; }
            if (memcmp(part, &g_bmp[off], want) != 0) { same = 0; break; }
            off += want;
        }
        CHECK(same, "448 字节分块读取与整块一致");
        CHECK_EQ(off, 7742U, "分块累计偏移到达文件末尾");
    }

    /* 7.6 越界读取 */
    CHECK_EQ(BMP_Read(7742U, (uint8_t *)&i, 4U), 0U, "offset == 文件长度时返回 0");
    CHECK_EQ(BMP_Read(7740U, (uint8_t *)&i, 10U), 2U, "跨越末尾时只返回剩余字节");

    /* 7.7 输出 BMP 文件供外部校验 */
    {
        FILE *fp = fopen("out/qr_hello123.bmp", "wb");
        if (fp != NULL) {
            (void)fwrite(g_bmp, 1U, sizeof(g_bmp), fp);
            (void)fclose(fp);
            printf("      已写出 out/qr_hello123.bmp\n");
        } else {
            printf("      [WARN] 无法写 out/qr_hello123.bmp\n");
        }
    }

    /* 7.8 再生成一个中文 + URL 的样例，供扫码验证 */
    {
        const char *samples[] = {
            "\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81\xE6\xB5\x8B\xE8\xAF\x95-\xE4\xB8\xAD\xE6\x96\x87 UTF-8",
            "https://example.com/qr-test"
        };
        const char *names[] = { "out/qr_chinese.bmp", "out/qr_url.bmp" };
        int k;
        for (k = 0; k < 2; k++) {
            uint16_t len = (uint16_t)strlen(samples[k]);
            if (QR_ServiceEncode((const uint8_t *)samples[k], len, QR_SYM_QRCODE, QR_ECC_M, &meta) == QR_OK) {
                QR_BitmapRender(&meta);
                BMP_Init(g_qr_bitmap);
                (void)BMP_Read(0U, g_bmp, (uint32_t)sizeof(g_bmp));
                {
                    FILE *f2 = fopen(names[k], "wb");
                    if (f2 != NULL) {
                        (void)fwrite(g_bmp, 1U, sizeof(g_bmp), f2);
                        (void)fclose(f2);
                        printf("      已写出 %s (V%u, %ux%u)\n", names[k],
                               (unsigned)meta.version, (unsigned)meta.modules, (unsigned)meta.modules);
                    }
                }
            } else {
                g_fail++;
                printf("  [FAIL] 样例编码失败: %s\n", names[k]);
            }
        }
    }
}

/* ======================================================================
 * 用例：导出"固件真实生成的帧"，供 Python 端做跨实现一致性比对
 * ==================================================================== */
/**
 * @brief 把固件函数（proto_send_frame / proto_send_ack）实际产生的帧写到 out/。
 * @note  配套脚本 tests/firmware/check_interop.py 会用 host/protocol.py
 *        重新构造同样的帧并逐字节比对，验证 MCU 与 PC 两端协议实现一致。
 */
static void test_dump_frames(void)
{
    const uint8_t setqr_pl[] = { 0x01U, 0x01U, 'H', 'e', 'l', 'l', 'o', ' ', '1', '2', '3' };
    uint8_t status_pl[5];
    struct { const char *name; const uint8_t *data; uint16_t len; } files[4];
    int i;

    printf("[8] 导出固件帧（跨实现一致性比对用）\n");

    (void)proto_send_frame(PROTO_CMD_PING, 0x07U, NULL, 0U);
    {
        static uint8_t f_ping[600];
        static uint8_t f_setqr[600];
        static uint8_t f_ack[600];
        static uint8_t f_status[600];
        uint16_t n_ping, n_setqr, n_ack, n_status;

        (void)memcpy(f_ping, s_uart_tx, s_uart_tx_len);
        n_ping = s_uart_tx_len;

        (void)proto_send_frame(PROTO_CMD_SET_QR, 0x42U, setqr_pl, (uint16_t)sizeof(setqr_pl));
        (void)memcpy(f_setqr, s_uart_tx, s_uart_tx_len);
        n_setqr = s_uart_tx_len;

        (void)proto_send_ack(PROTO_CMD_SET_QR, 0x42U, PROTO_STATUS_OK, PROTO_ERR_NONE, 3U, 29U);
        (void)memcpy(f_ack, s_uart_tx, s_uart_tx_len);
        n_ack = s_uart_tx_len;

        status_pl[0] = PROTO_STATUS_OK;
        status_pl[1] = 1U;
        status_pl[2] = (uint8_t)PROTO_SYM_QRCODE;
        status_pl[3] = 3U;
        status_pl[4] = 29U;
        (void)proto_send_frame(PROTO_ACK_CMD(PROTO_CMD_GET_STATUS), 0x10U,
                               status_pl, (uint16_t)sizeof(status_pl));
        (void)memcpy(f_status, s_uart_tx, s_uart_tx_len);
        n_status = s_uart_tx_len;

        files[0].name = "out/f_ping.bin";      files[0].data = f_ping;   files[0].len = n_ping;
        files[1].name = "out/f_setqr.bin";     files[1].data = f_setqr;  files[1].len = n_setqr;
        files[2].name = "out/f_ack.bin";       files[2].data = f_ack;    files[2].len = n_ack;
        files[3].name = "out/f_status_ack.bin";files[3].data = f_status; files[3].len = n_status;

        for (i = 0; i < 4; i++) {
            FILE *fp = fopen(files[i].name, "wb");
            if (fp != NULL) {
                (void)fwrite(files[i].data, 1U, files[i].len, fp);
                (void)fclose(fp);
                printf("      已写出 %s (%u 字节)\n", files[i].name, (unsigned)files[i].len);
            } else {
                g_fail++;
                printf("  [FAIL] 无法写出 %s\n", files[i].name);
            }
        }
    }
}

/* ======================================================================
 * 入口
 * ==================================================================== */int main(void)
{
    printf("==============================================\n");
    printf(" STM32 二维码系统 - 固件逻辑主机端单元测试\n");
    printf("==============================================\n");

    test_crc();
    test_frame_build();
    test_parser();
    test_ack();
    test_qr();
    test_bitmap();
    test_bmp();
    test_dump_frames();

    printf("----------------------------------------------\n");
    printf(" 通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    printf("----------------------------------------------\n");

    return (g_fail == 0) ? 0 : 1;
}
