"""界面模块的无窗口导入测试。"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HOST_DIR = Path(__file__).resolve().parents[1]
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))

import protocol

try:
    import main
except ModuleNotFoundError as exc:  # 某些精简 Python 构建不带 Tkinter
    if exc.name != "tkinter":
        raise
    main = None  # type: ignore[assignment]
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None


@unittest.skipIf(main is None, f"当前 Python 缺少 Tkinter：{_IMPORT_ERROR}")
class TestMainModule(unittest.TestCase):
    """确认 main.py 不需要 pyserial，也不会在导入时创建窗口。"""

    def test_import_main_without_serial(self) -> None:
        """导入 main 应成功，并暴露 HostApp 与字节上限常量。"""
        self.assertTrue(hasattr(main, "HostApp"))
        self.assertEqual(protocol.MAX_TEXT_BYTES, 256)

    def test_default_save_name_format(self) -> None:
        """默认文件名模板必须符合 qr_YYYYMMDD_HHMMSS.bmp。"""
        from datetime import datetime

        name = datetime(2026, 10, 28, 23, 2, 45).strftime(main.DEFAULT_SAVE_NAME_FORMAT)
        self.assertEqual(name, "qr_20261028_230245.bmp")


if __name__ == "__main__":
    unittest.main()
