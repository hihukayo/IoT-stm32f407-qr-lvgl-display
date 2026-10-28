"""串口后台工作线程。

设计要点：
- 串口对象只由工作线程打开、读取、发送和关闭。
- Tk 主线程调用 :meth:`SerialWorker.send_command` 仅投递请求，不等待 ACK；
  匹配 ACK、重试、超时和错误处理全部在工作线程完成，结果放入
  :class:`queue.Queue`，由界面用 ``after()`` 轮询。
- 本模块在函数内部延迟导入 pyserial。因此未安装 pyserial 时，协议模块、
  图像接收模块、测试和界面模块仍可正常导入。
"""

from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass
from enum import Enum
from typing import Any

import protocol

__all__ = [
    "AckEvent",
    "CommandTicket",
    "ConnectionEvent",
    "ConnectionState",
    "LogEvent",
    "PortInfo",
    "SerialWorker",
    "SerialWorkerError",
    "list_serial_ports",
    "pyserial_available",
]


class SerialWorkerError(RuntimeError):
    """串口不可用、参数错误或发送前的状态错误。"""


class ConnectionState(str, Enum):
    """串口连接状态。"""

    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    STOPPING = "stopping"


@dataclass(frozen=True)
class PortInfo:
    """一个可用串口的信息。"""

    device: str
    description: str = ""
    hwid: str = ""

    @property
    def display_name(self) -> str:
        """返回适合日志显示的端口描述。"""
        if self.description and self.description != "n/a":
            return f"{self.device}（{self.description}）"
        return self.device


@dataclass(frozen=True)
class ConnectionEvent:
    """串口连接建立或断开事件。"""

    connected: bool
    port: str
    baudrate: int
    message: str


@dataclass(frozen=True)
class LogEvent:
    """供界面写入日志区域的工作线程消息。"""

    level: str
    message: str


@dataclass(frozen=True)
class AckEvent:
    """已匹配请求的 ACK 结果，或请求超时/断开结果。"""

    seq: int
    cmd: int
    status: int
    error_code: int
    qr_version: int
    module_count: int
    last_qr_valid: int | None = None
    symbology: int | None = None
    payload: bytes = b""
    timed_out: bool = False
    attempts: int = 1
    message: str = ""

    @property
    def ok(self) -> bool:
        """请求是否成功完成。"""
        return (
            not self.timed_out
            and self.status == protocol.STATUS_OK
            and self.error_code == protocol.ERR_NONE
        )


@dataclass(frozen=True)
class CommandTicket:
    """一次发送请求的票据，仅用于日志和界面状态跟踪。"""

    seq: int
    cmd: int
    max_attempts: int = 1


@dataclass
class _PendingRequest:
    """工作线程内部等待 ACK 的请求。"""

    seq: int
    cmd: int
    payload: bytes
    timeout: float
    remaining_retries: int
    attempts: int = 1
    deadline: float = 0.0


def pyserial_available() -> bool:
    """返回当前环境是否安装了 pyserial。"""
    try:
        import serial  # noqa: F401
    except ImportError:
        return False
    return True


def _import_pyserial() -> Any:
    """延迟导入 pyserial，缺失时给出中文安装提示。"""
    try:
        import serial
    except ImportError as exc:
        raise SerialWorkerError(
            "未安装 pyserial。请先在本目录执行：python -m pip install -r requirements.txt"
        ) from exc
    return serial


def list_serial_ports() -> list[PortInfo]:
    """枚举系统可用串口，并返回按设备名排序的列表。

    异常:
        :class:`SerialWorkerError`: pyserial 未安装或枚举失败。
    """
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise SerialWorkerError(
            "未安装 pyserial，无法枚举串口。请执行：python -m pip install -r requirements.txt"
        ) from exc
    except Exception as exc:  # 某些损坏安装可能抛出非 ImportError
        raise SerialWorkerError(f"加载串口枚举模块失败：{exc}") from exc

    try:
        ports = list(list_ports.comports())
    except Exception as exc:
        raise SerialWorkerError(f"枚举串口失败：{exc}") from exc

    result = [
        PortInfo(
            device=str(item.device),
            description=str(getattr(item, "description", "") or ""),
            hwid=str(getattr(item, "hwid", "") or ""),
        )
        for item in ports
    ]
    result.sort(key=lambda item: item.device)
    return result


class SerialWorker:
    """维护一个串口连接的后台线程。

    典型用法::

        worker = SerialWorker("COM3", 115200, event_queue)
        worker.start()
        ticket = worker.send_command(protocol.CMD_PING)
        # 主线程轮询 event_queue 获取 AckEvent / 图像事件 / 连接事件
        worker.stop()
    """

    def __init__(
        self,
        port: str,
        baudrate: int = protocol.DEFAULT_BAUDRATE,
        event_queue: queue.Queue[Any] | None = None,
    ) -> None:
        """配置但暂不打开串口，需再调用 :meth:`start`。"""
        if not port or not str(port).strip():
            raise SerialWorkerError("串口名称不能为空")
        if not isinstance(baudrate, int) or baudrate <= 0:
            raise SerialWorkerError("波特率必须是正整数")

        self.port = str(port).strip()
        self.baudrate = baudrate
        self.events: queue.Queue[Any] = event_queue or queue.Queue()
        # 为调用方提供语义明确的队列别名。
        self.event_queue = self.events

        self._state = ConnectionState.DISCONNECTED
        self._state_lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._serial: Any | None = None
        self._stop_event = threading.Event()
        self._outbound: queue.Queue[_PendingRequest] = queue.Queue()
        self._seq_lock = threading.Lock()
        self._next_seq = 0
        self._parser = protocol.FrameParser()

    # ------------------------------------------------------------------
    # 生命周期与状态
    # ------------------------------------------------------------------
    @property
    def state(self) -> ConnectionState:
        """当前连接状态。"""
        with self._state_lock:
            return self._state

    @property
    def is_connected(self) -> bool:
        """是否已经完成串口打开。"""
        return self.state is ConnectionState.CONNECTED

    @property
    def is_running(self) -> bool:
        """工作线程是否仍在运行。"""
        thread = self._thread
        return bool(thread and thread.is_alive())

    def start(self) -> None:
        """启动后台线程；串口的实际打开在后台线程中完成。

        打开结果通过 ``ConnectionEvent`` 放入事件队列，不会在 Tk 主线程中
        阻塞等待驱动。
        """
        with self._state_lock:
            if self._thread is not None and self._thread.is_alive():
                raise SerialWorkerError("串口工作线程已经在运行")
            self._stop_event.clear()
            self._parser.reset()
            self._state = ConnectionState.CONNECTING
            self._thread = threading.Thread(
                target=self._run,
                name=f"SerialWorker-{self.port}",
                daemon=True,
            )
            self._thread.start()

    def stop(self) -> None:
        """请求停止并等待工作线程清理串口。

        正常情况下读取超时为 50 ms，线程会很快退出。如果线程被驱动阻塞，
        则尝试从外部关闭串口以解除阻塞，然后再等待一次。
        """
        self._stop_event.set()
        with self._state_lock:
            if self._state is not ConnectionState.DISCONNECTED:
                self._state = ConnectionState.STOPPING

        thread = self._thread
        if thread is None:
            self._set_state(ConnectionState.DISCONNECTED)
            return
        if thread is threading.current_thread():
            return
        if thread.is_alive():
            thread.join(timeout=2.0)
        if thread.is_alive():
            self._force_close_serial()
            thread.join(timeout=1.0)
        if not thread.is_alive():
            self._set_state(ConnectionState.DISCONNECTED)

    def _set_state(self, state: ConnectionState) -> None:
        """线程安全地更新连接状态。"""
        with self._state_lock:
            self._state = state

    # ------------------------------------------------------------------
    # 发送接口
    # ------------------------------------------------------------------
    def send_command(
        self,
        cmd: int,
        payload: bytes = b"",
        timeout: float = protocol.ACK_TIMEOUT,
        retries: int = 0,
    ) -> CommandTicket:
        """投递一个命令并立即返回票据，真正的等待发生在工作线程。

        参数:
            cmd: 命令 ID。
            payload: 原始 payload，最大 512 字节。
            timeout: 每个尝试等待 ACK 的秒数，默认 2 秒。
            retries: 显式重试次数。只有 ``CMD_SET_QR`` 允许重试，且最多 2 次；
                其他命令会强制为 0。默认不自动重试，避免重复副作用。

        异常:
            :class:`SerialWorkerError`: 未连接或参数非法。
        """
        if not self.is_connected:
            raise SerialWorkerError("串口尚未连接，无法发送命令")
        if timeout <= 0:
            raise SerialWorkerError("ACK 超时时间必须大于 0")
        if not isinstance(retries, int) or retries < 0:
            raise SerialWorkerError("重试次数必须是非负整数")

        try:
            payload_bytes = bytes(payload)
        except (TypeError, ValueError) as exc:
            raise SerialWorkerError("payload 必须为字节序列") from exc

        # 复用 build_frame 做长度和 ID 校验，序列号先用 0 占位。
        try:
            protocol.build_frame(cmd, 0, payload_bytes)
        except protocol.ProtocolError as exc:
            raise SerialWorkerError(str(exc)) from exc

        if cmd != protocol.CMD_SET_QR:
            retries = 0
        retries = min(retries, 2)

        seq = self._allocate_seq()
        request = _PendingRequest(
            seq=seq,
            cmd=cmd,
            payload=payload_bytes,
            timeout=float(timeout),
            remaining_retries=retries,
        )
        self._outbound.put(request)
        return CommandTicket(seq=seq, cmd=cmd, max_attempts=retries + 1)

    def _allocate_seq(self) -> int:
        """循环分配 0..255 的请求序号。"""
        with self._seq_lock:
            seq = self._next_seq
            self._next_seq = (self._next_seq + 1) & 0xFF
            return seq

    # ------------------------------------------------------------------
    # 工作线程主流程
    # ------------------------------------------------------------------
    def _run(self) -> None:
        """后台线程入口：打开串口、循环收发、退出时清理。"""
        serial_module: Any | None = None
        connection: Any | None = None
        pending: list[_PendingRequest] = []
        announced_connected = False
        failure_message = ""

        try:
            serial_module = _import_pyserial()
            connection = serial_module.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial_module.EIGHTBITS,
                parity=serial_module.PARITY_NONE,
                stopbits=serial_module.STOPBITS_ONE,
                timeout=0.05,
                write_timeout=1.0,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False,
            )
            self._serial = connection
            self._set_state(ConnectionState.CONNECTED)
            announced_connected = True
            self.events.put(
                ConnectionEvent(
                    connected=True,
                    port=self.port,
                    baudrate=self.baudrate,
                    message=f"串口 {self.port} 已连接（{self.baudrate} 8N1）",
                )
            )
            self._loop(connection, pending)
        except SerialWorkerError as exc:
            failure_message = str(exc)
        except Exception as exc:  # 包含 serial.SerialException、OSError 等
            failure_message = _format_serial_error(exc, self.port)
        finally:
            if connection is not None:
                try:
                    connection.close()
                except Exception:
                    pass
            self._serial = None
            disconnect_message = failure_message or "串口连接已断开"
            self._set_state(ConnectionState.DISCONNECTED)

            # 先通知界面连接已结束，再投递未完成请求的失败事件。这样界面
            # 不会在断线后又为每个 pending 请求弹出错误对话框。
            if announced_connected:
                self.events.put(
                    ConnectionEvent(
                        connected=False,
                        port=self.port,
                        baudrate=self.baudrate,
                        message=disconnect_message,
                    )
                )
            elif failure_message:
                self.events.put(
                    ConnectionEvent(
                        connected=False,
                        port=self.port,
                        baudrate=self.baudrate,
                        message=failure_message,
                    )
                )

            self._fail_pending(pending, disconnect_message)
            self._fail_outbound(disconnect_message)

    def _loop(self, connection: Any, pending: list[_PendingRequest]) -> None:
        """读取、解析、匹配 ACK 并检查超时的主循环。"""
        while not self._stop_event.is_set():
            self._process_outbound(connection, pending)

            try:
                data = connection.read(4096)
            except Exception as exc:
                raise RuntimeError(str(exc)) from exc

            now = time.monotonic()
            if data:
                frames = self._parser.feed(data, now=now)
                for frame in frames:
                    self._handle_frame(frame, pending)

            if self._parser.poll_timeout(now):
                self.events.put(
                    LogEvent(
                        "warning",
                        "串口帧接收 1 秒超时，已丢弃不完整帧并重新搜索 A5 5A",
                    )
                )

            self._expire_pending(connection, pending, now)

    def _process_outbound(self, connection: Any, pending: list[_PendingRequest]) -> None:
        """把主线程投递的命令写入串口，并加入等待 ACK 列表。"""
        while True:
            try:
                request = self._outbound.get_nowait()
            except queue.Empty:
                return

            frame = protocol.build_frame(request.cmd, request.seq, request.payload)
            try:
                connection.write(frame)
                connection.flush()
            except Exception as exc:
                message = f"串口发送失败：{exc}。请检查 USB 连接或重新连接串口。"
                self.events.put(
                    AckEvent(
                        seq=request.seq,
                        cmd=request.cmd,
                        status=protocol.STATUS_ERROR,
                        error_code=protocol.ERR_INTERNAL,
                        qr_version=0,
                        module_count=0,
                        timed_out=False,
                        attempts=request.attempts,
                        message=message,
                    )
                )
                raise RuntimeError(message) from exc

            request.deadline = time.monotonic() + request.timeout
            pending.append(request)

    def _handle_frame(self, frame: protocol.Frame, pending: list[_PendingRequest]) -> None:
        """按命令类型分派完整帧。"""
        if frame.cmd in (
            protocol.EVT_IMAGE_BEGIN,
            protocol.EVT_IMAGE_DATA,
            protocol.EVT_IMAGE_END,
        ):
            self._handle_image_frame(frame)
            return

        if frame.cmd in (
            protocol.ACK_PING,
            protocol.ACK_SET_QR,
            protocol.ACK_GET_STATUS,
            protocol.ACK_GET_IMAGE,
        ):
            self._handle_ack(frame, pending)
            return

        self.events.put(
            LogEvent(
                "warning",
                f"收到未知命令帧 CMD=0x{frame.cmd:02X}，SEQ={frame.seq}，已忽略",
            )
        )

    def _handle_ack(self, frame: protocol.Frame, pending: list[_PendingRequest]) -> None:
        """匹配请求序号并解析 ACK payload。"""
        request = next(
            (
                item
                for item in pending
                if item.seq == frame.seq and (item.cmd | protocol.ACK_CMD_OFFSET) == frame.cmd
            ),
            None,
        )
        if request is None:
            self.events.put(
                LogEvent(
                    "warning",
                    f"收到未匹配的 ACK CMD=0x{frame.cmd:02X}，"
                    f"SEQ={frame.seq}（可能已超时或重复）",
                )
            )
            return

        try:
            if request.cmd == protocol.CMD_GET_STATUS:
                status_ack = protocol.decode_get_status_ack(frame.payload)
                event = AckEvent(
                    seq=frame.seq,
                    cmd=request.cmd,
                    status=status_ack.status,
                    error_code=protocol.ERR_NONE
                    if status_ack.status == protocol.STATUS_OK
                    else protocol.ERR_INTERNAL,
                    qr_version=status_ack.qr_version,
                    module_count=status_ack.module_count,
                    last_qr_valid=status_ack.last_qr_valid,
                    symbology=status_ack.symbology,
                    payload=frame.payload,
                    attempts=request.attempts,
                )
            else:
                ack = protocol.decode_ack_payload(frame.payload)
                event = AckEvent(
                    seq=frame.seq,
                    cmd=request.cmd,
                    status=ack.status,
                    error_code=ack.error_code,
                    qr_version=ack.qr_version,
                    module_count=ack.module_count,
                    payload=frame.payload,
                    attempts=request.attempts,
                    message=""
                    if ack.ok
                    else f"{protocol.status_text(ack.status)}："
                    f"{protocol.error_code_text(ack.error_code)}",
                )
        except protocol.ProtocolError as exc:
            # payload 结构错误不能当成正常 ACK，让该请求继续等到超时。
            self.events.put(
                LogEvent(
                    "error",
                    f"ACK payload 解析失败（CMD=0x{frame.cmd:02X}，"
                    f"SEQ={frame.seq}）：{exc}",
                )
            )
            return

        pending.remove(request)
        self.events.put(event)

    def _handle_image_frame(self, frame: protocol.Frame) -> None:
        """解析图像事件并放入队列，由界面交给 ImageReceiver。"""
        try:
            if frame.cmd == protocol.EVT_IMAGE_BEGIN:
                event: Any = protocol.decode_image_begin(frame.payload)
            elif frame.cmd == protocol.EVT_IMAGE_DATA:
                event = protocol.decode_image_data(frame.payload)
            else:
                event = protocol.decode_image_end(frame.payload)
        except protocol.ProtocolError as exc:
            self.events.put(
                LogEvent(
                    "error",
                    f"图像事件解析失败（CMD=0x{frame.cmd:02X}，"
                    f"SEQ={frame.seq}）：{exc}",
                )
            )
            return
        self.events.put(event)

    def _expire_pending(
        self,
        connection: Any,
        pending: list[_PendingRequest],
        now: float,
    ) -> None:
        """处理 ACK 超时，并按需对 CMD_SET_QR 做有限次重试。"""
        while True:
            request = next(
                (item for item in pending if now >= item.deadline),
                None,
            )
            if request is None:
                return

            if request.remaining_retries > 0:
                request.remaining_retries -= 1
                request.attempts += 1
                request.seq = self._allocate_seq()
                request.deadline = now + request.timeout
                frame = protocol.build_frame(request.cmd, request.seq, request.payload)
                try:
                    connection.write(frame)
                    connection.flush()
                except Exception as exc:
                    message = f"串口重试发送失败：{exc}"
                    pending.remove(request)
                    self.events.put(
                        AckEvent(
                            seq=request.seq,
                            cmd=request.cmd,
                            status=protocol.STATUS_ERROR,
                            error_code=protocol.ERR_INTERNAL,
                            qr_version=0,
                            module_count=0,
                            timed_out=False,
                            attempts=request.attempts,
                            message=message,
                        )
                    )
                    raise RuntimeError(message) from exc

                self.events.put(
                    LogEvent(
                        "warning",
                        f"CMD=0x{request.cmd:02X} 等待 ACK 超时，"
                        f"正在第 {request.attempts - 1} 次重试（SEQ={request.seq}）",
                    )
                )
                continue

            pending.remove(request)
            command_name = _command_name(request.cmd)
            message = f"{command_name} 等待 ACK 超时（{request.timeout:.1f} 秒）"
            self.events.put(
                AckEvent(
                    seq=request.seq,
                    cmd=request.cmd,
                    status=protocol.STATUS_ERROR,
                    error_code=protocol.ERR_INTERNAL,
                    qr_version=0,
                    module_count=0,
                    timed_out=True,
                    attempts=request.attempts,
                    message=message,
                )
            )

    def _fail_pending(self, pending: list[_PendingRequest], message: str) -> None:
        """连接结束时把所有仍在等待的请求标记为失败。"""
        for request in list(pending):
            self.events.put(
                AckEvent(
                    seq=request.seq,
                    cmd=request.cmd,
                    status=protocol.STATUS_ERROR,
                    error_code=protocol.ERR_INTERNAL,
                    qr_version=0,
                    module_count=0,
                    attempts=request.attempts,
                    message=message,
                )
            )
        pending.clear()

    def _fail_outbound(self, message: str) -> None:
        """连接结束时把尚未写入串口的投递请求也标记为失败。"""
        while True:
            try:
                request = self._outbound.get_nowait()
            except queue.Empty:
                return
            self.events.put(
                AckEvent(
                    seq=request.seq,
                    cmd=request.cmd,
                    status=protocol.STATUS_ERROR,
                    error_code=protocol.ERR_INTERNAL,
                    qr_version=0,
                    module_count=0,
                    attempts=request.attempts,
                    message=message,
                )
            )

    def _force_close_serial(self) -> None:
        """在停止超时时从外部关闭串口，解除驱动阻塞。"""
        connection = self._serial
        if connection is None:
            return
        try:
            connection.close()
        except Exception:
            pass


def _format_serial_error(exc: BaseException, port: str) -> str:
    """把 pyserial/系统异常转换成可操作的中文提示。"""
    text = str(exc).strip() or exc.__class__.__name__
    return (
        f"串口 {port} 打开或通信失败：{text}。"
        "请确认端口存在且未被其他程序占用、USB 转串口驱动已安装、"
        "开发板已上电，并检查波特率是否与固件一致。"
    )


def _command_name(cmd: int) -> str:
    """返回命令的中文名称，用于日志。"""
    names = {
        protocol.CMD_PING: "PING",
        protocol.CMD_SET_QR: "SET_QR",
        protocol.CMD_GET_STATUS: "GET_STATUS",
        protocol.CMD_GET_IMAGE: "GET_IMAGE",
    }
    return names.get(cmd, f"CMD 0x{cmd:02X}")
