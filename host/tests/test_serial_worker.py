"""串口工作线程的无硬件测试（使用假串口对象）。"""

from __future__ import annotations

import queue
import sys
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock

HOST_DIR = Path(__file__).resolve().parents[1]
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))

import protocol
import serial_worker


class _FakeSerial:
    """最小串口替身：写入请求后自动回 ACK，供线程逻辑测试。"""

    def __init__(self) -> None:
        self.written: list[bytes] = []
        self._rx = bytearray()
        self._lock = threading.Lock()
        self.closed = False

    def write(self, data: bytes) -> int:
        self.written.append(bytes(data))
        frame = protocol.parse_frame(data)
        if frame.cmd == protocol.CMD_GET_STATUS:
            payload = bytes((protocol.STATUS_OK, 1, protocol.SYM_QR, 3, 29))
        else:
            payload = bytes((protocol.STATUS_OK, protocol.ERR_NONE, 3, 29))
        ack = protocol.build_frame(
            frame.cmd | protocol.ACK_CMD_OFFSET, frame.seq, payload
        )
        with self._lock:
            self._rx.extend(ack)
        return len(data)

    def flush(self) -> None:
        return None

    def read(self, size: int) -> bytes:
        with self._lock:
            if self._rx:
                data = bytes(self._rx[:size])
                del self._rx[:size]
                return data
        time.sleep(0.003)
        return b""

    def close(self) -> None:
        self.closed = True


def _wait_for_event(
    events: queue.Queue[object],
    event_type: type[object],
    timeout: float = 2.0,
) -> object:
    """在超时内等待指定类型事件。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            event = events.get(timeout=0.05)
        except queue.Empty:
            continue
        if isinstance(event, event_type):
            return event
    raise AssertionError(f"等待 {event_type.__name__} 超时")


class _RetryOnceSerial(_FakeSerial):
    """第一次不回复，收到重试帧后才回复 ACK，用于验证显式重试。"""

    def write(self, data: bytes) -> int:
        self.written.append(bytes(data))
        frame = protocol.parse_frame(data)
        if len(self.written) >= 2:
            payload = bytes((protocol.STATUS_OK, protocol.ERR_NONE, 2, 25))
            ack = protocol.build_frame(
                frame.cmd | protocol.ACK_CMD_OFFSET, frame.seq, payload
            )
            with self._lock:
                self._rx.extend(ack)
        return len(data)


class TestSerialWorkerLifecycle(unittest.TestCase):
    """工作线程的启动、ACK 匹配和关闭。"""

    def test_disconnected_send_rejected(self) -> None:
        """未连接时发送命令应抛出中文错误。"""
        worker = serial_worker.SerialWorker("COM_UNIT_TEST", 115200)
        self.assertFalse(worker.is_connected)
        self.assertEqual(worker.state, serial_worker.ConnectionState.DISCONNECTED)
        with self.assertRaises(serial_worker.SerialWorkerError):
            worker.send_command(protocol.CMD_PING)

    def test_ping_ack_matched_in_worker_thread(self) -> None:
        """假串口回 ACK 后，工作线程应投递匹配的 AckEvent。"""
        instance = _FakeSerial()

        def factory(**_kwargs: object) -> _FakeSerial:
            return instance

        fake_module = types.SimpleNamespace(
            Serial=factory,
            EIGHTBITS=8,
            PARITY_NONE="N",
            STOPBITS_ONE=1,
        )
        events: queue.Queue[object] = queue.Queue()
        worker = serial_worker.SerialWorker(
            "COM_FAKE", 115200, event_queue=events
        )
        with mock.patch.object(serial_worker, "_import_pyserial", return_value=fake_module):
            worker.start()
            try:
                connected = _wait_for_event(
                    events, serial_worker.ConnectionEvent, timeout=2.0
                )
                assert isinstance(connected, serial_worker.ConnectionEvent)
                self.assertTrue(connected.connected)
                ticket = worker.send_command(protocol.CMD_PING, timeout=1.0)
                ack = _wait_for_event(events, serial_worker.AckEvent, timeout=2.0)
                assert isinstance(ack, serial_worker.AckEvent)
                self.assertTrue(ack.ok, ack.message)
                self.assertEqual(ack.cmd, protocol.CMD_PING)
                self.assertEqual(ack.seq, ticket.seq)
                self.assertEqual((ack.qr_version, ack.module_count), (3, 29))
                self.assertEqual(len(instance.written), 1)
                sent = protocol.parse_frame(instance.written[0])
                self.assertEqual(sent.cmd, protocol.CMD_PING)
                self.assertEqual(sent.seq, ticket.seq)

                status_ticket = worker.send_command(protocol.CMD_GET_STATUS, timeout=1.0)
                status_ack = _wait_for_event(
                    events, serial_worker.AckEvent, timeout=2.0
                )
                assert isinstance(status_ack, serial_worker.AckEvent)
                self.assertTrue(status_ack.ok, status_ack.message)
                self.assertEqual(status_ack.cmd, protocol.CMD_GET_STATUS)
                self.assertEqual(status_ack.seq, status_ticket.seq)
                self.assertEqual(status_ack.last_qr_valid, 1)
                self.assertEqual(status_ack.symbology, protocol.SYM_QR)
                self.assertEqual((status_ack.qr_version, status_ack.module_count), (3, 29))
            finally:
                worker.stop()
        self.assertTrue(instance.closed)
        self.assertFalse(worker.is_connected)

    def test_set_qr_explicit_retry_uses_new_seq(self) -> None:
        """显式 retries=2 时，超时重试应换新 SEQ 并最终匹配 ACK。"""
        instance = _RetryOnceSerial()

        def factory(**_kwargs: object) -> _RetryOnceSerial:
            return instance

        fake_module = types.SimpleNamespace(
            Serial=factory,
            EIGHTBITS=8,
            PARITY_NONE="N",
            STOPBITS_ONE=1,
        )
        events: queue.Queue[object] = queue.Queue()
        worker = serial_worker.SerialWorker("COM_RETRY", 115200, event_queue=events)
        with mock.patch.object(serial_worker, "_import_pyserial", return_value=fake_module):
            worker.start()
            try:
                _wait_for_event(events, serial_worker.ConnectionEvent, timeout=2.0)
                ticket = worker.send_command(
                    protocol.CMD_SET_QR,
                    protocol.encode_set_qr("retry"),
                    timeout=0.1,
                    retries=2,
                )
                ack = _wait_for_event(events, serial_worker.AckEvent, timeout=2.0)
                assert isinstance(ack, serial_worker.AckEvent)
                self.assertTrue(ack.ok, ack.message)
                self.assertEqual(ack.attempts, 2)
                self.assertEqual(ack.qr_version, 2)
                self.assertEqual(len(instance.written), 2)
                first = protocol.parse_frame(instance.written[0])
                second = protocol.parse_frame(instance.written[1])
                self.assertEqual(first.seq, ticket.seq)
                self.assertNotEqual(first.seq, second.seq)
            finally:
                worker.stop()

    def test_missing_pyserial_reports_connection_error(self) -> None:
        """pyserial 缺失时通过连接事件报告中文安装提示。"""
        events: queue.Queue[object] = queue.Queue()
        worker = serial_worker.SerialWorker("COM_NO_SERIAL", 115200, event_queue=events)

        def raise_error() -> None:
            raise serial_worker.SerialWorkerError("未安装 pyserial 的测试提示")

        with mock.patch.object(serial_worker, "_import_pyserial", side_effect=raise_error):
            worker.start()
            event = _wait_for_event(events, serial_worker.ConnectionEvent, timeout=2.0)
            assert isinstance(event, serial_worker.ConnectionEvent)
            self.assertFalse(event.connected)
            self.assertIn("pyserial", event.message)
            deadline = time.monotonic() + 2.0
            while worker.is_running and time.monotonic() < deadline:
                time.sleep(0.01)
        self.assertFalse(worker.is_connected)


if __name__ == "__main__":
    unittest.main()
