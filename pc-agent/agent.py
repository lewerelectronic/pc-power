"""PC 원격 터치패드 (PC Remote Touchpad) — PC 쪽 프로그램.

폰 브라우저를 터치패드로 써서 이 PC의 마우스·키를 움직임.
1) 시작할 때 내부망 IP와 무작위 토큰을 만들고 (실행할 때마다 새 토큰)
2) USB로 연결된 보드(ESP32-C3)에 주소를 알려 줌 → 텔레그램 🖱️ 마우스 버튼이 이 링크를 답장
3) 폰이 링크를 열면 이 프로그램이 터치패드 페이지를 주고, WebSocket으로 받은 동작을 입력으로 넣음

실행 파일(.exe)을 처음 실행하면 %LOCALAPPDATA%\\PCRemoteTouchpad 에 복사하고
윈도우 로그인 시 자동 실행되게 등록, 방화벽(개인 네트워크) 허용 규칙을 추가함.
"""
import argparse
import json
import logging
import logging.handlers
import os
import secrets
import sys
import threading
import time

import c3link
import netutil
from tpserver import TouchpadServer

VERSION = "1.0.0"
DEFAULT_PORT = 48765
IS_WIN = sys.platform == "win32"
RESEND_S = 120          # 보드에 다시 알리는 주기 (보드는 10분 지나면 지움)
TICK_S = 5

log = logging.getLogger("agent")

T = {
    "ko": {
        "app": "PC 원격 터치패드",
        "install_q": "PC 원격 터치패드를 설치할까요?\n\n"
                     "• 이 PC에 복사하고, 윈도우에 로그인하면 자동으로 실행돼요\n"
                     "• 폰이 접속할 수 있게 방화벽을 허용해요 (개인 네트워크만)\n"
                     "  → 다음에 뜨는 관리자 확인 창에서 '예'를 눌러 주세요",
        "update_q": "새 버전으로 업데이트할까요? (v{v})\n실행 중인 이전 버전은 자동으로 꺼져요.",
        "install_fail": "설치하지 못했어요: {e}",
        "fw_declined": "방화벽 허용을 하지 않아서 폰에서 접속이 안 될 수 있어요.\n"
                       "트레이 아이콘 메뉴의 '방화벽 허용'에서 다시 할 수 있어요.",
        "already": "이미 실행 중이에요. 화면 오른쪽 아래 트레이 아이콘을 확인해 주세요.",
        "installed": "설치 완료! 텔레그램에서 🖱️ 마우스를 눌러 보세요.",
        "started": "실행 중이에요. 텔레그램에서 🖱️ 마우스를 누르면 폰이 터치패드가 돼요.",
        "phones": "폰 {n}대 연결됨",
        "no_phone": "폰 연결 대기 중",
        "board_ok": "보드: 연결됨 (v{fw})",
        "board_none": "보드: USB에서 못 찾음",
        "board_busy": "보드: 설정 페이지가 사용 중 (닫으면 다시 알려요)",
        "board_old": "보드: 펌웨어 업데이트 필요 (2.3.0 이상)",
        "board_err": "보드: 통신 실패 ({e})",
        "board_wait": "보드: 확인 중…",
        "no_net": "네트워크 없음",
        "server_err": "포트 {p}를 열 수 없어요 (다른 프로그램이 사용 중?)",
        "addr": "주소: {u}",
        "resend": "보드에 다시 알리기",
        "copy": "폰 링크 복사",
        "firewall": "방화벽 허용 (관리자)",
        "netset": "네트워크 설정 열기 (공용 → 개인)",
        "autostart": "윈도우 시작 시 자동 실행",
        "logs": "로그 폴더 열기",
        "uninstall": "제거",
        "quit": "종료",
        "copied": "링크를 복사했어요. 같은 와이파이의 폰에서 열면 돼요.",
        "public_warn": "지금 네트워크가 '공용'으로 되어 있어서 폰에서 접속이 막힐 수 있어요.\n"
                       "트레이 메뉴 → 네트워크 설정에서 '개인'으로 바꿔 주세요.",
        "old_fw": "보드 펌웨어가 예전 버전이에요. 설정 페이지에서 펌웨어를 업데이트해 주세요 (2.3.0 이상).",
        "uninstall_q": "PC 원격 터치패드를 제거할까요?\n(자동 실행·방화벽 규칙·설치 폴더를 지워요)",
        "fw_ok": "방화벽을 허용했어요.",
        "fw_no": "방화벽 허용이 안 됐어요.",
    },
    "en": {
        "app": "PC Remote Touchpad",
        "install_q": "Install PC Remote Touchpad?\n\n"
                     "• Copies itself to this PC and starts automatically when you sign in to Windows\n"
                     "• Allows it through the firewall so your phone can connect (private networks only)\n"
                     "  → Click 'Yes' in the administrator prompt that follows",
        "update_q": "Update to the new version? (v{v})\nThe running older version will be closed automatically.",
        "install_fail": "Installation failed: {e}",
        "fw_declined": "The firewall wasn't allowed, so your phone may not be able to connect.\n"
                       "You can do it later from the tray icon menu → 'Allow through firewall'.",
        "already": "It's already running. Check the tray icon at the bottom right of the screen.",
        "installed": "Installed! Tap 🖱️ Mouse in Telegram.",
        "started": "Running. Tap 🖱️ Mouse in Telegram to use your phone as a touchpad.",
        "phones": "{n} phone(s) connected",
        "no_phone": "Waiting for a phone",
        "board_ok": "Board: connected (v{fw})",
        "board_none": "Board: not found on USB",
        "board_busy": "Board: settings page is using it (will retry after it closes)",
        "board_old": "Board: firmware update needed (2.3.0 or later)",
        "board_err": "Board: communication failed ({e})",
        "board_wait": "Board: checking…",
        "no_net": "No network",
        "server_err": "Can't open port {p} (used by another program?)",
        "addr": "Address: {u}",
        "resend": "Notify the board again",
        "copy": "Copy phone link",
        "firewall": "Allow through firewall (admin)",
        "netset": "Open network settings (Public → Private)",
        "autostart": "Start with Windows",
        "logs": "Open log folder",
        "uninstall": "Uninstall",
        "quit": "Quit",
        "copied": "Link copied. Open it on a phone on the same Wi-Fi.",
        "public_warn": "This network is set to 'Public', which may block your phone.\n"
                       "Tray menu → network settings, and switch it to 'Private'.",
        "old_fw": "The board firmware is outdated. Update it from the settings page (2.3.0 or later).",
        "uninstall_q": "Uninstall PC Remote Touchpad?\n(Removes autostart, the firewall rule and the install folder)",
        "fw_ok": "Firewall allowed.",
        "fw_no": "The firewall wasn't allowed.",
    },
}


class MockInjector:
    """윈도우가 아닐 때/테스트용: 입력 대신 기록."""

    def __init__(self, path=None):
        self.events = []
        self.path = path

    def _rec(self, *e):
        self.events.append(e)
        log.info("inject %s", e)
        if self.path:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(json.dumps(e, ensure_ascii=False) + "\n")

    def move(self, dx, dy): self._rec("move", dx, dy)
    def click(self, b): self._rec("click", b)
    def wheel(self, dy): self._rec("wheel", dy)
    def key(self, k): self._rec("key", k)
    def text(self, s): self._rec("text", s)


class State:
    def __init__(self):
        self.ip = None
        self.board = "wait"          # wait | ok | none | busy | old | err
        self.board_err = ""
        self.board_fw = ""
        self.board_ip = None
        self.pushed_at = 0
        self.public_net = False


class Agent:
    def __init__(self, args, lang):
        self.args = args
        self.t = T[lang]
        self.port = args.port
        self.token = args.token or secrets.token_urlsafe(16)
        self.st = State()
        self.stop = threading.Event()
        self.kick = threading.Event()       # 즉시 다시 알리기
        self.tray = None
        self.notified = set()
        if args.mock or not IS_WIN:
            inj = MockInjector(args.mock_log)
        else:
            from winput import WinInjector
            inj = WinInjector()
        self.server = TouchpadServer(inj, self.port, self.token, on_change=self.refresh)

    # ---------- 표시 ----------
    def url(self, host=None):
        host = host or self.st.ip or "127.0.0.1"
        return f"http://{host}:{self.port}/?t={self.token}"

    def status_lines(self):
        t, st = self.t, self.st
        lines = []
        if self.server.error:
            lines.append(t["server_err"].format(p=self.port))
        elif not st.ip:
            lines.append(t["no_net"])
        else:
            lines.append(t["phones"].format(n=self.server.clients) if self.server.clients else t["no_phone"])
        lines.append({
            "ok": t["board_ok"].format(fw=st.board_fw or "?"),
            "none": t["board_none"], "busy": t["board_busy"], "old": t["board_old"],
            "err": t["board_err"].format(e=st.board_err), "wait": t["board_wait"],
        }[st.board])
        return lines

    def icon_state(self):
        if self.server.error or self.st.board in ("old", "err"):
            return "error"
        if not self.st.ip or self.st.board != "ok":
            return "wait"
        return "ok"

    def refresh(self):
        if not self.tray:
            return
        try:
            from icon import make_icon
            self.tray.icon = make_icon(self.icon_state())
            self.tray.title = (self.t["app"] + " — " + " · ".join(self.status_lines()))[:127]
            self.tray.update_menu()
        except Exception as e:
            log.debug("tray refresh: %s", e)

    def notify(self, key, text=None, once=True):
        if once and key in self.notified:
            return
        self.notified.add(key)
        log.info("notify: %s", key)
        if self.tray:
            try:
                self.tray.notify(text or self.t[key], self.t["app"])
            except Exception as e:
                log.debug("notify failed: %s", e)

    # ---------- 보드 동기화 ----------
    def push(self):
        st = self.st
        st.ip = netutil.pick_local_ip(st.board_ip)
        if not st.ip or not self.server.running:
            return False
        r = c3link.push(st.ip, self.port, self.token)
        if r.ok:
            st.board, st.board_fw, st.pushed_at = "ok", r.fw or "", time.time()
            if r.board_ip and r.board_ip != st.board_ip:
                st.board_ip = r.board_ip
                better = netutil.pick_local_ip(r.board_ip)
                if better and better != st.ip:     # 보드와 같은 망 주소로 다시 알림
                    log.info("switching address %s → %s (board %s)", st.ip, better, r.board_ip)
                    st.ip = better
                    r = c3link.push(st.ip, self.port, self.token)
            self.notified.discard("old_fw")
        else:
            st.board = {"no_board": "none", "busy": "busy", "unknown_cmd": "old"}.get(r.error, "err")
            st.board_err = r.error or ""
            if st.board == "old":
                self.notify("old_fw")
        self.refresh()
        return r.ok

    def sync_loop(self):
        if self.args.no_serial:
            self.st.ip = netutil.pick_local_ip()
            self.refresh()
            return
        next_at = 0.0
        backoff = 3
        last_wall = time.time()
        last_ports = None
        last_ip = None
        net_checked_ip = object()
        while not self.stop.is_set():
            now = time.time()
            resumed = now - last_wall > TICK_S * 6        # 절전에서 깨어남
            last_wall = now
            ports = c3link.find_ports()
            ip = netutil.pick_local_ip(self.st.board_ip)
            changed = ports != last_ports or ip != last_ip
            last_ports, last_ip = ports, ip
            if ip != net_checked_ip and IS_WIN and not self.args.mock:
                net_checked_ip = ip
                threading.Thread(target=self.check_network, daemon=True).start()
            if self.kick.is_set() or resumed or changed or now >= next_at:
                self.kick.clear()
                if resumed:
                    log.info("resumed from sleep")
                ok = self.push()
                if ok:
                    next_at, backoff = now + RESEND_S, 3
                elif self.st.board == "old":
                    next_at = now + 600
                elif self.st.board == "none" or not ip:
                    next_at = now + 60                    # 꽂히면 포트 변화로 바로 감지
                else:
                    next_at, backoff = now + backoff, min(backoff * 2, 60)
            if IS_WIN and not self.args.mock:
                import winsys
                if winsys.wait_quit_request(TICK_S * 1000):
                    log.info("quit requested by a newer version")
                    self.quit()
                    return
            else:
                self.kick.wait(TICK_S)

    def check_network(self):
        import winsys
        cats = winsys.network_categories()
        pub = any(c == "Public" for _, c in cats)
        self.st.public_net = pub
        if pub:
            log.info("network profiles: %s", cats)
            self.notify("public_warn")
        self.refresh()

    # ---------- 메뉴 동작 ----------
    def do_resend(self, *_):
        self.kick.set()

    def do_copy(self, *_):
        from winput import set_clipboard
        set_clipboard(self.url())
        self.notify("copied", once=False)

    def do_firewall(self, *_):
        def work():
            import winsys
            ok = winsys.add_firewall_rule(self.port)
            self.notify("fw_ok" if ok else "fw_no", once=False)
        threading.Thread(target=work, daemon=True).start()

    def do_netset(self, *_):
        import winsys
        winsys.open_network_settings()

    def do_autostart(self, *_):
        import winsys
        winsys.set_autostart(not winsys.autostart_enabled(),
                             winsys.installed_exe() if winsys.running_installed() else winsys.current_exe())
        self.refresh()

    def do_logs(self, *_):
        import winsys
        winsys.open_folder(log_dir())

    def do_uninstall(self, *_):
        import winsys
        if winsys.msgbox(self.t["uninstall_q"], self.t["app"], winsys.MB_YESNO | winsys.MB_ICONWARN) != winsys.IDYES:
            return
        winsys.uninstall_later()
        self.quit()

    def quit(self, *_):
        self.stop.set()
        self.kick.set()
        self.server.stop()
        if self.tray:
            self.tray.stop()

    # ---------- 실행 ----------
    def build_tray(self):
        import pystray
        import winsys
        from icon import make_icon
        t = self.t
        I = pystray.MenuItem
        info = lambda i: I(lambda _: (self.status_lines() + [""])[i], None, enabled=False,
                           visible=lambda _: len(self.status_lines()) > i)
        menu = pystray.Menu(
            info(0), info(1),
            I(lambda _: t["addr"].format(u=f"{self.st.ip}:{self.port}"), None, enabled=False,
              visible=lambda _: bool(self.st.ip)),
            pystray.Menu.SEPARATOR,
            I(t["resend"], self.do_resend),
            I(t["copy"], self.do_copy, enabled=lambda _: bool(self.st.ip)),
            I(t["firewall"], self.do_firewall),
            I(t["netset"], self.do_netset, visible=lambda _: self.st.public_net),
            I(t["autostart"], self.do_autostart, checked=lambda _: winsys.autostart_enabled()),
            I(t["logs"], self.do_logs),
            pystray.Menu.SEPARATOR,
            I(t["uninstall"], self.do_uninstall, visible=lambda _: winsys.running_installed()),
            I(t["quit"], self.quit),
        )
        self.tray = pystray.Icon(winsys.APP_ID, make_icon("wait"), t["app"], menu)

    def run(self):
        log.info("PC Remote Touchpad v%s starting (port %d)", VERSION, self.port)
        self.server.start()
        threading.Thread(target=self.sync_loop, name="sync", daemon=True).start()
        if self.args.no_tray or not IS_WIN:
            if sys.stdout:
                print("touchpad url:", self.url(), flush=True)
            try:
                while not self.stop.is_set():
                    self.stop.wait(1)
            except KeyboardInterrupt:
                self.quit()
            return

        def setup(icon):
            icon.visible = True
            self.refresh()
            self.notify("installed" if self.args.installed else "started")

        self.build_tray()
        self.tray.run(setup=setup)


def log_dir():
    if IS_WIN:
        import winsys
        return winsys.install_dir()
    return os.path.join(os.path.expanduser("~"), ".pc-remote-touchpad")


def setup_logging(debug=False):
    d = log_dir()
    os.makedirs(d, exist_ok=True)
    h = logging.handlers.RotatingFileHandler(os.path.join(d, "agent.log"), maxBytes=256 * 1024,
                                             backupCount=1, encoding="utf-8")
    handlers = [h]
    if sys.stderr:
        handlers.append(logging.StreamHandler())
    logging.basicConfig(level=logging.DEBUG if debug else logging.INFO, handlers=handlers,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    logging.getLogger("websockets").setLevel(logging.WARNING)


def run_installer(t):
    import winsys
    updating = os.path.exists(winsys.installed_exe())
    q = t["update_q"].format(v=VERSION) if updating else t["install_q"]
    if winsys.msgbox(q, t["app"], winsys.MB_YESNO | winsys.MB_ICONINFO) != winsys.IDYES:
        return
    try:
        exe = winsys.install_self()
    except Exception as e:
        log.exception("install failed")
        winsys.msgbox(t["install_fail"].format(e=e), t["app"], winsys.MB_ICONWARN)
        return
    if not winsys.firewall_rule_exists() or not updating:
        if not winsys.add_firewall_rule(DEFAULT_PORT, exe):
            winsys.msgbox(t["fw_declined"], t["app"], winsys.MB_ICONWARN)
    winsys.launch(exe, "--installed")


def main():
    p = argparse.ArgumentParser(description="PC Remote Touchpad")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--token", help="고정 토큰 (테스트용)")
    p.add_argument("--mock", action="store_true", help="입력을 넣지 않고 기록만 (테스트용)")
    p.add_argument("--mock-log", help="--mock 기록 파일")
    p.add_argument("--no-tray", action="store_true")
    p.add_argument("--no-serial", action="store_true", help="보드에 알리지 않음 (테스트용)")
    p.add_argument("--no-install", action="store_true", help="설치 없이 그 자리에서 실행")
    p.add_argument("--installed", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--lang", choices=["ko", "en"])
    p.add_argument("--debug", action="store_true")
    args = p.parse_args()

    setup_logging(args.debug)
    lang = args.lang
    if not lang:
        if IS_WIN:
            import winsys
            lang = "ko" if winsys.is_korean() else "en"
        else:
            lang = "ko"
    t = T[lang]

    if IS_WIN and not args.mock:
        import winsys
        if winsys.frozen() and not winsys.running_installed() and not args.no_install:
            run_installer(t)
            return
        if not winsys.acquire_single_instance():
            winsys.msgbox(t["already"], t["app"])
            return

    Agent(args, lang).run()


if __name__ == "__main__":
    main()
