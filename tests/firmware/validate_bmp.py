#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    validate_bmp.py
@brief   校验 test_fw.exe 导出的 BMP 是否为合法 1bpp BMP，并尝试真实解码二维码。
@details
          两层校验：
          1. **结构校验**（必需，只依赖 Pillow）：文件大小 7742、尺寸 240x240、
             模式 '1'、关键头字段（bfOffBits/biBitCount/biHeight/biSizeImage…）；
          2. **内容校验**（可选，需要 opencv-python）：用 OpenCV 的 QRCodeDetector
             真正解码，确认 `Hello 123`、URL、中文 UTF-8 三组文本逐字节一致。
             该步骤证明"编码 -> 1bpp 位图 -> BMP 文件"整条链路正确。

@usage   python validate_bmp.py [输出目录]
@return  0 全部通过；1 有失败
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

EXPECT = {
    "qr_hello123.bmp": "Hello 123",
    "qr_url.bmp": "https://example.com/qr-test",
    "qr_chinese.bmp": "二维码测试-中文 UTF-8",
}

FAILS: list[str] = []
CHECKS = 0


def check(cond: bool, msg: str) -> None:
    """记录一项检查结果。"""
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILS.append(msg)
        print(f"  [FAIL] {msg}")


def check_header(path: Path, data: bytes) -> None:
    """按 BMP 规范逐字段校验文件头。"""
    check(data[0:2] == b"BM", f"{path.name}: bfType == 'BM'")
    (bf_size,) = struct.unpack_from("<I", data, 2)
    (bf_off,) = struct.unpack_from("<I", data, 10)
    (bi_size,) = struct.unpack_from("<I", data, 14)
    (bi_w,) = struct.unpack_from("<i", data, 18)
    (bi_h,) = struct.unpack_from("<i", data, 22)
    (bi_planes,) = struct.unpack_from("<H", data, 26)
    (bi_bpp,) = struct.unpack_from("<H", data, 28)
    (bi_comp,) = struct.unpack_from("<I", data, 30)
    (bi_img,) = struct.unpack_from("<I", data, 34)
    (bi_clrused,) = struct.unpack_from("<I", data, 46)

    check(bf_size == 7742, f"{path.name}: bfSize == 7742 (got {bf_size})")
    check(bf_off == 62, f"{path.name}: bfOffBits == 62")
    check(bi_size == 40, f"{path.name}: biSize == 40")
    check(bi_w == 240, f"{path.name}: biWidth == 240")
    check(bi_h == -240, f"{path.name}: biHeight == -240 (top-down)")
    check(bi_planes == 1, f"{path.name}: biPlanes == 1")
    check(bi_bpp == 1, f"{path.name}: biBitCount == 1")
    check(bi_comp == 0, f"{path.name}: biCompression == BI_RGB")
    check(bi_img == 7680, f"{path.name}: biSizeImage == 7680")
    check(bi_clrused == 2, f"{path.name}: biClrUsed == 2")
    check(len(data) == 7742, f"{path.name}: 实际文件长度 == 7742")

    # 调色板：索引 0 黑，索引 1 白
    check(data[54:57] == b"\x00\x00\x00", f"{path.name}: 调色板[0] 为黑")
    check(data[58:61] == b"\xff\xff\xff", f"{path.name}: 调色板[1] 为白")


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "out"

    if not out_dir.is_dir():
        print(f"[ERROR] 输出目录不存在：{out_dir}（请先运行 test_fw.exe）")
        return 1

    # ---- Pillow ----
    try:
        from PIL import Image  # noqa: PLC0415
    except ImportError:
        print("[SKIP] 未安装 Pillow，跳过结构校验")
        Image = None  # type: ignore[assignment]

    # ---- OpenCV（可选）----
    decoder = None
    try:
        import cv2  # noqa: PLC0415

        decoder = cv2.QRCodeDetector()
    except ImportError:
        print("[SKIP] 未安装 opencv-python，跳过二维码真实解码")

    print(f"校验目录：{out_dir}")

    for name, text in EXPECT.items():
        path = out_dir / name
        if not path.exists():
            check(False, f"{name}: 文件不存在")
            continue

        data = path.read_bytes()
        check_header(path, data)

        if Image is not None:
            img = Image.open(path)
            check(img.size == (240, 240), f"{name}: Pillow 解析尺寸 240x240")
            check(img.mode == "1", f"{name}: Pillow 解析模式为 1bpp")
            img.close()

        if decoder is not None:
            import cv2  # noqa: PLC0415

            gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
            decoded, _pts, _straight = decoder.detectAndDecode(gray)
            check(
                decoded == text,
                f"{name}: OpenCV 解码结果 == 期望文本 (got {decoded!r})",
            )

    print("-" * 46)
    print(f" 检查 {CHECKS} 项，失败 {len(FAILS)} 项")
    print("-" * 46)

    return 0 if not FAILS else 1


if __name__ == "__main__":
    raise SystemExit(main())
