"""STM32 二维码生成系统 PC 上位机（Tkinter）。

界面与串口解耦：
- 所有串口读写、ACK 等待、超时和图像事件解析都在 ``serial_worker`` 线程。
- 工作线程只向 ``queue.Queue`` 投递事件。
- Tk 主线程用 ``after()`` 轮询队列，绝不从后台线程直接操作控件。

本文件不导入 pyserial；即使系统未安装 pyserial，界面也能启动并在日志/状态栏
给出明确的安装提示。
"""

from __future__ import annotations

import os
import queue
import shutil
import sys
import tkinter as tk
import webbrowser
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any

import protocol
import serial_worker
from image_receiver import ImageReceiveResult, ImageReceiver

APP_TITLE = "STM32 二维码生成系统 - PC 上位机"
BAUD_RATES = ("9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600")
SYM_QR_TEXT = "QR Code"
SYM_DM_TEXT = "Data Matrix (暂未支持)"
DEFAULT_SAVE_NAME_FORMAT = "qr_%Y%m%d_%H%M%S.bmp"


class HostApp(tk.Tk):
    """上位机主窗口。"""

    def __init__(self) -> None:
        """创建窗口、控件、后台队列并启动事件轮询。"""
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("1040x720")
        self.minsize(920, 660)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # 串口与队列状态
        self._events: queue.Queue[Any] = queue.Queue()
        self._worker: serial_worker.SerialWorker | None = None
        self._connected = False
        self._connecting = False
        self._manual_disconnect = False
        self._last_qr_valid = False

        # 生成请求和图像接收状态
        self._pending_generate = False
        self._image_active = False
        self._image_receiver: ImageReceiver | None = None
        self._receivers: list[ImageReceiver] = []
        self._image_timer_id: str | None = None
        self._image_last_percent = -1
        self._image_last_log_percent = -1
        self._image_progress_var = tk.DoubleVar(value=0.0)

        self._preview_photo: tk.PhotoImage | None = None
        self._closing = False
        self._poll_job: str | None = None
        self._log_char_limit = 120_000

        self._configure_style()
        self._build_ui()
        self._refresh_ports(initial=True, show_dialog=False)
        self._update_byte_counter()
        self._refresh_controls()
        self._log("info", "上位机已启动。请选择串口、打开设备电源后点击“连接”。")
        self._poll_job = self.after(40, self._poll_events)

    # ------------------------------------------------------------------
    # 界面构建
    # ------------------------------------------------------------------
    def _configure_style(self) -> None:
        """设置适合 Windows 中文界面的字体与主题。"""
        try:
            self.option_add("*Font", ("Microsoft YaHei UI", 9))
        except tk.TclError:
            pass
        style = ttk.Style(self)
        try:
            if "vista" in style.theme_names():
                style.theme_use("vista")
        except tk.TclError:
            pass
        style.configure("Status.TLabel", padding=(6, 3))

    def _build_ui(self) -> None:
        """创建连接栏、输入区、预览区、日志区和状态栏。"""
        root = ttk.Frame(self, padding=(10, 8, 10, 6))
        root.grid(row=0, column=0, sticky="nsew")
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(0, weight=1)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(1, weight=1)
        root.rowconfigure(2, weight=1)

        # ---- 串口连接栏 ----
        connection = ttk.LabelFrame(root, text="串口连接", padding=(8, 6))
        connection.grid(row=0, column=0, sticky="ew", pady=(0, 7))
        connection.columnconfigure(8, weight=1)

        ttk.Label(connection, text="串口：").grid(row=0, column=0, sticky="w")
        self._port_var = tk.StringVar()
        self._port_combo = ttk.Combobox(
            connection, textvariable=self._port_var, width=18, state="readonly"
        )
        self._port_combo.grid(row=0, column=1, sticky="w", padx=(2, 4))

        self._refresh_btn = ttk.Button(
            connection, text="刷新", width=6, command=self._refresh_ports
        )
        self._refresh_btn.grid(row=0, column=2, padx=(0, 12))

        ttk.Label(connection, text="波特率：").grid(row=0, column=3, sticky="w")
        self._baud_var = tk.StringVar(value=str(protocol.DEFAULT_BAUDRATE))
        self._baud_combo = ttk.Combobox(
            connection,
            textvariable=self._baud_var,
            values=BAUD_RATES,
            width=10,
            state="readonly",
        )
        self._baud_combo.grid(row=0, column=4, sticky="w", padx=(2, 12))

        self._connect_btn = ttk.Button(
            connection, text="连接", width=8, command=self._on_connect
        )
        self._connect_btn.grid(row=0, column=5, padx=(0, 5))
        self._disconnect_btn = ttk.Button(
            connection, text="断开", width=8, command=self._on_disconnect
        )
        self._disconnect_btn.grid(row=0, column=6)

        self._ping_btn = ttk.Button(
            connection, text="Ping", width=7, command=self._on_ping
        )
        self._ping_btn.grid(row=0, column=7, padx=(5, 0))

        self._port_hint_var = tk.StringVar(value="默认 115200 8N1")
        ttk.Label(
            connection, textvariable=self._port_hint_var, foreground="#666666"
        ).grid(row=0, column=8, sticky="e", padx=(12, 0))

        # ---- 中部：输入与预览 ----
        content = ttk.Frame(root)
        content.grid(row=1, column=0, sticky="nsew", pady=(0, 7))
        content.columnconfigure(0, weight=3)
        content.columnconfigure(1, weight=2)
        content.rowconfigure(0, weight=1)

        left = ttk.LabelFrame(content, text="文本与生成", padding=(8, 6))
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        left.columnconfigure(0, weight=1)
        left.rowconfigure(1, weight=1)

        ttk.Label(left, text="待编码文本（UTF-8，1–256 字节）：").grid(
            row=0, column=0, sticky="w"
        )

        text_frame = ttk.Frame(left)
        text_frame.grid(row=1, column=0, sticky="nsew", pady=(3, 3))
        text_frame.columnconfigure(0, weight=1)
        text_frame.rowconfigure(0, weight=1)
        self._text = tk.Text(
            text_frame,
            height=9,
            wrap="word",
            undo=True,
            font=("Microsoft YaHei UI", 10),
            relief="solid",
            borderwidth=1,
            padx=6,
            pady=5,
        )
        self._text.grid(row=0, column=0, sticky="nsew")
        text_scroll = ttk.Scrollbar(
            text_frame, orient="vertical", command=self._text.yview
        )
        text_scroll.grid(row=0, column=1, sticky="ns")
        self._text.configure(yscrollcommand=text_scroll.set)
        self._text.bind("<<Modified>>", self._on_text_modified)

        self._byte_label = tk.Label(
            left,
            text="UTF-8 字节：0 / 256（字符：0）",
            anchor="w",
            fg="#1f6f43",
            font=("Microsoft YaHei UI", 9),
        )
        self._byte_label.grid(row=2, column=0, sticky="ew", pady=(0, 6))

        options = ttk.Frame(left)
        options.grid(row=3, column=0, sticky="ew", pady=(0, 6))
        options.columnconfigure(5, weight=1)

        ttk.Label(options, text="码制：").grid(row=0, column=0, sticky="w")
        self._symbology_var = tk.StringVar(value=SYM_QR_TEXT)
        self._symbology_combo = ttk.Combobox(
            options,
            textvariable=self._symbology_var,
            values=(SYM_QR_TEXT, SYM_DM_TEXT),
            width=24,
            state="readonly",
        )
        self._symbology_combo.grid(row=0, column=1, sticky="w", padx=(2, 16))
        self._symbology_combo.bind("<<ComboboxSelected>>", self._on_symbology_changed)

        ttk.Label(options, text="纠错级别：").grid(row=0, column=2, sticky="w")
        self._ecc_var = tk.StringVar(value="M")
        self._ecc_combo = ttk.Combobox(
            options,
            textvariable=self._ecc_var,
            values=("L", "M", "Q", "H"),
            width=6,
            state="readonly",
        )
        self._ecc_combo.grid(row=0, column=3, sticky="w", padx=(2, 0))

        action_row = ttk.Frame(left)
        action_row.grid(row=4, column=0, sticky="ew", pady=(0, 4))
        action_row.columnconfigure(4, weight=1)

        self._generate_btn = ttk.Button(
            action_row,
            text="生成并显示",
            width=14,
            command=self._on_generate,
        )
        self._generate_btn.grid(row=0, column=0, padx=(0, 6))
        self._export_btn = ttk.Button(
            action_row,
            text="导出 BMP",
            width=12,
            command=self._on_export_bmp,
        )
        self._export_btn.grid(row=0, column=1)

        self._qr_info_var = tk.StringVar(value="尚未生成二维码")
        ttk.Label(
            left,
            textvariable=self._qr_info_var,
            foreground="#1f4e79",
            wraplength=560,
        ).grid(row=5, column=0, sticky="w", pady=(3, 0))

        # ---- 右侧预览 ----
        right = ttk.LabelFrame(content, text="图像预览（240×240 BMP）", padding=(8, 6))
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(0, weight=1)

        self._preview_canvas = tk.Canvas(
            right,
            width=260,
            height=260,
            bg="#ffffff",
            highlightthickness=1,
            highlightbackground="#b8c2cc",
        )
        self._preview_canvas.grid(row=0, column=0, sticky="nsew")
        self._preview_canvas.create_text(
            130,
            130,
            text="尚未接收图像\n点击“导出 BMP”后在此预览",
            fill="#888888",
            justify="center",
            font=("Microsoft YaHei UI", 9),
        )

        self._image_progress = ttk.Progressbar(
            right,
            orient="horizontal",
            mode="determinate",
            maximum=100,
            variable=self._image_progress_var,
        )
        self._image_progress.grid(row=1, column=0, sticky="ew", pady=(6, 3))

        self._preview_path_var = tk.StringVar(value="")
        ttk.Label(
            right,
            textvariable=self._preview_path_var,
            foreground="#555555",
            wraplength=330,
            justify="left",
        ).grid(row=2, column=0, sticky="w")

        # ---- 日志 ----
        log_frame = ttk.LabelFrame(root, text="日志", padding=(8, 6))
        log_frame.grid(row=2, column=0, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(1, weight=1)

        log_toolbar = ttk.Frame(log_frame)
        log_toolbar.grid(row=0, column=0, sticky="ew", pady=(0, 3))
        log_toolbar.columnconfigure(0, weight=1)
        ttk.Label(
            log_toolbar,
            text="串口命令、ACK、图像进度和错误信息",
            foreground="#666666",
        ).grid(row=0, column=0, sticky="w")
        ttk.Button(
            log_toolbar, text="清空日志", width=9, command=self._clear_log
        ).grid(row=0, column=1, sticky="e")

        log_text_frame = ttk.Frame(log_frame)
        log_text_frame.grid(row=1, column=0, sticky="nsew")
        log_text_frame.columnconfigure(0, weight=1)
        log_text_frame.rowconfigure(0, weight=1)
        self._log_text = tk.Text(
            log_text_frame,
            height=9,
            wrap="none",
            state="disabled",
            font=("Consolas", 9),
            relief="solid",
            borderwidth=1,
            padx=5,
            pady=4,
        )
        self._log_text.grid(row=0, column=0, sticky="nsew")
        log_scroll_y = ttk.Scrollbar(
            log_text_frame, orient="vertical", command=self._log_text.yview
        )
        log_scroll_y.grid(row=0, column=1, sticky="ns")
        log_scroll_x = ttk.Scrollbar(
            log_text_frame, orient="horizontal", command=self._log_text.xview
        )
        log_scroll_x.grid(row=1, column=0, sticky="ew")
        self._log_text.configure(
            yscrollcommand=log_scroll_y.set, xscrollcommand=log_scroll_x.set
        )
        self._log_text.tag_configure("info", foreground="#1f3d2b")
        self._log_text.tag_configure("warning", foreground="#9a6700")
        self._log_text.tag_configure("error", foreground="#b00020")
        self._log_text.tag_configure("debug", foreground="#666666")

        # ---- 状态栏 ----
        self._status_var = tk.StringVar(value="未连接")
        status = ttk.Label(
            root,
            textvariable=self._status_var,
            style="Status.TLabel",
            relief="sunken",
            anchor="w",
        )
        status.grid(row=3, column=0, sticky="ew", pady=(7, 0))

    # ------------------------------------------------------------------
    # 通用界面辅助
    # ------------------------------------------------------------------
    def _log(self, level: str, message: str) -> None:
        """向日志区域追加一行（只允许在 Tk 主线程调用）。"""
        if level not in ("info", "warning", "error", "debug"):
            level = "info"
        timestamp = datetime.now().strftime("%H:%M:%S")
        try:
            self._log_text.configure(state="normal")
            self._log_text.insert("end", f"[{timestamp}] {message}\n", level)
            # 防止长时间运行后日志无限增长。
            if int(self._log_text.index("end-1c").split(".")[0]) > 4000:
                self._log_text.delete("1.0", "1000.0")
            if self._log_char_limit and len(self._log_text.get("1.0", "end-1c")) > self._log_char_limit:
                self._log_text.delete("1.0", "2000.0")
            self._log_text.see("end")
            self._log_text.configure(state="disabled")
        except tk.TclError:
            # 窗口正在销毁时忽略日志写入。
            pass

    def _clear_log(self) -> None:
        """清空日志区域。"""
        self._log_text.configure(state="normal")
        self._log_text.delete("1.0", "end")
        self._log_text.configure(state="disabled")

    def _set_status(self, text: str) -> None:
        """更新状态栏文本。"""
        self._status_var.set(text)

    def _refresh_controls(self) -> None:
        """根据连接、请求和图像传输状态统一刷新控件可用性。"""
        if self._closing:
            return
        connected = self._connected
        connecting = self._connecting
        busy_connection = connected or connecting
        has_port = bool(self._port_var.get().strip())

        self._connect_btn.configure(
            state="normal" if (not busy_connection and has_port) else "disabled"
        )
        self._disconnect_btn.configure(
            state="normal" if busy_connection else "disabled"
        )
        self._ping_btn.configure(state="normal" if connected else "disabled")
        self._refresh_btn.configure(state="disabled" if busy_connection else "normal")
        self._port_combo.configure(state="disabled" if busy_connection else "readonly")
        self._baud_combo.configure(state="disabled" if busy_connection else "readonly")

        generate_ok = connected and not self._pending_generate and not self._image_active
        self._generate_btn.configure(state="normal" if generate_ok else "disabled")

        export_ok = (
            connected
            and self._last_qr_valid
            and not self._pending_generate
            and not self._image_active
        )
        self._export_btn.configure(state="normal" if export_ok else "disabled")

    # ------------------------------------------------------------------
    # 串口枚举与连接
    # ------------------------------------------------------------------
    def _refresh_ports(self, initial: bool = False, show_dialog: bool = True) -> None:
        """刷新串口下拉框。"""
        if self._connected or self._connecting:
            return
        try:
            ports = serial_worker.list_serial_ports()
        except serial_worker.SerialWorkerError as exc:
            self._port_combo.configure(values=())
            self._port_var.set("")
            self._log("error", str(exc))
            self._set_status("串口不可用，请查看日志")
            self._port_hint_var.set("串口枚举失败")
            self._refresh_controls()
            if show_dialog and not initial:
                messagebox.showerror("串口不可用", str(exc), parent=self)
            return
        except Exception as exc:
            self._log("error", f"刷新串口时发生异常：{exc}")
            self._set_status("刷新串口失败")
            if show_dialog and not initial:
                messagebox.showerror("刷新串口失败", str(exc), parent=self)
            return

        values = tuple(port.device for port in ports)
        self._port_combo.configure(values=values)
        if values:
            current = self._port_var.get().strip()
            if current not in values:
                self._port_var.set(values[0])
            self._log(
                "info",
                "发现串口：" + "；".join(port.display_name for port in ports),
            )
            self._port_hint_var.set(f"共 {len(values)} 个串口")
            self._set_status(f"检测到 {len(values)} 个串口，请选择后连接")
        else:
            self._port_var.set("")
            self._port_hint_var.set("未发现串口")
            self._log(
                "warning",
                "未检测到串口。请检查 USB 转串口线、驱动和开发板供电。",
            )
            self._set_status("未检测到串口")
        self._refresh_controls()

    def _on_connect(self) -> None:
        """连接所选串口。"""
        if self._connected or self._connecting:
            return
        port = self._port_var.get().strip()
        if not port:
            messagebox.showwarning(
                "请选择串口",
                "没有可用的串口。请连接 USB 转串口设备，安装驱动后点击“刷新”。",
                parent=self,
            )
            return
        try:
            baudrate = int(self._baud_var.get().strip())
            if baudrate <= 0:
                raise ValueError
        except ValueError:
            messagebox.showerror("波特率无效", "请输入正整数波特率。", parent=self)
            return

        # 清掉上一次线程可能残留的事件，避免重连后误把旧线程的
        # “连接已断开”事件应用到新连接。
        self._drain_event_queue()
        self._manual_disconnect = False
        self._connecting = True
        self._set_status(f"正在连接 {port} …")
        self._log("info", f"正在打开串口 {port}，波特率 {baudrate}（8N1）…")
        self._refresh_controls()

        try:
            worker = serial_worker.SerialWorker(port, baudrate, self._events)
        except Exception as exc:
            self._connecting = False
            self._worker = None
            self._log("error", f"创建串口工作线程失败：{exc}")
            self._set_status("连接失败")
            self._refresh_controls()
            messagebox.showerror("连接失败", str(exc), parent=self)
            return

        self._worker = worker
        try:
            worker.start()
        except Exception as exc:
            self._worker = None
            self._connecting = False
            self._log("error", f"启动串口工作线程失败：{exc}")
            self._set_status("连接失败")
            self._refresh_controls()
            messagebox.showerror("连接失败", str(exc), parent=self)

    def _on_disconnect(self) -> None:
        """主动断开串口并停止工作线程。"""
        if not self._connected and not self._connecting and self._worker is None:
            return
        self._manual_disconnect = True
        self._log("info", "正在断开串口…")

        if self._image_active:
            self._abort_image("用户断开串口，图像接收已中止", show_dialog=False)

        worker = self._worker
        self._worker = None
        if worker is not None:
            try:
                worker.stop()
            except Exception as exc:
                self._log("warning", f"停止串口线程时出现异常：{exc}")
        # 线程已 join，清空其收尾事件（连接断开、未完成 ACK 等）。
        self._drain_event_queue()

        self._connected = False
        self._connecting = False
        self._pending_generate = False
        self._last_qr_valid = False
        self._qr_info_var.set("未连接设备")
        self._set_status("串口已断开")
        self._log("info", "串口已断开。")
        self._refresh_controls()

    def _on_connection_event(self, event: serial_worker.ConnectionEvent) -> None:
        """处理工作线程的串口连接/断开事件。"""
        if event.connected:
            # 用户在“正在连接”期间已经点击断开时，忽略迟到的连接成功事件。
            if self._worker is None or self._manual_disconnect:
                return
            self._connecting = False
            self._connected = True
            self._last_qr_valid = False
            self._log("info", event.message)
            self._set_status(
                f"已连接 {event.port} @ {event.baudrate}，正在读取设备状态…"
            )
            self._qr_info_var.set("已连接，正在读取设备状态")
            self._refresh_controls()
            self._send_get_status()
            return

        # 断开或打开失败
        manual = self._manual_disconnect or self._worker is None
        self._manual_disconnect = False
        self._worker = None
        was_connected = self._connected
        self._connected = False
        self._connecting = False
        self._pending_generate = False
        self._last_qr_valid = False
        self._qr_info_var.set("未连接设备")
        self._refresh_controls()

        if manual:
            return

        self._log("error", event.message or "串口连接已断开")
        self._set_status("串口未连接")
        if self._image_active:
            self._abort_image("串口连接已断开，图像接收中止", show_dialog=False)
        prefix = "串口连接断开" if was_connected else "串口连接失败"
        try:
            messagebox.showerror(prefix, event.message or "串口连接已断开", parent=self)
        except tk.TclError:
            pass

    def _on_ping(self) -> None:
        """发送 CMD_PING，用于快速验证串口链路。"""
        if not self._connected or self._worker is None:
            return
        try:
            ticket = self._worker.send_command(protocol.CMD_PING)
        except serial_worker.SerialWorkerError as exc:
            self._log("error", f"发送 PING 失败：{exc}")
            messagebox.showerror("PING 发送失败", str(exc), parent=self)
            return
        self._log("info", f"发送 PING（SEQ={ticket.seq}）")
        self._set_status("已发送 PING，等待设备应答…")

    def _send_get_status(self) -> None:
        """连接成功后查询设备当前状态，决定是否允许导出。"""
        if not self._connected or self._worker is None:
            return
        try:
            ticket = self._worker.send_command(protocol.CMD_GET_STATUS)
        except serial_worker.SerialWorkerError as exc:
            self._log("error", f"发送 GET_STATUS 失败：{exc}")
            return
        self._log("info", f"发送 GET_STATUS（SEQ={ticket.seq}）")

    # ------------------------------------------------------------------
    # 文本计数与生成
    # ------------------------------------------------------------------
    def _on_text_modified(self, _event: tk.Event[Any] | None = None) -> None:
        """文本框内容变化时刷新 UTF-8 字节计数。"""
        try:
            if self._text.edit_modified():
                self._text.edit_modified(False)
        except tk.TclError:
            return
        self._update_byte_counter()

    def _text_value(self) -> str:
        """读取文本框内容，不包含 Tk 自动附加的末尾换行。"""
        return self._text.get("1.0", "end-1c")

    def _update_byte_counter(self) -> None:
        """按 UTF-8 而不是字符数更新字节计数。"""
        text = self._text_value()
        byte_length = len(text.encode("utf-8"))
        characters = len(text)
        if byte_length > protocol.MAX_TEXT_BYTES:
            over = byte_length - protocol.MAX_TEXT_BYTES
            self._byte_label.configure(
                text=(
                    f"UTF-8 字节：{byte_length} / {protocol.MAX_TEXT_BYTES}"
                    f"（字符：{characters}）——超出 {over} 字节，禁止发送"
                ),
                fg="#b00020",
            )
        else:
            self._byte_label.configure(
                text=(
                    f"UTF-8 字节：{byte_length} / {protocol.MAX_TEXT_BYTES}"
                    f"（字符：{characters}）"
                ),
                fg="#1f6f43",
            )

    def _on_symbology_changed(self, _event: tk.Event[Any] | None = None) -> None:
        """Data Matrix 未实现：用户选中时立即回退并说明原因。"""
        if self._symbology_var.get() != SYM_QR_TEXT:
            self._symbology_var.set(SYM_QR_TEXT)
            self._log("warning", "Data Matrix 暂未实现，当前只支持 QR Code。")
            messagebox.showinfo(
                "暂未支持",
                "Data Matrix 编码尚未实现，本版本只能生成 QR Code。",
                parent=self,
            )

    def _on_generate(self) -> None:
        """校验文本和参数，发送 CMD_SET_QR。"""
        if not self._connected or self._worker is None:
            messagebox.showwarning("未连接", "请先连接串口。", parent=self)
            return
        if self._pending_generate:
            return

        if self._symbology_var.get() != SYM_QR_TEXT:
            messagebox.showwarning(
                "码制不支持",
                "Data Matrix 暂未实现，请选择 QR Code。",
                parent=self,
            )
            return

        text = self._text_value()
        raw_bytes = text.encode("utf-8")
        if not raw_bytes:
            messagebox.showwarning(
                "文本为空", "请输入 1–256 字节的 UTF-8 文本。", parent=self
            )
            self._text.focus_set()
            return
        if len(raw_bytes) > protocol.MAX_TEXT_BYTES:
            reason = (
                f"当前文本为 {len(raw_bytes)} 个 UTF-8 字节，"
                f"超过 {protocol.MAX_TEXT_BYTES} 字节上限。\n"
                "中文通常每个字符占 3 字节，请删减内容后重试。"
            )
            self._log("error", reason.replace("\n", " "))
            self._set_status("文本超过 256 UTF-8 字节，已阻止发送")
            messagebox.showerror("文本过长", reason, parent=self)
            self._text.focus_set()
            return
        if b"\x00" in raw_bytes:
            messagebox.showerror(
                "文本非法", "文本不能包含 NUL（0x00）字节。", parent=self
            )
            return

        ecc = protocol.ECC_BY_NAME.get(self._ecc_var.get(), protocol.ECC_M)
        try:
            payload = protocol.encode_set_qr(text, ecc=ecc)
        except protocol.ProtocolError as exc:
            self._log("error", f"生成 SET_QR payload 失败：{exc}")
            messagebox.showerror("参数错误", str(exc), parent=self)
            return

        try:
            ticket = self._worker.send_command(
                protocol.CMD_SET_QR, payload, timeout=protocol.ACK_TIMEOUT
            )
        except serial_worker.SerialWorkerError as exc:
            self._log("error", f"发送 SET_QR 失败：{exc}")
            messagebox.showerror("发送失败", str(exc), parent=self)
            return

        self._pending_generate = True
        self._qr_info_var.set("已发送 SET_QR，等待设备应答…")
        self._set_status("等待设备生成二维码（ACK 超时 2 秒）…")
        self._log(
            "info",
            f"发送 SET_QR（SEQ={ticket.seq}，ECC={protocol.ECC_NAMES.get(ecc, '?')}，"
            f"文本 {len(raw_bytes)} 字节）",
        )
        self._refresh_controls()

    # ------------------------------------------------------------------
    # ACK 处理
    # ------------------------------------------------------------------
    def _ack_reason(self, event: serial_worker.AckEvent) -> str:
        """把超时或设备错误 ACK 转换成中文原因。"""
        if event.message:
            return event.message
        if event.timed_out:
            return "等待设备 ACK 超时"
        if event.status != protocol.STATUS_OK:
            return (
                f"{protocol.status_text(event.status)}"
                f"（错误码 {event.error_code}：{protocol.error_code_text(event.error_code)}）"
            )
        if event.error_code != protocol.ERR_NONE:
            return f"错误码 {event.error_code}：{protocol.error_code_text(event.error_code)}"
        return "未知错误"

    def _on_ack(self, event: serial_worker.AckEvent) -> None:
        """处理已匹配的 ACK，按请求命令更新界面。"""
        if not self._connected:
            # 断开串口时工作线程会把未完成请求标记为失败；此时不再弹错误框，
            # 连接断开事件本身已经给出明确提示。
            if event.cmd == protocol.CMD_SET_QR:
                self._pending_generate = False
                self._refresh_controls()
            self._log(
                "debug",
                f"连接已断开，忽略迟到的 ACK/失败事件：CMD=0x{event.cmd:02X}",
            )
            return

        attempts_note = f"，尝试 {event.attempts} 次" if event.attempts > 1 else ""

        if event.cmd == protocol.CMD_SET_QR:
            self._pending_generate = False
            self._refresh_controls()
            if event.ok:
                self._last_qr_valid = True
                self._qr_info_var.set(
                    f"生成成功：QR 版本 {event.qr_version}，"
                    f"模块数 {event.module_count}×{event.module_count}"
                )
                self._set_status(
                    f"生成成功：版本 {event.qr_version}，"
                    f"模块 {event.module_count}×{event.module_count}{attempts_note}"
                )
                self._log(
                    "info",
                    f"SET_QR 成功{attempts_note}：QR 版本 {event.qr_version}，"
                    f"模块 {event.module_count}×{event.module_count}",
                )
            else:
                reason = self._ack_reason(event)
                self._log("error", f"SET_QR 失败：{reason}")
                self._set_status(f"生成失败：{reason}")
                self._qr_info_var.set("生成失败；设备保留上一张有效二维码")
                self._refresh_controls()
                messagebox.showerror("生成失败", reason, parent=self)
            return

        if event.cmd == protocol.CMD_GET_STATUS:
            if event.timed_out:
                self._log(
                    "warning",
                    "GET_STATUS 等待 ACK 超时。可在生成成功后直接导出 BMP。",
                )
                self._set_status("设备状态查询超时")
            elif event.status != protocol.STATUS_OK:
                self._log(
                    "warning",
                    f"GET_STATUS 返回失败：{protocol.status_text(event.status)}",
                )
            else:
                self._last_qr_valid = event.last_qr_valid == 1
                self._log(
                    "info",
                    f"设备状态：last_qr_valid={event.last_qr_valid}，"
                    f"码制=0x{event.symbology or 0:02X}，"
                    f"版本={event.qr_version}，模块={event.module_count}",
                )
                if self._last_qr_valid:
                    self._qr_info_var.set(
                        f"设备已有有效二维码：版本 {event.qr_version}，"
                        f"模块 {event.module_count}×{event.module_count}"
                    )
                    self._set_status("设备已有可导出的二维码")
                else:
                    self._qr_info_var.set("设备当前没有可导出的二维码")
                    self._set_status("设备暂无有效二维码，请先生成")
            self._refresh_controls()
            return

        if event.cmd == protocol.CMD_GET_IMAGE:
            if not self._image_active:
                self._log("warning", "收到过期的 GET_IMAGE ACK，已忽略。")
                return
            if event.ok:
                self._log("info", "设备已确认 GET_IMAGE，开始接收 BMP 图像流…")
                self._set_status("图像传输中…")
            else:
                reason = self._ack_reason(event)
                self._log("error", f"GET_IMAGE 被拒绝：{reason}")
                self._abort_image(f"设备拒绝导出 BMP：{reason}", show_dialog=True)
            return

        if event.cmd == protocol.CMD_PING:
            if event.ok:
                self._log("info", f"PING 成功（SEQ={event.seq}{attempts_note}）")
            else:
                self._log("warning", f"PING 失败：{self._ack_reason(event)}")
            return

        self._log(
            "warning",
            f"收到未处理的 ACK：CMD=0x{event.cmd:02X}，SEQ={event.seq}",
        )

    # ------------------------------------------------------------------
    # 图像导出
    # ------------------------------------------------------------------
    def _on_export_bmp(self) -> None:
        """发送 CMD_GET_IMAGE 并创建临时 BMP 接收器。"""
        if not self._connected or self._worker is None:
            messagebox.showwarning("未连接", "请先连接串口。", parent=self)
            return
        if not self._last_qr_valid:
            messagebox.showwarning(
                "暂无可导出的二维码",
                "设备当前没有有效二维码。请先点击“生成并显示”。",
                parent=self,
            )
            return
        if self._image_active:
            return

        try:
            receiver = ImageReceiver()
        except OSError as exc:
            self._log("error", f"创建图像临时目录失败：{exc}")
            messagebox.showerror("无法导出", f"创建临时目录失败：{exc}", parent=self)
            return

        self._image_receiver = receiver
        self._receivers.append(receiver)
        self._image_active = True
        self._image_last_percent = -1
        self._image_last_log_percent = -1
        self._image_progress_var.set(0.0)
        self._preview_path_var.set("正在接收 BMP 图像流…")
        self._set_status("已请求导出 BMP，等待设备应答…")
        self._refresh_controls()

        try:
            ticket = self._worker.send_command(
                protocol.CMD_GET_IMAGE, timeout=protocol.ACK_TIMEOUT
            )
        except serial_worker.SerialWorkerError as exc:
            receiver.abort(f"发送 GET_IMAGE 失败：{exc}")
            self._log("error", f"发送 GET_IMAGE 失败：{exc}")
            self._finish_image_state()
            self._set_status("导出失败")
            messagebox.showerror("导出失败", str(exc), parent=self)
            return

        self._log("info", f"发送 GET_IMAGE（SEQ={ticket.seq}）")
        self._image_timer_id = self.after(
            int(protocol.IMAGE_TIMEOUT * 1000), self._on_image_timeout
        )

    def _on_image_event(self, event: Any) -> None:
        """把图像事件交给 ImageReceiver 并刷新进度/结果。"""
        receiver = self._image_receiver
        if not self._image_active or receiver is None:
            self._log("warning", "收到图像事件，但当前没有活动的导出任务，已忽略。")
            return

        result = receiver.feed(event)

        if isinstance(event, protocol.ImageBegin) and result is None:
            self._log(
                "info",
                f"图像开始：format={event.format}，{event.width}×{event.height}，"
                f"文件 {event.file_size} 字节，数据 {event.data_size} 字节，"
                f"行跨度 {event.row_stride}",
            )
            self._set_status(f"图像传输中：0 / {event.file_size} 字节")
            return

        if isinstance(event, protocol.ImageData) and result is None:
            percent = int(receiver.progress * 100)
            if percent != self._image_last_percent:
                self._image_last_percent = percent
                self._image_progress_var.set(float(percent))
            if percent >= self._image_last_log_percent + 10:
                self._image_last_log_percent = percent
                self._log(
                    "info",
                    f"图像接收进度 {percent}%："
                    f"{receiver.bytes_received} / {receiver.expected_size} 字节",
                )
            self._set_status(
                f"图像传输中：{receiver.bytes_received} / {receiver.expected_size} 字节"
            )
            return

        if result is None:
            return
        if result.success:
            self._on_image_success(result)
        else:
            self._on_image_failure(result)

    def _on_image_success(self, result: ImageReceiveResult) -> None:
        """图像校验成功：显示预览并提示保存路径。"""
        self._finish_image_state()
        self._image_progress_var.set(100.0)
        path = result.final_path
        if path is None:
            self._log("error", "图像接收成功但未返回正式文件路径。")
            return

        self._log(
            "info",
            f"BMP 接收并校验成功：{result.bytes_received} 字节，"
            f"CRC32=0x{result.crc32:08X}，临时文件 {path}",
        )
        self._set_status("BMP 校验成功，正在显示预览…")
        self._preview_path_var.set(f"临时预览文件：{path}")
        self._show_preview(path)
        self.update_idletasks()

        default_name = datetime.now().strftime(DEFAULT_SAVE_NAME_FORMAT)
        pictures = Path.home() / "Pictures"
        initial_dir = pictures if pictures.is_dir() else Path.home()
        try:
            selected = filedialog.asksaveasfilename(
                parent=self,
                title="保存二维码 BMP",
                defaultextension=".bmp",
                initialfile=default_name,
                initialdir=str(initial_dir),
                filetypes=(("BMP 图像", "*.bmp"), ("所有文件", "*.*")),
            )
        except tk.TclError:
            selected = ""

        if not selected:
            self._log(
                "warning",
                f"已取消保存；临时 BMP 将在关闭窗口时清理：{path}",
            )
            self._set_status("已取消保存，预览文件保留至窗口关闭")
            return

        target = Path(selected)
        try:
            if target.resolve() == path.resolve():
                raise OSError("目标文件与临时预览文件相同，请选择其他路径")
            shutil.copy2(path, target)
        except OSError as exc:
            self._log("error", f"保存 BMP 失败：{exc}")
            self._set_status("保存 BMP 失败")
            messagebox.showerror("保存失败", f"无法保存到：\n{target}\n\n{exc}", parent=self)
            return

        self._preview_path_var.set(f"已保存：{target}")
        self._set_status(f"BMP 已保存：{target}")
        self._log("info", f"BMP 已保存到：{target}")
        messagebox.showinfo("保存成功", f"二维码 BMP 已保存：\n{target}", parent=self)

    def _on_image_failure(self, result: ImageReceiveResult) -> None:
        """图像校验失败：保留 .part 并提示原因。"""
        self._finish_image_state()
        reason = result.error or "图像接收失败"
        part_note = (
            f"\n\n临时文件保留为 .part：\n{result.part_path}"
            if result.part_path is not None
            else ""
        )
        self._log("error", f"BMP 接收失败：{reason}{part_note.replace(chr(10), ' ')}")
        self._set_status("BMP 接收失败")
        self._preview_path_var.set("接收失败；未生成正式 .bmp 文件")
        messagebox.showerror(
            "BMP 接收失败",
            f"{reason}{part_note}\n\n不会生成正式 .bmp 文件。",
            parent=self,
        )

    def _abort_image(self, message: str, show_dialog: bool) -> None:
        """中止图像接收并更新状态。"""
        receiver = self._image_receiver
        result = (
            receiver.abort(message)
            if receiver is not None
            else ImageReceiveResult(
                success=False,
                final_path=None,
                part_path=None,
                bytes_received=0,
                expected_size=0,
                crc32=0,
                error=message,
            )
        )
        self._finish_image_state()
        self._log("error", f"图像接收中止：{message}")
        self._set_status("图像接收中止")
        if show_dialog:
            part_note = (
                f"\n\n临时文件保留为 .part：\n{result.part_path}"
                if result.part_path is not None
                else ""
            )
            messagebox.showerror(
                "BMP 接收失败",
                f"{message}{part_note}\n\n不会生成正式 .bmp 文件。",
                parent=self,
            )

    def _on_image_timeout(self) -> None:
        """图像流整体超时（默认 10 秒）。"""
        self._image_timer_id = None
        if not self._image_active:
            return
        self._abort_image(
            f"图像传输超过 {protocol.IMAGE_TIMEOUT:.0f} 秒，已中止；"
            "临时文件保留为 .part",
            show_dialog=True,
        )

    def _finish_image_state(self) -> None:
        """清理图像任务的定时器和界面状态。"""
        self._cancel_image_timer()
        self._image_active = False
        self._image_receiver = None
        self._refresh_controls()

    def _cancel_image_timer(self) -> None:
        """取消图像整体超时定时器。"""
        timer_id = self._image_timer_id
        self._image_timer_id = None
        if timer_id is None:
            return
        try:
            self.after_cancel(timer_id)
        except tk.TclError:
            pass

    def _show_preview(self, path: Path) -> None:
        """优先用 Tk PhotoImage 显示 BMP，失败时调用系统图片查看器。"""
        try:
            photo = tk.PhotoImage(file=str(path))
        except tk.TclError as exc:
            self._preview_photo = None
            self._preview_canvas.delete("all")
            self._preview_canvas.create_text(
                130,
                118,
                text="Tk 无法内置预览该 BMP\n正在尝试调用系统图片查看器…",
                fill="#555555",
                justify="center",
                width=220,
                font=("Microsoft YaHei UI", 9),
            )
            self._log("warning", f"Tk PhotoImage 无法预览 BMP：{exc}")
            opened = self._open_with_system_viewer(path)
            if opened:
                self._log("info", "已用系统图片查看器打开 BMP。")
                self._preview_path_var.set(f"已用系统查看器打开：{path}")
            else:
                self._log(
                    "warning",
                    "无法调用系统图片查看器，请手动打开上述 BMP 文件。",
                )
            return

        self._preview_photo = photo
        self._preview_canvas.delete("all")
        self._preview_canvas.create_image(130, 130, image=photo, anchor="center")
        self._preview_path_var.set(f"预览文件：{path}")

    def _open_with_system_viewer(self, path: Path) -> bool:
        """用操作系统默认程序打开文件，成功返回 True。"""
        try:
            if hasattr(os, "startfile"):
                os.startfile(str(path))  # type: ignore[attr-defined]
                return True
            return bool(webbrowser.open(path.resolve().as_uri()))
        except Exception:
            return False

    # ------------------------------------------------------------------
    # 工作线程事件轮询
    # ------------------------------------------------------------------
    def _drain_event_queue(self) -> None:
        """丢弃当前队列中的残留事件（仅在连接切换/断开时调用）。"""
        while True:
            try:
                self._events.get_nowait()
            except queue.Empty:
                return

    def _poll_events(self) -> None:
        """在主线程轮询队列；每个周期最多处理 200 条事件。"""
        if self._closing:
            return
        for _ in range(200):
            try:
                event = self._events.get_nowait()
            except queue.Empty:
                break
            try:
                self._handle_event(event)
            except Exception as exc:
                self._log("error", f"处理后台事件时发生内部错误：{exc}")
        if not self._closing:
            self._poll_job = self.after(40, self._poll_events)

    def _handle_event(self, event: Any) -> None:
        """按事件类型分派。"""
        if isinstance(event, serial_worker.ConnectionEvent):
            self._on_connection_event(event)
        elif isinstance(event, serial_worker.AckEvent):
            self._on_ack(event)
        elif isinstance(event, serial_worker.LogEvent):
            self._log(event.level, event.message)
        elif isinstance(
            event,
            (protocol.ImageBegin, protocol.ImageData, protocol.ImageEnd),
        ):
            self._on_image_event(event)
        else:
            self._log("warning", f"收到未知后台事件：{type(event).__name__}")

    # ------------------------------------------------------------------
    # 窗口关闭
    # ------------------------------------------------------------------
    def _on_close(self) -> None:
        """安全停止线程、关闭串口并清理临时文件后退出。"""
        if self._closing:
            return
        self._closing = True
        self._cancel_image_timer()
        self._set_status("正在关闭串口并清理临时文件…")
        try:
            self.update_idletasks()
        except tk.TclError:
            pass

        if self._image_active and self._image_receiver is not None:
            self._image_receiver.abort("窗口关闭，图像接收中止")
        self._image_active = False

        worker = self._worker
        self._worker = None
        if worker is not None:
            try:
                worker.stop()
            except Exception:
                pass

        for receiver in self._receivers:
            try:
                receiver.cleanup_temp()
            except Exception:
                pass

        if self._poll_job is not None:
            try:
                self.after_cancel(self._poll_job)
            except tk.TclError:
                pass
            self._poll_job = None

        try:
            self.destroy()
        except tk.TclError:
            pass


def main() -> int:
    """程序入口，返回进程退出码。"""
    try:
        app = HostApp()
    except tk.TclError as exc:
        print(f"无法创建 Tkinter 窗口：{exc}", file=sys.stderr)
        print("请确认当前 Python 已安装 Tkinter，并在图形桌面环境中运行。", file=sys.stderr)
        return 1
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
