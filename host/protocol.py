"""STM32 二维码生成系统 UART 二进制协议（PC 端实现）。

协议固定格式（见需求规格说明第 5 节）::

    SOF0(0xA5) | SOF1(0x5A) | VER(0x01) | CMD | SEQ |
    LEN_L | LEN_H | PAYLOAD... | CRC_L | CRC_H

约定：
- LEN 为 PAYLOAD 长度，uint16 小端，最大 512；总帧长 = 9 + LEN。
- CRC 为 CRC-16/MODBUS，初值 0xFFFF，反射多项式 0xA001，低字节先发。
- CRC 覆盖 VER、CMD、SEQ、LEN_L、LEN_H 和全部 PAYLOAD，不覆盖 SOF 与 CRC 自身。
- 本模块只使用标准库，不导入 pyserial，便于无串口环境下运行单元测试。
"""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass
from typing import Callable, Iterable

from crc import Crc32, crc32_iso_hdlc

__all__ = [
    "SOF0",
    "SOF1",
    "SOF_BYTES",
    "PROTOCOL_VERSION",
    "FRAME_VER",
    "MAX_PAYLOAD",
    "FRAME_MIN_SIZE",
    "FRAME_TIMEOUT",
    "ACK_TIMEOUT",
    "IMAGE_TIMEOUT",
    "DEFAULT_BAUDRATE",
    "CMD_PING",
    "CMD_SET_QR",
    "CMD_GET_STATUS",
    "CMD_GET_IMAGE",
    "EVT_IMAGE_BEGIN",
    "EVT_IMAGE_DATA",
    "EVT_IMAGE_END",
    "ACK_PING",
    "ACK_SET_QR",
    "ACK_GET_STATUS",
    "ACK_GET_IMAGE",
    "ACK_CMD_OFFSET",
    "SYM_QR",
    "SYM_DATA_MATRIX",
    "ECC_L",
    "ECC_M",
    "ECC_Q",
    "ECC_H",
    "ECC_VALUES",
    "STATUS_OK",
    "STATUS_ERROR",
    "ERR_NONE",
    "ERR_PARAM",
    "ERR_TOO_LONG",
    "ERR_ENCODE_FAIL",
    "ERR_NOT_READY",
    "ERR_BUSY",
    "ERR_CRC",
    "ERR_UNSUPPORTED",
    "ERR_INTERNAL",
    "ERROR_TEXT",
    "STATUS_TEXT",
    "IMAGE_WIDTH",
    "IMAGE_HEIGHT",
    "IMAGE_FILE_SIZE",
    "IMAGE_DATA_SIZE",
    "IMAGE_ROW_STRIDE",
    "IMAGE_BMP_OFF_BITS",
    "IMAGE_FORMAT_BMP1",
    "ProtocolError",
    "FrameError",
    "PayloadError",
    "TextTooLongError",
    "Frame",
    "AckPayload",
    "StatusAckPayload",
    "SetQrPayload",
    "ImageBegin",
    "ImageData",
    "ImageEnd",
    "crc16_modbus",
    "crc16_modbus_bytes",
    "crc32_iso_hdlc",
    "Crc32",
    "build_frame",
    "parse_frame",
    "FrameParser",
    "utf8_byte_length",
    "encode_utf8_text",
    "encode_set_qr",
    "decode_set_qr",
    "decode_ack_payload",
    "decode_get_status_ack",
    "decode_status_ack",
    "encode_image_begin",
    "encode_image_data",
    "encode_image_end",
    "decode_image_begin",
    "decode_image_data",
    "decode_image_end",
    "error_code_text",
    "status_text",
]

# ---------------------------------------------------------------------------
# 帧常量
# ---------------------------------------------------------------------------
SOF0 = 0xA5
SOF1 = 0x5A
SOF_BYTES = bytes((SOF0, SOF1))
PROTOCOL_VERSION = 0x01
FRAME_VER = PROTOCOL_VERSION
MAX_PAYLOAD = 512
FRAME_MIN_SIZE = 9
FRAME_TIMEOUT = 1.0
ACK_TIMEOUT = 2.0
IMAGE_TIMEOUT = 10.0
DEFAULT_BAUDRATE = 115200

# 命令与事件 ID
CMD_PING = 0x01
CMD_SET_QR = 0x02
CMD_GET_STATUS = 0x03
CMD_GET_IMAGE = 0x04
EVT_IMAGE_BEGIN = 0x30
EVT_IMAGE_DATA = 0x31
EVT_IMAGE_END = 0x32

ACK_CMD_OFFSET = 0x80
ACK_PING = CMD_PING | ACK_CMD_OFFSET
ACK_SET_QR = CMD_SET_QR | ACK_CMD_OFFSET
ACK_GET_STATUS = CMD_GET_STATUS | ACK_CMD_OFFSET
ACK_GET_IMAGE = CMD_GET_IMAGE | ACK_CMD_OFFSET

# 码制与纠错等级
SYM_QR = 0x01
SYM_DATA_MATRIX = 0x02
ECC_L = 0
ECC_M = 1
ECC_Q = 2
ECC_H = 3
ECC_VALUES = (ECC_L, ECC_M, ECC_Q, ECC_H)
ECC_NAMES = {ECC_L: "L", ECC_M: "M", ECC_Q: "Q", ECC_H: "H"}
ECC_BY_NAME = {name: value for value, name in ECC_NAMES.items()}

# 状态码与错误码
STATUS_OK = 0
STATUS_ERROR = 1
ERR_NONE = 0
ERR_PARAM = 1
ERR_TOO_LONG = 2
ERR_ENCODE_FAIL = 3
ERR_NOT_READY = 4
ERR_BUSY = 5
ERR_CRC = 6
ERR_UNSUPPORTED = 7
ERR_INTERNAL = 8

STATUS_TEXT = {
    STATUS_OK: "成功",
    1: "失败",
}

ERROR_TEXT = {
    ERR_NONE: "无错误",
    ERR_PARAM: "参数错误",
    ERR_TOO_LONG: "文本过长",
    ERR_ENCODE_FAIL: "编码失败",
    ERR_NOT_READY: "尚未就绪或没有可导出的二维码",
    ERR_BUSY: "设备忙",
    ERR_CRC: "CRC 校验错误",
    ERR_UNSUPPORTED: "不支持的功能",
    ERR_INTERNAL: "设备内部错误",
}

# 图像固定规格：240×240、1bpp、top-down BMP
IMAGE_WIDTH = 240
IMAGE_HEIGHT = 240
IMAGE_FILE_SIZE = 7742
IMAGE_DATA_SIZE = 7680
IMAGE_ROW_STRIDE = 32
IMAGE_BMP_OFF_BITS = 62
IMAGE_FORMAT_BMP1 = 1
IMAGE_BEGIN_PAYLOAD_SIZE = 15
IMAGE_END_PAYLOAD_SIZE = 8
MAX_TEXT_BYTES = 256


# ---------------------------------------------------------------------------
# 异常类型
# ---------------------------------------------------------------------------
class ProtocolError(ValueError):
    """协议层错误的基类，消息均为可展示的中文。"""


class FrameError(ProtocolError):
    """完整帧格式、版本或 CRC 非法。"""


class PayloadError(ProtocolError):
    """命令 payload 长度、字段或编码非法。"""


class TextTooLongError(PayloadError):
    """UTF-8 文本超过 256 字节。"""

    def __init__(self, byte_length: int, max_bytes: int = MAX_TEXT_BYTES) -> None:
        """记录实际字节数和上限，供界面显示。"""
        self.byte_length = byte_length
        self.max_bytes = max_bytes
        super().__init__(
            f"文本编码后为 {byte_length} 字节，超过 {max_bytes} 字节上限（按 UTF-8 计算）"
        )


# ---------------------------------------------------------------------------
# CRC-16/MODBUS 与帧封装
# ---------------------------------------------------------------------------
def crc16_modbus(data: Iterable[int] | bytes | bytearray | memoryview) -> int:
    """计算 CRC-16/MODBUS。

    参数:
        data: 参与校验的字节序列。

    返回:
        16 位无符号整数，0xFFFF 初值、0xA001 反射多项式。
    """
    crc = 0xFFFF
    for byte in bytes(data):
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
        crc &= 0xFFFF
    return crc


def crc16_modbus_bytes(data: Iterable[int] | bytes | bytearray | memoryview) -> bytes:
    """返回 CRC-16/MODBUS 的两个传输字节（低字节在前）。"""
    return struct.pack("<H", crc16_modbus(data))


def build_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    """按协议封装一个发送帧。

    参数:
        cmd: 命令 ID，0..255。
        seq: 请求序号，0..255；调用方负责循环递增。
        payload: 原始 payload，长度 0..512。

    返回:
        完整帧字节。CRC 覆盖 VER 到 payload 末尾。
    """
    if not isinstance(cmd, int) or not 0 <= cmd <= 0xFF:
        raise ProtocolError("命令 ID 必须是 0..255 的整数")
    if not isinstance(seq, int) or not 0 <= seq <= 0xFF:
        raise ProtocolError("请求序号必须是 0..255 的整数")
    try:
        payload_bytes = bytes(payload)
    except (TypeError, ValueError) as exc:
        raise ProtocolError("payload 必须为字节序列") from exc
    if len(payload_bytes) > MAX_PAYLOAD:
        raise ProtocolError(
            f"payload 长度为 {len(payload_bytes)} 字节，超过上限 {MAX_PAYLOAD} 字节"
        )

    body = bytes((PROTOCOL_VERSION, cmd, seq)) + struct.pack("<H", len(payload_bytes))
    body += payload_bytes
    return SOF_BYTES + body + struct.pack("<H", crc16_modbus(body))


def parse_frame(frame: bytes | bytearray | memoryview) -> Frame:
    """解析一个完整帧，非法时抛出 :class:`FrameError`。

    流式接收应使用 :class:`FrameParser`，本函数适合测试和已切分好的完整帧。
    """
    raw = bytes(frame)
    if len(raw) < FRAME_MIN_SIZE:
        raise FrameError(f"帧长度不足：{len(raw)} 字节，最小 {FRAME_MIN_SIZE} 字节")
    if raw[0:2] != SOF_BYTES:
        raise FrameError("帧头不是 A5 5A")
    if raw[2] != PROTOCOL_VERSION:
        raise FrameError(f"协议版本非法：0x{raw[2]:02X}")
    length = raw[5] | (raw[6] << 8)
    if length > MAX_PAYLOAD:
        raise FrameError(f"payload 长度 {length} 超过上限 {MAX_PAYLOAD}")
    total = FRAME_MIN_SIZE + length
    if len(raw) != total:
        raise FrameError(f"帧长度不匹配：期望 {total} 字节，实际 {len(raw)} 字节")
    expected = raw[7 + length] | (raw[8 + length] << 8)
    actual = crc16_modbus(raw[2 : 7 + length])
    if expected != actual:
        raise FrameError(
            f"CRC 校验失败：帧内 0x{expected:04X}，计算值 0x{actual:04X}"
        )
    return Frame(cmd=raw[3], seq=raw[4], payload=raw[7 : 7 + length])


@dataclass(frozen=True)
class Frame:
    """一个已经通过版本、长度和 CRC 校验的完整帧。"""

    cmd: int
    seq: int
    payload: bytes

    @property
    def payload_length(self) -> int:
        """payload 字节数。"""
        return len(self.payload)

    @property
    def ack_cmd(self) -> int:
        """该命令对应的普通 ACK 命令 ID。"""
        return self.cmd | ACK_CMD_OFFSET


class FrameParser:
    """带缓冲的流式帧解析器。

    解析器可以接受任意长度的字节块，适用于串口回调、单元测试分块注入。
    遇到坏 CRC、未知版本、LEN > 512 时会丢弃候选帧头并继续搜索下一个
    SOF，因此 payload 中出现 ``A5 5A`` 不会破坏解析。

    帧超时：
        ``feed()`` 和 :meth:`poll_timeout` 都会检查“接收中途 1 秒无新字节”。
        串口线程应在空闲时定期调用 :meth:`poll_timeout`，测试可以注入时钟。
    """

    def __init__(
        self,
        frame_timeout: float = FRAME_TIMEOUT,
        clock: Callable[[], float] | None = None,
    ) -> None:
        """创建解析器。

        参数:
            frame_timeout: 帧内空闲超时，默认 1.0 秒。
            clock: 单调时钟函数，默认 ``time.monotonic``；测试可注入假时钟。
        """
        if frame_timeout <= 0:
            raise ValueError("frame_timeout 必须大于 0")
        self.frame_timeout = float(frame_timeout)
        self._clock = clock or time.monotonic
        self._buffer = bytearray()
        self._last_rx_time = self._clock()

        # 统计信息便于日志和测试观察
        self.dropped_bytes = 0
        self.crc_errors = 0
        self.length_errors = 0
        self.version_errors = 0
        self.timeout_discards = 0

    @property
    def waiting_bytes(self) -> int:
        """缓冲区中尚未组成完整帧的字节数。"""
        return len(self._buffer)

    def reset(self) -> None:
        """丢弃缓冲区并重置超时计时（不清空累计统计）。"""
        self._buffer.clear()
        self._last_rx_time = self._clock()

    def feed(self, data: bytes | bytearray | memoryview, now: float | None = None) -> list[Frame]:
        """注入一段串口数据并返回本次解析出的完整帧。

        参数:
            data: 任意长度的新字节，可为空。
            now: 可选单调时间，测试用。

        返回:
            本次新解析出的 :class:`Frame` 列表，可能为空。
        """
        current = self._clock() if now is None else float(now)
        self._expire_if_needed(current)

        raw = bytes(data)
        if raw:
            self._buffer.extend(raw)
            self._last_rx_time = current

        return self._drain()

    def poll_timeout(self, now: float | None = None) -> bool:
        """检查并处理帧内 1 秒空闲超时。

        返回:
            True 表示本次调用丢弃了一个不完整帧；False 表示没有超时。
        """
        current = self._clock() if now is None else float(now)
        return self._expire_if_needed(current)

    def _expire_if_needed(self, now: float) -> bool:
        """若缓冲帧超过空闲阈值则清空，并返回是否发生丢弃。"""
        if not self._buffer:
            return False
        if now - self._last_rx_time < self.frame_timeout:
            return False
        discarded = len(self._buffer)
        self._buffer.clear()
        self.dropped_bytes += discarded
        self.timeout_discards += 1
        return True

    def _drain(self) -> list[Frame]:
        """尽可能多地从缓冲区中提取合法帧。"""
        frames: list[Frame] = []

        while True:
            start = self._buffer.find(SOF_BYTES)
            if start < 0:
                # 保留末尾的 0xA5，它可能是下一帧帧头被分块截断的前半部分。
                if self._buffer and self._buffer[-1] == SOF0:
                    if len(self._buffer) > 1:
                        self.dropped_bytes += len(self._buffer) - 1
                        del self._buffer[:-1]
                else:
                    self.dropped_bytes += len(self._buffer)
                    self._buffer.clear()
                break

            if start > 0:
                self.dropped_bytes += start
                del self._buffer[:start]

            if len(self._buffer) < FRAME_MIN_SIZE:
                break

            version = self._buffer[2]
            if version != PROTOCOL_VERSION:
                self.version_errors += 1
                self.dropped_bytes += 1
                del self._buffer[0]
                continue

            length = self._buffer[5] | (self._buffer[6] << 8)
            if length > MAX_PAYLOAD:
                self.length_errors += 1
                self.dropped_bytes += 1
                del self._buffer[0]
                continue

            total = FRAME_MIN_SIZE + length
            if len(self._buffer) < total:
                # 数据尚未收齐，等待后续 chunk；超时由 poll_timeout 处理。
                break

            expected_crc = self._buffer[7 + length] | (self._buffer[8 + length] << 8)
            actual_crc = crc16_modbus(self._buffer[2 : 7 + length])
            if actual_crc != expected_crc:
                # 不能直接丢掉整段，坏帧 payload 中可能还藏着下一帧的 SOF。
                self.crc_errors += 1
                self.dropped_bytes += 1
                del self._buffer[0]
                continue

            payload = bytes(self._buffer[7 : 7 + length])
            frames.append(
                Frame(cmd=self._buffer[3], seq=self._buffer[4], payload=payload)
            )
            del self._buffer[:total]

        return frames


# ---------------------------------------------------------------------------
# 文本与 SET_QR
# ---------------------------------------------------------------------------
def utf8_byte_length(text: str | bytes) -> int:
    """返回文本按 UTF-8 编码后的字节数。

    中文一个字符通常占 3 字节，因此不能使用 ``len(text)`` 作为协议长度。
    """
    if isinstance(text, str):
        return len(text.encode("utf-8"))
    try:
        return len(bytes(text))
    except (TypeError, ValueError) as exc:
        raise PayloadError("文本必须为字符串或 UTF-8 字节") from exc


def encode_utf8_text(text: str | bytes, max_bytes: int = MAX_TEXT_BYTES) -> bytes:
    """校验并返回文本的原始 UTF-8 字节。

    规则：1..256 字节，不允许嵌入 NUL，必须是合法 UTF-8。
    超长时抛出 :class:`TextTooLongError`，其他问题抛出 :class:`PayloadError`。
    """
    if isinstance(text, str):
        raw = text.encode("utf-8")
    elif isinstance(text, (bytes, bytearray, memoryview)):
        raw = bytes(text)
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise PayloadError("文本不是合法的 UTF-8 字节序列") from exc
    else:
        raise PayloadError("文本必须为字符串或 UTF-8 字节")

    if not raw:
        raise PayloadError("文本不能为空")
    if len(raw) > max_bytes:
        raise TextTooLongError(len(raw), max_bytes)
    if b"\x00" in raw:
        raise PayloadError("文本不能包含 NUL（0x00）字节")
    return raw


@dataclass(frozen=True)
class SetQrPayload:
    """解析后的 CMD_SET_QR payload。"""

    symbology: int
    ecc: int
    text: str
    raw_text: bytes


def encode_set_qr(
    text: str | bytes,
    ecc: int = ECC_M,
    symbology: int = SYM_QR,
) -> bytes:
    """构造 CMD_SET_QR 的 payload：``[symbology][ecc][UTF-8 文本]``。

    参数:
        text: 待编码文本，按 UTF-8 原样发送。
        ecc: 0=L、1=M、2=Q、3=H，默认 M。
        symbology: 0x01=QR、0x02=Data Matrix（协议保留）。
    """
    if ecc not in ECC_VALUES:
        raise PayloadError(f"纠错等级非法：{ecc!r}，必须是 0/1/2/3")
    if symbology not in (SYM_QR, SYM_DATA_MATRIX):
        raise PayloadError(f"码制非法：{symbology!r}")
    raw = encode_utf8_text(text)
    return bytes((symbology, ecc)) + raw


def decode_set_qr(payload: bytes | bytearray | memoryview) -> SetQrPayload:
    """解析 CMD_SET_QR payload，用于测试或回环验证。"""
    raw = bytes(payload)
    if len(raw) < 3:
        raise PayloadError("CMD_SET_QR payload 至少需要 3 字节")
    symbology, ecc = raw[0], raw[1]
    if ecc not in ECC_VALUES:
        raise PayloadError(f"纠错等级非法：{ecc}")
    try:
        text = raw[2:].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PayloadError("CMD_SET_QR 文本不是合法 UTF-8") from exc
    return SetQrPayload(symbology=symbology, ecc=ecc, text=text, raw_text=raw[2:])


# ---------------------------------------------------------------------------
# ACK 与状态 payload
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class AckPayload:
    """普通 ACK 的 4 字节 payload。"""

    status: int
    error_code: int
    qr_version: int
    module_count: int
    raw: bytes = b""

    @property
    def ok(self) -> bool:
        """是否成功（status 与 error_code 都为零）。"""
        return self.status == STATUS_OK and self.error_code == ERR_NONE


@dataclass(frozen=True)
class StatusAckPayload:
    """CMD_GET_STATUS 的 5 字节 ACK payload。"""

    status: int
    last_qr_valid: int
    symbology: int
    qr_version: int
    module_count: int
    raw: bytes = b""

    @property
    def ok(self) -> bool:
        """设备是否正常返回状态。"""
        return self.status == STATUS_OK

    @property
    def has_valid_qr(self) -> bool:
        """设备当前是否存在可导出的有效二维码。"""
        return self.ok and self.last_qr_valid == 1


def decode_ack_payload(payload: bytes | bytearray | memoryview) -> AckPayload:
    """解析普通 ACK（PING / SET_QR / GET_IMAGE），必须正好 4 字节。"""
    raw = bytes(payload)
    if len(raw) != 4:
        raise PayloadError(f"普通 ACK payload 必须为 4 字节，实际 {len(raw)} 字节")
    return AckPayload(
        status=raw[0],
        error_code=raw[1],
        qr_version=raw[2],
        module_count=raw[3],
        raw=raw,
    )


def decode_get_status_ack(payload: bytes | bytearray | memoryview) -> StatusAckPayload:
    """解析 CMD_GET_STATUS 的 5 字节 ACK。

    注意其布局与普通 4 字节 ACK 不同：
    ``[status][last_qr_valid][symbology][qr_version][module_count]``。
    """
    raw = bytes(payload)
    if len(raw) != 5:
        raise PayloadError(f"GET_STATUS ACK payload 必须为 5 字节，实际 {len(raw)} 字节")
    return StatusAckPayload(
        status=raw[0],
        last_qr_valid=raw[1],
        symbology=raw[2],
        qr_version=raw[3],
        module_count=raw[4],
        raw=raw,
    )


# 便于阅读的别名：两种 ACK 都可用 decode_status_ack 表达。
decode_status_ack = decode_get_status_ack


def error_code_text(error_code: int) -> str:
    """返回错误码对应的中文说明。"""
    return ERROR_TEXT.get(error_code, f"未知错误码 {error_code}")


def status_text(status: int) -> str:
    """返回状态码的中文说明。"""
    return STATUS_TEXT.get(status, f"未知状态 {status}")


# ---------------------------------------------------------------------------
# 图像事件
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class ImageBegin:
    """EVT_IMAGE_BEGIN 解析结果。"""

    format: int
    width: int
    height: int
    file_size: int
    data_size: int
    row_stride: int


@dataclass(frozen=True)
class ImageData:
    """EVT_IMAGE_DATA 解析结果。"""

    offset: int
    chunk: bytes


@dataclass(frozen=True)
class ImageEnd:
    """EVT_IMAGE_END 解析结果。"""

    file_size: int
    crc32: int


def encode_image_begin(
    format_code: int = IMAGE_FORMAT_BMP1,
    width: int = IMAGE_WIDTH,
    height: int = IMAGE_HEIGHT,
    file_size: int = IMAGE_FILE_SIZE,
    data_size: int = IMAGE_DATA_SIZE,
    row_stride: int = IMAGE_ROW_STRIDE,
) -> bytes:
    """构造 EVT_IMAGE_BEGIN payload，默认使用固定 240×240 BMP 参数。"""
    return struct.pack(
        "<BHHIIH",
        format_code,
        width,
        height,
        file_size,
        data_size,
        row_stride,
    )


def encode_image_data(offset: int, chunk: bytes) -> bytes:
    """构造 EVT_IMAGE_DATA payload：``[offset:u32 LE][chunk]``。"""
    if not 0 <= offset <= 0xFFFFFFFF:
        raise PayloadError("图像数据 offset 超出 uint32 范围")
    raw = bytes(chunk)
    return struct.pack("<I", offset) + raw


def encode_image_end(file_size: int, crc32: int) -> bytes:
    """构造 EVT_IMAGE_END payload：``[file_size:u32 LE][crc32:u32 LE]``。"""
    if not 0 <= file_size <= 0xFFFFFFFF:
        raise PayloadError("图像文件大小超出 uint32 范围")
    if not 0 <= crc32 <= 0xFFFFFFFF:
        raise PayloadError("图像 CRC32 超出 uint32 范围")
    return struct.pack("<II", file_size, crc32)


def decode_image_begin(payload: bytes | bytearray | memoryview) -> ImageBegin:
    """解析 EVT_IMAGE_BEGIN payload（固定 15 字节）。"""
    raw = bytes(payload)
    if len(raw) != IMAGE_BEGIN_PAYLOAD_SIZE:
        raise PayloadError(
            f"EVT_IMAGE_BEGIN payload 必须为 {IMAGE_BEGIN_PAYLOAD_SIZE} 字节，实际 {len(raw)} 字节"
        )
    format_code, width, height, file_size, data_size, row_stride = struct.unpack(
        "<BHHIIH", raw
    )
    return ImageBegin(
        format=format_code,
        width=width,
        height=height,
        file_size=file_size,
        data_size=data_size,
        row_stride=row_stride,
    )


def decode_image_data(payload: bytes | bytearray | memoryview) -> ImageData:
    """解析 EVT_IMAGE_DATA payload（至少 5 字节，offset 后必须带数据）。"""
    raw = bytes(payload)
    if len(raw) < 5:
        raise PayloadError(
            f"EVT_IMAGE_DATA payload 至少 5 字节，实际 {len(raw)} 字节"
        )
    (offset,) = struct.unpack_from("<I", raw, 0)
    return ImageData(offset=offset, chunk=raw[4:])


def decode_image_end(payload: bytes | bytearray | memoryview) -> ImageEnd:
    """解析 EVT_IMAGE_END payload（固定 8 字节）。"""
    raw = bytes(payload)
    if len(raw) != IMAGE_END_PAYLOAD_SIZE:
        raise PayloadError(
            f"EVT_IMAGE_END payload 必须为 {IMAGE_END_PAYLOAD_SIZE} 字节，实际 {len(raw)} 字节"
        )
    file_size, crc = struct.unpack("<II", raw)
    return ImageEnd(file_size=file_size, crc32=crc)
