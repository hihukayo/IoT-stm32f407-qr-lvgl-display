"""协议、CRC、文本和图像事件单元测试。"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

# 允许从 host 目录直接运行 ``python -m unittest discover -s tests -v``。
HOST_DIR = Path(__file__).resolve().parents[1]
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))

import protocol
from crc import crc32_iso_hdlc


class TestCrc(unittest.TestCase):
    """T-001：CRC 标准测试向量。"""

    def test_crc16_modbus_standard_vector(self) -> None:
        """ASCII 123456789 的 CRC-16/MODBUS 必须为 0x4B37。"""
        crc = protocol.crc16_modbus(b"123456789")
        self.assertEqual(crc, 0x4B37)
        self.assertEqual(struct.pack("<H", crc), b"\x37\x4B")

    def test_crc32_iso_hdlc_standard_vector(self) -> None:
        """ASCII 123456789 的 CRC-32/ISO-HDLC 必须为 0xCBF43926。"""
        self.assertEqual(crc32_iso_hdlc(b"123456789"), 0xCBF43926)

    def test_build_frame_crc_scope(self) -> None:
        """CRC 必须覆盖 VER 到 payload 末尾，不覆盖 SOF 和 CRC 本身。"""
        frame = protocol.build_frame(protocol.CMD_SET_QR, 0x5A, b"abc\xA5\x5A")
        self.assertEqual(frame[:2], b"\xA5\x5A")
        self.assertEqual(frame[-2:], struct.pack("<H", protocol.crc16_modbus(frame[2:-2])))
        self.assertEqual(len(frame), 9 + 5)


class TestFrameBuildAndParse(unittest.TestCase):
    """帧封装与完整帧解析。"""

    def test_round_trip(self) -> None:
        """构造后解析应保持 cmd/seq/payload。"""
        frame = protocol.build_frame(protocol.CMD_SET_QR, 7, b"Hello 123")
        parsed = protocol.parse_frame(frame)
        self.assertEqual(parsed.cmd, protocol.CMD_SET_QR)
        self.assertEqual(parsed.seq, 7)
        self.assertEqual(parsed.payload, b"Hello 123")

    def test_empty_payload(self) -> None:
        """空 payload 的帧长为 9。"""
        frame = protocol.build_frame(protocol.CMD_PING, 0)
        self.assertEqual(len(frame), 9)
        parsed = protocol.parse_frame(frame)
        self.assertEqual(parsed.payload, b"")
        self.assertEqual(parsed.ack_cmd, protocol.ACK_PING)

    def test_exact_field_order_and_little_endian_length(self) -> None:
        """字段顺序必须为 SOF/VER/CMD/SEQ/LEN_L/LEN_H/payload/CRC。"""
        frame = protocol.build_frame(protocol.CMD_SET_QR, 0x7B, b"AB")
        self.assertEqual(frame[0:2], b"\xA5\x5A")
        self.assertEqual(frame[2], 0x01)
        self.assertEqual(frame[3], protocol.CMD_SET_QR)
        self.assertEqual(frame[4], 0x7B)
        self.assertEqual(frame[5:7], b"\x02\x00")
        self.assertEqual(frame[7:9], b"AB")
        self.assertEqual(len(frame), 11)

    def test_max_payload_512(self) -> None:
        """最大 512 字节 payload 必须可构造和解析。"""
        payload = bytes(range(256)) + bytes(range(256))
        frame = protocol.build_frame(protocol.CMD_SET_QR, 0xFE, payload)
        self.assertEqual(len(frame), 9 + 512)
        parsed = protocol.parse_frame(frame)
        self.assertEqual(parsed.payload, payload)

    def test_over_max_payload_rejected(self) -> None:
        """构造超过 512 字节 payload 时应拒绝。"""
        with self.assertRaises(protocol.ProtocolError):
            protocol.build_frame(protocol.CMD_SET_QR, 1, b"x" * 513)

    def test_invalid_crc_frame_rejected(self) -> None:
        """完整帧解析遇到坏 CRC 应失败。"""
        frame = bytearray(protocol.build_frame(protocol.CMD_PING, 3, b"data"))
        frame[-1] ^= 0xFF
        with self.assertRaises(protocol.FrameError):
            protocol.parse_frame(frame)


class TestFrameParser(unittest.TestCase):
    """流式解析器：分块、粘连、坏帧、超时与重同步。"""

    def _frame(self, cmd: int, seq: int, payload: bytes) -> bytes:
        """测试用帧构造快捷方法。"""
        return protocol.build_frame(cmd, seq, payload)

    def test_payload_contains_sof_bytes(self) -> None:
        """payload 中含 A5 5A 时仍按 LEN/CRC 正确解析。"""
        payload = b"pre\xA5\x5Apost\xA5\x5A" * 7
        frame = self._frame(protocol.CMD_SET_QR, 9, payload)
        parser = protocol.FrameParser()
        # 故意在 A5 5A 中间切块，验证不会把 payload 当成帧头。
        frames = parser.feed(frame[:10]) + parser.feed(frame[10:])
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].payload, payload)

    def test_one_byte_at_a_time(self) -> None:
        """逐字节输入应得到同一帧。"""
        payload = b"\xA5\x5A" + bytes(range(64)) + b"\xA5\x5A"
        frame = self._frame(protocol.CMD_SET_QR, 2, payload)
        parser = protocol.FrameParser()
        frames = []
        for byte in frame:
            frames.extend(parser.feed(bytes((byte,))))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].payload, payload)
        self.assertEqual(parser.waiting_bytes, 0)

    def test_two_concatenated_frames(self) -> None:
        """一个 chunk 中的两帧必须全部解析。"""
        first = self._frame(protocol.CMD_PING, 1, b"")
        second = self._frame(protocol.CMD_GET_STATUS, 2, b"")
        parser = protocol.FrameParser()
        frames = parser.feed(first + second)
        self.assertEqual([(item.cmd, item.seq) for item in frames], [(1, 1), (3, 2)])

    def test_noise_before_frame(self) -> None:
        """帧头前的噪声应被丢弃。"""
        frame = self._frame(protocol.CMD_PING, 5, b"x")
        parser = protocol.FrameParser()
        frames = parser.feed(b"noise\x00\x01" + frame)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 5)
        self.assertGreaterEqual(parser.dropped_bytes, 7)

    def test_partial_sof_across_chunks(self) -> None:
        """0xA5 与 0x5A 被拆到两个 chunk 时不能丢帧。"""
        frame = self._frame(protocol.CMD_PING, 6, b"")
        parser = protocol.FrameParser()
        self.assertEqual(parser.feed(frame[:1]), [])
        frames = parser.feed(frame[1:])
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 6)

    def test_bad_crc_discarded_then_good_frame(self) -> None:
        """坏 CRC 帧应丢弃，后续正常帧仍能解析。"""
        bad = bytearray(self._frame(protocol.CMD_SET_QR, 10, b"garbage"))
        bad[-2] ^= 0xFF
        good = self._frame(protocol.CMD_PING, 11, b"ok")
        parser = protocol.FrameParser()
        frames = parser.feed(bytes(bad) + good)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 11)
        self.assertEqual(frames[0].payload, b"ok")
        self.assertEqual(parser.crc_errors, 1)

    def test_len_over_512_discarded_then_good_frame(self) -> None:
        """LEN=513 的伪帧应拒绝，后续正常帧仍能解析。"""
        bad = (
            protocol.SOF_BYTES
            + bytes((protocol.PROTOCOL_VERSION, protocol.CMD_PING, 0x20))
            + struct.pack("<H", 513)
            + b"\x00" * 513
            + b"\x00\x00"
        )
        good = self._frame(protocol.CMD_PING, 0x21, b"")
        parser = protocol.FrameParser()
        frames = parser.feed(bad + good)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 0x21)
        self.assertEqual(parser.length_errors, 1)

    def test_wrong_version_discarded_then_good_frame(self) -> None:
        """未知 VER 应拒绝，后续正常帧仍能解析。"""
        bad = bytearray(self._frame(protocol.CMD_PING, 0x30, b"bad"))
        bad[2] = 0x02
        good = self._frame(protocol.CMD_PING, 0x31, b"")
        parser = protocol.FrameParser()
        frames = parser.feed(bytes(bad) + good)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 0x31)
        self.assertEqual(parser.version_errors, 1)

    def test_truncated_frame_timeout_and_resync(self) -> None:
        """截断帧 1 秒无新字节后应被清空，旧剩余字节不得拼成完整帧。"""
        now = [0.0]
        parser = protocol.FrameParser(clock=lambda: now[0])
        frame = self._frame(protocol.CMD_SET_QR, 0x40, b"timeout-test")
        self.assertEqual(parser.feed(frame[:-3], now=now[0]), [])
        self.assertGreater(parser.waiting_bytes, 0)
        now[0] = 0.5
        self.assertFalse(parser.poll_timeout(now=now[0]))
        now[0] = 1.0
        self.assertTrue(parser.poll_timeout(now=now[0]))
        self.assertEqual(parser.waiting_bytes, 0)

        # 超时后才到达的旧帧尾部不能独立成帧。
        self.assertEqual(parser.feed(frame[-3:], now=now[0]), [])
        good = self._frame(protocol.CMD_PING, 0x41, b"")
        frames = parser.feed(good, now=now[0] + 0.2)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 0x41)
        self.assertGreaterEqual(parser.timeout_discards, 1)

    def test_feed_after_idle_expires_partial_frame(self) -> None:
        """空闲超过 1 秒后直接喂入新数据，也应先丢弃旧的不完整帧。"""
        now = [0.0]
        parser = protocol.FrameParser(clock=lambda: now[0])
        frame = self._frame(protocol.CMD_PING, 0x50, b"partial")
        parser.feed(frame[:5], now=now[0])
        now[0] = 1.05
        good = self._frame(protocol.CMD_PING, 0x51, b"")
        frames = parser.feed(good, now=now[0])
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].seq, 0x51)
        self.assertGreaterEqual(parser.timeout_discards, 1)

    def test_gap_less_than_timeout_keeps_waiting(self) -> None:
        """未超过 1 秒时不得清空缓冲。"""
        now = [0.0]
        parser = protocol.FrameParser(clock=lambda: now[0])
        frame = self._frame(protocol.CMD_PING, 0x60, b"")
        parser.feed(frame[:4], now=now[0])
        now[0] = 0.999
        self.assertFalse(parser.poll_timeout(now=now[0]))
        frames = parser.feed(frame[4:], now=now[0])
        self.assertEqual(len(frames), 1)


class TestPayloadCodec(unittest.TestCase):
    """SET_QR、ACK 与图像事件 payload 编解码。"""

    def test_set_qr_contains_utf8_bytes(self) -> None:
        """中文文本必须原样按 UTF-8 字节写入 payload。"""
        text = "二维码测试-中文 UTF-8"
        payload = protocol.encode_set_qr(text, ecc=protocol.ECC_M)
        self.assertEqual(payload[0], protocol.SYM_QR)
        self.assertEqual(payload[1], protocol.ECC_M)
        self.assertEqual(payload[2:], text.encode("utf-8"))
        decoded = protocol.decode_set_qr(payload)
        self.assertEqual(decoded.text, text)
        self.assertEqual(decoded.raw_text, text.encode("utf-8"))

    def test_utf8_byte_count_chinese(self) -> None:
        """T-006：中文字符按 UTF-8 字节计算，不按字符数。"""
        text = "二维码测试-中文 UTF-8"
        self.assertEqual(protocol.utf8_byte_length(text), 28)
        self.assertEqual(len(text.encode("utf-8")), 28)
        self.assertNotEqual(protocol.utf8_byte_length(text), len(text))

    def test_text_boundary_256_bytes(self) -> None:
        """85 个汉字加 1 个 ASCII 恰好 256 字节，允许发送。"""
        text = "汉" * 85 + "A"
        self.assertEqual(protocol.utf8_byte_length(text), 256)
        payload = protocol.encode_set_qr(text)
        self.assertEqual(len(payload), 258)

    def test_text_over_256_bytes_rejected(self) -> None:
        """86 个汉字为 258 字节，必须拒绝。"""
        with self.assertRaises(protocol.TextTooLongError) as ctx:
            protocol.encode_set_qr("汉" * 86)
        self.assertEqual(ctx.exception.byte_length, 258)

    def test_empty_and_nul_text_rejected(self) -> None:
        """空文本和嵌入 NUL 均拒绝。"""
        with self.assertRaises(protocol.PayloadError):
            protocol.encode_set_qr("")
        with self.assertRaises(protocol.PayloadError):
            protocol.encode_set_qr("abc\x00def")

    def test_generic_ack_decode(self) -> None:
        """普通 ACK 为 4 字节。"""
        ack = protocol.decode_ack_payload(bytes((0, 0, 5, 37)))
        self.assertTrue(ack.ok)
        self.assertEqual((ack.status, ack.error_code, ack.qr_version, ack.module_count), (0, 0, 5, 37))

    def test_get_status_ack_decode(self) -> None:
        """GET_STATUS ACK 为 5 字节且字段布局不同。"""
        ack = protocol.decode_get_status_ack(bytes((0, 1, 1, 4, 33)))
        self.assertTrue(ack.ok)
        self.assertTrue(ack.has_valid_qr)
        self.assertEqual(ack.last_qr_valid, 1)
        self.assertEqual(ack.symbology, 1)
        self.assertEqual(ack.qr_version, 4)
        self.assertEqual(ack.module_count, 33)

    def test_ack_length_validation(self) -> None:
        """ACK payload 长度错误应抛异常。"""
        with self.assertRaises(protocol.PayloadError):
            protocol.decode_ack_payload(b"\x00\x00\x01")
        with self.assertRaises(protocol.PayloadError):
            protocol.decode_get_status_ack(b"\x00\x00\x01\x01")

    def test_image_events_round_trip(self) -> None:
        """图像三类事件按大小端编码后可正确解码。"""
        begin_payload = protocol.encode_image_begin()
        self.assertEqual(len(begin_payload), 15)
        begin = protocol.decode_image_begin(begin_payload)
        self.assertEqual((begin.format, begin.width, begin.height), (1, 240, 240))
        self.assertEqual((begin.file_size, begin.data_size, begin.row_stride), (7742, 7680, 32))

        data_payload = protocol.encode_image_data(0x01020304, b"\xA5\x5A")
        data = protocol.decode_image_data(data_payload)
        self.assertEqual(data.offset, 0x01020304)
        self.assertEqual(data.chunk, b"\xA5\x5A")

        end_payload = protocol.encode_image_end(7742, 0xDEADBEEF)
        end = protocol.decode_image_end(end_payload)
        self.assertEqual(end.file_size, 7742)
        self.assertEqual(end.crc32, 0xDEADBEEF)

    def test_image_data_empty_chunk_rejected(self) -> None:
        """没有数据块的 EVT_IMAGE_DATA 视为非法。"""
        with self.assertRaises(protocol.PayloadError):
            protocol.decode_image_data(struct.pack("<I", 0))


if __name__ == "__main__":
    unittest.main()
