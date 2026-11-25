"""폰 터치패드 서버 — 한 포트에서 HTML(/)과 WebSocket(/ws)을 같이 처리.

- 토큰(?t=)이 맞아야 하고, 접속자는 내부망 주소만 허용 (아니면 403)
- WebSocket 메시지(JSON):
    {"t":"m","x":dx,"y":dy}      마우스 상대 이동 (폰에서 가속·감도 적용 후 정수)
    {"t":"c","b":"l"|"r"}        클릭
    {"t":"s","y":n}              휠 (120 = 한 칸, 음수 = 아래로)
    {"t":"k","k":"space|j|l|f|volup|voldown|mute|enter|backspace|esc"}
    {"t":"txt","v":"문자열"}      클립보드에 넣고 Ctrl+V
    {"t":"p"}                    핑 → {"t":"p"} 응답
"""
import asyncio
import hmac
import json
import logging
import os
import sys
import threading
from http import HTTPStatus
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.http11 import Response

from netutil import client_allowed

log = logging.getLogger("server")

KEYS = {"space", "j", "l", "f", "volup", "voldown", "mute", "enter", "backspace", "esc"}
MAX_MOVE = 4000
MAX_WHEEL = 6000
MAX_TEXT = 2000


def resource_path(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


class TouchpadServer:
    def __init__(self, injector, port, token, on_change=None):
        self.inj = injector
        self.port = port
        self.token = token
        self.on_change = on_change or (lambda: None)
        self.clients = 0
        self.error = None          # 포트 열기 실패 등
        self.running = False
        self._loop = None
        self._stop = None
        with open(resource_path("touchpad.html"), "rb") as f:
            self.html = f.read()

    # ---------- HTTP ----------
    def _token_ok(self, query):
        t = parse_qs(query).get("t", [""])[0]
        return hmac.compare_digest(t.encode(), self.token.encode())

    @staticmethod
    def _resp(status, body, ctype="text/plain; charset=utf-8"):
        h = Headers()
        h["Content-Type"] = ctype
        h["Content-Length"] = str(len(body))
        h["Cache-Control"] = "no-store"
        h["Referrer-Policy"] = "no-referrer"
        h["X-Content-Type-Options"] = "nosniff"
        h["Connection"] = "close"
        st = HTTPStatus(status)
        return Response(st.value, st.phrase, h, body)

    def process_request(self, conn, req):
        peer = conn.remote_address[0] if conn.remote_address else ""
        if not client_allowed(peer):
            log.warning("blocked non-private client %s", peer)
            return self._resp(403, b"Forbidden\n")
        u = urlsplit(req.path)
        if u.path == "/favicon.ico":
            return self._resp(404, b"")
        if not self._token_ok(u.query):
            log.info("wrong token from %s (%s)", peer, u.path)
            return self._resp(403, "링크가 만료됐어요. 텔레그램에서 🖱️ 마우스를 다시 눌러 주세요.\n"
                                   "This link has expired. Tap 🖱️ Mouse in Telegram again.\n".encode())
        if u.path == "/":
            h = self._resp(200, self.html, "text/html; charset=utf-8")
            h.headers["Content-Security-Policy"] = (
                "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                "connect-src 'self' ws: http:; img-src data:")
            return h
        if u.path == "/ping":
            return self._resp(200, b"ok\n")
        if u.path == "/ws":
            host = req.headers.get("Host", "")
            origin = req.headers.get("Origin")
            if origin is not None and origin != "http://" + host:
                log.warning("bad origin %s from %s", origin, peer)
                return self._resp(403, b"Forbidden\n")
            return None            # → WebSocket 핸드셰이크 계속
        return self._resp(404, b"Not found\n")

    # ---------- WebSocket ----------
    async def handler(self, ws):
        peer = ws.remote_address[0] if ws.remote_address else "?"
        self.clients += 1
        log.info("phone connected: %s (%d)", peer, self.clients)
        self.on_change()
        try:
            async for raw in ws:
                if not isinstance(raw, str) or len(raw) > 4 * MAX_TEXT + 64:
                    continue
                try:
                    msg = json.loads(raw)
                    reply = await self.dispatch(msg)
                except Exception as e:                      # 잘못된 메시지 하나로 끊지 않음
                    log.debug("bad message: %r (%s)", raw[:80], e)
                    continue
                if reply is not None:
                    await ws.send(reply)
        except Exception as e:
            log.debug("connection ended: %s", e)
        finally:
            self.clients -= 1
            log.info("phone disconnected: %s (%d)", peer, self.clients)
            self.on_change()

    @staticmethod
    def _int(v, lim):
        v = int(v)
        return max(-lim, min(lim, v))

    async def dispatch(self, m):
        t = m.get("t")
        if t == "m":
            dx, dy = self._int(m.get("x", 0), MAX_MOVE), self._int(m.get("y", 0), MAX_MOVE)
            if dx or dy:
                self.inj.move(dx, dy)
        elif t == "c":
            b = m.get("b")
            if b in ("l", "r"):
                self.inj.click(b)
        elif t == "s":
            dy = self._int(m.get("y", 0), MAX_WHEEL)
            if dy:
                self.inj.wheel(dy)
        elif t == "k":
            k = m.get("k")
            if k in KEYS:
                self.inj.key(k)
        elif t == "txt":
            v = m.get("v")
            if isinstance(v, str) and v and len(v) <= MAX_TEXT:
                await asyncio.to_thread(self.inj.text, v)
        elif t == "p":
            return '{"t":"p"}'
        return None

    # ---------- 실행 ----------
    async def _main(self, ready):
        self._stop = asyncio.Event()
        try:
            async with serve(self.handler, "0.0.0.0", self.port,
                             process_request=self.process_request,
                             compression=None, max_size=16384, max_queue=64,
                             ping_interval=15, ping_timeout=15,
                             server_header=None):
                self.running = True
                self.error = None
                log.info("touchpad server on port %d", self.port)
                ready.set()
                self.on_change()
                await self._stop.wait()
        except OSError as e:
            self.error = str(e)
            log.error("cannot listen on port %d: %s", self.port, e)
            ready.set()
            self.on_change()
        finally:
            self.running = False

    def start(self):
        """백그라운드 스레드에서 실행. 시작될 때까지 기다림."""
        ready = threading.Event()

        def run():
            self._loop = asyncio.new_event_loop()
            self._loop.run_until_complete(self._main(ready))

        threading.Thread(target=run, name="server", daemon=True).start()
        ready.wait(10)

    def stop(self):
        if self._loop and self._stop:
            self._loop.call_soon_threadsafe(self._stop.set)
