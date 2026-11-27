"""윈도우 시스템 연동: 중복 실행 방지, 설치(복사·자동 실행), 방화벽, 메시지 상자."""
import ctypes
import logging
import os
import shutil
import subprocess
import sys
import time
import winreg
from ctypes import wintypes

log = logging.getLogger("sys")

APP_ID = "PCRemoteTouchpad"
EXE_NAME = APP_ID + ".exe"
RULE_NAME = "PC Remote Touchpad"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
CREATE_NO_WINDOW = 0x08000000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
kernel32.CreateMutexW.argtypes = (wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.CreateMutexW.restype = wintypes.HANDLE
kernel32.CreateEventW.argtypes = (wintypes.LPVOID, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.OpenEventW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.OpenEventW.restype = wintypes.HANDLE
kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
user32.MessageBoxW.argtypes = (wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.UINT)
shell32.ShellExecuteW.argtypes = (wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                  wintypes.LPCWSTR, ctypes.c_int)
shell32.ShellExecuteW.restype = ctypes.c_void_p

ERROR_ALREADY_EXISTS = 183
EVENT_MODIFY_STATE = 0x0002
SYNCHRONIZE = 0x00100000
MUTEX_NAME = "Local\\" + APP_ID + ".Mutex"
QUIT_NAME = "Local\\" + APP_ID + ".Quit"

_mutex = None
_quit_event = None


def is_korean():
    try:
        return (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF) == 0x12
    except Exception:
        return True


def install_dir():
    return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), APP_ID)


def installed_exe():
    return os.path.join(install_dir(), EXE_NAME)


def frozen():
    return getattr(sys, "frozen", False)


def current_exe():
    return os.path.abspath(sys.executable)


def running_installed():
    return frozen() and os.path.normcase(current_exe()) == os.path.normcase(installed_exe())


# ---------- 중복 실행 ----------
def acquire_single_instance():
    """True면 이 프로세스가 유일한 실행본. 종료 요청 이벤트도 만들어 둠."""
    global _mutex, _quit_event
    _mutex = kernel32.CreateMutexW(None, False, MUTEX_NAME)
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        return False
    _quit_event = kernel32.CreateEventW(None, True, False, QUIT_NAME)
    return True


def wait_quit_request(timeout_ms):
    """다른 실행본(새 버전 설치)이 종료를 요청하면 True."""
    if not _quit_event:
        time.sleep(timeout_ms / 1000)
        return False
    return kernel32.WaitForSingleObject(_quit_event, timeout_ms) == 0


def ask_running_instance_to_quit(wait_s=8):
    """실행 중인 이전 실행본에 종료 요청 → 끝날 때까지 기다림. 끝났으면 True."""
    h = kernel32.OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, False, QUIT_NAME)
    if not h:
        return True
    kernel32.SetEvent(h)
    kernel32.CloseHandle(h)
    end = time.time() + wait_s
    while time.time() < end:
        m = kernel32.CreateMutexW(None, False, MUTEX_NAME)
        exists = ctypes.get_last_error() == ERROR_ALREADY_EXISTS
        kernel32.CloseHandle(m)
        if not exists:
            return True
        time.sleep(0.3)
    return False


# ---------- 메시지 ----------
MB_OK, MB_YESNO, MB_ICONINFO, MB_ICONWARN, MB_TOPMOST = 0x0, 0x4, 0x40, 0x30, 0x40000
IDYES = 6


def msgbox(text, title="PC Remote Touchpad", flags=MB_OK | MB_ICONINFO):
    return user32.MessageBoxW(None, text, title, flags | MB_TOPMOST)


# ---------- 자동 실행 ----------
def autostart_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            v, _ = winreg.QueryValueEx(k, APP_ID)
            return bool(v)
    except OSError:
        return False


def set_autostart(on, exe=None):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if on:
            winreg.SetValueEx(k, APP_ID, 0, winreg.REG_SZ, f'"{exe or installed_exe()}"')
        else:
            try:
                winreg.DeleteValue(k, APP_ID)
            except FileNotFoundError:
                pass


# ---------- 방화벽 (개인 네트워크, 이 프로그램 + 이 포트만) ----------
def firewall_rule_exists():
    try:
        r = subprocess.run(["netsh", "advfirewall", "firewall", "show", "rule", f"name={RULE_NAME}"],
                           capture_output=True, creationflags=CREATE_NO_WINDOW, timeout=10)
        return r.returncode == 0
    except Exception:
        return False


def _run_elevated_cmd(cmdline, wait_s=30):
    """UAC 확인 창을 띄워 관리자 권한으로 cmd 실행. 사용자가 거절하면 False."""
    rc = shell32.ShellExecuteW(None, "runas", "cmd.exe", "/c " + cmdline, None, 0)
    if not rc or rc <= 32:
        return False
    end = time.time() + wait_s
    while time.time() < end:
        time.sleep(1)
        if firewall_rule_exists():
            return True
    return False


def add_firewall_rule(port, exe=None):
    exe = exe or (installed_exe() if frozen() else current_exe())
    base = 'netsh advfirewall firewall'
    cmd = (f'{base} delete rule name="{RULE_NAME}" >nul 2>&1 & '
           f'{base} add rule name="{RULE_NAME}" dir=in action=allow protocol=TCP '
           f'localport={port} program="{exe}" profile=private')
    ok = _run_elevated_cmd(cmd)
    log.info("firewall rule %s", "added" if ok else "not added")
    return ok


def remove_firewall_rule():
    if not firewall_rule_exists():
        return True
    rc = shell32.ShellExecuteW(None, "runas", "cmd.exe",
                               f'/c netsh advfirewall firewall delete rule name="{RULE_NAME}"', None, 0)
    return bool(rc) and rc > 32


# ---------- 네트워크 종류 (공용이면 폰에서 접속 안 될 수 있음) ----------
def network_categories():
    """[(어댑터 이름, 'Public'|'Private'|'DomainAuthenticated')]  실패하면 []."""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command",
             "Get-NetConnectionProfile | ForEach-Object { $_.InterfaceAlias + '|' + $_.NetworkCategory }"],
            capture_output=True, text=True, creationflags=CREATE_NO_WINDOW, timeout=15)
        out = []
        for line in r.stdout.splitlines():
            if "|" in line:
                n, c = line.rsplit("|", 1)
                out.append((n.strip(), c.strip()))
        return out
    except Exception:
        return []


def open_network_settings():
    os.startfile("ms-settings:network-status")


def open_folder(path):
    os.startfile(path)


# ---------- 설치 ----------
def install_self():
    """다운로드한 exe를 실행하면: %LOCALAPPDATA%\\PCRemoteTouchpad 로 복사, 자동 실행 등록.
    이미 실행 중인 이전 버전이 있으면 끄고 덮어씀. 복사된 실행 파일 경로를 돌려줌."""
    dst = installed_exe()
    os.makedirs(install_dir(), exist_ok=True)
    if not ask_running_instance_to_quit():
        raise RuntimeError("running instance did not quit")
    last = None
    for _ in range(10):
        try:
            shutil.copyfile(current_exe(), dst)
            break
        except OSError as e:
            last = e
            time.sleep(0.5)
    else:
        raise last
    set_autostart(True, dst)
    make_shortcut(dst, is_korean())
    return dst


def shortcut_path(korean=True):
    name = "PC 원격 터치패드.lnk" if korean else "PC Remote Touchpad.lnk"
    return os.path.join(os.environ.get("APPDATA", ""), "Microsoft", "Windows", "Start Menu", "Programs", name)


def make_shortcut(exe, korean=True):
    """시작 메뉴 바로가기 (꺼 놨을 때 다시 켤 수 있게)."""
    lnk = shortcut_path(korean).replace("'", "''")
    target = exe.replace("'", "''")
    ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{lnk}');"
          f"$s.TargetPath='{target}';$s.WorkingDirectory='{os.path.dirname(target)}';$s.Save()")
    try:
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                       creationflags=CREATE_NO_WINDOW, timeout=20, capture_output=True)
    except Exception as e:
        log.warning("shortcut failed: %s", e)


def launch(exe, *args):
    subprocess.Popen([exe, *args], close_fds=True, creationflags=0x00000008)  # DETACHED_PROCESS


def uninstall_later():
    """자동 실행·방화벽 규칙 제거 후, 이 프로그램이 끝나면 설치 폴더를 지움."""
    set_autostart(False)
    remove_firewall_rule()
    for k in (True, False):
        try:
            os.remove(shortcut_path(k))
        except OSError:
            pass
    d = install_dir()
    subprocess.Popen(["cmd.exe", "/c", f'timeout /t 3 /nobreak >nul & rmdir /s /q "{d}"'],
                     creationflags=CREATE_NO_WINDOW, close_fds=True)
