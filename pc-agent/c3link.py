"""보드(ESP32-C3)에 이 PC의 터치패드 주소를 USB 시리얼로 알려 주기.

- 포트는 잠깐만 열고(한 줄 보내고 응답 읽고) 바로 닫음 → 설정 페이지(Web Serial)와 안 부딪힘
- DTR/RTS를 꺼 둔 채로 열어서 C3(USB Serial/JTAG)가 리셋되지 않게 함
- 보내는 것: {"id":N,"cmd":"agent","ip":"...","port":P,"token":"..."}
- 응답: @{"id":N,"ok":true,"fw":"2.3.0","pc":"on","boardIp":"192.168.0.23"}
"""
import json
import logging
import time

import serial
from serial.tools import list_ports

log = logging.getLogger("c3")

VID, PID = 0x303A, 0x1001


def find_ports():
    return sorted(p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID)


class PushResult:
    def __init__(self, ok, port=None, error=None, board_ip=None, fw=None):
        self.ok, self.port, self.error, self.board_ip, self.fw = ok, port, error, board_ip, fw

    def __repr__(self):
        return f"PushResult(ok={self.ok}, port={self.port}, error={self.error}, board_ip={self.board_ip}, fw={self.fw})"


_req_id = int(time.time()) % 100000


def push_one(device, ip, port, token, timeout=2.5, serial_factory=serial.Serial):
    global _req_id
    _req_id += 1
    rid = _req_id
    line = json.dumps({"id": rid, "cmd": "agent", "ip": ip, "port": port, "token": token},
                      separators=(",", ":")) + "\n"
    s = serial_factory()
    s.port = device
    s.baudrate = 115200
    s.timeout = 0.2
    s.write_timeout = 1
    s.dtr = False                      # 열기 전에 꺼 둠 → 리셋 신호 안 나감
    s.rts = False
    try:
        s.open()
    except (serial.SerialException, OSError) as e:
        msg = str(e)
        busy = "PermissionError" in msg or "Access is denied" in msg or "busy" in msg.lower()
        return PushResult(False, device, "busy" if busy else "open: " + msg[:120])
    try:
        s.reset_input_buffer()
        s.write(line.encode())
        s.flush()
        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = s.read(512)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                raw = raw.strip()
                if not raw.startswith(b"@{"):
                    continue                       # 로그 줄
                try:
                    r = json.loads(raw[1:].decode("utf-8", "replace"))
                except ValueError:
                    continue
                if r.get("id") != rid:
                    continue
                if r.get("ok"):
                    return PushResult(True, device, None, r.get("boardIp"), r.get("fw"))
                return PushResult(False, device, r.get("error", "error"))
        return PushResult(False, device, "no_reply")
    except (serial.SerialException, OSError) as e:
        return PushResult(False, device, "io: " + str(e)[:120])
    finally:
        try:
            s.close()
        except Exception:
            pass


def push(ip, port, token):
    """연결된 모든 C3에 보내고, 하나라도 성공하면 그 결과를 돌려줌."""
    devices = find_ports()
    if not devices:
        return PushResult(False, None, "no_board")
    errors = []
    for d in devices:
        r = push_one(d, ip, port, token)
        log.info("push %s → %s", d, r)
        if r.ok:
            return r
        errors.append(r)
    for want in ("unknown_cmd", "busy"):          # 사용자에게 알려 줄 만한 이유 우선
        for r in errors:
            if r.error == want:
                return r
    return errors[0]
