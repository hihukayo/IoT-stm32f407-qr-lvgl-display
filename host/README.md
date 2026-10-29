# STM32 二维码生成系统 PC 上位机

本目录是“基于 STM32F407 的二维码生成与 LCD 显示系统”的 PC 上位机，使用
**Python 3.10+、Tkinter、pyserial** 实现。上位机通过 UART 二进制帧发送待编码
文本，接收生成结果，并可导出设备生成的 240×240、1bpp BMP 二维码图片。

> 说明：本目录中的软件已经完成协议、CRC、BMP 接收等单元测试；**未进行实物
> STM32 联调，也未验证实际扫码效果**。README 中的“通过”均指软件/单元测试通过。

## 1. 文件说明

| 文件 | 作用 |
| --- | --- |
| `main.py` | Tkinter 图形界面、串口事件轮询、图像预览与保存 |
| `protocol.py` | 协议常量、CRC-16/MODBUS、帧封装/流式解析、payload 编解码 |
| `crc.py` | CRC-32/ISO-HDLC 实现，供整份 BMP 校验 |
| `serial_worker.py` | 串口后台线程、非阻塞命令投递、ACK 匹配、超时与有限重试 |
| `image_receiver.py` | 图像接收状态机、`.part` 临时文件、offset/长度/CRC32/BMP 头校验 |
| `qr_decode.py` | 图片二维码解码（Pillow + zxing-cpp），供「导入图片」使用 |
| `requirements.txt` | Python 依赖：`pyserial>=3.5`（必需）、`Pillow` / `zxing-cpp`（导入图片功能） |
| `run.bat` | Windows 启动脚本，优先 `pyw/py`，其次 `pythonw/python` |
| `tests/` | 纯标准库 `unittest` 测试，不需要串口和硬件 |

## 2. 环境与安装

### 2.1 运行环境

- Windows 10/11（主要目标平台）。
- Python 3.10 或更高版本。
- Python 安装时需包含 **Tcl/Tk**（Windows 官方安装包默认勾选）。
- pyserial 3.5 或更高版本。

检查 Python 与 Tkinter：

```bat
python --version
python -c "import tkinter; print(tkinter.TkVersion)"
```

### 2.2 安装依赖

在本目录（`host`）打开命令提示符：

```bat
py -3 -m pip install -r requirements.txt
```

如果本机只有一个 Python 且 `python` 指向它，也可以使用：

```bat
python -m pip install -r requirements.txt
```

注意：依赖必须安装到实际启动 `main.py` 的那个 Python 解释器中。`run.bat` 优先
使用 `pyw.exe -3`，因此建议用 `py -3 -m pip` 安装。pyserial 采用函数内延迟导入：
即使未安装 pyserial，界面仍能启动，但刷新串口/连接时会给出中文安装提示。

### 2.3 启动

方式一：

```bat
py -3 main.py
```

如果 `python` 指向带 Tkinter 的正确解释器，也可以使用 `python main.py`。

方式二：双击 `run.bat`。脚本优先通过 Windows Python Launcher（`pyw.exe -3` /
`py.exe -3`）启动，其次使用 `pythonw.exe` / `python.exe`；无控制台启动时不会
保留错误输出，如果遇到启动问题，请改用 `python main.py` 查看错误信息。

## 3. 硬件连接与串口参数

- 默认串口参数：**115200、8 数据位、无校验、1 停止位、无流控（115200 8N1）**。
- 使用 3.3 V USB-TTL 模块连接 MCU UART：
  - PC 的 TX 接 MCU 的 RX，PC 的 RX 接 MCU 的 TX；
  - USB-TTL 与开发板必须共地；
  - 不得把传统 RS-232 正负电压直接接到 MCU。
- 上位机允许在界面中修改波特率，但必须与固件一致。
- 具体 USART 实例和引脚由固件/开发板决定，上位机不假设固定 COM 号。

## 4. 界面操作

### 4.1 连接

1. 打开开发板电源，连接 USB-TTL。
2. 点击“刷新”，在串口下拉框选择设备对应的 COM 口。
3. 选择波特率（默认 115200），点击“连接”。
4. 连接成功后，上位机会自动发送一次 `CMD_GET_STATUS`，读取设备是否已有可导出
   的二维码。若设备没有响应，仍可在成功生成二维码后直接导出。
5. 点击“断开”可主动关闭串口；窗口关闭时会自动停止工作线程、关闭串口并清理
   临时文件。

### 4.2 输入与生成

- 输入框下方实时显示 **UTF-8 字节数 / 256** 和字符数。中文通常占 3 字节，
  例如“二维码测试-中文 UTF-8”为 28 字节、14 个字符。
- 超过 256 UTF-8 字节时，计数变红，点击“生成并显示”会被阻止并弹出明确提示，
  不会静默截断。
- 空文本、包含 NUL 的文本也会被阻止。
- 码制下拉框显示 QR Code 和“Data Matrix (暂未支持)”。选择 Data Matrix 会自动
  回退并提示：当前版本未实现该码制。
- 纠错级别可选 L/M/Q/H，默认 M。
- 点击“生成并显示”发送 `CMD_SET_QR`。成功 ACK 后界面显示 QR 版本和模块数
  （例如“QR 版本 4，模块 33×33”）；失败时显示状态码和错误码，并保留上一张
  有效二维码（由固件行为保证）。

### 4.3 从图片导入二维码（图片 → 文本）

固件只能按文本现场生成二维码、**不能接收图片**，所以「把一张带二维码的图片显示到
屏幕上」的做法是在 PC 侧把图片解成文本，再交给设备重新编码。点 **“导入图片”**：

1. 选择 PNG / JPG / BMP / GIF / WEBP 等图片（屏幕截图、微信名片、文档插图都行）；
2. 上位机用 `zxing-cpp` 识别其中的二维码，把**原文填入输入框**并刷新字节计数；
3. 已经连接串口时**自动接着发送 `SET_QR`**，屏幕立刻换成新的码；未连接时只填
   文本，状态栏提示“连接串口后点「生成并显示」”；
4. 日志记录：码制、解出的字节数、图中识别到的码数量（多个时取第一个）、来源文件名。

边界：

- 解出的内容超过 **256 UTF-8 字节**时不发送，文本仍填入输入框供删减；
- 只认标准二维码/条码载荷；微信小程序码、公众号“太阳码”这类非标准图形解不出内容；
- 支持旋转、缩放、反色；首次失败会自动再做一次灰度 + 2 倍放大重试。

> 该功能需要 `Pillow` 与 `zxing-cpp`（见 `requirements.txt`）。未安装时点按钮会弹出
> 安装命令，界面其它功能不受影响。

### 4.4 导出 BMP

1. 生成成功，或设备状态显示已有有效二维码后，“导出 BMP”按钮可用。
2. 点击后发送 `CMD_GET_IMAGE`。设备先回 ACK，再发送
   `EVT_IMAGE_BEGIN/DATA/END`。
3. 上位机把数据写入**与目标文件同目录**的 `.part` 文件（同卷，保证成功时的一次性
   重命名可用；旧版本写在系统临时目录，保存到另一个盘会报 `WinError 17`），
   实时显示进度。
4. 只有同时满足以下条件才会把 `.part` 重命名为正式 `.bmp`：
   - offset 从 0 开始、连续、无重复、无缺口、无越界；
   - 累计长度与 BEGIN/END 声明一致（固定 7742 字节）；
   - 整文件 CRC-32/ISO-HDLC 与 END 一致；
   - BMP 文件头符合固定规格（240×240、1bpp、top-down、像素偏移 62、
     数据区 7680 字节）。
5. 校验成功后会先尝试用 `tkinter.PhotoImage(file=...)` 在界面预览；如果当前
   Tk 构建不支持 BMP，则自动调用 Windows 默认图片查看器打开该文件。
6. 随后弹出保存对话框，默认文件名格式为：

   ```text
   qr_YYYYMMDD_HHMMSS.bmp
   ```

   例如 `qr_20261028_230245.bmp`。可以选择任意目录保存。
7. 若保存对话框取消，临时 BMP 保留到窗口关闭时清理。
8. 图像流整体超时为 **10 秒**；ACK 等待超时为 **2 秒**。超时、offset 错误、
   CRC32 错误或无法重命名时，`.part` 会保留用于排查，**不会产生正式 `.bmp`**。

### 4.5 日志与状态

- 日志区域显示发送命令、SEQ、ACK、错误码、图像进度和保存路径。
- “清空日志”只清空界面文本，不影响串口。
- 状态栏显示连接状态、生成结果、图像进度和最终保存结果。

## 5. UART 协议摘要

完整字段顺序与 CRC 覆盖范围严格按需求文档第 5 节实现：

```text
SOF0(0xA5) | SOF1(0x5A) | VER(0x01) | CMD | SEQ |
LEN_L | LEN_H | PAYLOAD... | CRC_L | CRC_H
```

- `LEN` 为 payload 长度，uint16 小端，最大 512；总帧长 = `9 + LEN`。
- CRC 为 **CRC-16/MODBUS**：初值 `0xFFFF`，反射多项式 `0xA001`，低字节先发。
- CRC 覆盖 `VER, CMD, SEQ, LEN_L, LEN_H, PAYLOAD`；不覆盖两个 SOF 字节，也不
  覆盖 CRC 自身。
- 测试向量：ASCII `"123456789"` → `0x4B37`，传输字节 `37 4B`。
- 半帧 1 秒无新字节时，解析器丢弃不完整帧并重新搜索 `A5 5A`。

命令：

| CMD | 名称 | ACK/说明 |
| --- | --- | --- |
| `0x01` | `CMD_PING` | ACK `0x81` |
| `0x02` | `CMD_SET_QR` | ACK `0x82` |
| `0x03` | `CMD_GET_STATUS` | ACK `0x83`，5 字节 payload |
| `0x04` | `CMD_GET_IMAGE` | ACK `0x84`，随后发送图像事件 |
| `0x30` | `EVT_IMAGE_BEGIN` | 15 字节 payload |
| `0x31` | `EVT_IMAGE_DATA` | `offset:u32 LE` + chunk |
| `0x32` | `EVT_IMAGE_END` | `file_size:u32 LE + crc32:u32 LE` |

普通 ACK payload 为 4 字节：

```text
[status][error_code][qr_version][module_count]
```

`CMD_GET_STATUS` 的 ACK 为 5 字节：

```text
[status][last_qr_valid][symbology][qr_version][module_count]
```

错误码：0 NONE、1 PARAM_ERR、2 TOO_LONG、3 ENCODE_FAIL、4 NOT_READY、
5 BUSY、6 CRC_ERR、7 UNSUPPORTED、8 INTERNAL_ERR。

`CMD_SET_QR` payload 为 `[symbology][ecc][UTF-8 文本]`：
- symbology：1=QR，2=Data Matrix（当前未支持）；
- ecc：0=L、1=M、2=Q、3=H；
- 文本 1–256 字节，不含 NUL。

BMP 固定规格：
- 14 字节 `BITMAPFILEHEADER` + 40 字节 `BITMAPINFOHEADER` + 8 字节调色板；
- `bfOffBits=62`，`biWidth=240`，`biHeight=-240`，`biBitCount=1`，
  `biCompression=0`，`biSizeImage=7680`，`biClrUsed=2`；
- 调色板索引 0=黑、1=白；每行 32 字节，共 240 行；
- 文件总长 7742 字节；整文件 CRC-32/ISO-HDLC。

## 6. 线程与响应性

- `serial_worker` 独占串口：打开、读取、写入、关闭均在后台线程完成。
- 主线程调用 `send_command()` 仅投递请求并拿到 SEQ 票据，**不会 sleep 等 ACK**。
- ACK 匹配、2 秒超时、SET_QR 的显式有限重试均由工作线程处理。
- 工作线程只向 `queue.Queue` 放事件；Tk 主线程每 40 ms 用 `after()` 轮询并更新
  控件，绝不在后台线程操作 Tk。
- 图像传输过程中“生成并显示”按钮被禁用，避免设备 BUSY 与请求交错。

## 7. 测试

在 `host` 目录运行：

```bat
py -3 -m unittest discover -s tests -v
```

如果 `python` 指向带 Tkinter 的正确解释器，也可以使用：

```bat
python -m unittest discover -s tests -v
```

当前软件测试共 **64 项**，全部通过。除「导入图片」那 10 项（需要 Pillow +
zxing-cpp，缺失时自动跳过）外，其余测试仅使用 Python 标准库，不需要
pyserial、串口或 STM32 硬件。覆盖内容：

- **T-001**：CRC16/MODBUS `"123456789"` → `0x4B37`，低字节先发；
- 帧构造/解析往返；payload 含 `A5 5A`；两帧粘连；逐字节输入；
- 坏 CRC、`LEN=513`、错误 VER 丢弃后继续解析；
- 截断帧 + 1 秒超时重同步；
- **T-004**：合成 7742 字节 BMP 的头部字段校验；
- **T-005**：图像 offset 缺口、重复、越界、长度错误、CRC32 错误、BMP 头错误
  均失败且不产生正式 `.bmp`，`.part` 保留；
- **T-006**：中文 UTF-8 字节计数、256 字节边界、超长和 NUL 拒绝；
- 串口工作线程用假串口对象验证 ACK 匹配、显式 SET_QR 重试与缺 pyserial 提示；
- **导入图片**：仓库示例图解出 `Hello 123`、缩小一半仍可识别、一张图两个码时
  取第一个并计数、文件不存在/非图片/空白图给出中文错误、`byte_length` 按 UTF-8 计。

编译检查：

```bat
py -3 -m py_compile protocol.py crc.py serial_worker.py image_receiver.py main.py
```

## 8. 常见问题

### 8.1 提示“未安装 pyserial”

在 `host` 目录执行：

```bat
py -3 -m pip install -r requirements.txt
```

若公司/学校网络需要代理，请配置 pip 代理后重试。不要只复制 `serial` 目录，
建议使用 pip 安装。

### 8.2 刷新后没有串口

- 检查 USB-TTL 是否插好、驱动是否安装（设备管理器是否有 COM 口）。
- 换 USB 线或 USB 口，避免只有充电功能的线。
- 关闭占用该串口的其他软件（串口助手、Keil 调试器等）。
- 点击“刷新”重新枚举。

### 8.3 连接失败或提示端口被占用

- 确认 COM 号没有选错。
- 关闭其他串口工具。
- 确认驱动正常，开发板已上电。
- USB-TTL 电平必须为 3.3 V，且与开发板共地。

### 8.4 连接成功但生成时 ACK 超时

- 确认固件波特率与界面一致（默认 115200 8N1）。
- 确认 TX/RX 交叉连接。
- 查看日志中发送的 SEQ，确认固件确实实现了协议帧和 CRC。
- 连接后可先点击连接栏的“Ping”按钮发送 `CMD_PING`，验证链路是否正常。

### 8.5 中文显示/扫码异常

上位机只保证把文本按 UTF-8 原样发送，不修改编码。二维码扫描结果取决于固件
QR 库是否写入 UTF-8/ECI 标记以及扫码软件。需要在实物上验证并记录结果。

### 8.6 BMP 无法在界面预览

部分 Tk 8.6 构建不包含 BMP 解码器，程序会按需求回退到系统图片查看器。如果
系统查看器也无法打开，请查看日志中的临时 BMP 路径，用画图或其他软件检查。

### 8.7 图像接收失败，出现 `.part`

这是有意的保护行为：失败时不会把损坏文件重命名为 `.bmp`。请检查 USB 连接
质量、降低波特率、重试导出；窗口关闭时临时文件会被清理。日志会给出具体原因
（offset 缺口/重复、长度、CRC32、BMP 头等）。

### 8.8 缺少 Tkinter

Windows 官方 Python 安装包默认包含 Tcl/Tk。若提示缺少 Tkinter，请重新运行
安装程序并勾选 “tcl/tk and IDLE”，或安装同版本完整 Python。

## 9. 已知限制

1. **Data Matrix 未实现**：下拉框仅显示说明，选择后自动回退；不会向设备发送
   Data Matrix 请求。
2. **未进行实物联调**：所有测试均为软件测试/假串口测试，不声称已通过 LCD 显示、
   实际扫码或长期稳定性验证。
3. **固定 BMP 尺寸**：仅支持 240×240、1bpp、top-down、7742 字节；任何尺寸变化
   必须同步修改固件、协议文档和 `image_receiver.py`。
4. **单图像任务**：同一时间只处理一个 GET_IMAGE 流；图像传输过程中禁用生成按钮。
5. **默认不自动重试 SET_QR**：`send_command(..., retries=N)` 支持显式重试，且
   `retries` 仅对 SET_QR 生效、最大 2 次；默认 0，避免重复执行副作用。当前 GUI
   不开启自动重试。
6. **预览依赖 Tk/系统查看器**：Tk 无法解码 BMP 时会调用系统图片查看器；临时
   BMP 仍会在窗口关闭时清理。
7. **图像分片必须连续**：chunk 必须按 offset 连续到达，不支持乱序、丢块重传或
   压缩格式。
