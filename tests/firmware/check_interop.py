#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    check_interop.py
@brief   MCU（C 固件）与 PC（Python 上位机）协议实现的跨语言一致性校验。

@details
    `test_main.c` 会把**固件真实函数**（`proto_send_frame()` / `proto_send_ack()`）
    生成的帧写到 `tests/firmware/out/*.bin`。本脚本用 `host/protocol.py`
    （上位机实际使用的模块）重新构造"本应产生"的同样帧，并逐字节比对；
    然后再用上位机的解析函数去读固件产出的帧，检查字段级一致。

    这样两端只要有任何一处布局、端序、CRC 覆盖范围不同，都会被立刻发现
    （风险 F 的对策：MCU/PC 共用测试向量）。

@usage   python tests/firmware/check_interop.py
@return  0 全部一致；1 有不一致
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
HOST_DIR = REPO / "host"
OUT_DIR = HERE / "out"

sys.path.insert(0, str(HOST_DIR))

import protocol as P  # noqa: E402  （必须先插入 sys.path）

FAILS: list[str] = []
CHECKS = 0


def check(cond: bool, msg: str) -> None:
    """记录一项检查结果。"""
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILS.append(msg)
        print(f"  [FAIL] {msg}")


def read_bin(name: str) -> bytes:
    """读取固件导出的帧文件。"""
    path = OUT_DIR / name
    if not path.exists():
        check(False, f"{name} 不存在（请先运行 test_fw.exe）")
        return b""
    return path.read_bytes()


def main() -> int:
    print("MCU(C) <-> PC(Python) 协议一致性比对")
    print(f"  固件帧目录：{OUT_DIR}")
    print(f"  上位机模块：{HOST_DIR / 'protocol.py'}")

    # ---- 1. PING ----
    c_ping = read_bin("f_ping.bin")
    py_ping = P.build_frame(P.CMD_PING, 0x07)
    check(c_ping == py_ping, f"PING 帧逐字节一致 (C={c_ping.hex()} Py={py_ping.hex()})")

    # ---- 2. SET_QR ----
    c_setqr = read_bin("f_setqr.bin")
    py_setqr = P.build_frame(
        P.CMD_SET_QR, 0x42, P.encode_set_qr("Hello 123", ecc=P.ECC_M, symbology=P.SYM_QR)
    )
    check(c_setqr == py_setqr, "SET_QR 帧逐字节一致（含 UTF-8 文本）")

    # 用上位机解析固件产生的 SET_QR 帧
    if c_setqr:
        try:
            fr = P.parse_frame(c_setqr)
            check(fr.cmd == P.CMD_SET_QR, "解析出的 CMD == SET_QR")
            check(fr.seq == 0x42, "解析出的 SEQ == 0x42")
            body = P.decode_set_qr(fr.payload)
            check(body.symbology == P.SYM_QR, "payload symbology == QR")
            check(body.ecc == P.ECC_M, "payload ecc == M")
            check(body.text == "Hello 123", f"payload 文本 == 'Hello 123'（got {body.text!r}）")
            check(body.raw_text == b"Hello 123", "payload 原始字节一致")
        except Exception as exc:  # noqa: BLE001
            check(False, f"上位机解析固件 SET_QR 帧失败：{exc}")

    # ---- 3. 普通 ACK ----
    c_ack = read_bin("f_ack.bin")
    py_ack = P.build_frame(
        P.CMD_SET_QR | 0x80, 0x42, bytes((P.STATUS_OK, P.ERR_NONE, 3, 29))
    )
    check(c_ack == py_ack, "ACK 帧逐字节一致（status/err/version/modules）")
    if c_ack:
        try:
            fr = P.parse_frame(c_ack)
            check(fr.cmd == (P.CMD_SET_QR | 0x80), "ACK CMD == SET_QR|0x80")
            check(fr.seq == 0x42, "ACK SEQ 与请求一致")
            ack = P.decode_ack_payload(fr.payload)
            check(ack.status == P.STATUS_OK, "ACK status == OK")
            check(ack.error_code == P.ERR_NONE, "ACK error_code == NONE")
            check(ack.qr_version == 3, "ACK qr_version == 3")
            check(ack.module_count == 29, "ACK module_count == 29")
        except Exception as exc:  # noqa: BLE001
            check(False, f"上位机解析固件 ACK 帧失败：{exc}")

    # ---- 4. GET_STATUS ACK（5 字节 payload）----
    c_status = read_bin("f_status_ack.bin")
    py_status = P.build_frame(
        P.CMD_GET_STATUS | 0x80, 0x10, bytes((P.STATUS_OK, 1, P.SYM_QR, 3, 29))
    )
    check(c_status == py_status, "GET_STATUS ACK 帧逐字节一致（5 字节 payload）")
    if c_status:
        try:
            fr = P.parse_frame(c_status)
            st = P.decode_get_status_ack(fr.payload)
            check(st.last_qr_valid == 1, "status last_qr_valid == 1")
            check(st.symbology == P.SYM_QR, "status symbology == QR")
            check(st.qr_version == 3, "status qr_version == 3")
            check(st.module_count == 29, "status module_count == 29")
        except Exception as exc:  # noqa: BLE001
            check(False, f"上位机解析固件 GET_STATUS ACK 失败：{exc}")

    # ---- 5. 共用 CRC 测试向量 ----
    check(P.crc16_modbus(b"123456789") == 0x4B37, "上位机 CRC16 测试向量 0x4B37")
    check(
        P.crc16_modbus_bytes(b"123456789") == b"\x37\x4b",
        "上位机 CRC16 线上字节序为低字节在前 37 4B",
    )

    print("-" * 46)
    print(f" 比对 {CHECKS} 项，失败 {len(FAILS)} 项")
    print("-" * 46)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    raise SystemExit(main())
