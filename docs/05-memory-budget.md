# 内存预算与实测

## 1. 结论（实测，来自 map 文件）

| 区域 | 用量 | 容量 | 占比 |
|:--|--:|--:|--:|
| **Flash**（Code + RO + RW） | **338 360 B / 330.43 kB** | 512 KiB | **64.5 %** |
| **RAM**（RW + ZI） | **74 384 B / 72.64 kB** | 128 KiB | **56.7 %** |

```
Program Size: Code=316444  RO-data=21792  RW-data=124  ZI-data=74260
Total RO  Size (Code + RO Data)             338236 ( 330.31kB)
Total RW  Size (RW Data + ZI Data)           74384 (  72.64kB)
Total ROM Size (Code + RO Data + RW Data)   338360 ( 330.43kB)
```

> 数据来源：`firmware/MDK-ARM/Listings/QR_LVGL_F407.map`（Keil armlink 实测）。
> 剩余 Flash ≈ 181 kB、剩余 RAM ≈ 56 kB，仍可容纳第二种码制或更大的界面。

## 2. 关键点：没有全屏 framebuffer

| 方案 | 需要 RAM | 是否采用 |
|:--|--:|:--|
| 240×320×2 RGB565 全屏帧缓冲 | **153 600 B** | ❌ 明确禁用 |
| LVGL PARTIAL 局部缓冲 240×20 | 9 600 B | ✅ 采用 |
| 240×240 1bpp 二维码画布（RGB565 版） | 115 200 B | ❌ 改用 1bpp 7 680 B |

map 文件中**不存在**任何 153 600 B 的对象；ZI 总计 74 260 B，即使想放也放不下，
这正是需求 NFR-008 / T-112 要求的静态证据。

## 3. 静态 RAM 明细（map 实测，按大小排序）

| 符号 | 字节 | 所属模块 | 用途 |
|:--|--:|:--|:--|
| `lv_mem_init.work_mem_int` | 40 960 | `lv_mem_core_builtin.c` | LVGL 内置内存池（`LV_MEM_SIZE`） |
| `s_draw_buf` | 9 600 | `lv_port_disp.c` | LVGL 局部绘图缓冲 240×20 RGB565 |
| `g_qr_bitmap` | 7 680 | `qr_bitmap.c` | 240×240 1bpp 二维码位图（**显示 + BMP 共用**） |
| `s_qr_temp` | 1 177 | `qr_service.c` | qrcodegen 工作缓冲 |
| `s_qr_staging` | 1 177 | `qr_service.c` | 编码暂存（成功才提交，保证失败不破坏旧图） |
| `s_qr_active` | 1 177 | `qr_service.c` | 当前有效矩阵 |
| `s_tx` | 521 | `uart_protocol.c` | 发送装配缓冲（9+512） |
| `s_buf` | 521 | `uart_protocol.c` | 接收装配缓冲（9+512） |
| `s_frame` | 516 | `app.c` | 解析出的完整帧 |
| `s_rx_ring` | 512 | `app.c` | 串口环形缓冲 |
| `s_line` | 480 | `lcd_st7789.c` | 单行像素缓冲（字节序转换/填充） |
| `s_text` | 257 | `qr_service.c` | NUL 结尾的文本副本（256+1） |
| `lv_global` | 528 | `lv_init.c` | LVGL 全局状态 |
| `Stack_Mem`（STACK） | **8 192** | `startup_stm32f407xx.s` | 主栈（已从默认 1 KiB 提到 8 KiB） |
| `Heap_Mem`（HEAP） | 512 | `startup_stm32f407xx.s` | C 库堆（本项目不用 malloc，可保持很小） |
| 其它（HAL 句柄、计数器、BMP 头等） | ≈ 700 | 多处 | — |

## 4. 设计预算 vs 实测

| 项目 | 设计预算（需求文档） | 实测 | 说明 |
|:--|--:|--:|:--|
| LVGL 单局部缓冲 | 9 600 B | 9 600 B | 240×20×2，单缓冲 |
| 二维码 1bpp 位图 | ≈ 7 680 B | 7 680 B | 240×240÷8 = 7 200，+ 行补齐 → 7 680 |
| QR 编码矩阵 | ≤ 3×1 177 | 3 531 B | 版本上限 20，而非 40 |
| 协议缓冲 | ≤ 2×521 | 1 042 B | 收发各一 |
| LVGL 堆 | 建议 32–64 KiB | 40 960 B | 界面仅 6 个对象 |
| 栈 | 建议 ≥ 4 KiB | 8 192 B | LVGL 绘制递归余量 |
| **合计 ZI** | — | **74 260 B** | 剩 56 kB 余量 |

## 5. 若后续要改大/改小

| 需求 | 改动 | 影响 |
|:--|:--|:--|
| 界面更流畅 | 启用第二个 9 600 B 缓冲（`lv_display_set_buffers` 第 3 参数） | +9.6 kB RAM，需配合 DMA 才有收益 |
| 支持更高版本 QR（21–40） | 把 `QR_VERSION_MAX` 改 40，`QR_CODEGEN_BUF_LEN` 改 3 918 | +约 8 kB RAM |
| LVGL 对象更多 | 调大 `LV_MEM_SIZE` | 线性增加 RAM |
| 更小 footprint | 关掉 `LV_USE_THEME_DEFAULT`、减少控件 | 省 Flash 与 LVGL 堆 |
| 加大栈 | `startup_stm32f407xx.s` 的 `Stack_Size` | 线性增加 RAM |

> ⚠️ 所有 LVGL 缓冲与 DMA 可用缓冲都应留在普通 SRAM（`0x2000_0000`，128 KiB），
> **不要放到 CCM（`0x1000_0000`，64 KiB）**——CCM 不能被 DMA 访问。
> 本工程 `Ir2Chk=0`，链接器只会把数据放进 IRAM1，从根上避免了这个问题。
