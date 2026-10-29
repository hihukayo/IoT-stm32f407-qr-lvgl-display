/**
 * @file    storage.h
 * @brief   用板载 W25Q16（SPI1，2 MiB）掉电保存「上一次的二维码文本」。
 * @details
 *          为什么只存文本、不存位图：
 *          - 二维码是**确定性**编码，同样的文本 + ECC 必然生成同样的模块矩阵；
 *          - 文本最多 256 B，位图要 7 680 B，存文本既省空间又能保留 ECC 选择；
 *          - 上电时重新编码一次即可恢复显示，代价约 3 ms。
 *
 *          掉电安全：使用两个 4 KiB 扇区轮流写（A/B 两个槽），记录带递增序号
 *          和 CRC16。写入过程中掉电只会损坏"正在写的那个槽"，另一个槽仍然有效，
 *          上电时取序号最大且 CRC 正确的记录，因此不会读到半条数据。
 *
 *          @note 本模块只在主循环上下文调用；W25Q16 的扇区擦除约 40~60 ms，
 *                期间 CPU 阻塞，可接受（仅在文本变化的 SET_QR 后发生一次）。
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "qr_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 从 Flash 读回的一条记录。 */
typedef struct {
    bool     valid;                          /**< 是否成功读到有效记录          */
    uint8_t  symbology;                      /**< qr_symbology_t                */
    uint8_t  ecc;                            /**< qr_ecc_t                      */
    uint16_t text_len;                       /**< 文本字节数 1..256             */
    uint8_t  text[QR_TEXT_MAX_LEN];          /**< 文本原文（UTF-8，无结尾 NUL）  */
} storage_saved_t;

/**
 * @brief  初始化 SPI1 片选并探测 W25Q16。
 * @return true = 检测到 W25Q16（JEDEC ID 匹配），false = 不在板/接线异常
 * @note   必须在 MX_SPI1_Init() 之后调用；失败时其余接口都返回 false，
 *         业务层据此退化为"无持久化"，不影响二维码生成与导出。
 */
bool STORAGE_Init(void);

/** @brief 上一次探测是否成功。 */
bool STORAGE_IsReady(void);

/**
 * @brief  读取保存的文本记录。
 * @param  out 输出结构，不可为 NULL
 * @return true = 存在有效记录且已填入 out
 */
bool STORAGE_Load(storage_saved_t *out);

/**
 * @brief  保存（或更新）文本记录。
 * @param  text      文本字节，1..QR_TEXT_MAX_LEN
 * @param  len       字节数
 * @param  symbology qr_symbology_t
 * @param  ecc       qr_ecc_t
 * @return true = 写入并校验成功
 * @note   内容与上次完全相同（文本/长度/ECC/码制都一致）时直接返回 true、
 *         不做任何擦写，避免重复发送同一文本时磨损 Flash。
 */
bool STORAGE_Save(const uint8_t *text, uint16_t len, uint8_t symbology, uint8_t ecc);

/**
 * @brief  擦除保存的记录（两个槽都清掉）。
 * @return true = 成功
 * @note   仅供测试/维护使用，正常运行不需要调用。
 */
bool STORAGE_EraseAll(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
