"""CRC 校验辅助模块。

本模块只依赖 Python 标准库，实现协议中使用的 CRC-32/ISO-HDLC：
- 反射多项式：0xEDB88320
- 初始值：0xFFFFFFFF
- 输出前与 0xFFFFFFFF 异或
- 校验向量：ASCII "123456789" -> 0xCBF43926

协议中的 CRC-16/MODBUS 位于 :mod:`protocol`，两者用途不同，不能混用。
"""

from __future__ import annotations

from typing import Iterable

__all__ = ["Crc32", "crc32_iso_hdlc"]


def _build_crc32_table() -> tuple[int, ...]:
    """生成 CRC-32 反射算法使用的 256 项查找表。"""
    table: list[int] = []
    for value in range(256):
        crc = value
        for _ in range(8):
            if crc & 0x1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
        table.append(crc & 0xFFFFFFFF)
    return tuple(table)


_CRC32_TABLE = _build_crc32_table()


class Crc32:
    """可增量更新的 CRC-32/ISO-HDLC 计算器。

    用于逐块接收 BMP 数据，避免为整份文件额外建立内存副本。
    """

    def __init__(self) -> None:
        """创建一个内部状态为 0xFFFFFFFF 的计算器。"""
        self._value = 0xFFFFFFFF

    def update(self, data: Iterable[int] | bytes | bytearray | memoryview) -> "Crc32":
        """追加数据并返回自身，便于链式调用。"""
        if isinstance(data, (bytes, bytearray, memoryview)):
            payload = bytes(data)
        else:
            payload = bytes(data)
        crc = self._value
        table = _CRC32_TABLE
        for byte in payload:
            crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
        self._value = crc & 0xFFFFFFFF
        return self

    def digest(self) -> int:
        """返回最终 CRC-32 值（已执行最终异或）。"""
        return self._value ^ 0xFFFFFFFF

    def reset(self) -> None:
        """恢复为初始状态。"""
        self._value = 0xFFFFFFFF


def crc32_iso_hdlc(data: bytes | bytearray | memoryview) -> int:
    """计算一段字节的 CRC-32/ISO-HDLC。

    参数:
        data: 待校验的原始字节。

    返回:
        32 位无符号整数。整个 BMP 文件的 CRC32 使用此函数。
    """
    return Crc32().update(data).digest()
