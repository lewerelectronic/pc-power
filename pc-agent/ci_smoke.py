"""윈도우 빌드 머신에서 돌리는 간단 점검 (Actions)."""
import ctypes
import sys

import winput
import winsys

assert ctypes.sizeof(winput.INPUT) == (40 if sys.maxsize > 2**32 else 28), ctypes.sizeof(winput.INPUT)
print("INPUT size OK:", ctypes.sizeof(winput.INPUT))

# 클립보드 (빌드 머신 세션에 따라 안 될 수 있어서 경고만)
try:
    text = "한글 테스트 hello 👋"
    if winput.set_clipboard(text):
        u = winput.user32
        u.OpenClipboard(None)
        u.GetClipboardData.restype = ctypes.c_void_p
        h = u.GetClipboardData(winput.CF_UNICODETEXT)
        p = winput.kernel32.GlobalLock(h)
        got = ctypes.wstring_at(p)
        winput.kernel32.GlobalUnlock(h)
        u.CloseClipboard()
        print("clipboard round-trip:", "OK" if got == text else f"MISMATCH {got!r}")
    else:
        print("WARN: clipboard not available on this runner")
except Exception as e:
    print("WARN: clipboard test:", e)

print("korean UI:", winsys.is_korean())
print("autostart:", winsys.autostart_enabled())
print("firewall rule:", winsys.firewall_rule_exists())
print("network profiles:", winsys.network_categories())
print("install dir:", winsys.install_dir())
print("smoke OK")
