/**
 * @file    qr_service.c
 * @brief   二维码编码服务实现（封装 Nayuki qrcodegen）。
 * @details
 *          编码流程：
 *          1. 参数检查（码制、ECC、长度 1..256、禁止嵌入 NUL）；
 *          2. 把文本复制到以 NUL 结尾的静态缓冲；
 *          3. 调用 `qrcodegen_encodeText()`，minVersion=1、maxVersion=20、
 *             `boostEcl = false`（严格使用用户选择的纠错等级，不得擅自升级，
 *             否则 PC 端选择的 ECC 与实际不符）；
 *          4. 成功后把矩阵从 staging 复制到 active，并计算显示元数据。
 *
 *          失败时 active 与 s_meta 保持不变，因此"一次失败不会破坏上一张
 *          已成功生成的二维码"（需求 FR-007）。
 */

#include "qr_service.h"
#include "qrcodegen.h"
#include <string.h>

/* ======================================================================
 * 静态存储
 * ==================================================================== */

/** @brief 当前有效二维码矩阵（qrcodegen 格式，版本 <= 20）。 */
static uint8_t s_qr_active[QR_CODEGEN_BUF_LEN];

/** @brief 编码暂存区：编码成功后才复制到 active，保证失败不破坏旧图。 */
static uint8_t s_qr_staging[QR_CODEGEN_BUF_LEN];

/** @brief qrcodegen 需要的工作缓冲（内容无意义）。 */
static uint8_t s_qr_temp[QR_CODEGEN_BUF_LEN];

/** @brief 以 NUL 结尾的文本副本。 */
static char s_text[QR_TEXT_MAX_LEN + 1U];

/** @brief 当前有效二维码元数据。 */
static qr_meta_t s_meta;

/* ======================================================================
 * 内部工具
 * ==================================================================== */

/**
 * @brief  把协议 ECC 枚举转换为 qrcodegen 的纠错等级。
 * @param  ecc qr_ecc_t（0..3）
 * @return qrcodegen 纠错等级
 */
static enum qrcodegen_Ecc ecc_to_codegen(uint8_t ecc)
{
    switch (ecc) {
    case QR_ECC_L: return qrcodegen_Ecc_LOW;
    case QR_ECC_Q: return qrcodegen_Ecc_QUARTILE;
    case QR_ECC_H: return qrcodegen_Ecc_HIGH;
    case QR_ECC_M:
    default:       return qrcodegen_Ecc_MEDIUM;
    }
}

/**
 * @brief  计算模块像素边长与画布原点（居中）。
 * @param  modules 矩阵每边模块数
 * @param  meta    待填充的元数据
 * @note   公式：scale = min(6, floor(240 / (modules + 8)))，至少为 1。
 */
static void compute_geometry(uint8_t modules, qr_meta_t *meta)
{
    uint32_t span = (uint32_t)modules + (QR_QUIET_MODULES * 2U);
    uint32_t scale = QR_CANVAS_SIZE / span;

    if (scale > QR_MAX_SCALE) {
        scale = QR_MAX_SCALE;
    }
    if (scale == 0U) {
        scale = 1U;
    }

    meta->scale = (uint8_t)scale;
    {
        uint32_t total = span * scale;
        meta->origin_x = (uint16_t)((QR_CANVAS_SIZE - total) / 2U);
        meta->origin_y = meta->origin_x;
    }
}

/* ======================================================================
 * 对外接口
 * ==================================================================== */
/**
 * @brief  复位编码服务状态（清空元数据、标记"无有效二维码"）。
 * @note   只清软件状态，不动 g_qr_bitmap；上电流程里由 APP_Init() 调用，
 * 随后可能被 STORAGE 恢复的二维码覆盖。
 */
void QR_ServiceInit(void)
{
    (void)memset(&s_meta, 0, sizeof(s_meta));
    (void)memset(s_qr_active, 0, sizeof(s_qr_active));
    (void)memset(s_text, 0, sizeof(s_text));
}

/**
 * @brief  把文本编码成 QR 模块矩阵并填元数据。
 * @note   接口契约见 qr_service.h。内部使用 qrcodegen 的 byte 模式，不写 ECI(UTF-8)。
 */
qr_result_t QR_ServiceEncode(const uint8_t *text, uint16_t len,
                             qr_symbology_t symbology, qr_ecc_t ecc,
                             qr_meta_t *out_meta)
{
    int size;
    uint8_t version;
    qr_meta_t meta;
    bool ok;

    if (symbology != QR_SYM_QRCODE) {
        return (symbology == QR_SYM_DATAMATRIX) ? QR_ERR_UNSUPPORTED : QR_ERR_PARAM;
    }
    if ((uint8_t)ecc > (uint8_t)QR_ECC_H) {
        return QR_ERR_PARAM;
    }
    if ((text == NULL) || (len == 0U)) {
        return QR_ERR_PARAM;
    }
    if (len > QR_TEXT_MAX_LEN) {
        return QR_ERR_TOO_LONG;
    }
    /* 不允许嵌入 NUL：协议以长度定界，NUL 会被 qrcodegen 当作字符串结尾 */
    if (memchr(text, 0, len) != NULL) {
        return QR_ERR_PARAM;
    }

    (void)memcpy(s_text, text, len);
    s_text[len] = '\0';

    ok = qrcodegen_encodeText(s_text, s_qr_temp, s_qr_staging,
                              ecc_to_codegen((uint8_t)ecc),
                              (int)QR_VERSION_MIN, (int)QR_VERSION_MAX,
                              qrcodegen_Mask_AUTO, false);
    if (!ok) {
        return QR_ERR_ENCODE;
    }

    size = qrcodegen_getSize(s_qr_staging);
    if ((size < 21) || (size > (int)(QR_VERSION_MAX * 4U + 17U))) {
        return QR_ERR_ENCODE;
    }
    version = (uint8_t)((size - 17) / 4);
    if ((version < QR_VERSION_MIN) || (version > QR_VERSION_MAX)) {
        return QR_ERR_ENCODE;
    }

    /* 成功：提交矩阵与元数据 */
    (void)memcpy(s_qr_active, s_qr_staging, sizeof(s_qr_active));

    (void)memset(&meta, 0, sizeof(meta));
    meta.valid      = true;
    meta.symbology  = (uint8_t)QR_SYM_QRCODE;
    meta.ecc        = (uint8_t)ecc;
    meta.version    = version;
    meta.modules    = (uint8_t)size;
    meta.text_len   = len;
    compute_geometry(meta.modules, &meta);

    s_meta = meta;
    if (out_meta != NULL) {
        *out_meta = meta;
    }

    return QR_OK;
}

/**
 * @brief  取当前有效二维码的元数据副本。
 */
void QR_ServiceGetMeta(qr_meta_t *out)
{
    if (out != NULL) {
        *out = s_meta;
    }
}

/**
 * @brief  查询模块矩阵中某个模块是否为深色。
 */
bool QR_ServiceGetModule(uint8_t x, uint8_t y)
{
    if (!s_meta.valid) {
        return false;
    }
    if ((x >= s_meta.modules) || (y >= s_meta.modules)) {
        return false;
    }
    return qrcodegen_getModule(s_qr_active, (int)x, (int)y);
}

/**
 * @brief  取模块矩阵每边的模块数。
 */
uint8_t QR_ServiceGetModuleCount(void)
{
    return s_meta.valid ? s_meta.modules : 0U;
}

/**
 * @brief  取当前二维码版本号。
 */
uint8_t QR_ServiceGetVersion(void)
{
    return s_meta.valid ? s_meta.version : 0U;
}

/**
 * @brief  是否已有一张成功生成的二维码。
 * @note   编码失败不会把它置回 false，这是"失败保留上一张"的实现基础。
 */
bool QR_ServiceIsValid(void)
{
    return s_meta.valid;
}

/**
 * @brief  把 ECC 枚举转成单字符名字。
 */
const char *QR_ServiceEccName(uint8_t ecc)
{
    switch (ecc) {
    case QR_ECC_L: return "L";
    case QR_ECC_Q: return "Q";
    case QR_ECC_H: return "H";
    case QR_ECC_M:
    default:       return "M";
    }
}
