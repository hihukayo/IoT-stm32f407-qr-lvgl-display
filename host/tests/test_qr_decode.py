"""图片二维码解码（qr_decode）单元测试。

需要 Pillow + zxing-cpp；未安装时整组用例跳过，不影响其它测试。
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

# 允许从 host 目录直接运行 ``python -m unittest discover -s tests -v``。
HOST_DIR = Path(__file__).resolve().parents[1]
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))

import qr_decode

REPO_DIR = Path(__file__).resolve().parents[2]
SAMPLE_QR = REPO_DIR / "docs" / "images" / "qr-example-hello123.png"

HAS_BACKEND = qr_decode.decoder_available()
SKIP_REASON = "未安装 Pillow 或 zxing-cpp，跳过图片解码测试"


@unittest.skipUnless(HAS_BACKEND, SKIP_REASON)
class TestDecodeQrImage(unittest.TestCase):
    """真实图片的解码行为。"""

    def test_decodes_repository_sample(self) -> None:
        """仓库自带的示例二维码应解出 Hello 123。"""
        self.assertTrue(SAMPLE_QR.is_file(), f"缺少示例图片 {SAMPLE_QR}")
        result = qr_decode.decode_qr_image(SAMPLE_QR)
        self.assertEqual(result.text, "Hello 123")
        self.assertIn("QR", result.format)
        self.assertEqual(result.count, 1)
        self.assertEqual(result.byte_length, len("Hello 123".encode("utf-8")))

    def test_still_decodes_after_downscale(self) -> None:
        """把图片缩小到一半仍应识别（模拟屏幕截图/缩略图）。"""
        from PIL import Image

        with tempfile.TemporaryDirectory() as tmp:
            small = Path(tmp) / "small.png"
            with Image.open(SAMPLE_QR) as img:
                img.resize((img.width // 2, img.height // 2), Image.NEAREST).save(small)
            result = qr_decode.decode_qr_image(small)
        self.assertEqual(result.text, "Hello 123")

    def test_counts_multiple_codes(self) -> None:
        """一张图里并排两个码时，取第一个并报告总数。"""
        from PIL import Image

        with Image.open(SAMPLE_QR) as img:
            sample = img.convert("RGB")
        canvas = Image.new("RGB", (sample.width * 2 + 40, sample.height), "white")
        canvas.paste(sample, (0, 0))
        canvas.paste(sample, (sample.width + 40, 0))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "two.png"
            canvas.save(path)
            result = qr_decode.decode_qr_image(path)
        self.assertEqual(result.text, "Hello 123")
        self.assertEqual(result.count, 2)


@unittest.skipUnless(HAS_BACKEND, SKIP_REASON)
class TestDecodeErrors(unittest.TestCase):
    """异常输入必须给出可展示的中文错误，而不是抛原生异常。"""

    def test_missing_file(self) -> None:
        with self.assertRaises(qr_decode.QrDecodeError) as ctx:
            qr_decode.decode_qr_image(REPO_DIR / "不存在的图片.png")
        self.assertIn("不存在", str(ctx.exception))

    def test_not_an_image(self) -> None:
        with self.assertRaises(qr_decode.QrDecodeError) as ctx:
            qr_decode.decode_qr_image(REPO_DIR / "README.md")
        self.assertIn("无法读取图片", str(ctx.exception))

    def test_blank_image_has_no_qr(self) -> None:
        from PIL import Image

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "blank.png"
            Image.new("RGB", (240, 240), "white").save(path)
            with self.assertRaises(qr_decode.QrDecodeError) as ctx:
                qr_decode.decode_qr_image(path)
        self.assertIn("没有识别到二维码", str(ctx.exception))

    def test_try_harder_off_reports_same_error(self) -> None:
        from PIL import Image

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "blank.png"
            Image.new("RGB", (120, 120), "gray").save(path)
            with self.assertRaises(qr_decode.QrDecodeError):
                qr_decode.decode_qr_image(path, try_harder=False)


class TestHelpers(unittest.TestCase):
    """不依赖后端的纯逻辑。"""

    def test_install_hint_mentions_both_packages(self) -> None:
        hint = qr_decode.install_hint()
        self.assertIn("Pillow", hint)
        self.assertIn("zxing-cpp", hint)

    def test_decoded_qr_byte_length_uses_utf8(self) -> None:
        result = qr_decode.DecodedQr(text="二维码", format="QR Code", count=1, source="x")
        self.assertEqual(result.byte_length, 9)

    def test_supported_suffixes_are_lowercase_dotted(self) -> None:
        for suffix in qr_decode.SUPPORTED_SUFFIXES:
            self.assertTrue(suffix.startswith("."))
            self.assertEqual(suffix, suffix.lower())


if __name__ == "__main__":
    unittest.main()
