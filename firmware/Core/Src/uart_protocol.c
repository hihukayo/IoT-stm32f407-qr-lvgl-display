/**
 * @file    uart_protocol.c
 * @brief   二进制帧协议的实现：CRC16/CRC32、字节流帧解析状态机、帧发送。
 * @details
 *          解析器是一个"逐字节推进"的有限状态机，天然支持：
 *          - 一帧被拆成任意多段到达（不假设一次回调收完整帧）；
 *          - payload 中出现 0xA5 0x5A（只有帧头位置才识别 SOF，payload 按 LEN 计数跳过）；
 *          - 两帧粘连在同一个缓冲区里（解析成功后自动回到 SOF0 状态）；
 *          - 坏 CRC / 坏 VER / LEN 超限时丢弃并重新寻找帧头。
 */

#include "uart_protocol.h"
#include "board_config.h"
#include <string.h>

/* ======================================================================
 * 内部常量与状态
 * ==================================================================== */

/** @brief AVR 风格的 CRC16/MODBUS 查找表（16 项，节省 Flash）。 */
static const uint16_t s_crc16_nibble[16] = {
    0x0000U, 0xCC01U, 0xD801U, 0x1400U,
    0xF001U, 0x3C00U, 0x2800U, 0xE401U,
    0xA001U, 0x6C00U, 0x7800U, 0xB401U,
    0x5000U, 0x9C01U, 0x8801U, 0x4400U
};

/** @brief 解析器状态。 */
enum {
    PARSER_SOF0 = 0,    /**< 等待帧头第 1 字节 0xA5 */
    PARSER_SOF1 = 1,    /**< 等待帧头第 2 字节 0x5A */
    PARSER_BODY = 2     /**< 收集中间字段（VER..CRC） */
};

static uint8_t  s_buf[PROTO_MAX_FRAME_SIZE];    /**< 当前帧装配缓冲           */
static uint16_t s_pos;                          /**< 已装配字节数             */
static uint16_t s_need;                         /**< 该帧的总字节数           */
static uint8_t  s_state;                        /**< PARSER_xxx               */
static uint32_t s_last_ms;                      /**< 最近一次收到字节的时刻   */

static uint32_t s_frames_ok;                    /**< 成功解析帧计数           */
static uint32_t s_crc_errors;                   /**< CRC 错误帧计数           */
static uint32_t s_bad_frames;                   /**< VER/LEN 非法计数         */

static uint8_t  s_tx[PROTO_MAX_FRAME_SIZE];     /**< 发送装配缓冲             */
static bool     s_tx_busy;                      /**< 发送互斥标志             */

/* ======================================================================
 * CRC 实现
 * ==================================================================== */
/**
 * @brief  计算 CRC-16/MODBUS（初值 0xFFFF，反射多项式 0xA001）。
 * @note   覆盖范围是帧的 VER..PAYLOAD（不含两个 SOF，也不含 CRC 自身）；
 * 线上按低字节先发。测试向量："123456789" -> 0x4B37。
 */
uint16_t proto_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;

    if (data == NULL) {
        return crc;
    }

    while (len-- != 0U) {
        /* 处理低半字节 */
        uint8_t idx = (uint8_t)((crc ^ (uint16_t)(*data)) & 0x0FU);
        crc = (uint16_t)((crc >> 4) ^ s_crc16_nibble[idx]);
        /* 处理高半字节 */
        idx = (uint8_t)((crc ^ (uint16_t)(*data >> 4)) & 0x0FU);
        crc = (uint16_t)((crc >> 4) ^ s_crc16_nibble[idx]);
        data++;
    }

    return crc;
}

/**
 * @brief  取 CRC-32/ISO-HDLC 的初值。
 */
uint32_t proto_crc32_start(void)
{
    return 0xFFFFFFFFU;
}

/**
 * @brief  增量更新 CRC-32/ISO-HDLC。
 */
uint32_t proto_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint8_t  bit;

    if (data == NULL) {
        return crc;
    }

    for (i = 0U; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc = crc >> 1;
            }
        }
    }

    return crc;
}

/**
 * @brief  收尾：对中间值取反。
 */
uint32_t proto_crc32_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

/**
 * @brief  一次性计算 CRC-32/ISO-HDLC。
 * @note   等价于 start -> update -> finish；"123456789" 的结果应为 0xCBF43926。
 */
uint32_t proto_crc32(const uint8_t *data, uint32_t len)
{
    return proto_crc32_finish(proto_crc32_update(proto_crc32_start(), data, len));
}

/* ======================================================================
 * 解析器
 * ==================================================================== */
/**
 * @brief  把解析器复位到"等待帧头 0xA5"的初始状态。
 * @note   坏帧、超时、显式复位都会走到这里；统计计数器不受影响。
 */
void proto_parser_reset(void)
{
    s_state   = PARSER_SOF0;
    s_pos     = 0U;
    s_need    = 0U;
    s_last_ms = HAL_GetTick();
}

/**
 * @brief  向流式解析器喂一个字节。
 * @note   状态机逐字节推进，因此不需要"一次收完整帧"；坏 VER、LEN 超限、CRC 错
 * 都会丢弃候选帧头并从下一个 0xA5 重新同步。
 */
bool proto_parser_feed(uint8_t byte, proto_frame_t *out)
{
    if (out == NULL) {
        return false;
    }

    s_last_ms = HAL_GetTick();

    switch (s_state) {
    case PARSER_SOF0:
        if (byte == PROTO_SOF0) {
            s_buf[0] = byte;
            s_pos    = 1U;
            s_state  = PARSER_SOF1;
        }
        break;

    case PARSER_SOF1:
        if (byte == PROTO_SOF1) {
            s_buf[1] = byte;
            s_pos    = 2U;
            s_state  = PARSER_BODY;
        } else if (byte == PROTO_SOF0) {
            /* 0xA5 0xA5 5A：重新以当前字节作为帧头第 1 字节 */
            s_pos = 1U;
        } else {
            s_state = PARSER_SOF0;
            s_pos   = 0U;
        }
        break;

    case PARSER_BODY:
    default:
        if (s_pos < (uint16_t)sizeof(s_buf)) {
            s_buf[s_pos++] = byte;
        } else {
            /* 理论上不可达：LEN 已在头部校验过 */
            proto_parser_reset();
            break;
        }

        if (s_pos == PROTO_HEADER_SIZE) {
            /* 头部齐了：立刻校验 VER 与 LEN，避免为非法帧白收 512 字节 */
            uint16_t payload_len = (uint16_t)s_buf[5] | (uint16_t)((uint16_t)s_buf[6] << 8);
            if ((s_buf[2] != PROTO_VER) || (payload_len > PROTO_MAX_PAYLOAD)) {
                s_bad_frames++;
                proto_parser_reset();
                /* 当前字节可能本身就是一个新的帧头 */
                if (byte == PROTO_SOF0) {
                    s_buf[0] = byte;
                    s_pos    = 1U;
                    s_state  = PARSER_SOF1;
                }
                break;
            }
            s_need = (uint16_t)(payload_len + PROTO_OVERHEAD);
        }

        if (s_need != 0U && s_pos == s_need) {
            uint16_t payload_len = (uint16_t)(s_need - PROTO_OVERHEAD);
            uint16_t crc_calc    = proto_crc16(&s_buf[2], (uint32_t)(5U + payload_len));
            uint16_t crc_recv    = (uint16_t)s_buf[s_need - 2U] |
                                   (uint16_t)((uint16_t)s_buf[s_need - 1U] << 8);

            if (crc_calc != crc_recv) {
                s_crc_errors++;
                proto_parser_reset();
                break;
            }

            out->cmd = s_buf[3];
            out->seq = s_buf[4];
            out->len = payload_len;
            if (payload_len != 0U) {
                (void)memcpy(out->payload, &s_buf[PROTO_HEADER_SIZE], payload_len);
            }
            s_frames_ok++;
            proto_parser_reset();
            return true;
        }
        break;
    }

    return false;
}

/**
 * @brief  半帧超时检测：超过 1 秒没有新字节就丢弃残帧。
 * @note   需在主循环里周期性调用；防止上位机半途断开后解析器一直卡在收帧状态。
 */
void proto_parser_poll(uint32_t now_ms)
{
    if ((s_state != PARSER_SOF0) && ((uint32_t)(now_ms - s_last_ms) > PROTO_FRAME_TIMEOUT_MS)) {
        /* 半帧超时：清空并重新寻找帧头（需求 5.1 节） */
        proto_parser_reset();
    }
}

/**
 * @brief  读取解析统计（用于调试与测试记录）。
 * @note   任一输出指针可为 NULL，表示不需要该统计。
 */
void proto_parser_stats(uint32_t *frames_ok, uint32_t *crc_errors, uint32_t *bad_frames)
{
    if (frames_ok != NULL) {
        *frames_ok = s_frames_ok;
    }
    if (crc_errors != NULL) {
        *crc_errors = s_crc_errors;
    }
    if (bad_frames != NULL) {
        *bad_frames = s_bad_frames;
    }
}

/* ======================================================================
 * 发送
 * ==================================================================== */
/**
 * @brief  按协议封装并发送一帧（阻塞发送）。
 * @note   CRC 覆盖 VER..PAYLOAD；LEN 为 payload 长度（不含 9 字节帧头与 CRC）。
 */
bool proto_send_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint16_t crc;
    uint16_t frame_len;
    HAL_StatusTypeDef st;

    if (len > PROTO_MAX_PAYLOAD) {
        return false;
    }
    if ((payload == NULL) && (len != 0U)) {
        return false;
    }
    if (s_tx_busy) {
        /* 单任务模型下不应发生；此处仅防御 */
        return false;
    }
    s_tx_busy = true;

    s_tx[0] = PROTO_SOF0;
    s_tx[1] = PROTO_SOF1;
    s_tx[2] = PROTO_VER;
    s_tx[3] = cmd;
    s_tx[4] = seq;
    s_tx[5] = (uint8_t)(len & 0xFFU);
    s_tx[6] = (uint8_t)(len >> 8);
    if (len != 0U) {
        (void)memcpy(&s_tx[PROTO_HEADER_SIZE], payload, len);
    }

    crc = proto_crc16(&s_tx[2], (uint32_t)(5U + len));
    s_tx[PROTO_HEADER_SIZE + len]      = (uint8_t)(crc & 0xFFU);
    s_tx[PROTO_HEADER_SIZE + len + 1U] = (uint8_t)(crc >> 8);

    frame_len = (uint16_t)(len + PROTO_OVERHEAD);
    /* 461 字节 @115200 约 40 ms，给 250 ms 余量 */
    st = HAL_UART_Transmit(BOARD_UART_HANDLE, s_tx, frame_len, 250U);

    s_tx_busy = false;
    return (st == HAL_OK);
}

/**
 * @brief  发送 4 字节标准 ACK。
 * @note   ACK payload 固定为 [status][error_code][qr_version][module_count]。
 */
bool proto_send_ack(uint8_t req_cmd, uint8_t seq, uint8_t status, uint8_t error_code,
                    uint8_t qr_version, uint8_t modules)
{
    uint8_t pl[4];

    pl[0] = status;
    pl[1] = error_code;
    pl[2] = qr_version;
    pl[3] = modules;

    return proto_send_frame(PROTO_ACK_CMD(req_cmd), seq, pl, 4U);
}
