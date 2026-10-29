/**
 * @file    app.c
 * @brief   应用层实现：串口环形缓冲、命令分派、二维码生成与显示、BMP 图片流。
 * @details
 *          分工：
 *          - `USART1` 中断只把字节压入环形缓冲（O(1)，不做任何耗时动作）；
 *          - 帧解析、二维码编码、LCD/LVGL 刷新、BMP 流发送都在 `APP_Process()`；
 *          - 图片流每调用一次只发一个数据块，保证导出期间界面仍然刷新。
 *
 *          错误恢复（需求 FR-007）：
 *          - 坏帧被解析器丢弃，不影响后续命令；
 *          - 编码失败时保留上一张有效二维码，`last_qr_valid` 不变。
 */

#include "app.h"
#include "board_config.h"
#include "qr_service.h"
#include "qr_bitmap.h"
#include "bmp_stream.h"
#include "lvgl_ui.h"
#include "uart_protocol.h"
#include "storage.h"
#include <string.h>

/* ======================================================================
 * 内部状态
 * ==================================================================== */

/** @brief 串口接收环形缓冲（单生产者/单消费者，2 的幂）。 */
static uint8_t s_rx_ring[APP_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0U;
static volatile uint16_t s_rx_tail = 0U;

/** @brief HAL 单字节接收缓冲。 */
static uint8_t s_rx_byte;

/** @brief 解析出的完整帧。 */
static proto_frame_t s_frame;

/** @brief 应用状态。 */
static app_state_t s_state = APP_STATE_IDLE;

/** @brief 当前二维码元数据（成功生成后才更新）。 */
static qr_meta_t s_meta;

/** @brief 图片流相关状态。 */
static uint32_t s_img_offset;      /**< 已发送到 BMP 文件的偏移 */
static uint32_t s_img_crc;         /**< 整份 BMP 的 CRC32      */
static uint8_t  s_img_seq;         /**< GET_IMAGE 请求的序号   */

/** @brief 心跳灯计时。 */
static uint32_t s_led_tick;

/* ======================================================================
 * 内部工具
 * ==================================================================== */

/**
 * @brief  环形缓冲写入。
 * @param  byte 字节
 * @return true 写入成功；false 缓冲区已满（该字节被丢弃）
 */
static inline bool ring_push(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_rx_head + 1U) & (APP_RX_RING_SIZE - 1U));
    if (next == s_rx_tail) {
        return false;
    }
    s_rx_ring[s_rx_head] = byte;
    s_rx_head = next;
    return true;
}

/**
 * @brief  环形缓冲读取。
 * @param  out 输出字节
 * @return true 取到数据；false 缓冲区为空
 */
static inline bool ring_pop(uint8_t *out)
{
    if (s_rx_tail == s_rx_head) {
        return false;
    }
    *out = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) & (APP_RX_RING_SIZE - 1U));
    return true;
}

/**
 * @brief  把无符号整数追加为十进制字符串。
 * @param  p 写入位置
 * @param  v 数值
 * @return 新的写入位置
 */
static char *append_u32(char *p, uint32_t v)
{
    char tmp[11];
    uint8_t n = 0U;

    if (v == 0U) {
        *p++ = '0';
        return p;
    }
    while ((v != 0U) && (n < (uint8_t)sizeof(tmp))) {
        tmp[n++] = (char)('0' + (char)(v % 10U));
        v /= 10U;
    }
    while (n > 0U) {
        *p++ = tmp[--n];
    }
    return p;
}

/**
 * @brief  生成右上角信息文本，例如 "V3 M 29x29"。
 * @param  out      输出缓冲区（至少 20 字节）
 * @param  meta     二维码元数据
 */
static void build_info_text(char *out, const qr_meta_t *meta)
{
    char *p = out;

    *p++ = 'V';
    p = append_u32(p, meta->version);
    *p++ = ' ';
    *p++ = QR_ServiceEccName(meta->ecc)[0];
    *p++ = ' ';
    p = append_u32(p, meta->modules);
    *p++ = 'x';
    p = append_u32(p, meta->modules);
    *p = '\0';
}

/**
 * @brief  状态灯心跳（空闲时 1 Hz 闪烁，导出时常亮）。
 */
static void led_poll(void)
{
#if BOARD_LED_ENABLE
    uint32_t now = HAL_GetTick();
    uint32_t period = (s_state == APP_STATE_STREAMING) ? 0U : 500U;

    if (period == 0U) {
        GPIO_PinState on = (BOARD_LED_ACTIVE_LOW != 0) ? GPIO_PIN_RESET : GPIO_PIN_SET;
        HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN, on);
        return;
    }

    if ((uint32_t)(now - s_led_tick) >= period) {
        s_led_tick = now;
        HAL_GPIO_TogglePin(BOARD_LED_PORT, BOARD_LED_PIN);
    }
#endif
}

/* ======================================================================
 * 命令处理
 * ==================================================================== */

/**
 * @brief  处理 CMD_SET_QR。
 * @param  fr 完整帧
 */
static void handle_set_qr(const proto_frame_t *fr)
{
    uint8_t  symbology;
    uint8_t  ecc;
    const uint8_t *text;
    uint16_t text_len;
    qr_result_t res;

    if (s_state == APP_STATE_STREAMING) {
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR, PROTO_ERR_BUSY, 0U, 0U);
        return;
    }

    /* payload = [symbology][ecc][text...]，长度至少 3 */
    if (fr->len < 3U) {
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR, PROTO_ERR_PARAM, 0U, 0U);
        UI_SetError("ERR: bad params");
        return;
    }

    symbology = fr->payload[0];
    ecc       = fr->payload[1];
    text      = &fr->payload[2];
    text_len  = (uint16_t)(fr->len - 2U);

    if (text_len > QR_TEXT_MAX_LEN) {
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR, PROTO_ERR_TOO_LONG, 0U, 0U);
        UI_SetError("ERR: text too long");
        return;
    }

    res = QR_ServiceEncode(text, text_len, (qr_symbology_t)symbology, (qr_ecc_t)ecc, &s_meta);

    if (res != QR_OK) {
        uint8_t code;
        switch (res) {
        case QR_ERR_TOO_LONG:   code = PROTO_ERR_TOO_LONG;   break;
        case QR_ERR_ENCODE:     code = PROTO_ERR_ENCODE_FAIL; break;
        case QR_ERR_UNSUPPORTED:code = PROTO_ERR_UNSUPPORTED; break;
        case QR_ERR_PARAM:
        default:                code = PROTO_ERR_PARAM;      break;
        }
        /* 关键：失败时保留上一张有效二维码，仅提示错误 */
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR, code, 0U, 0U);
        UI_SetError((code == PROTO_ERR_ENCODE_FAIL) ? "ERR: encode failed"
                   : (code == PROTO_ERR_UNSUPPORTED) ? "ERR: unsupported"
                   : "ERR: invalid input");
        return;
    }

    /* 成功：渲染位图 -> 刷新界面 -> 绑定 BMP 像素源 */
    QR_BitmapRender(&s_meta);
    BMP_Init(g_qr_bitmap);
    UI_ShowQr(&s_meta);

    {
        char info[20];
        build_info_text(info, &s_meta);
        UI_SetInfo(info);
    }
    UI_SetStatus("QR code generated");

    (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_OK, PROTO_ERR_NONE,
                         s_meta.version, s_meta.modules);

#if BOARD_FLASH_ENABLE
    /* 内容持久化：写入板载 W25Q16，掉电/复位后上电自动恢复。
     * 放在 ACK 之后，避免 40~60 ms 的扇区擦除延迟影响上位机的应答体验；
     * 文本没变时 STORAGE_Save() 内部会直接返回，不产生擦写。 */
    if (STORAGE_Save(text, text_len, symbology, ecc)) {
        /* 静默成功：不额外占用状态栏 */
    }
#endif
}

/**
 * @brief  处理 CMD_GET_STATUS，回应 5 字节状态 payload。
 * @param  fr 完整帧
 */
static void handle_get_status(const proto_frame_t *fr)
{
    uint8_t pl[5];

    pl[0] = PROTO_STATUS_OK;
    pl[1] = QR_ServiceIsValid() ? 1U : 0U;
    pl[2] = (uint8_t)QR_SYM_QRCODE;
    pl[3] = QR_ServiceGetVersion();
    pl[4] = QR_ServiceGetModuleCount();

    (void)proto_send_frame(PROTO_ACK_CMD(fr->cmd), fr->seq, pl, (uint16_t)sizeof(pl));
}

/**
 * @brief  处理 CMD_GET_IMAGE：先回 ACK，再启动图片流。
 * @param  fr 完整帧
 */
static void handle_get_image(const proto_frame_t *fr)
{
    uint8_t pl[15];

    if (s_state == APP_STATE_STREAMING) {
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR, PROTO_ERR_BUSY, 0U, 0U);
        return;
    }
    if (!QR_ServiceIsValid()) {
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR, PROTO_ERR_NOT_READY, 0U, 0U);
        UI_SetError("ERR: no QR code yet");
        return;
    }

    /* 1) 先回 ACK（含版本/模块数，便于上位机日志） */
    (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_OK, PROTO_ERR_NONE,
                         s_meta.version, s_meta.modules);

    /* 2) 绑定像素源并计算整份文件的 CRC32 */
    BMP_Init(g_qr_bitmap);
    s_img_crc    = BMP_ComputeCrc32();
    s_img_offset = 0U;
    s_img_seq    = fr->seq;

    /* 3) EVT_IMAGE_BEGIN：format,width,height,file_size,data_size,row_stride */
    pl[0]  = (uint8_t)BMP_FORMAT_CODE;
    pl[1]  = (uint8_t)(BMP_WIDTH & 0xFFU);
    pl[2]  = (uint8_t)(BMP_WIDTH >> 8);
    pl[3]  = (uint8_t)(BMP_HEIGHT & 0xFFU);
    pl[4]  = (uint8_t)(BMP_HEIGHT >> 8);
    pl[5]  = (uint8_t)(BMP_FILE_SIZE & 0xFFU);
    pl[6]  = (uint8_t)((BMP_FILE_SIZE >> 8) & 0xFFU);
    pl[7]  = (uint8_t)((BMP_FILE_SIZE >> 16) & 0xFFU);
    pl[8]  = (uint8_t)((BMP_FILE_SIZE >> 24) & 0xFFU);
    pl[9]  = (uint8_t)(BMP_DATA_SIZE & 0xFFU);
    pl[10] = (uint8_t)((BMP_DATA_SIZE >> 8) & 0xFFU);
    pl[11] = (uint8_t)((BMP_DATA_SIZE >> 16) & 0xFFU);
    pl[12] = (uint8_t)((BMP_DATA_SIZE >> 24) & 0xFFU);
    pl[13] = (uint8_t)(BMP_ROW_STRIDE & 0xFFU);
    pl[14] = (uint8_t)(BMP_ROW_STRIDE >> 8);

    if (!proto_send_frame(PROTO_EVT_IMAGE_BEGIN, s_img_seq, pl, (uint16_t)sizeof(pl))) {
        return;
    }

    s_state = APP_STATE_STREAMING;
    UI_SetStatus("Exporting BMP...");
}

/**
 * @brief  处理一个完整帧（在主循环上下文中调用）。
 * @param  fr 完整帧
 */
static void handle_frame(const proto_frame_t *fr)
{
    switch (fr->cmd) {
    case PROTO_CMD_PING:
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_OK, PROTO_ERR_NONE, 0U, 0U);
        break;

    case PROTO_CMD_SET_QR:
        handle_set_qr(fr);
        break;

    case PROTO_CMD_GET_STATUS:
        handle_get_status(fr);
        break;

    case PROTO_CMD_GET_IMAGE:
        handle_get_image(fr);
        break;

    default:
        /* 未知命令：统一错误回应（status=1, error=UNSUPPORTED） */
        (void)proto_send_ack(fr->cmd, fr->seq, PROTO_STATUS_ERROR,
                             PROTO_ERR_UNSUPPORTED, 0U, 0U);
        break;
    }
}

/* ======================================================================
 * 图片流推进
 * ==================================================================== */
/**
 * @brief  发送一个图片数据块；全部发完后发送 EVT_IMAGE_END 并回到空闲。
 * @note   每次 `APP_Process()` 只调用一次，避免长时间占用主循环。
 */
static void pump_image_stream(void)
{
    uint8_t buf[4U + APP_IMAGE_CHUNK_SIZE];

    if (s_state != APP_STATE_STREAMING) {
        return;
    }

    if (s_img_offset < (uint32_t)BMP_FILE_SIZE) {
        uint32_t remain = (uint32_t)BMP_FILE_SIZE - s_img_offset;
        uint32_t n = (remain > APP_IMAGE_CHUNK_SIZE) ? APP_IMAGE_CHUNK_SIZE : remain;
        uint32_t got;

        buf[0] = (uint8_t)(s_img_offset & 0xFFU);
        buf[1] = (uint8_t)((s_img_offset >> 8) & 0xFFU);
        buf[2] = (uint8_t)((s_img_offset >> 16) & 0xFFU);
        buf[3] = (uint8_t)((s_img_offset >> 24) & 0xFFU);

        got = BMP_Read(s_img_offset, &buf[4], n);
        if (got != n) {
            /* 读失败属于内部错误：终止传输，不发 END，让上位机判为失败 */
            s_state = APP_STATE_IDLE;
            UI_SetError("ERR: export failed");
            return;
        }

        if (!proto_send_frame(PROTO_EVT_IMAGE_DATA, s_img_seq, buf, (uint16_t)(4U + n))) {
            s_state = APP_STATE_IDLE;
            UI_SetError("ERR: uart busy");
            return;
        }

        s_img_offset += n;
        return;
    }

    /* 所有数据已发完：发送 END（文件长度 + CRC32） */
    {
        uint8_t end_pl[8];

        end_pl[0] = (uint8_t)(BMP_FILE_SIZE & 0xFFU);
        end_pl[1] = (uint8_t)((BMP_FILE_SIZE >> 8) & 0xFFU);
        end_pl[2] = (uint8_t)((BMP_FILE_SIZE >> 16) & 0xFFU);
        end_pl[3] = (uint8_t)((BMP_FILE_SIZE >> 24) & 0xFFU);
        end_pl[4] = (uint8_t)(s_img_crc & 0xFFU);
        end_pl[5] = (uint8_t)((s_img_crc >> 8) & 0xFFU);
        end_pl[6] = (uint8_t)((s_img_crc >> 16) & 0xFFU);
        end_pl[7] = (uint8_t)((s_img_crc >> 24) & 0xFFU);

        (void)proto_send_frame(PROTO_EVT_IMAGE_END, s_img_seq, end_pl, (uint16_t)sizeof(end_pl));
    }

    s_state = APP_STATE_IDLE;
    UI_SetStatus("Export done");
}

/* ======================================================================
 * 对外接口
 * ==================================================================== */
#if BOARD_FLASH_ENABLE
/**
 * @brief  上电恢复：从 W25Q16 读回上次的文本，重新编码并显示。
 * @return true = 恢复成功并已上屏
 * @note   只存文本不存位图：编码是确定性的，同样的文本 + ECC 必然得到同样的
 *         模块矩阵，重算一次约 3 ms，比存 7 680 字节位图省得多。
 */
static bool restore_last_qr(void)
{
    storage_saved_t saved;
    qr_result_t     res;

    (void)memset(&saved, 0, sizeof(saved));
    if (!STORAGE_Load(&saved) || (!saved.valid)) {
        return false;
    }

    res = QR_ServiceEncode(saved.text, saved.text_len,
                           (qr_symbology_t)saved.symbology,
                           (qr_ecc_t)saved.ecc, &s_meta);
    if (res != QR_OK) {
        return false;
    }

    QR_BitmapRender(&s_meta);
    BMP_Init(g_qr_bitmap);
    UI_ShowQr(&s_meta);
    {
        char info[20];
        build_info_text(info, &s_meta);
        UI_SetInfo(info);
    }
    UI_SetStatus("Restored from flash");
    return true;
}
#endif /* BOARD_FLASH_ENABLE */

void APP_Init(void)
{
    s_state = APP_STATE_IDLE;
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_img_offset = 0U;
    s_img_crc = 0U;
    s_led_tick = HAL_GetTick();

    (void)memset(&s_meta, 0, sizeof(s_meta));

    proto_parser_reset();
    QR_ServiceInit();
    QR_BitmapClear();
    BMP_Init(g_qr_bitmap);

    /* 界面 */
    UI_Init();
    UI_SetInfo("READY");
    UI_SetStatus("Waiting for data...");
    UI_HideQr();

#if BOARD_FLASH_ENABLE
    /* 掉电恢复：有历史记录就直接显示上一张二维码 */
    if (STORAGE_Init()) {
        (void)restore_last_qr();
    }
#endif

    /* 启动串口单字节中断接收 */
    (void)HAL_UART_Receive_IT(BOARD_UART_HANDLE, &s_rx_byte, 1U);
}

/**
 * @brief  应用主循环任务：超时检测 -> 排空环形缓冲 -> 分派命令 -> 推进图片流。
 * @note   单次调用有工作量上限（每次最多排空 256 字节），保证主循环能及时返回
 * 去跑 lv_timer_handler()，界面上不会"假死"。
 */
void APP_Process(void)
{
    uint32_t i;

    /* 1. 半帧超时检测（1 秒） */
    proto_parser_poll(HAL_GetTick());

    /* 2. 排空环形缓冲（单次上限，保证主循环不会被长期占用） */
    for (i = 0U; i < 256U; i++) {
        uint8_t byte;
        if (!ring_pop(&byte)) {
            break;
        }
        if (proto_parser_feed(byte, &s_frame)) {
            handle_frame(&s_frame);
        }
    }

    /* 3. 推进图片流（每次最多一块） */
    pump_image_stream();

    /* 4. 状态灯心跳 */
    led_poll();
}

/**
 * @brief  查询是否正在发送 BMP 图片流。
 * @return true = 忙（此时 SET_QR 会返回 BUSY）
 */
bool APP_IsStreaming(void)
{
    return (s_state == APP_STATE_STREAMING);
}

/**
 * @brief  取当前应用状态。
 * @return APP_STATE_IDLE 或 APP_STATE_STREAMING
 */
app_state_t APP_GetState(void)
{
    return s_state;
}

/* ======================================================================
 * HAL 回调
 * ==================================================================== */
/**
 * @brief  UART 接收完成回调：把字节压入环形缓冲并重新挂起接收。
 * @param  huart 触发回调的串口
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == BOARD_UART_HANDLE->Instance) {
        (void)ring_push(s_rx_byte);
        (void)HAL_UART_Receive_IT(BOARD_UART_HANDLE, &s_rx_byte, 1U);
    }
}

/**
 * @brief  UART 错误回调：清除溢出/噪声/帧错误后重新挂起接收，避免串口"卡死"。
 * @param  huart 触发回调的串口
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == BOARD_UART_HANDLE->Instance) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        (void)HAL_UART_Receive_IT(BOARD_UART_HANDLE, &s_rx_byte, 1U);
    }
}
