"""T-004/T-005：BMP 文件头与图像接收异常单元测试。"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

HOST_DIR = Path(__file__).resolve().parents[1]
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))

import protocol
from crc import crc32_iso_hdlc
from image_receiver import (
    ImageReceiveError,
    ImageReceiver,
    ImageState,
    parse_bmp_header,
    validate_bmp_bytes,
    validate_bmp_file,
)

BMP_WIDTH = 240
BMP_HEIGHT = 240
BMP_ROW_STRIDE = 32
BMP_DATA_SIZE = 7680
BMP_FILE_SIZE = 7742
BMP_OFF_BITS = 62


def build_synthetic_bmp() -> bytes:
    """构造测试用 7742 字节、240×240、1bpp、top-down BMP。

    像素内容为白色背景加左侧一条黑色竖带，仅用于验证字节流和头字段。
    """
    file_header = struct.pack(
        "<2sIHHI",
        b"BM",
        BMP_FILE_SIZE,
        0,
        0,
        BMP_OFF_BITS,
    )
    info_header = struct.pack(
        "<IiiHHIIiiII",
        40,
        BMP_WIDTH,
        -BMP_HEIGHT,
        1,
        1,
        0,
        BMP_DATA_SIZE,
        2835,
        2835,
        2,
        0,
    )
    palette = b"\x00\x00\x00\x00" + b"\xFF\xFF\xFF\x00"

    rows = bytearray()
    for _y in range(BMP_HEIGHT):
        row = bytearray(b"\xFF" * BMP_ROW_STRIDE)
        # 每行前 8 个像素置黑（位 0），其余位保持白色（位 1）。
        row[0] = 0x00
        rows.extend(row)
    pixel_data = bytes(rows)
    assert len(pixel_data) == BMP_DATA_SIZE
    bmp = file_header + info_header + palette + pixel_data
    assert len(bmp) == BMP_FILE_SIZE
    return bmp


def begin_event() -> protocol.ImageBegin:
    """返回固定参数的 EVT_IMAGE_BEGIN。"""
    return protocol.decode_image_begin(protocol.encode_image_begin())


def end_event(bmp: bytes, crc_value: int | None = None, file_size: int | None = None) -> protocol.ImageEnd:
    """返回 EVT_IMAGE_END；可故意覆盖大小或 CRC 以模拟异常。"""
    return protocol.ImageEnd(
        file_size=BMP_FILE_SIZE if file_size is None else file_size,
        crc32=crc32_iso_hdlc(bmp) if crc_value is None else crc_value,
    )


class TestBmpHeader(unittest.TestCase):
    """T-004：验证标准 BMP 关键字段。"""

    def test_synthetic_bmp_header(self) -> None:
        """文件大小、尺寸、1bpp、像素偏移和图像数据大小均正确。"""
        bmp = build_synthetic_bmp()
        self.assertEqual(len(bmp), 7742)
        info = parse_bmp_header(bmp)
        self.assertEqual(info.file_size, 7742)
        self.assertEqual((info.width, info.height), (240, -240))
        self.assertTrue(info.top_down)
        self.assertEqual(info.planes, 1)
        self.assertEqual(info.bits_per_pixel, 1)
        self.assertEqual(info.compression, 0)
        self.assertEqual(info.off_bits, 62)
        self.assertEqual(info.image_size, 7680)
        self.assertEqual(info.data_size, 7680)
        self.assertEqual(info.row_stride, 32)
        self.assertEqual(info.colors_used, 2)

    def test_strict_validation_accepts_synthetic_bmp(self) -> None:
        """严格校验函数接受符合设备规格的 BMP。"""
        info = validate_bmp_bytes(build_synthetic_bmp())
        self.assertEqual(info.width, 240)
        self.assertEqual(info.height, -240)

    def test_header_too_short_rejected(self) -> None:
        """不足 62 字节必须拒绝。"""
        with self.assertRaises(ImageReceiveError):
            parse_bmp_header(b"BM" + b"\x00" * 10)

    def test_wrong_width_rejected(self) -> None:
        """宽度不为 240 时应拒绝。"""
        raw = bytearray(build_synthetic_bmp())
        struct.pack_into("<i", raw, 18, 320)
        with self.assertRaises(ImageReceiveError):
            validate_bmp_bytes(bytes(raw))

    def test_wrong_bpp_rejected(self) -> None:
        """位深不为 1bpp 时应拒绝。"""
        raw = bytearray(build_synthetic_bmp())
        struct.pack_into("<H", raw, 28, 24)
        with self.assertRaises(ImageReceiveError):
            validate_bmp_bytes(bytes(raw))


class TestImageReceiverSuccess(unittest.TestCase):
    """图像接收成功路径。"""

    def test_full_bmp_received_and_renamed(self) -> None:
        """完整数据 + 正确 CRC32 后应生成正式 .bmp。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            final = tmp_path / "received.bmp"
            receiver = ImageReceiver(final_path=final, temp_dir=tmp_path)
            self.assertIsNone(receiver.feed(begin_event()))
            self.assertTrue(receiver.part_path.exists())
            self.assertFalse(final.exists())

            offset = 0
            chunk_size = 997
            while offset < len(bmp):
                chunk = bmp[offset : offset + chunk_size]
                result = receiver.feed(protocol.ImageData(offset=offset, chunk=chunk))
                self.assertIsNone(result)
                offset += len(chunk)

            result = receiver.feed(end_event(bmp))
            self.assertIsNotNone(result)
            assert result is not None
            self.assertTrue(result.success, result.error)
            self.assertEqual(result.bytes_received, BMP_FILE_SIZE)
            self.assertEqual(result.crc32, crc32_iso_hdlc(bmp))
            self.assertEqual(receiver.state, ImageState.DONE)
            self.assertTrue(final.exists())
            self.assertFalse(receiver.part_path.exists())
            self.assertEqual(final.read_bytes(), bmp)
            validate_bmp_file(final)

    def test_part_file_sits_next_to_final_path(self) -> None:
        """指定正式路径且不指定 temp_dir 时，.part 必须建在目标同目录（同卷）。

        这是跨盘 bug 的回归测试：.part 在系统临时目录（C 盘）而目标在 D 盘时，
        os.replace 会报 WinError 17，导致导出失败。
        """
        with tempfile.TemporaryDirectory() as tmp:
            final = Path(tmp) / "sub" / "qr.bmp"
            receiver = ImageReceiver(final_path=final)
            self.assertEqual(receiver.part_path.parent, final.parent)
            self.assertEqual(receiver.temp_dir, final.parent)
            self.assertFalse(receiver._owns_temp_dir)  # 不得删用户目录

    def test_explicit_temp_dir_is_honored(self) -> None:
        """显式给 temp_dir 时仍按调用方指定，不被 final_path 覆盖。"""
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            receiver = ImageReceiver(final_path=Path(tmp_b) / "x.bmp", temp_dir=Path(tmp_a))
            self.assertEqual(receiver.part_path.parent, Path(tmp_a))
            self.assertEqual(receiver.final_path, Path(tmp_b) / "x.bmp")

    def test_default_final_path_is_in_temp_dir_and_cleanup(self) -> None:
        """不指定正式路径时自动生成临时 BMP，清理时一并删除。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            receiver = ImageReceiver(temp_dir=Path(tmp))
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp))
            result = receiver.feed(end_event(bmp))
            assert result is not None
            self.assertTrue(result.success, result.error)
            final = receiver.final_path
            self.assertTrue(final.exists())
            receiver.cleanup_temp()
            self.assertFalse(final.exists())
            self.assertFalse(receiver.part_path.exists())


class TestImageReceiverFailures(unittest.TestCase):
    """T-005：offset、长度、CRC32 异常时不得生成正式 BMP。"""

    def _assert_failed_without_bmp(
        self,
        receiver: ImageReceiver,
        result: object,
        expected_text: str,
    ) -> None:
        """统一断言失败结果、.part 保留且目录内没有 .bmp。"""
        self.assertIsNotNone(result)
        assert result is not None
        self.assertFalse(result.success)  # type: ignore[attr-defined]
        self.assertIn(expected_text, result.error)  # type: ignore[attr-defined]
        self.assertEqual(receiver.state, ImageState.FAILED)
        self.assertTrue(receiver.part_path.exists())
        self.assertEqual(list(receiver.temp_dir.glob("*.bmp")), [])

    def test_offset_gap_rejected(self) -> None:
        """offset 出现缺口应失败且不生成正式文件。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "gap.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp[:100]))
            result = receiver.feed(protocol.ImageData(101, bmp[101:200]))
            self._assert_failed_without_bmp(receiver, result, "不连续")
            self.assertFalse((tmp_path / "gap.bmp").exists())

    def test_duplicate_offset_rejected(self) -> None:
        """重复 offset 应失败。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "dup.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp[:100]))
            result = receiver.feed(protocol.ImageData(0, bmp[:100]))
            self._assert_failed_without_bmp(receiver, result, "重复")
            self.assertFalse((tmp_path / "dup.bmp").exists())

    def test_out_of_range_chunk_rejected(self) -> None:
        """块尾超出文件总长应失败。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "range.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp[: BMP_FILE_SIZE - 3]))
            result = receiver.feed(protocol.ImageData(BMP_FILE_SIZE - 3, b"\x00" * 10))
            self._assert_failed_without_bmp(receiver, result, "越界")

    def test_short_data_at_end_rejected(self) -> None:
        """END 前累计字节不足应失败。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "short.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp[:300]))
            result = receiver.feed(end_event(bmp))
            self._assert_failed_without_bmp(receiver, result, "总长度不匹配")
            self.assertFalse((tmp_path / "short.bmp").exists())

    def test_end_file_size_mismatch_rejected(self) -> None:
        """END 声明的 file_size 与 BEGIN 不一致应失败。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "size.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp))
            result = receiver.feed(end_event(bmp, file_size=BMP_FILE_SIZE - 1))
            self._assert_failed_without_bmp(receiver, result, "文件大小不匹配")
            self.assertFalse((tmp_path / "size.bmp").exists())

    def test_wrong_crc32_rejected(self) -> None:
        """CRC32 错误应失败，.part 保留，不生成 .bmp。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "crc.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp))
            wrong_crc = crc32_iso_hdlc(bmp) ^ 0x1
            result = receiver.feed(end_event(bmp, crc_value=wrong_crc))
            self._assert_failed_without_bmp(receiver, result, "CRC32")
            self.assertFalse((tmp_path / "crc.bmp").exists())

    def test_wrong_bmp_header_after_crc_passes_rejected(self) -> None:
        """CRC32 正确但 BMP 头字段错误时也必须失败。"""
        raw = bytearray(build_synthetic_bmp())
        struct.pack_into("<i", raw, 18, 241)  # 篡改 biWidth
        tampered = bytes(raw)
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "header.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, tampered))
            result = receiver.feed(end_event(tampered))
            self._assert_failed_without_bmp(receiver, result, "BMP")
            self.assertFalse((tmp_path / "header.bmp").exists())

    def test_invalid_begin_rejected(self) -> None:
        """BEGIN 尺寸错误应立即失败。"""
        with tempfile.TemporaryDirectory() as tmp:
            receiver = ImageReceiver(temp_dir=Path(tmp))
            bad_begin = protocol.decode_image_begin(
                protocol.encode_image_begin(width=320)
            )
            result = receiver.feed(bad_begin)
            self.assertIsNotNone(result)
            assert result is not None
            self.assertFalse(result.success)
            self.assertEqual(receiver.state, ImageState.FAILED)
            self.assertFalse(receiver.part_path.exists())

    def test_abort_preserves_part(self) -> None:
        """中止接收时保留 .part。"""
        bmp = build_synthetic_bmp()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            receiver = ImageReceiver(tmp_path / "abort.bmp", temp_dir=tmp_path)
            receiver.feed(begin_event())
            receiver.feed(protocol.ImageData(0, bmp[:500]))
            result = receiver.abort("测试中止")
            self.assertFalse(result.success)
            self.assertTrue(receiver.part_path.exists())
            self.assertFalse((tmp_path / "abort.bmp").exists())


if __name__ == "__main__":
    unittest.main()
