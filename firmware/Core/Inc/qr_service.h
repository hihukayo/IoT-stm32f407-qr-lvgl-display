/**
 * @file    qr_service.h
 * @brief   二维码编码服务：基于 Nayuki qrcodegen（MIT）的本地 QR Code Model 2 生成。
 * @details
 *          本模块只负责"文本 -> QR 模块矩阵 + 元数据"，不接触 LCD 与 LVGL。
 *
 *          容量与版本策略（需求 FR-004 / 5.4 节）：
 *          - 版本限制 1..20；
 *          - ECC 可选 L/M/Q/H，默认 M；
 *          - 输入为 UTF-8 原始字节，1..256 字节，不允许嵌入 NUL；
 *          - 编码失败时**保留上一张成功的二维码**，`valid` 与元数据保持不变。
 *
 *          @warning qrcodegen 的 `qrcodegen_encodeText()` 使用 byte 模式按原样写入
 *                   字节流，**不会**输出 ECI 26（UTF-8）指示符。中文等非 ASCII 文本
 *                   能否被正确解码取决于扫码软件是否默认按 UTF-8 解释；该限制已在
 *                   README 与测试记录中如实说明，不得宣称"所有扫码器都支持中文"。
 */

#ifndef QR_SERVICE_H
#define QR_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * 容量参数
 * ==================================================================== */
#define QR_TEXT_MAX_LEN         256U    /**< 文本最大字节数（需求 FR-003）     */
#define QR_VERSION_MIN          1U      /**< 允许的最小版本                    */
#define QR_VERSION_MAX          20U     /**< 允许的最大版本                    */
#define QR_CANVAS_SIZE          240U    /**< 二维码画布边长（像素）            */
#define QR_QUIET_MODULES        4U      /**< 四模块静区                        */
#define QR_MAX_SCALE            6U      /**< 模块最大像素边长                  */
/** qrcodegen 需要的单缓冲区字节数（版本 20） */
#define QR_CODEGEN_BUF_LEN      1177U

/* ======================================================================
 * 类型
 * ==================================================================== */
/** @brief 纠错等级（与协议 payload 的 ecc 字段一一对应）。 */
typedef enum {
    QR_ECC_L = 0,   /**< 约 7% 冗余  */
    QR_ECC_M = 1,   /**< 约 15% 冗余 */
    QR_ECC_Q = 2,   /**< 约 25% 冗余 */
    QR_ECC_H = 3    /**< 约 30% 冗余 */
} qr_ecc_t;

/** @brief 码制（与协议 payload 的 symbology 字段一一对应）。 */
typedef enum {
    QR_SYM_QRCODE     = 0x01,   /**< QR Code（已实现）        */
    QR_SYM_DATAMATRIX = 0x02    /**< Data Matrix（未实现）    */
} qr_symbology_t;

/** @brief 编码结果码。 */
typedef enum {
    QR_OK             = 0,  /**< 成功                                        */
    QR_ERR_PARAM      = 1,  /**< 参数非法（空文本/NUL/码制非法/ECC 越界）     */
    QR_ERR_TOO_LONG   = 2,  /**< 文本超过 256 字节                            */
    QR_ERR_ENCODE     = 3,  /**< 编码失败（内容超出所选版本/ECC 的容量）       */
    QR_ERR_UNSUPPORTED= 4   /**< 码制未实现                                  */
} qr_result_t;

/** @brief 当前有效二维码的元数据。 */
typedef struct {
    bool     valid;         /**< true = 存在一张成功生成的二维码            */
    uint8_t  symbology;     /**< qr_symbology_t                             */
    uint8_t  ecc;           /**< qr_ecc_t                                   */
    uint8_t  version;       /**< QR 版本 1..20                              */
    uint8_t  modules;       /**< 矩阵每边模块数 = 17 + 4*version            */
    uint8_t  scale;         /**< 每个模块的像素边长                          */
    uint16_t origin_x;      /**< 二维码块（含静区）在 240x240 画布中的左上 X */
    uint16_t origin_y;      /**< 二维码块（含静区）在 240x240 画布中的左上 Y */
    uint16_t text_len;      /**< 文本字节数                                 */
} qr_meta_t;

/* ======================================================================
 * 接口
 * ==================================================================== */
/** @brief 模块初始化（清空状态）。 */
void QR_ServiceInit(void);

/**
 * @brief  将文本编码为 QR Code 模块矩阵。
 * @param  text      文本字节（UTF-8），无需以 NUL 结尾
 * @param  len       文本字节数，1..256
 * @param  symbology qr_symbology_t
 * @param  ecc       qr_ecc_t
 * @param  out_meta  成功时写入元数据；失败时**不修改**（保留上一张有效二维码）
 * @return qr_result_t
 */
qr_result_t QR_ServiceEncode(const uint8_t *text, uint16_t len,
                             qr_symbology_t symbology, qr_ecc_t ecc,
                             qr_meta_t *out_meta);

/**
 * @brief  读取当前有效二维码的元数据副本。
 * @param  out 输出结构，不可为 NULL
 */
void QR_ServiceGetMeta(qr_meta_t *out);

/**
 * @brief  查询模块颜色。
 * @param  x 列索引，0 <= x < modules
 * @param  y 行索引，0 <= y < modules
 * @return true 表示该模块为深色（黑）
 * @note   越界返回 false。
 */
bool QR_ServiceGetModule(uint8_t x, uint8_t y);

/**
 * @brief  获取模块矩阵每边模块数（无有效二维码时返回 0）。
 * @return 模块数
 */
uint8_t QR_ServiceGetModuleCount(void);

/**
 * @brief  获取当前二维码版本（无有效二维码时返回 0）。
 * @return 版本号
 */
uint8_t QR_ServiceGetVersion(void);

/**
 * @brief  是否存在一张成功生成的二维码。
 * @return true 存在
 */
bool QR_ServiceIsValid(void);

/**
 * @brief  把 ECC 枚举转成可读名字（用于日志/UI）。
 * @param  ecc qr_ecc_t
 * @return 静态字符串 "L"/"M"/"Q"/"H"
 */
const char *QR_ServiceEccName(uint8_t ecc);

#ifdef __cplusplus
}
#endif

#endif /* QR_SERVICE_H */
