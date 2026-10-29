"""BMP 图像接收状态机。

职责：
- 消费 ``EVT_IMAGE_BEGIN`` / ``EVT_IMAGE_DATA`` / ``EVT_IMAGE_END``。
- 在系统临时目录中创建 ``.part`` 文件，按 offset 顺序追加数据。
- 校验 offset 连续性、总长度、BMP 文件头和整文件 CRC-32/ISO-HDLC。
- 只有全部校验通过后才把 ``.part`` 重命名为正式 ``.bmp``。
- 任一失败都不删除 ``.part``（便于排查），也不会产生正式 ``.bmp``。

本模块不依赖 pyserial/Tkinter，可独立进行单元测试。BMP 固定规格来自需求
第 5.8 节：240×240、1bpp、无压缩、top-down、文件 7742 字节。
"""

from __future__ import annotations

import os
import shutil
import struct
import tempfile
import uuid
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import BinaryIO

import protocol
from crc import Crc32, crc32_iso_hdlc

__all__ = [
    "BmpInfo",
    "ImageReceiveError",
    "ImageReceiveResult",
    "ImageReceiver",
    "ImageState",
    "parse_bmp_header",
    "validate_bmp_bytes",
    "validate_bmp_file",
]

BMP_FILE_HEADER_SIZE = 14
BMP_INFO_HEADER_SIZE = 40
BMP_HEADER_SIZE = BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE  # 54
BMP_PALETTE_SIZE = 8
BMP_DATA_OFFSET = BMP_HEADER_SIZE + BMP_PALETTE_SIZE  # 62
BMP_PALETTE_BLACK = b"\x00\x00\x00\x00"
BMP_PALETTE_WHITE = b"\xFF\xFF\xFF\x00"


class ImageReceiveError(RuntimeError):
    """图像接收过程中出现的可展示错误。"""


class ImageState(str, Enum):
    """图像接收状态。"""

    IDLE = "idle"
    RECEIVING = "receiving"
    DONE = "done"
    FAILED = "failed"


@dataclass(frozen=True)
class BmpInfo:
    """从 BMP 文件头解析出的关键字段。"""

    file_size: int
    off_bits: int
    header_size: int
    width: int
    height: int
    planes: int
    bits_per_pixel: int
    compression: int
    image_size: int
    colors_used: int

    @property
    def top_down(self) -> bool:
        """高度为负数表示按屏幕顺序自上而下存储。"""
        return self.height < 0

    @property
    def row_stride(self) -> int:
        """按 BMP 4 字节对齐规则计算的行跨度。"""
        return ((self.width * self.bits_per_pixel + 31) // 32) * 4

    @property
    def data_size(self) -> int:
        """像素数据区字节数。"""
        return self.image_size


@dataclass
class ImageReceiveResult:
    """一次图像接收的最终结果。"""

    success: bool
    final_path: Path | None
    part_path: Path | None
    bytes_received: int
    expected_size: int
    crc32: int
    error: str = ""

    @property
    def ok(self) -> bool:
        """成功标志的别名。"""
        return self.success


def parse_bmp_header(data: bytes | bytearray | memoryview) -> BmpInfo:
    """解析并做基础结构校验的 BMP 文件头。

    参数:
        data: 至少包含 62 字节（文件头 + 信息头 + 调色板）的数据。

    返回:
        :class:`BmpInfo`。

    异常:
        :class:`ImageReceiveError`: 数据不足、签名错误或信息头非法。
    """
    raw = bytes(data)
    if len(raw) < protocol.IMAGE_BMP_OFF_BITS:
        raise ImageReceiveError(
            f"BMP 数据不足 62 字节，无法解析文件头（实际 {len(raw)} 字节）"
        )

    try:
        bf_type, bf_size, _reserved1, _reserved2, bf_off_bits = struct.unpack_from(
            "<2sIHHI", raw, 0
        )
        (
            bi_size,
            width,
            height,
            planes,
            bit_count,
            compression,
            image_size,
            _x_ppm,
            _y_ppm,
            clr_used,
            _clr_important,
        ) = struct.unpack_from("<IiiHHIIiiII", raw, BMP_FILE_HEADER_SIZE)
    except struct.error as exc:
        raise ImageReceiveError(f"BMP 文件头解析失败：{exc}") from exc

    if bf_type != b"BM":
        raise ImageReceiveError("BMP 文件签名不是 'BM'")
    if bi_size != BMP_INFO_HEADER_SIZE:
        raise ImageReceiveError(f"BITMAPINFOHEADER 大小应为 40，实际 {bi_size}")
    if bf_off_bits < protocol.IMAGE_BMP_OFF_BITS:
        raise ImageReceiveError(f"像素数据偏移非法：{bf_off_bits}")
    if width <= 0 or height == 0:
        raise ImageReceiveError(f"BMP 宽高非法：{width}×{height}")
    if planes != 1:
        raise ImageReceiveError(f"BMP 平面数应为 1，实际 {planes}")

    return BmpInfo(
        file_size=bf_size,
        off_bits=bf_off_bits,
        header_size=bi_size,
        width=width,
        height=height,
        planes=planes,
        bits_per_pixel=bit_count,
        compression=compression,
        image_size=image_size,
        colors_used=clr_used,
    )


def validate_bmp_bytes(data: bytes | bytearray | memoryview) -> BmpInfo:
    """严格校验设备约定的 240×240、1bpp、top-down BMP。

    检查项包括：文件大小 7742、宽高、1bpp、BI_RGB、像素偏移 62、
    像素数据 7680、行跨度 32、调色板索引 0=黑/1=白。
    """
    raw = bytes(data)
    info = parse_bmp_header(raw)

    if info.file_size != protocol.IMAGE_FILE_SIZE:
        raise ImageReceiveError(
            f"BMP 文件大小字段应为 {protocol.IMAGE_FILE_SIZE}，实际 {info.file_size}"
        )
    if len(raw) != info.file_size:
        raise ImageReceiveError(
            f"BMP 实际字节数应为 {info.file_size}，实际收到 {len(raw)}"
        )
    if info.off_bits != protocol.IMAGE_BMP_OFF_BITS:
        raise ImageReceiveError(
            f"BMP 像素偏移应为 {protocol.IMAGE_BMP_OFF_BITS}，实际 {info.off_bits}"
        )
    if info.width != protocol.IMAGE_WIDTH or info.height != -protocol.IMAGE_HEIGHT:
        raise ImageReceiveError(
            f"BMP 尺寸应为 {protocol.IMAGE_WIDTH}×-{protocol.IMAGE_HEIGHT}，"
            f"实际 {info.width}×{info.height}"
        )
    if info.bits_per_pixel != 1:
        raise ImageReceiveError(f"BMP 必须为 1bpp，实际 {info.bits_per_pixel}bpp")
    if info.compression != 0:
        raise ImageReceiveError(f"BMP 必须为无压缩（BI_RGB=0），实际 {info.compression}")
    if info.image_size != protocol.IMAGE_DATA_SIZE:
        raise ImageReceiveError(
            f"BMP 像素数据大小应为 {protocol.IMAGE_DATA_SIZE}，实际 {info.image_size}"
        )
    if info.colors_used != 2:
        raise ImageReceiveError(f"BMP 调色板颜色数应为 2，实际 {info.colors_used}")
    if info.row_stride != protocol.IMAGE_ROW_STRIDE:
        raise ImageReceiveError(
            f"BMP 行跨度应为 {protocol.IMAGE_ROW_STRIDE}，实际 {info.row_stride}"
        )
    if raw[BMP_HEADER_SIZE:BMP_DATA_OFFSET] != BMP_PALETTE_BLACK + BMP_PALETTE_WHITE:
        raise ImageReceiveError("BMP 调色板必须为索引 0=黑、索引 1=白")
    if len(raw) - info.off_bits != protocol.IMAGE_DATA_SIZE:
        raise ImageReceiveError(
            f"BMP 像素区应为 {protocol.IMAGE_DATA_SIZE} 字节，"
            f"实际 {len(raw) - info.off_bits}"
        )
    return info


def validate_bmp_file(path: str | Path) -> BmpInfo:
    """读取文件并执行 :func:`validate_bmp_bytes` 的严格校验。"""
    file_path = Path(path)
    try:
        data = file_path.read_bytes()
    except OSError as exc:
        raise ImageReceiveError(f"无法读取 BMP 文件：{exc}") from exc
    return validate_bmp_bytes(data)


class ImageReceiver:
    """消费图像事件、写临时文件并校验的接收状态机。

    推荐用法::

        receiver = ImageReceiver()
        result = None
        for event in events:
            result = receiver.feed(event)
            if result is not None:
                break

    也可直接调用 :meth:`begin`、:meth:`data`、:meth:`end`。失败时这些方法会
    抛出 :class:`ImageReceiveError`，但 ``feed()`` 会捕获并返回失败结果，界面
    无需使用异常控制流程。
    """

    def __init__(
        self,
        final_path: str | Path | None = None,
        temp_dir: str | Path | None = None,
    ) -> None:
        """创建接收器。

        参数:
            final_path: 成功后的正式 BMP 路径；省略时在临时目录自动命名。
            temp_dir: ``.part`` 所在目录；省略时**优先放在 final_path 同目录**，
                没有 final_path 才退化为系统临时目录。

        说明:
            ``.part`` 必须与最终文件位于同一卷，否则成功时的 ``os.replace()``
            在 Windows 上会报 ``WinError 17``（系统无法把文件移到不同的磁盘驱动器）。
            因此这里默认把临时文件建在目标文件旁边；只有调用方显式指定
            ``temp_dir`` 时才会覆盖该策略（此时 ``end()`` 会在跨卷时退化为复制+删除）。
        """
        explicit_temp = temp_dir is not None
        target = None if final_path is None else Path(final_path)

        if explicit_temp:
            base = Path(temp_dir)          # 调用方指定，尊重其选择
            self._owns_temp_dir = False
        elif target is not None:
            # 关键：与目标同目录，保证 os.replace 是同卷重命名
            base = target.parent if str(target.parent) else Path(".")
            self._owns_temp_dir = False
        else:
            base = Path(tempfile.mkdtemp(prefix="qr_bmp_"))
            self._owns_temp_dir = True

        base.mkdir(parents=True, exist_ok=True)
        self.temp_dir = base

        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        unique = uuid.uuid4().hex[:8]
        stem = f"qr_{stamp}_{unique}"
        self.part_path = base / f"{stem}.part"
        self._final_is_temporary = final_path is None
        self.final_path = base / f"{stem}.bmp" if target is None else target

        self._state = ImageState.IDLE
        self._file: BinaryIO | None = None
        self._crc = Crc32()
        self._expected_offset = 0
        self._file_size = 0
        self._data_size = 0
        self._row_stride = 0
        self._bytes_received = 0
        self._error = ""
        self._end_crc = 0

    # ------------------------------------------------------------------
    # 属性
    # ------------------------------------------------------------------
    @property
    def state(self) -> ImageState:
        """当前状态。"""
        return self._state

    @property
    def error(self) -> str:
        """最近一次失败原因，成功时为空串。"""
        return self._error

    @property
    def bytes_received(self) -> int:
        """已接收的 BMP 文件字节数。"""
        return self._bytes_received

    @property
    def expected_size(self) -> int:
        """BEGIN 声明的文件总大小；尚未收到 BEGIN 时为 0。"""
        return self._file_size

    @property
    def progress(self) -> float:
        """接收进度，范围 0.0..1.0。"""
        if self._file_size <= 0:
            return 0.0
        return min(1.0, self._bytes_received / self._file_size)

    # ------------------------------------------------------------------
    # 事件处理
    # ------------------------------------------------------------------
    def feed(self, event: object) -> ImageReceiveResult | None:
        """消费一个图像事件。

        返回:
            - BEGIN/DATA 成功时返回 ``None``；
            - END 或任一失败时返回 :class:`ImageReceiveResult`。
        """
        if self._state is ImageState.DONE:
            return self.result()
        if self._state is ImageState.FAILED:
            return self.result()

        try:
            if isinstance(event, protocol.ImageBegin):
                self.begin(event)
                return None
            if isinstance(event, protocol.ImageData):
                self.data(event)
                return None
            if isinstance(event, protocol.ImageEnd):
                return self.end(event)
            message = f"未知图像事件类型：{type(event).__name__}"
            self._fail(message)
            return self.result()
        except ImageReceiveError as exc:
            if self._state is not ImageState.FAILED:
                self._fail(str(exc))
            return self.result()
        except OSError as exc:
            message = f"写入临时文件失败：{exc}"
            self._fail(message)
            return self.result()

    # 语义别名，便于调用方按“消费事件”理解。
    consume = feed
    handle_event = feed

    def begin(self, event: protocol.ImageBegin) -> None:
        """处理 EVT_IMAGE_BEGIN，校验固定格式并打开 ``.part`` 文件。"""
        if self._state is ImageState.RECEIVING:
            message = "收到重复的 EVT_IMAGE_BEGIN，正在接收的图像已中止"
            self._fail(message)
            raise ImageReceiveError(message)
        if self._state is not ImageState.IDLE:
            message = f"当前状态 {self._state.value} 不能开始新的图像接收"
            self._fail(message)
            raise ImageReceiveError(message)

        message = self._validate_begin(event)
        if message:
            self._fail(message)
            raise ImageReceiveError(message)

        try:
            self._file = open(self.part_path, "wb")
        except OSError as exc:
            message = f"无法创建临时文件 {self.part_path}：{exc}"
            self._fail(message)
            raise ImageReceiveError(message) from exc

        self._file_size = event.file_size
        self._data_size = event.data_size
        self._row_stride = event.row_stride
        self._expected_offset = 0
        self._bytes_received = 0
        self._crc = Crc32()
        self._error = ""
        self._state = ImageState.RECEIVING

    def data(self, event: protocol.ImageData) -> None:
        """处理 EVT_IMAGE_DATA，校验 offset 连续并追加写入。"""
        if self._state is not ImageState.RECEIVING or self._file is None:
            message = "尚未收到 EVT_IMAGE_BEGIN，拒绝图像数据"
            self._fail(message)
            raise ImageReceiveError(message)

        chunk = bytes(event.chunk)
        if not chunk:
            message = f"offset={event.offset} 的图像数据块为空"
            self._fail(message)
            raise ImageReceiveError(message)

        if event.offset != self._expected_offset:
            if event.offset < self._expected_offset:
                kind = "重复或重叠"
            else:
                kind = "缺口"
            message = (
                f"图像数据 offset 不连续（{kind}）："
                f"期望 {self._expected_offset}，收到 {event.offset}"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        end_offset = event.offset + len(chunk)
        if end_offset > self._file_size:
            message = (
                f"图像数据越界：offset={event.offset}，块长 {len(chunk)}，"
                f"文件总长 {self._file_size}"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        try:
            self._file.write(chunk)
        except OSError as exc:
            message = f"写入临时文件失败：{exc}"
            self._fail(message)
            raise ImageReceiveError(message) from exc

        self._crc.update(chunk)
        self._bytes_received = end_offset
        self._expected_offset = end_offset

    def end(self, event: protocol.ImageEnd) -> ImageReceiveResult:
        """处理 EVT_IMAGE_END，校验长度、CRC32 和 BMP 头后重命名。"""
        if self._state is not ImageState.RECEIVING or self._file is None:
            message = "尚未收到完整图像数据，却收到了 EVT_IMAGE_END"
            self._fail(message)
            raise ImageReceiveError(message)

        if event.file_size != self._file_size:
            message = (
                f"EVT_IMAGE_END 文件大小不匹配：BEGIN 为 {self._file_size}，"
                f"END 为 {event.file_size}"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        if self._bytes_received != event.file_size:
            message = (
                f"图像数据总长度不匹配：期望 {event.file_size} 字节，"
                f"实际收到 {self._bytes_received} 字节"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        calculated_crc = self._crc.digest()
        if calculated_crc != event.crc32:
            message = (
                f"BMP 整文件 CRC32 校验失败："
                f"期望 0x{event.crc32:08X}，实际 0x{calculated_crc:08X}"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        self._close_file()
        try:
            raw = self.part_path.read_bytes()
        except OSError as exc:
            message = f"无法读取临时文件进行最终校验：{exc}"
            self._fail(message)
            raise ImageReceiveError(message) from exc

        if len(raw) != event.file_size:
            message = (
                f"临时文件长度异常：期望 {event.file_size} 字节，实际 {len(raw)} 字节"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        # 再从磁盘复算一次，防止写入缓存/短写导致内存状态与文件不一致。
        file_crc = crc32_iso_hdlc(raw)
        if file_crc != event.crc32:
            message = (
                f"临时文件 CRC32 复算失败：期望 0x{event.crc32:08X}，"
                f"实际 0x{file_crc:08X}"
            )
            self._fail(message)
            raise ImageReceiveError(message)

        try:
            validate_bmp_bytes(raw)
        except ImageReceiveError as exc:
            self._fail(f"BMP 文件头校验失败：{exc}")
            raise ImageReceiveError(self._error) from exc

        try:
            self.final_path.parent.mkdir(parents=True, exist_ok=True)
            os.replace(self.part_path, self.final_path)
        except OSError as exc:
            # 跨卷（例如显式指定了 temp_dir 而目标在另一个盘）时 os.replace 会失败
            # （Windows: WinError 17），退化为"复制 + 删除"，功能等价但非原子。
            try:
                shutil.move(str(self.part_path), str(self.final_path))
            except OSError:
                message = f"无法把临时文件重命名为 {self.final_path}：{exc}"
                self._fail(message)
                raise ImageReceiveError(message) from exc

        self._end_crc = event.crc32
        self._state = ImageState.DONE
        self._error = ""
        return self.result()

    def abort(self, message: str = "图像接收已中止") -> ImageReceiveResult:
        """中止当前接收；``.part`` 保留，正式 BMP 不产生。"""
        if self._state is not ImageState.DONE:
            self._fail(message)
        return self.result()

    # ------------------------------------------------------------------
    # 结果与资源清理
    # ------------------------------------------------------------------
    def result(self, success: bool | None = None) -> ImageReceiveResult:
        """生成当前结果快照。"""
        if success is None:
            success = self._state is ImageState.DONE
        return ImageReceiveResult(
            success=bool(success),
            final_path=self.final_path if success else None,
            part_path=self.part_path if self.part_path.exists() else None,
            bytes_received=self._bytes_received,
            expected_size=self._file_size,
            crc32=self._end_crc,
            error=self._error,
        )

    def cleanup_temp(self) -> None:
        """删除本接收器产生的临时 ``.part``/临时 BMP 和自建临时目录。

        只有最终 BMP 同样位于临时目录时才会删除它，绝不删除用户另行指定的
        正式保存路径。
        """
        self._close_file()
        try:
            if self.part_path.exists():
                self.part_path.unlink()
        except OSError:
            pass

        try:
            if self._final_is_temporary and self.final_path.exists():
                self.final_path.unlink()
        except OSError:
            pass

        if self._owns_temp_dir:
            try:
                self.temp_dir.rmdir()
            except OSError:
                # 目录中可能还有其他文件，保留目录。
                pass

    # ------------------------------------------------------------------
    # 内部工具
    # ------------------------------------------------------------------
    def _validate_begin(self, event: protocol.ImageBegin) -> str:
        """返回 BEGIN 的校验错误文本，空串表示合法。"""
        if event.format != protocol.IMAGE_FORMAT_BMP1:
            return f"图像格式不支持：format={event.format}，期望 1（BMP 1bpp）"
        if event.width != protocol.IMAGE_WIDTH or event.height != protocol.IMAGE_HEIGHT:
            return (
                f"图像尺寸不符：{event.width}×{event.height}，"
                f"期望 {protocol.IMAGE_WIDTH}×{protocol.IMAGE_HEIGHT}"
            )
        if event.file_size != protocol.IMAGE_FILE_SIZE:
            return (
                f"图像文件大小不符：{event.file_size}，"
                f"期望 {protocol.IMAGE_FILE_SIZE}"
            )
        if event.data_size != protocol.IMAGE_DATA_SIZE:
            return (
                f"图像像素数据大小不符：{event.data_size}，"
                f"期望 {protocol.IMAGE_DATA_SIZE}"
            )
        if event.row_stride != protocol.IMAGE_ROW_STRIDE:
            return (
                f"图像行跨度不符：{event.row_stride}，"
                f"期望 {protocol.IMAGE_ROW_STRIDE}"
            )
        expected_data = event.file_size - protocol.IMAGE_BMP_OFF_BITS
        if expected_data != event.data_size:
            return (
                f"BEGIN 字段自相矛盾：file_size({event.file_size}) - "
                f"off_bits({protocol.IMAGE_BMP_OFF_BITS}) != data_size({event.data_size})"
            )
        return ""

    def _fail(self, message: str) -> None:
        """进入失败状态，保留 ``.part``，记录中文错误。"""
        if self._state is ImageState.DONE:
            return
        self._close_file()
        self._state = ImageState.FAILED
        if message:
            self._error = message

    def _close_file(self) -> None:
        """安全关闭临时文件句柄。"""
        if self._file is not None:
            try:
                self._file.flush()
            except OSError:
                pass
            try:
                self._file.close()
            except OSError:
                pass
            self._file = None
