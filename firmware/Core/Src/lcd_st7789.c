/**
 * @file    lcd_st7789.c
 * @brief   ST7789 4 线 SPI 驱动实现（无全屏帧缓冲，流式写 GRAM）。
 * @details
 *          写时序：CS 拉低 -> DC 区分命令/数据 -> SPI 发送 -> CS 拉高。
 *          像素数据通过 HAL_SPI_Transmit() 阻塞发送；一个 flush 区域发出后
 *          才允许 LVGL 复用缓冲区（见 lv_port_disp.c）。
 *
 *          @note 屏幕初始化序列取自 ST7789 数据手册推荐的 240x320 常规配置，
 *                并已针对常见 2.4 英寸 IPS 模块打开反色（INVON）。
 *                若上板后出现"颜色反相"或"红蓝互换"，只需修改
 *                board_config.h 中的 BOARD_LCD_INVERT_COLOR / BOARD_LCD_MADCTL_BGR。
 */

#include "lcd_st7789.h"
#include <string.h>

/* ======================================================================
 * ST7789 命令
 * ==================================================================== */
#define ST7789_CMD_SWRESET      0x01U   /**< 软件复位                       */
#define ST7789_CMD_SLPOUT       0x11U   /**< 退出睡眠                       */
#define ST7789_CMD_NORON        0x13U   /**< 普通显示模式                   */
#define ST7789_CMD_INVON        0x21U   /**< 打开反色                       */
#define ST7789_CMD_DISPON       0x29U   /**< 打开显示                       */
#define ST7789_CMD_CASET        0x2AU   /**< 列地址设置                     */
#define ST7789_CMD_RASET        0x2BU   /**< 行地址设置                     */
#define ST7789_CMD_RAMWR        0x2CU   /**< 显存写入                       */
#define ST7789_CMD_MADCTL       0x36U   /**< 存储访问控制                   */
#define ST7789_CMD_COLMOD       0x3AU   /**< 像素格式                       */
#define ST7789_CMD_PORCTRL      0xB2U   /**< 门廊控制                       */
#define ST7789_CMD_GCTRL        0xB7U   /**< 门驱动控制                     */
#define ST7789_CMD_VCOMS        0xBBU   /**< VCOM 设置                      */
#define ST7789_CMD_LCMCTRL      0xC0U   /**< LCM 控制                       */
#define ST7789_CMD_VDVVRHEN     0xC2U   /**< VDV/VRH 使能                   */
#define ST7789_CMD_VRHS         0xC3U   /**< VRH 设置                       */
#define ST7789_CMD_VDVS         0xC4U   /**< VDV 设置                       */
#define ST7789_CMD_FRCTRL2      0xC6U   /**< 刷新率控制                     */
#define ST7789_CMD_PWCTRL1      0xD0U   /**< 电源控制 1                     */
#define ST7789_CMD_PVGAMCTRL    0xE0U   /**< 正极性伽马                     */
#define ST7789_CMD_NVGAMCTRL    0xE1U   /**< 负极性伽马                     */

/** @brief MADCTL 位定义。 */
#define ST7789_MADCTL_MY        0x80U
#define ST7789_MADCTL_MX        0x40U
#define ST7789_MADCTL_MV        0x20U
#define ST7789_MADCTL_BGR       0x08U

/* ======================================================================
 * 内部状态
 * ==================================================================== */
static uint16_t s_width  = BOARD_LCD_WIDTH;
static uint16_t s_height = BOARD_LCD_HEIGHT;
static uint16_t s_x_off  = BOARD_LCD_X_OFFSET;
static uint16_t s_y_off  = BOARD_LCD_Y_OFFSET;
static uint8_t  s_rotation = BOARD_LCD_ROTATION;
static bool     s_ready  = false;

/** @brief 行缓冲：用于 FillRect / WritePixels 的字节序转换（240 x 2 = 480 B）。 */
static uint16_t s_line[BOARD_LCD_WIDTH];

/* ======================================================================
 * 底层：CS / DC / SPI
 * ==================================================================== */
/**
 * @brief  拉低片选，开始一次 SPI 事务。
 */
static inline void lcd_cs_low(void)
{
    HAL_GPIO_WritePin(BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN, GPIO_PIN_RESET);
}

/**
 * @brief  拉高片选，结束一次 SPI 事务。
 */
static inline void lcd_cs_high(void)
{
    HAL_GPIO_WritePin(BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN, GPIO_PIN_SET);
}

/**
 * @brief  把 DC 拉低，表示接下来发送的是**命令**。
 */
static inline void lcd_dc_cmd(void)
{
    HAL_GPIO_WritePin(BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN, GPIO_PIN_RESET);
}

/**
 * @brief  把 DC 拉高，表示接下来发送的是**数据/参数**。
 */
static inline void lcd_dc_data(void)
{
    HAL_GPIO_WritePin(BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN, GPIO_PIN_SET);
}

/**
 * @brief  发送一次 SPI 数据块。
 * @param  data 数据指针
 * @param  len  字节数
 * @param  tout 超时（ms）
 * @return true 成功
 */
static bool lcd_spi_tx(const uint8_t *data, uint32_t len, uint32_t tout)
{
    if ((data == NULL) || (len == 0U)) {
        return true;
    }
    return (HAL_SPI_Transmit(BOARD_LCD_SPI_HANDLE, (uint8_t *)data, (uint16_t)len, tout) == HAL_OK);
}

/**
 * @brief  发送命令 + 可选参数。
 * @param  cmd    命令字
 * @param  params 参数数组，可为 NULL
 * @param  len    参数个数
 */
static void lcd_write_cmd(uint8_t cmd, const uint8_t *params, uint8_t len)
{
    lcd_cs_low();
    lcd_dc_cmd();
    (void)lcd_spi_tx(&cmd, 1U, BOARD_LCD_SPI_TIMEOUT);

    if ((params != NULL) && (len != 0U)) {
        lcd_dc_data();
        (void)lcd_spi_tx(params, len, BOARD_LCD_SPI_TIMEOUT);
    }
    lcd_cs_high();
}

/**
 * @brief  仅发送数据（用于连续写显存）。
 * @param  data 数据指针
 * @param  len  字节数
 * @param  tout 超时（ms）
 */
static void lcd_write_data(const uint8_t *data, uint32_t len, uint32_t tout)
{
    lcd_cs_low();
    lcd_dc_data();
    (void)lcd_spi_tx(data, len, tout);
    lcd_cs_high();
}

/* ======================================================================
 * 初始化
 * ==================================================================== */
/**
 * @brief  用独立 GPIO 给 ST7789 做硬件复位。
 * @note   仅在 BOARD_LCD_USE_HW_RESET = 1 时编译进来：RST 拉低 20 ms 再拉高，
 * 之后等 120 ms 让内部寄存器稳定。本板 J4 无 RST 脚，默认关闭。
 */
static void lcd_hw_reset(void)
{
#if BOARD_LCD_USE_HW_RESET
    HAL_GPIO_WritePin(BOARD_LCD_RST_PORT, BOARD_LCD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(20U);
    HAL_GPIO_WritePin(BOARD_LCD_RST_PORT, BOARD_LCD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(120U);
#else
    /* 该核心板 J4 无 RST 引脚：使用 ST7789 软件复位命令（0x01） */
    lcd_write_cmd(ST7789_CMD_SWRESET, NULL, 0U);
    HAL_Delay(150U);
#endif
}

/**
 * @brief  根据 board_config.h 的旋转/颜色顺序写 MADCTL（0x36）。
 * @note   MX/ MY/ MV 位由 BOARD_LCD_ROTATION 决定，BGR 位由 BOARD_LCD_MADCTL_BGR 决定。
 * 换屏后若红蓝互换或方向不对，改这两个宏即可，不必改本函数。
 */
static void lcd_apply_madctl(void)
{
    uint8_t m = 0x00U;

    switch (s_rotation & 0x03U) {
    case 0U: m = 0x00U; break;                               /* 竖屏       */
    case 1U: m = ST7789_MADCTL_MV | ST7789_MADCTL_MX; break; /* 横屏       */
    case 2U: m = ST7789_MADCTL_MX | ST7789_MADCTL_MY; break; /* 竖屏翻转   */
    case 3U: m = ST7789_MADCTL_MV | ST7789_MADCTL_MY; break; /* 横屏翻转   */
    default: m = 0x00U; break;
    }

#if BOARD_LCD_MADCTL_BGR
    m |= ST7789_MADCTL_BGR;
#endif

    lcd_write_cmd(ST7789_CMD_MADCTL, &m, 1U);
}

/**
 * @brief  初始化 ST7789：复位、上电序列、颜色/旋转、清屏、点亮背光。
 * @note   接口契约见 lcd_st7789.h。J4 无 RESET 引脚时走软件复位（0x01）并等 150 ms。
 * @note   结尾会使能背光，因此调用后屏幕立即变为白底（清屏色）。
 */
void ST7789_Init(void)
{
    static const uint8_t porctrl[]  = {0x0CU, 0x0CU, 0x00U, 0x33U, 0x33U};
    static const uint8_t gctrl[]    = {0x35U};
    static const uint8_t vcoms[]    = {0x19U};
    static const uint8_t lcmctrl[]  = {0x2CU};
    static const uint8_t vdvvrhen[] = {0x01U};
    static const uint8_t vrhs[]     = {0x12U};
    static const uint8_t vdvs[]     = {0x20U};
    static const uint8_t frctrl2[]  = {0x0FU};
    static const uint8_t pwctrl1[]  = {0xA4U, 0xA1U};
    static const uint8_t pgamma[]   = {0xD0U, 0x04U, 0x0DU, 0x11U, 0x13U, 0x2BU, 0x3FU,
                                       0x54U, 0x4CU, 0x18U, 0x0DU, 0x0BU, 0x1FU, 0x23U};
    static const uint8_t ngamma[]   = {0xD0U, 0x04U, 0x0CU, 0x11U, 0x13U, 0x2CU, 0x3FU,
                                       0x44U, 0x51U, 0x2FU, 0x1FU, 0x1FU, 0x20U, 0x23U};

    /* 先建立确定的引脚电平，避免上电毛刺 */
    lcd_cs_high();
    lcd_dc_data();
    ST7789_SetBacklight(false);

    lcd_hw_reset();

    lcd_write_cmd(ST7789_CMD_SLPOUT, NULL, 0U);
    HAL_Delay(120U);

    {
        uint8_t colmod = 0x55U;   /* RGB565，16 bit/pixel */
        lcd_write_cmd(ST7789_CMD_COLMOD, &colmod, 1U);
    }

    lcd_apply_madctl();

    lcd_write_cmd(ST7789_CMD_PORCTRL, porctrl, (uint8_t)sizeof(porctrl));
    lcd_write_cmd(ST7789_CMD_GCTRL, gctrl, (uint8_t)sizeof(gctrl));
    lcd_write_cmd(ST7789_CMD_VCOMS, vcoms, (uint8_t)sizeof(vcoms));
    lcd_write_cmd(ST7789_CMD_LCMCTRL, lcmctrl, (uint8_t)sizeof(lcmctrl));
    lcd_write_cmd(ST7789_CMD_VDVVRHEN, vdvvrhen, (uint8_t)sizeof(vdvvrhen));
    lcd_write_cmd(ST7789_CMD_VRHS, vrhs, (uint8_t)sizeof(vrhs));
    lcd_write_cmd(ST7789_CMD_VDVS, vdvs, (uint8_t)sizeof(vdvs));
    lcd_write_cmd(ST7789_CMD_FRCTRL2, frctrl2, (uint8_t)sizeof(frctrl2));
    lcd_write_cmd(ST7789_CMD_PWCTRL1, pwctrl1, (uint8_t)sizeof(pwctrl1));
    lcd_write_cmd(ST7789_CMD_PVGAMCTRL, pgamma, (uint8_t)sizeof(pgamma));
    lcd_write_cmd(ST7789_CMD_NVGAMCTRL, ngamma, (uint8_t)sizeof(ngamma));

#if BOARD_LCD_INVERT_COLOR
    lcd_write_cmd(ST7789_CMD_INVON, NULL, 0U);
#endif

    lcd_write_cmd(ST7789_CMD_NORON, NULL, 0U);
    HAL_Delay(10U);
    lcd_write_cmd(ST7789_CMD_DISPON, NULL, 0U);
    HAL_Delay(20U);

    ST7789_FillScreen(BOARD_COLOR_BLACK);
    ST7789_SetBacklight(true);
    s_ready = true;
}

/**
 * @brief  运行时切换屏幕方向并重设坐标映射。
 * @note   会重写 MADCTL 并交换逻辑宽高；LVGL 侧分辨率不会自动跟随，
 * 因此固件只在初始化时使用，运行中切换需同时重建 LVGL 显示。
 */
void ST7789_SetRotation(lcd_rotation_t rotation)
{
    s_rotation = (uint8_t)(rotation & 0x03U);

    if (s_rotation == LCD_ROTATION_0 || s_rotation == LCD_ROTATION_180) {
        s_width  = BOARD_LCD_WIDTH;
        s_height = BOARD_LCD_HEIGHT;
        s_x_off  = BOARD_LCD_X_OFFSET;
        s_y_off  = BOARD_LCD_Y_OFFSET;
    } else {
        s_width  = BOARD_LCD_HEIGHT;
        s_height = BOARD_LCD_WIDTH;
        s_x_off  = BOARD_LCD_Y_OFFSET;
        s_y_off  = BOARD_LCD_X_OFFSET;
    }

    lcd_apply_madctl();
    ST7789_SetWindow(0U, 0U, (uint16_t)(s_width - 1U), (uint16_t)(s_height - 1U));
}

/**
 * @brief  取当前逻辑宽度（像素）。
 */
uint16_t ST7789_GetWidth(void)
{
    return s_width;
}

/**
 * @brief  取当前逻辑高度（像素）。
 */
uint16_t ST7789_GetHeight(void)
{
    return s_height;
}

/* ======================================================================
 * 窗口与像素写入
 * ==================================================================== */
/**
 * @brief  设置 ST7789 的显示窗口（CASET/RASET）。
 * @note   内部会把 board_config.h 的 X/Y 偏移加进去，并按当前分辨率裁剪；
 * 随后所有 ST7789_WritePixels() 都写进这个窗口，坐标自动递增。
 */
void ST7789_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t caset[4];
    uint8_t raset[4];

    /* 边界裁剪，防止越界坐标写坏其他区域 */
    if (x0 >= s_width) {
        x0 = (uint16_t)(s_width - 1U);
    }
    if (y0 >= s_height) {
        y0 = (uint16_t)(s_height - 1U);
    }
    if (x1 >= s_width) {
        x1 = (uint16_t)(s_width - 1U);
    }
    if (y1 >= s_height) {
        y1 = (uint16_t)(s_height - 1U);
    }
    if (x1 < x0) {
        uint16_t t = x0; x0 = x1; x1 = t;
    }
    if (y1 < y0) {
        uint16_t t = y0; y0 = y1; y1 = t;
    }

    caset[0] = (uint8_t)((x0 + s_x_off) >> 8);
    caset[1] = (uint8_t)((x0 + s_x_off) & 0xFFU);
    caset[2] = (uint8_t)((x1 + s_x_off) >> 8);
    caset[3] = (uint8_t)((x1 + s_x_off) & 0xFFU);
    lcd_write_cmd(ST7789_CMD_CASET, caset, 4U);

    raset[0] = (uint8_t)((y0 + s_y_off) >> 8);
    raset[1] = (uint8_t)((y0 + s_y_off) & 0xFFU);
    raset[2] = (uint8_t)((y1 + s_y_off) >> 8);
    raset[3] = (uint8_t)((y1 + s_y_off) & 0xFFU);
    lcd_write_cmd(ST7789_CMD_RASET, raset, 4U);

    lcd_write_cmd(ST7789_CMD_RAMWR, NULL, 0U);
}

/**
 * @brief  向当前窗口连续写入 RGB565 像素。
 * @note   内部按行缓冲做字节序交换（SPI 需要高字节先发）；调用前必须先
 * ST7789_SetWindow()。单次最多一批 240 像素，避免行缓冲溢出。
 */
void ST7789_WritePixels(const uint16_t *pixels, uint32_t count)
{
    uint32_t i = 0U;

    if ((pixels == NULL) || (count == 0U)) {
        return;
    }

    /* 分批经行缓冲做字节序转换（SPI 需要高字节先发） */
    while (i < count) {
        uint32_t n = count - i;
        uint32_t k;
        if (n > BOARD_LCD_WIDTH) {
            n = BOARD_LCD_WIDTH;
        }
        for (k = 0U; k < n; k++) {
            uint16_t c = pixels[i + k];
            s_line[k] = (uint16_t)((c >> 8) | (c << 8));
        }
        lcd_write_data((const uint8_t *)s_line, n * 2U, 500U);
        i += n;
    }
}

/**
 * @brief  把主机内存中排列好的 RGB565 位图写入指定矩形区域。
 * @note   这是 LVGL flush 的落点：stride == w*2 时一次性整块发送，
 * 否则逐行发送，保证任意对齐宽度都正确。
 */
void ST7789_BlitRGB565(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint32_t stride, const uint8_t *data)
{
    uint32_t row;
    uint32_t total_ms;

    if ((data == NULL) || (w == 0U) || (h == 0U)) {
        return;
    }

    ST7789_SetWindow(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U));

    /* 按实际字节数估算超时（SPI 10.5 MHz 约 1.3 MB/s），并留足余量 */
    total_ms = ((uint32_t)w * (uint32_t)h * 2U) / 1000U + 100U;

    if (stride == (uint32_t)w * 2U) {
        /* 行跨度连续：一次性发送（LVGL 的常规路径） */
        lcd_write_data(data, (uint32_t)w * 2U * h, total_ms);
    } else {
        for (row = 0U; row < h; row++) {
            lcd_write_data(data + row * stride, (uint32_t)w * 2U, total_ms);
        }
    }
}

/**
 * @brief  用单一颜色填充矩形（自动裁剪到屏幕范围）。
 * @note   用于清屏、标题栏/状态栏底色；越界部分被裁掉，不会写出屏幕。
 */
void ST7789_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint16_t swapped;
    uint32_t row;

    if ((w == 0U) || (h == 0U)) {
        return;
    }

    if ((x >= s_width) || (y >= s_height)) {
        return;
    }
    if (((uint32_t)x + w) > s_width) {
        w = (uint16_t)(s_width - x);
    }
    if (((uint32_t)y + h) > s_height) {
        h = (uint16_t)(s_height - y);
    }
    if ((w == 0U) || (h == 0U)) {
        return;
    }

    swapped = (uint16_t)((color >> 8) | (color << 8));

    /* 行缓冲只有 BOARD_LCD_WIDTH 个像素，横向超宽时分段填充 */
    while (w > 0U) {
        uint16_t cw = (w > BOARD_LCD_WIDTH) ? (uint16_t)BOARD_LCD_WIDTH : w;

        for (row = 0U; row < cw; row++) {
            s_line[row] = swapped;
        }

        ST7789_SetWindow(x, y, (uint16_t)(x + cw - 1U), (uint16_t)(y + h - 1U));
        for (row = 0U; row < h; row++) {
            lcd_write_data((const uint8_t *)s_line, (uint32_t)cw * 2U, 500U);
        }

        x = (uint16_t)(x + cw);
        w = (uint16_t)(w - cw);
    }
}

/**
 * @brief  整屏填充单色。
 */
void ST7789_FillScreen(uint16_t color)
{
    ST7789_FillRect(0U, 0U, s_width, s_height, color);
}

/**
 * @brief  画一个像素。
 * @note   越界直接返回；主要用于上板时确认坐标原点与方向（四角打点）。
 */
void ST7789_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if ((x >= s_width) || (y >= s_height)) {
        return;
    }
    ST7789_SetWindow(x, y, x, y);
    ST7789_WritePixels(&color, 1U);
}

/* ======================================================================
 * 背光与状态
 * ==================================================================== */
/**
 * @brief  打开或关闭背光。
 * @note   极性由 BOARD_LCD_BL_ACTIVE_HIGH 决定（本板高电平点亮）；
 * ST7789_Init() 结束时会调用一次点亮。
 */
void ST7789_SetBacklight(bool on)
{
#if BOARD_LCD_BL_ACTIVE_HIGH
    GPIO_PinState st = on ? GPIO_PIN_SET : GPIO_PIN_RESET;
#else
    GPIO_PinState st = on ? GPIO_PIN_RESET : GPIO_PIN_SET;
#endif
    HAL_GPIO_WritePin(BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN, st);
}

/**
 * @brief  查询屏幕是否已完成初始化。
 * @note   只反映软件状态，不做硬件探测（SPI 是只写，读不回来）。
 */
bool ST7789_IsReady(void)
{
    return s_ready;
}
