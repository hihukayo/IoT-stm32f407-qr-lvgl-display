/**
 * @file    app.h
 * @brief   应用层：串口命令分派、二维码生成、界面刷新、BMP 图片流状态机。
 * @details
 *          线程/上下文约定（需求 3.1 FR-007、6.2 节）：
 *          - UART 中断只把收到的字节压入环形缓冲，**不做**帧解析以外的任何工作；
 *          - 帧解析、二维码编码、屏幕绘制、BMP 流发送全部在 `APP_Process()`（主循环）
 *            中完成，因此不会在中断里执行耗时操作；
 *          - BMP 图片流每帧只发送一个数据块，块与块之间主循环可以继续跑 LVGL，
 *            界面不会在导出期间"假死"。
 */

#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stdbool.h>
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UART 接收环形缓冲大小（字节），必须为 2 的幂。 */
#define APP_RX_RING_SIZE        512U

/** @brief BMP 图片流每块的最多数据字节数（payload = 4 + 该值，需 <= 512）。 */
#define APP_IMAGE_CHUNK_SIZE    448U

/** @brief 应用状态。 */
typedef enum {
    APP_STATE_IDLE      = 0,    /**< 空闲，可接受 SET_QR              */
    APP_STATE_STREAMING = 1     /**< 正在发送 BMP 图片流（忙）        */
} app_state_t;

/**
 * @brief  应用初始化：协议解析器、BMP 流、界面初始画面、启动串口中断接收。
 * @note   必须在 ST7789_Init()、LV_Port_Init()、UI_Init() 之后调用。
 */
void APP_Init(void);

/**
 * @brief  应用主循环任务：排空串口环形缓冲 -> 解析帧 -> 分派命令 -> 推进图片流。
 * @note   需在主循环中不加延时地反复调用；内部单次工作量有上限，不会长时间阻塞。
 */
void APP_Process(void);

/**
 * @brief  查询是否正在传输图片（忙）。
 * @return true 忙
 */
bool APP_IsStreaming(void);

/**
 * @brief  获取当前应用状态。
 * @return app_state_t
 */
app_state_t APP_GetState(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_H */
