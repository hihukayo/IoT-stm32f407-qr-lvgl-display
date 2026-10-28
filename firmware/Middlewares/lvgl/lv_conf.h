/**
 * @file    lv_conf.h
 * @brief   LVGL v9.6.0 工程裁剪配置（STM32F407VET6 / 240x320 ST7789 / RGB565）。
 * @details
 *          本文件只**覆盖**需要改动的选项，其余全部沿用 LVGL 的
 *          `lv_conf_internal.h` 默认值。版本固定为 v9.6.0，禁止与 v8 API 混用。
 *
 *          关键取舍：
 *          - 颜色格式：RGB565（显示缓冲），屏幕侧使用 RGB565_SWAPPED 让
 *            SPI 字节序无需在运行时逐像素交换；
 *          - 内存：使用 LVGL 自带的内存池（LV_STDLIB_BUILTIN），大小 40 KiB，
 *            不使用 C 库 malloc（避免 newlib heap/`_sbrk` 依赖）；
 *          - 操作系统：LV_OS_NONE（裸机主循环）；
 *          - 绘制：软件渲染，必须启用 I1（二维码 1bpp 位图）与 RGB565_SWAPPED；
 *          - 日志：关闭（节省 Flash，且本工程不需要串口打印调试信息）。
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*============================================================================
 * 颜色与显示
 *============================================================================*/
/** 默认颜色格式：16 位 RGB565（LV_COLOR_DEPTH 在 v9 已废弃，勿再使用） */
#define LV_COLOR_FORMAT_DEFAULT         LV_COLOR_FORMAT_RGB565

/*============================================================================
 * 内存与标准库
 *============================================================================*/
/** 使用 LVGL 内置内存池，避免依赖 C 库 malloc */
#define LV_USE_STDLIB_MALLOC            LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING            LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF           LV_STDLIB_BUILTIN

/** LVGL 堆大小（字节）。界面对象/样式/主题约需 8~12 KiB，40 KiB 留足余量。
 *  内存预算见 docs/memory-budget.md。 */
#define LV_MEM_SIZE                     (40U * 1024U)

/** 裸机，无操作系统 */
#define LV_USE_OS                       LV_OS_NONE

/*============================================================================
 * 刷新与 DPI
 *============================================================================*/
/** 默认刷新周期 30 ms（约 33 fps），对静态界面足够 */
#define LV_DEF_REFR_PERIOD              30
/** 屏幕 DPI，用于默认样式尺寸推算 */
#define LV_DPI_DEF                      130

/*============================================================================
 * 日志与断言
 *============================================================================*/
#define LV_USE_LOG                      0
#define LV_USE_ASSERT_NULL              1
#define LV_USE_ASSERT_MALLOC            1
#define LV_USE_ASSERT_MEM_INTEGRITY     0
#define LV_USE_ASSERT_STYLE             0
#define LV_USE_ASSERT_OBJ               0

/*============================================================================
 * 软件渲染器：必须支持 I1（二维码）与 RGB565_SWAPPED（SPI 字节序）
 *============================================================================*/
#define LV_USE_DRAW_SW                  1
#define LV_DRAW_SW_SUPPORT_I1           1
#define LV_DRAW_SW_SUPPORT_RGB565       1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 1
#define LV_DRAW_SW_SUPPORT_ARGB8888     1
#define LV_DRAW_SW_SUPPORT_L8           1
#define LV_DRAW_SW_SUPPORT_AL88         1
#define LV_DRAW_SW_SUPPORT_A8           1

/*============================================================================
 * 控件与主题
 *============================================================================*/
/** 本项目只用到 label / image（二维码）/ canvas，其余控件保持默认关闭由裁剪决定。
 *  这里不显式关闭，以便后续扩展时无需改动本文件。 */
#define LV_USE_LABEL                    1
#define LV_USE_IMAGE                    1
#define LV_USE_CANVAS                   1

/** 默认主题：提供基础字体/背景，使 label 无需逐个设置字体 */
#define LV_USE_THEME_DEFAULT            1
#define LV_USE_THEME_SIMPLE             0
#define LV_USE_THEME_MONO               0

/** 布局：仅启用 Flex，禁用 Grid 以省 Flash */
#define LV_USE_FLEX                     1
#define LV_USE_GRID                     0

/*============================================================================
 * 字体：只保留界面用到的字号
 *============================================================================*/
#define LV_FONT_MONTSERRAT_14           1
#define LV_FONT_MONTSERRAT_16           0
#define LV_FONT_MONTSERRAT_20           0
#define LV_FONT_MONTSERRAT_24           0
#define LV_FONT_DEFAULT                 &lv_font_montserrat_14

/*============================================================================
 * 不需要的子系统（显式关闭，避免误引入依赖）
 *============================================================================*/
#define LV_USE_PRIVATE_API              0
#define LV_USE_QRCODE                   0
#define LV_USE_BARCODE                  0
#define LV_USE_SYSMON                   0
#define LV_USE_PERF_MONITOR             0
#define LV_USE_MEM_MONITOR              0
#define LV_USE_SNAPSHOT                 0
#define LV_USE_TRANSLATION              0
#define LV_USE_FILE_EXPLORER            0
#define LV_USE_FRAGMENT                 0
#define LV_USE_OBJ_ID                   0
#define LV_USE_OBJ_ID_BUILTIN           0
#define LV_USE_OBJ_PROPERTY             0
#define LV_USE_FS_STDIO                 0
#define LV_USE_FS_POSIX                 0
#define LV_USE_FS_WIN32                 0
#define LV_USE_FS_FATFS                 0
#define LV_USE_FS_MEMFS                 0

#endif /* LV_CONF_H */
