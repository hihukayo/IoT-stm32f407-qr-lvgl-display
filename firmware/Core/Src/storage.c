/**
 * @file    storage.c
 * @brief   板载 W25Q16（SPI1）上的「上一次二维码文本」持久化实现。
 * @details
 *          Flash 布局（相对 W25Q16 地址 0）：
 *          @verbatim
 *          0x000000 ~ 0x000FFF   槽 A（4 KiB 扇区）
 *          0x001000 ~ 0x001FFF   槽 B（4 KiB 扇区）
 *          其余 2 MiB 未使用
 *          @endverbatim
 *
 *          记录格式（270 字节，小端）：
 *          @verbatim
 *          off  size  字段
 *          0    4     magic  = 0x31535251 ("QRS1")
 *          4    2     fmt    = 1（记录格式版本）
 *          6    2     seq    （递增序号，越大越新）
 *          8    1     symbology
 *          9    1     ecc
 *          10   2     text_len
 *          12   256   text（不足部分补 0）
 *          268  2     crc16（覆盖 0..267，CRC-16/MODBUS）
 *          @endverbatim
 *
 *          掉电安全策略：两个槽轮流写。写入前先擦目标槽，再分两次页编程；
 *          中途掉电只会让目标槽 CRC 失效，另一个槽仍保留上一条完整记录。
 *          上电取"序号最大且 CRC 正确"的那个槽，因此不会出现半条数据。
 */

#include "storage.h"
#include "board_config.h"
#include "main.h"

#include <string.h>

/* ======================================================================
 * W25Q16 命令
 * ==================================================================== */
#define W25Q_CMD_WRITE_ENABLE   0x06U
#define W25Q_CMD_READ_STATUS1   0x05U
#define W25Q_CMD_READ_DATA      0x03U
#define W25Q_CMD_PAGE_PROGRAM   0x02U
#define W25Q_CMD_SECTOR_ERASE   0x20U
#define W25Q_CMD_JEDEC_ID       0x9FU

#define W25Q_STATUS_BUSY        0x01U

/* ======================================================================
 * 槽与记录
 * ==================================================================== */
#define SLOT_COUNT          2U
#define SLOT0_ADDR          0x000000U
#define SLOT1_ADDR          0x001000U
#define SLOT_BYTES          4096U

#define REC_MAGIC           0x31535251U     /**< "QRS1" */
#define REC_FMT             1U
#define REC_TEXT_OFF        12U
#define REC_CRC_OFF         (REC_TEXT_OFF + QR_TEXT_MAX_LEN)    /* 268 */
#define REC_SIZE            (REC_CRC_OFF + 2U)                  /* 270 */
#define REC_FIRST_PAGE_SIZE 256U                                /**< 一次页编程上限 */

/* ======================================================================
 * 模块状态
 * ==================================================================== */
static bool     s_ready;        /**< W25Q16 探测成功 */
static int8_t   s_slot = -1;    /**< 最近一次有效记录所在槽（0/1），-1 = 无 */
static uint16_t s_seq;          /**< 最近一次有效记录的序号 */
static bool     s_cache_valid;  /**< 去重用的内存副本是否有效 */
static storage_saved_t s_cache; /**< 最近写入/读出的内容 */

static uint8_t  s_rec[REC_SIZE];    /**< 记录组装/读取缓冲（避免大栈） */
static uint8_t  s_verify[REC_SIZE]; /**< 回读校验缓冲 */

/* ======================================================================
 * 底层 SPI 辅助
 * ==================================================================== */
static void flash_cs(bool level)
{
    HAL_GPIO_WritePin(BOARD_FLASH_CS_PORT, BOARD_FLASH_CS_PIN,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/**
 * @brief  只发不收地写一段数据到 W25Q16。
 * @param  data 数据指针
 * @param  len  字节数
 * @return true = HAL 报告发送完成
 * @note   片选由调用方负责拉低/拉高，本函数不碰 CS。
 */
static bool flash_tx(const uint8_t *data, uint16_t len)
{
    return HAL_SPI_Transmit(BOARD_FLASH_SPI_HANDLE, (uint8_t *)data, len, 100U) == HAL_OK;
}

/**
 * @brief  从 W25Q16 读回一段数据（时钟由 HAL 产生）。
 * @param  data 输出缓冲
 * @param  len  字节数
 * @return true = 读取成功
 */
static bool flash_rx(uint8_t *data, uint16_t len)
{
    return HAL_SPI_Receive(BOARD_FLASH_SPI_HANDLE, data, len, 100U) == HAL_OK;
}

/** @brief 轮询状态寄存器 BUSY 位，直到空闲或超时。 */
static bool flash_wait_ready(uint32_t timeout_ms)
{
    uint8_t  cmd = W25Q_CMD_READ_STATUS1;
    uint8_t  status = 0xFFU;
    uint32_t start = HAL_GetTick();

    for (;;) {
        flash_cs(false);
        if (!flash_tx(&cmd, 1U) || !flash_rx(&status, 1U)) {
            flash_cs(true);
            return false;
        }
        flash_cs(true);

        if ((status & W25Q_STATUS_BUSY) == 0U) {
            return true;
        }
        if ((HAL_GetTick() - start) > timeout_ms) {
            return false;
        }
    }
}

/**
 * @brief  发送 Write Enable（0x06），擦写前必须先发。
 * @return true = 命令已发出
 * @note   命令发出后芯片内部 WEL 位置 1，擦除/编程完成后自动清零。
 */
static bool flash_write_enable(void)
{
    uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
    bool    ok;

    flash_cs(false);
    ok = flash_tx(&cmd, 1U);
    flash_cs(true);
    return ok;
}

/** @brief 读 JEDEC ID（0x9F 后 3 字节）。 */
static bool flash_read_jedec(uint32_t *out_id)
{
    uint8_t cmd = W25Q_CMD_JEDEC_ID;
    uint8_t id[3] = {0};
    bool    ok;

    flash_cs(false);
    ok = flash_tx(&cmd, 1U) && flash_rx(id, 3U);
    flash_cs(true);
    if (!ok) {
        return false;
    }
    *out_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
    return true;
}

/**
 * @brief  从指定地址读任意长度数据（0x03 Read Data）。
 * @param  addr 24 位字节地址
 * @param  buf  输出缓冲
 * @param  len  字节数
 * @return true = 读取成功
 */
static bool flash_read(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t cmd[4];
    bool    ok;

    cmd[0] = W25Q_CMD_READ_DATA;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    flash_cs(false);
    ok = flash_tx(cmd, 4U) && flash_rx(buf, len);
    flash_cs(true);
    return ok;
}

/**
 * @brief  页编程（0x02），一次最多 256 字节且不得跨页。
 * @param  addr 页内目标地址
 * @param  data 数据指针
 * @param  len  字节数（1..256）
 * @return true = 编程完成且芯片已退出 BUSY
 * @note   内部先发 Write Enable；调用方需保证 len 不超过页边界剩余空间。
 */
static bool flash_page_program(uint32_t addr, const uint8_t *data, uint16_t len)
{
    uint8_t cmd[4];
    bool    ok;

    if ((len == 0U) || (len > REC_FIRST_PAGE_SIZE)) {
        return false;
    }
    if (!flash_write_enable()) {
        return false;
    }

    cmd[0] = W25Q_CMD_PAGE_PROGRAM;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    flash_cs(false);
    ok = flash_tx(cmd, 4U) && flash_tx(data, len);
    flash_cs(true);

    return ok && flash_wait_ready(50U);
}

/**
 * @brief  擦除 4 KiB 扇区（0x20），整扇区变 0xFF。
 * @param  addr 扇区内任意地址
 * @return true = 擦除完成（超时 1000 ms）
 * @note   这是唯一的擦除粒度：擦除期间 CPU 阻塞约 40~60 ms，因此只在
 * 文本变化时调用。
 */
static bool flash_sector_erase(uint32_t addr)
{
    uint8_t cmd[4];
    bool    ok;

    if (!flash_write_enable()) {
        return false;
    }

    cmd[0] = W25Q_CMD_SECTOR_ERASE;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    flash_cs(false);
    ok = flash_tx(cmd, 4U);
    flash_cs(true);

    return ok && flash_wait_ready(1000U);
}

/* ======================================================================
 * 记录编解码
 * ==================================================================== */
/** @brief CRC-16/MODBUS（与串口协议同一套算法，初值 0xFFFF，反射多项式 0xA001）。 */
static uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t  bit;

    for (i = 0U; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x0001U) != 0U) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

/**
 * @brief  以小端格式写入 16 位整数。
 * @param  p 目标地址
 * @param  v 数值
 */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/**
 * @brief  以小端格式读取 16 位整数。
 * @param  p 源地址
 * @return 读到的值
 */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
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

/**
 * @brief  以小端格式读取 32 位整数。
 * @param  p 源地址
 * @return 读到的值
 */
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief  读取一个槽并校验。
 * @param  base 槽起始地址
 * @param  out  校验通过时填入内容
 * @param  seq  校验通过时填入序号
 * @return true = 该槽存在完整、有效的记录
 */
static bool slot_read(uint32_t base, storage_saved_t *out, uint16_t *seq)
{
    uint16_t text_len;
    uint16_t crc;
    uint8_t  symbology;
    uint8_t  ecc;

    if (!flash_read(base, s_rec, (uint16_t)REC_SIZE)) {
        return false;
    }
    if (get_u32(&s_rec[0]) != REC_MAGIC) {
        return false;
    }
    if (get_u16(&s_rec[4]) != REC_FMT) {
        return false;
    }
    symbology = s_rec[8];
    ecc       = s_rec[9];
    text_len  = get_u16(&s_rec[10]);

    if ((symbology != (uint8_t)QR_SYM_QRCODE) || (ecc > (uint8_t)QR_ECC_H)) {
        return false;
    }
    if ((text_len == 0U) || (text_len > QR_TEXT_MAX_LEN)) {
        return false;
    }
    crc = get_u16(&s_rec[REC_CRC_OFF]);
    if (crc != crc16_modbus(s_rec, (uint16_t)REC_CRC_OFF)) {
        return false;
    }

    (void)memset(out, 0, sizeof(*out));
    out->valid     = true;
    out->symbology = symbology;
    out->ecc       = ecc;
    out->text_len  = text_len;
    (void)memcpy(out->text, &s_rec[REC_TEXT_OFF], text_len);
    *seq = get_u16(&s_rec[6]);
    return true;
}

/** @brief 扫描两个槽，选出序号最大且有效的那条记录。 */
static bool scan_latest(storage_saved_t *out)
{
    storage_saved_t cand;
    uint16_t        seq;
    bool            found = false;
    uint16_t        best_seq = 0U;
    int8_t          best_slot = -1;

    s_seq = 0U;
    s_slot = -1;

    if (slot_read(SLOT0_ADDR, &cand, &seq)) {
        found = true;
        best_seq = seq;
        best_slot = 0;
    }
    if (slot_read(SLOT1_ADDR, &cand, &seq)) {
        /* 两个都有效时取序号大的；序号相同时保持槽 A（先读到者） */
        if ((!found) || (seq > best_seq)) {
            found = true;
            best_seq = seq;
            best_slot = 1;
        }
    }

    if (!found) {
        return false;
    }

    s_slot = best_slot;
    s_seq  = best_seq;
    if (!slot_read((best_slot == 0) ? SLOT0_ADDR : SLOT1_ADDR, out, &seq)) {
        return false;
    }
    return true;
}

/* ======================================================================
 * 对外接口
 * ==================================================================== */
bool STORAGE_Init(void)
{
    uint32_t id = 0U;

    s_ready       = false;
    s_slot        = -1;
    s_seq         = 0U;
    s_cache_valid = false;

    flash_cs(true);
    HAL_Delay(1U);   /* 上电后给 Flash 一点准备时间 */

    if (!flash_read_jedec(&id)) {
        return false;
    }
    if (id != BOARD_FLASH_JEDEC_ID) {
        return false;
    }

    s_ready = true;

    /* 预扫描：建立"最新记录在哪个槽"的缓存，供 Save 选择下一个槽 */
    if (scan_latest(&s_cache)) {
        s_cache_valid = true;
    }
    return true;
}

/**
 * @brief  查询 W25Q16 是否可用。
 * @return true = 初始化时 JEDEC ID 匹配成功
 */
bool STORAGE_IsReady(void)
{
    return s_ready;
}

/**
 * @brief  读取最近一条有效记录。
 * @param  out 输出结构，不可为 NULL
 * @return true = 存在有效记录（CRC 正确且序号最大）
 * @note   会顺便刷新内部缓存，供 STORAGE_Save 去重与选择下一个槽。
 */
bool STORAGE_Load(storage_saved_t *out)
{
    storage_saved_t tmp;
    uint16_t        seq;

    if ((out == NULL) || (!s_ready)) {
        return false;
    }
    if (!scan_latest(&tmp)) {
        out->valid = false;
        return false;
    }
    (void)seq;
    *out = tmp;
    s_cache = tmp;
    s_cache_valid = true;
    return true;
}

/**
 * @brief  保存文本记录（内容未变则直接返回，不擦写）。
 * @param  text      文本字节，1..256
 * @param  len       字节数
 * @param  symbology qr_symbology_t
 * @param  ecc       qr_ecc_t
 * @return true = 已写入并回读校验通过
 * @note   写入走"另一个槽"：先擦 4 KiB 扇区，再分两页编程（256 + 14 字节），
 * 最后回读比对。擦除期间阻塞约 40~60 ms，故调用方应放在 ACK 之后。
 */
bool STORAGE_Save(const uint8_t *text, uint16_t len, uint8_t symbology, uint8_t ecc)
{
    uint32_t base;
    uint16_t next_seq;
    uint16_t crc;
    int8_t   target;

    if ((!s_ready) || (text == NULL)) {
        return false;
    }
    if ((len == 0U) || (len > QR_TEXT_MAX_LEN)) {
        return false;
    }
    if ((symbology != (uint8_t)QR_SYM_QRCODE) || (ecc > (uint8_t)QR_ECC_H)) {
        return false;
    }

    /* 内容完全没变就不擦写：既省时间也保护擦写寿命 */
    if (s_cache_valid &&
        (s_cache.text_len == len) &&
        (s_cache.ecc == ecc) &&
        (s_cache.symbology == symbology) &&
        (memcmp(s_cache.text, text, len) == 0)) {
        return true;
    }

    /* 写另一个槽，保留当前槽作为掉电回退点 */
    target = (s_slot == 0) ? 1 : 0;
    base   = (target == 0) ? SLOT0_ADDR : SLOT1_ADDR;
    next_seq = (uint16_t)(s_seq + 1U);

    (void)memset(s_rec, 0, sizeof(s_rec));
    put_u32(&s_rec[0], REC_MAGIC);
    put_u16(&s_rec[4], (uint16_t)REC_FMT);
    put_u16(&s_rec[6], next_seq);
    s_rec[8] = symbology;
    s_rec[9] = ecc;
    put_u16(&s_rec[10], len);
    (void)memcpy(&s_rec[REC_TEXT_OFF], text, len);
    crc = crc16_modbus(s_rec, (uint16_t)REC_CRC_OFF);
    put_u16(&s_rec[REC_CRC_OFF], crc);

    if (!flash_sector_erase(base)) {
        return false;
    }
    /* 270 字节超过一次页编程上限（256），按页边界拆成两段 */
    if (!flash_page_program(base, s_rec, REC_FIRST_PAGE_SIZE)) {
        return false;
    }
    if (!flash_page_program(base + REC_FIRST_PAGE_SIZE,
                           &s_rec[REC_FIRST_PAGE_SIZE],
                           (uint16_t)(REC_SIZE - REC_FIRST_PAGE_SIZE))) {
        return false;
    }

    /* 回读校验：确保真的写进去了（也顺带验证 SPI 读通路） */
    if (!flash_read(base, s_verify, (uint16_t)REC_SIZE)) {
        return false;
    }
    if (memcmp(s_verify, s_rec, REC_SIZE) != 0) {
        return false;
    }

    s_slot  = target;
    s_seq   = next_seq;
    s_cache_valid = true;
    (void)memset(&s_cache, 0, sizeof(s_cache));
    s_cache.valid     = true;
    s_cache.symbology = symbology;
    s_cache.ecc       = ecc;
    s_cache.text_len  = len;
    (void)memcpy(s_cache.text, text, len);

    return true;
}

/**
 * @brief  擦除两个槽，清空持久化内容。
 * @return true = 两个扇区都擦除成功
 * @note   仅供维护/测试使用；正常运行不需要调用（清空后可重新开始记录）。
 */
bool STORAGE_EraseAll(void)
{
    if (!s_ready) {
        return false;
    }
    if (!flash_sector_erase(SLOT0_ADDR) || !flash_sector_erase(SLOT1_ADDR)) {
        return false;
    }
    s_slot = -1;
    s_seq = 0U;
    s_cache_valid = false;
    (void)memset(&s_cache, 0, sizeof(s_cache));
    (void)SLOT_BYTES;
    return true;
}
