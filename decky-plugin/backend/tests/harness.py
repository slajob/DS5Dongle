"""Test harness for ds5bridged.

FakeHost plays the kernel's vhci-hcd: the daemon (started with
--usbip-connect) connects to it and receives USB/IP CMD_SUBMIT/CMD_UNLINK
exactly as the real kernel would send them.

FakeController plays the Bluetooth DualSense: the daemon (started with
--fake-bt) connects to two SOCK_SEQPACKET sockets that carry the same reports
/dev/hidrawN would (input reports in, output reports out, feature requests on
the control socket).
"""
from __future__ import annotations

import json
import os
import pathlib
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zlib

HERE = pathlib.Path(__file__).resolve().parent
BUILD = pathlib.Path(os.environ.get("DS5BRIDGE_BUILD", HERE.parent / "build"))
DAEMON = BUILD / "ds5bridged"
TESTTOOL = BUILD / "ds5bridge_testtool"
FIRMWARE_SRC = HERE.parents[2] / "src"

CMD_SUBMIT, CMD_UNLINK, RET_SUBMIT, RET_UNLINK = 1, 2, 3, 4
DIR_OUT, DIR_IN = 0, 1


def crc32_prefixed(prefix: int, data: bytes) -> int:
    return zlib.crc32(bytes([prefix]) + data) & 0xFFFFFFFF


def check_output_crc(report: bytes) -> bool:
    return crc32_prefixed(0xA2, report[:-4]) == struct.unpack("<I", report[-4:])[0]


def make_input_report(body: bytes, tag: int = 0x01) -> bytes:
    """Builds a 78-byte BT input report 0x31 (as read from hidraw)."""
    assert len(body) == 63
    report = bytearray(78)
    report[0] = 0x31
    report[1] = tag
    report[2:65] = body
    crc = crc32_prefixed(0xA1, bytes(report[:74]))
    report[74:78] = struct.pack("<I", crc)
    return bytes(report)


def neutral_body() -> bytearray:
    body = bytearray(63)
    body[0:4] = b"\x80\x80\x80\x80"
    body[7] = 0x08
    body[52] = 0x07  # 75 % battery, discharging
    return body


class Urb:
    def __init__(self, seqnum: int, direction: int, ep: int, iso: bool):
        self.seqnum = seqnum
        self.direction = direction
        self.ep = ep
        self.iso = iso
        self.done = threading.Event()
        self.status = None
        self.actual_length = 0
        self.data = b""
        self.iso_packets: list[tuple[int, int, int, int]] = []
        self.completed_at = 0.0
        self.unlink_status = None


class FakeHost:
    """The vhci-hcd side of USB/IP."""

    def __init__(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.conn: socket.socket | None = None
        self.lock = threading.Lock()
        self.urbs: dict[int, Urb] = {}
        self.unlinks: dict[int, Urb] = {}
        self.seq = 100
        self.closed = threading.Event()
        self.reader: threading.Thread | None = None

    def accept(self, timeout: float = 10.0) -> None:
        self.server.settimeout(timeout)
        self.conn, _ = self.server.accept()
        self.conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def close(self) -> None:
        if self.conn:
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.conn.close()
            except OSError:
                pass
        self.server.close()

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.conn.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def _read_loop(self) -> None:
        try:
            while True:
                header = self._recv_exact(48)
                command, seqnum = struct.unpack(">II", header[:8])
                if command == RET_SUBMIT:
                    status, actual, start, npackets, errors = struct.unpack(">iiiii", header[20:40])
                    with self.lock:
                        urb = self.urbs.get(seqnum)
                    assert urb is not None, f"RET_SUBMIT for unknown seqnum {seqnum}"
                    data = b""
                    if urb.direction == DIR_IN and actual > 0:
                        data = self._recv_exact(actual)
                    packets = []
                    for _ in range(npackets):
                        packets.append(struct.unpack(">IIIi", self._recv_exact(16)))
                    urb.status = status
                    urb.actual_length = actual
                    urb.data = data
                    urb.iso_packets = packets
                    urb.completed_at = time.monotonic()
                    urb.done.set()
                elif command == RET_UNLINK:
                    (status,) = struct.unpack(">i", header[20:24])
                    with self.lock:
                        urb = self.unlinks.get(seqnum)
                    urb.unlink_status = status
                    urb.done.set()
                else:
                    raise AssertionError(f"unexpected command {command}")
        except (EOFError, OSError):
            self.closed.set()

    def _next_seq(self) -> int:
        with self.lock:
            self.seq += 1
            return self.seq

    def submit(self, ep: int, direction: int, length: int, data: bytes = b"", setup: bytes = bytes(8),
               iso_lengths: list[int] | None = None) -> Urb:
        seqnum = self._next_seq()
        urb = Urb(seqnum, direction, ep, iso_lengths is not None)
        with self.lock:
            self.urbs[seqnum] = urb
        npackets = len(iso_lengths) if iso_lengths is not None else 0
        header = struct.pack(">IIIII", CMD_SUBMIT, seqnum, 0x00010002, direction, ep)
        header += struct.pack(">IiiiI", 0, length, 0, npackets, 1)
        header += setup
        payload = data if direction == DIR_OUT else b""
        iso = b""
        offset = 0
        for plen in iso_lengths or []:
            iso += struct.pack(">IIIi", offset, plen, 0, 0)
            offset += plen
        self.conn.sendall(header + payload + iso)
        return urb

    def unlink(self, target: Urb) -> Urb:
        seqnum = self._next_seq()
        marker = Urb(seqnum, DIR_OUT, 0, False)
        with self.lock:
            self.unlinks[seqnum] = marker
        header = struct.pack(">IIIII", CMD_UNLINK, seqnum, 0x00010002, 0, 0)
        header += struct.pack(">I", target.seqnum) + bytes(24)
        self.conn.sendall(header)
        return marker

    def control(self, request_type: int, request: int, value: int, index: int, length: int,
                data: bytes = b"", timeout: float = 5.0) -> Urb:
        setup = struct.pack("<BBHHH", request_type, request, value, index, length if request_type & 0x80 else len(data))
        direction = DIR_IN if request_type & 0x80 else DIR_OUT
        urb = self.submit(0, direction, length if direction == DIR_IN else len(data), data, setup)
        assert urb.done.wait(timeout), f"control request {request_type:#x}/{request:#x} timed out"
        return urb


class FakeController:
    """A DualSense on the other end of the daemon's fake Bluetooth link."""

    def __init__(self, path: str, features: dict[int, bytes]):
        self.path = path
        self.features = dict(features)
        self.intr_server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.intr_server.bind(path)
        self.intr_server.listen(1)
        self.ctl_server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.ctl_server.bind(path + ".ctl")
        self.ctl_server.listen(1)
        self.intr: socket.socket | None = None
        self.ctl: socket.socket | None = None
        self.outputs: list[tuple[float, bytes]] = []
        self.feature_requests: list[int] = []
        self.feature_sets: list[bytes] = []
        self.lock = threading.Lock()
        self.connected = threading.Event()
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self) -> None:
        self.intr, _ = self.intr_server.accept()
        self.ctl, _ = self.ctl_server.accept()
        self.connected.set()
        threading.Thread(target=self._intr_loop, daemon=True).start()
        threading.Thread(target=self._ctl_loop, daemon=True).start()

    def _intr_loop(self) -> None:
        while True:
            try:
                data = self.intr.recv(4096)
            except OSError:
                return
            if not data:
                return
            with self.lock:
                self.outputs.append((time.monotonic(), data))

    def _ctl_loop(self) -> None:
        while True:
            try:
                req = self.ctl.recv(4096)
            except OSError:
                return
            if not req:
                return
            if req[0] == ord("G"):
                report_id = req[1]
                self.feature_requests.append(report_id)
                self.ctl.send(self.features.get(report_id, b""))
            elif req[0] == ord("S"):
                self.feature_sets.append(req[1:])
                self.ctl.send(b"\x00")

    def send_input(self, report: bytes) -> None:
        self.intr.send(report)

    def disconnect(self) -> None:
        for s in (self.intr, self.ctl):
            if s:
                try:
                    s.shutdown(socket.SHUT_RDWR)  # wakes our reader threads, peer sees EOF
                except OSError:
                    pass
                s.close()

    def close(self) -> None:
        self.disconnect()
        self.intr_server.close()
        self.ctl_server.close()

    def reports(self, report_id: int | None = None, since: float = 0.0) -> list[bytes]:
        with self.lock:
            return [r for t, r in self.outputs if t >= since and (report_id is None or r[0] == report_id)]

    def wait_for(self, predicate, timeout: float = 3.0, since: float = 0.0) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for r in self.reports(since=since):
                if predicate(r):
                    return r
            time.sleep(0.005)
        raise AssertionError("expected output report never arrived")


def default_features() -> dict[int, bytes]:
    def feature(report_id: int, size: int, fill: int) -> bytes:
        data = bytearray([report_id] + [fill] * (size - 1))
        return bytes(data)

    firmware = bytearray(feature(0x20, 64, 0x00))
    firmware[1:12] = b"Jun 10 2024"
    firmware[22] = 0x00  # not an Edge
    return {
        0x05: feature(0x05, 41, 0x11),
        0x09: feature(0x09, 20, 0x22),
        0x20: bytes(firmware),
        0x22: feature(0x22, 64, 0x33),
        0x81: feature(0x81, 64, 0x44),
    }


class DaemonUnderTest:
    def __init__(self, features: dict[int, bytes] | None = None, extra_args: list[str] | None = None,
                 fake_pid: int = 0x0CE6):
        self.tmp = tempfile.TemporaryDirectory(prefix="ds5bridge-test-")
        self.socket_path = os.path.join(self.tmp.name, "ctl.sock")
        self.host = FakeHost()
        self.controller = FakeController(os.path.join(self.tmp.name, "bt"), features or default_features())
        args = [str(DAEMON), "--no-hide", "--verbose", "--socket", self.socket_path,
                "--usbip-connect", f"127.0.0.1:{self.host.port}",
                "--fake-bt", self.controller.path, "--fake-pid", hex(fake_pid)]
        args += extra_args or []
        self.log_path = os.path.join(self.tmp.name, "daemon.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        assert self.controller.connected.wait(10), "daemon never connected to the fake controller"
        self.host.accept()

    def ipc(self, command: str) -> str:
        deadline = time.monotonic() + 5
        while True:
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.connect(self.socket_path)
                    s.sendall(command.encode() + b"\n")
                    buf = b""
                    while not buf.endswith(b"\n"):
                        chunk = s.recv(65536)
                        if not chunk:
                            break
                        buf += chunk
                    return buf.decode().strip()
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)

    def status(self) -> dict:
        return json.loads(self.ipc("STATUS"))

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.host.close()
        self.controller.close()
        self.log.close()
        self.final_log = pathlib.Path(self.log_path).read_text()
        self.tmp.cleanup()

    def log_text(self) -> str:
        if getattr(self, "final_log", None) is not None:
            return self.final_log
        return pathlib.Path(self.log_path).read_text()

    # --- USB helpers -------------------------------------------------------
    def enumerate(self) -> None:
        dev = self.host.control(0x80, 6, 0x0100, 0, 18)
        assert dev.status == 0
        config = self.host.control(0x80, 6, 0x0200, 0, 9)
        total = struct.unpack("<H", config.data[2:4])[0]
        self.host.control(0x80, 6, 0x0200, 0, total)
        assert self.host.control(0x00, 9, 1, 0, 0).status == 0


# --- firmware parity helpers ------------------------------------------------
def firmware_configuration_descriptor() -> bytes:
    """Evaluates descriptor_configuration[] from the firmware source for the
    default build (ENABLE_SERIAL=0, ENABLE_WAKE_HID undefined)."""
    import re

    src = (FIRMWARE_SRC / "usb_descriptors.cpp").read_text()
    body = re.search(r"uint8_t descriptor_configuration\[\] = \{(.*?)\n\};", src, re.S).group(1)
    defines = {"ENABLE_SERIAL": False, "ENABLE_WAKE_HID": False}
    out_lines = []
    stack: list[bool] = []
    for line in body.split("\n"):
        stripped = line.strip()
        if stripped.startswith("#if "):
            stack.append(defines[stripped.split()[1]])
            continue
        if stripped.startswith("#ifdef"):
            stack.append(defines[stripped.split()[1]])
            continue
        if stripped.startswith("#else"):
            stack[-1] = not stack[-1]
            continue
        if stripped.startswith("#endif"):
            stack.pop()
            continue
        if all(stack):
            out_lines.append(line.split("//")[0])
    tokens = [t.strip() for t in ",".join(out_lines).split(",") if t.strip()]
    values: list[int] = []
    for token in tokens:
        if token == "U16_TO_U8S_LE(CONFIG_DESC_LEN_TOTAL)":
            values += [0xE3, 0x00]
        elif token == "ITF_NUM_TOTAL":
            values.append(4)
        else:
            values.append(int(token, 16))
    return bytes(values)


def firmware_hid_report_descriptor(name: str) -> bytes:
    import re

    src = (FIRMWARE_SRC / "usb_descriptors.cpp").read_text()
    body = re.search(r"uint8_t const " + name + r"\[\] = \{(.*?)\n\};", src, re.S).group(1)
    values = []
    for line in body.split("\n"):
        values += [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{1,2}\b", line.split("//")[0])]
    return bytes(values)
