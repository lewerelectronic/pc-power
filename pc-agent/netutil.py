"""이 PC의 내부망 IPv4 주소 고르기."""
import ipaddress
import logging
import socket

log = logging.getLogger("net")

try:
    import psutil
except Exception:          # 없으면 소켓 방식만
    psutil = None


def is_private_v4(ip: str) -> bool:
    """10/8, 172.16/12, 192.168/16 만 (펌웨어와 같은 기준)."""
    try:
        a = ipaddress.IPv4Address(ip)
    except ValueError:
        return False
    return a in ipaddress.IPv4Network("10.0.0.0/8") or \
        a in ipaddress.IPv4Network("172.16.0.0/12") or \
        a in ipaddress.IPv4Network("192.168.0.0/16")


def client_allowed(ip: str) -> bool:
    """폰(접속자) 주소 허용 여부: 내부망 + 이 PC 자신(테스트용)."""
    if ip.startswith("::ffff:"):
        ip = ip[7:]
    return is_private_v4(ip) or ip == "127.0.0.1"


def _default_route_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))          # 패킷은 안 나감, 경로만 고름
        return s.getsockname()[0]
    except Exception:
        return None
    finally:
        s.close()


def interfaces():
    """[(어댑터 이름, IPv4)] — 켜져 있는 어댑터만, 루프백·자동 할당(169.254) 제외."""
    out = []
    if psutil:
        try:
            stats = psutil.net_if_stats()
            for name, addrs in psutil.net_if_addrs().items():
                st = stats.get(name)
                if st is not None and not st.isup:
                    continue
                for a in addrs:
                    if a.family == socket.AF_INET and not a.address.startswith(("127.", "169.254.")):
                        out.append((name, a.address))
        except Exception as e:
            log.warning("psutil failed: %s", e)
    if not out:
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                ip = info[4][0]
                if not ip.startswith(("127.", "169.254.")) and ip not in [i for _, i in out]:
                    out.append(("?", ip))
        except Exception as e:                       # 한글·긴 컴퓨터 이름이면 UnicodeError도 남
            log.warning("getaddrinfo(hostname) failed: %s", e)
    d = _default_route_ip()
    if d and d not in [i for _, i in out]:
        out.insert(0, ("default", d))
    return out


_VIRTUAL = ("vethernet", "virtualbox", "vmware", "hyper-v", "wsl", "docker", "loopback",
            "tailscale", "zerotier", "openvpn", "wireguard", "tap-", "radmin", "hamachi")


def _is_virtual(name):
    n = name.lower()
    return any(v in n for v in _VIRTUAL)


def pick_local_ip(board_ip=None, ifaces=None):
    """보드(C3)와 같은 /24 주소 > 기본 경로 쪽 내부망 주소 > 실제 어댑터의 내부망 주소."""
    ifaces = interfaces() if ifaces is None else ifaces
    private = [(n, ip) for n, ip in ifaces if is_private_v4(ip)]
    if board_ip and is_private_v4(board_ip):
        prefix = board_ip.rsplit(".", 1)[0] + "."
        for _, ip in private:
            if ip.startswith(prefix):
                return ip
    d = _default_route_ip()
    if d and is_private_v4(d):
        return d
    real = [(n, ip) for n, ip in private if not _is_virtual(n)]
    for pref in ("192.168.", "10.", "172."):
        for _, ip in real:
            if ip.startswith(pref):
                return ip
    return None


def describe(ifaces=None):
    """로그·트레이용: '이더넷=211.x.x.x, Wi-Fi=192.168.0.5'"""
    ifaces = interfaces() if ifaces is None else ifaces
    return ", ".join(f"{n}={ip}" for n, ip in ifaces if n != "default") or "-"
