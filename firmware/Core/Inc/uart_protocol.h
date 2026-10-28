/**
 * @file    uart_protocol.h
 * @brief   上位机 <-> STM32 的二进制帧协议：帧结构、命令字、状态码、CRC16 与收发接口。
 * @details
 *          帧格式（字段顺序固定，PC 端 `host/protocol.py` 必须完全一致）：
 *
 *          @verbatim
 *          SOF0 | SOF1 | VER | CMD | SEQ | LEN_L | LEN_H | PAYLOAD... | CRC_L | CRC_H
 *          0xA5 | 0x5A |0x01 |     |     |  uint16 little-endian |      | uint16 LE
 *          @endverbatim
 *
 *          - `LEN` 为 PAYLOAD 字节数，范围 0..512；整帧长度 = 9 + LEN。
 *          - `CRC` 为 CRC-16/MODBUS（初值 0xFFFF，反射多项式 0xA001），
 *            **覆盖 VER 到 PAYLOAD 的全部字节**（不含 SOF，也不含 CRC 本身），
 *            低字节先发。
 *          - 校验失败、VER 不符、LEN 超限的帧一律丢弃并重新寻找帧头；
 *            接收中途超过 1 秒没有新字节则清空当前不完整帧。
 *
 * @note    本模块只做"字节 -> 完整帧"的解析与"完整帧 -> 字节"的发送，
 *          不做任何耗时操作（二维码编码、屏幕绘制都在主循环的应用层完成）。
 */

#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * 协议常量
 * ==================================================================== */
#define PROTO_SOF0              0xA5U   /**< 帧头第 1 字节                     */
#define PROTO_SOF1              0x5AU   /**< 帧头第 2 字节                     */
#define PROTO_VER               0x01U   /**< 协议版本                          */
#define PROTO_HEADER_SIZE       7U      /**< SOF0+SOF1+VER+CMD+SEQ+LEN(2)      */
#define PROTO_TRAILER_SIZE      2U      /**< CRC16 占 2 字节                   */
#define PROTO_MIN_FRAME_SIZE    (PROTO_HEADER_SIZE + PROTO_TRAILER_SIZE) /**< 9 */
#define PROTO_OVERHEAD          9U      /**< 整帧 = 9 + LEN                    */
#define PROTO_MAX_PAYLOAD       512U    /**< PAYLOAD 上限                      */
#define PROTO_MAX_FRAME_SIZE    (PROTO_MAX_PAYLOAD + PROTO_OVERHEAD)     /**< 521 */
#define PROTO_FRAME_TIMEOUT_MS  1000U   /**< 帧内字节间隔超时                  */
#define PROTO_ACK_TIMEOUT_MS    2000U   /**< PC 端等待 ACK 的建议超时（文档用） */

/** @brief 计算 ACK 命令字：普通请求的回应命令 = 请求命令 | 0x80。 */
#define PROTO_ACK_CMD(cmd)      ((uint8_t)((cmd) | 0x80U))

/* ======================================================================
 * 命令与事件 ID
 * ==================================================================== */
/** @brief 命令字。 */
typedef enum {
    PROTO_CMD_PING          = 0x01, /**< 连通性测试，无 payload；回应 0x81 */
    PROTO_CMD_SET_QR        = 0x02, /**< 设置文本并生成二维码；回应 0x82   */
    PROTO_CMD_GET_STATUS    = 0x03, /**< 读取设备状态；回应 0x83           */
    PROTO_CMD_GET_IMAGE     = 0x04, /**< 请求导出 BMP；回应 0x84 后进入图片流 */
    PROTO_EVT_IMAGE_BEGIN   = 0x30, /**< 图像传输开始事件                  */
    PROTO_EVT_IMAGE_DATA    = 0x31, /**< 图像数据块事件                    */
    PROTO_EVT_IMAGE_END     = 0x32  /**< 图像传输结束事件                  */
} proto_cmd_t;

/* ======================================================================
 * 状态码 / 错误码
 * ==================================================================== */
/** @brief ACK payload 中的 status 字段：0 = 成功，非 0 = 失败。 */
typedef enum {
    PROTO_STATUS_OK     = 0,    /**< 成功                                  */
    PROTO_STATUS_ERROR  = 1     /**< 失败，具体原因见 error_code           */
} proto_status_t;

/** @brief ACK payload 中的 error_code 字段。 */
typedef enum {
    PROTO_ERR_NONE          = 0,    /**< 无错误                            */
    PROTO_ERR_PARAM         = 1,    /**< 参数非法（长度/ECC/码制/NUL 等）  */
    PROTO_ERR_TOO_LONG      = 2,    /**< 文本超过 256 字节                 */
    PROTO_ERR_ENCODE_FAIL   = 3,    /**< 二维码编码失败（如容量不足）      */
    PROTO_ERR_NOT_READY     = 4,    /**< 尚无可导出的二维码                */
    PROTO_ERR_BUSY          = 5,    /**< 设备忙（正在传图）                */
    PROTO_ERR_CRC           = 6,    /**< 校验错误                          */
    PROTO_ERR_UNSUPPORTED   = 7,    /**< 不支持的码制/命令                 */
    PROTO_ERR_INTERNAL      = 8     /**< 内部错误                          */
} proto_error_t;

/** @brief 码制（SET_QR payload 第 1 字节）。 */
typedef enum {
    PROTO_SYM_QRCODE        = 0x01, /**< QR Code（已实现）                 */
    PROTO_SYM_DATAMATRIX    = 0x02  /**< Data Matrix（本版本未实现）       */
} proto_symbology_t;

/* ======================================================================
 * 帧结构
 * ==================================================================== */
/** @brief 一个已通过 CRC 校验的完整帧。 */
typedef struct {
    uint8_t  cmd;                           /**< 命令字                     */
    uint8_t  seq;                           /**< 序号，回应沿用             */
    uint16_t len;                           /**< payload 字节数             */
    uint8_t  payload[PROTO_MAX_PAYLOAD];    /**< payload 数据               */
} proto_frame_t;

/** @brief 通用 ACK payload（4 字节）。 */
typedef struct {
    uint8_t status;         /**< 0 = OK                                    */
    uint8_t error_code;     /**< proto_error_t                             */
    uint8_t qr_version;     /**< QR 版本 1..20，失败/不适用为 0            */
    uint8_t module_count;   /**< 矩阵每边模块数，失败/不适用为 0           */
} proto_ack_t;

/** @brief GET_STATUS 回应 payload（5 字节，注意与通用 ACK 不同）。 */
typedef struct {
    uint8_t status;         /**< 0 = OK                                    */
    uint8_t last_qr_valid;  /**< 1 = 存在上一张成功生成的二维码            */
    uint8_t symbology;      /**< proto_symbology_t                         */
    uint8_t qr_version;     /**< QR 版本                                   */
    uint8_t module_count;   /**< 矩阵每边模块数                            */
} proto_status_t_pl;

/* ======================================================================
 * CRC
 * ==================================================================== */
/**
 * @brief  计算 CRC-16/MODBUS。
 * @param  data 数据起始地址
 * @param  len  字节数
 * @return 16 位校验值；发送时低字节在前
 * @note   测试向量：ASCII "123456789" -> 0x4B37（线上字节序 37 4B）。
 */
uint16_t proto_crc16(const uint8_t *data, uint32_t len);

/**
 * @brief  计算 CRC-32/ISO-HDLC（反射多项式 0xEDB88320）。
 * @param  data 数据起始地址
 * @param  len  字节数
 * @return 32 位校验值
 * @note   初值 0xFFFFFFFF，结果再异或 0xFFFFFFFF；用于整份 BMP 文件校验。
 */
uint32_t proto_crc32(const uint8_t *data, uint32_t len);

/**
 * @brief  开始一次分段 CRC-32 计算。
 * @return 初始中间值（0xFFFFFFFF）
 * @note   用于"文件头 + 像素数据"两段不连续内存的合并校验。
 */
uint32_t proto_crc32_start(void);

/**
 * @brief  继续累加一段数据的 CRC-32。
 * @param  crc  上一次的中间值
 * @param  data 数据起始地址
 * @param  len  字节数
 * @return 新的中间值
 */
uint32_t proto_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/**
 * @brief  结束分段 CRC-32 计算（执行最终异或）。
 * @param  crc 中间值
 * @return 最终校验值
 */
uint32_t proto_crc32_finish(uint32_t crc);

/* ======================================================================
 * 接收解析器
 * ==================================================================== */
/** @brief 复位解析器（丢弃当前不完整帧，重新寻找 SOF）。 */
void proto_parser_reset(void);

/**
 * @brief  向解析器喂入一个字节。
 * @param  byte 收到的字节
 * @param  out  解析成功时填入完整帧
 * @return true 表示 out 中已产生一个通过校验的完整帧
 * @note   可在 UART 中断上下文中调用（本工程实际在主循环中调用）；不阻塞。
 */
bool proto_parser_feed(uint8_t byte, proto_frame_t *out);

/**
 * @brief  帧超时检测：接收过程中超过 1 秒无新字节则丢弃半帧。
 * @param  now_ms 当前毫秒时基（HAL_GetTick()）
 * @note   需在主循环中周期性调用。
 */
void proto_parser_poll(uint32_t now_ms);

/**
 * @brief  获取解析器统计信息（用于调试与自检）。
 * @param  frames_ok   输出：成功解析帧数，可为 NULL
 * @param  crc_errors  输出：CRC 错误帧数，可为 NULL
 * @param  bad_frames  输出：因 VER/LEN 非法丢弃的帧数，可为 NULL
 */
void proto_parser_stats(uint32_t *frames_ok, uint32_t *crc_errors, uint32_t *bad_frames);

/* ======================================================================
 * 发送
 * ==================================================================== */
/**
 * @brief  发送一个完整帧（阻塞到发送完成）。
 * @param  cmd     命令字
 * @param  seq     序号
 * @param  payload payload 数据，可为 NULL（len 为 0 时）
 * @param  len     payload 字节数，必须 <= PROTO_MAX_PAYLOAD
 * @return true 发送成功
 * @note   内部串行化，避免多任务交错写串口。
 */
bool proto_send_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len);

/**
 * @brief  发送通用 4 字节 ACK。
 * @param  req_cmd    请求命令（内部自动 |0x80）
 * @param  seq        请求序号
 * @param  status     proto_status_t
 * @param  error_code proto_error_t
 * @param  qr_version QR 版本，不适用填 0
 * @param  modules    模块数，不适用填 0
 * @return true 发送成功
 */
bool proto_send_ack(uint8_t req_cmd, uint8_t seq, uint8_t status, uint8_t error_code,
                    uint8_t qr_version, uint8_t modules);

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTOCOL_H */
