"""이 PC의 내부망 IPv4 주소 고르기."""
import ipaddress
import socket


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
    except OSError:
        return None
    finally:
        s.close()


def all_ipv4():
    ips = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips:
                ips.append(ip)
    except OSError:
        pass
    d = _default_route_ip()
    if d and d not in ips:
        ips.insert(0, d)
    return ips


def pick_local_ip(board_ip=None):
    """보드(C3)와 같은 /24 주소가 있으면 그걸, 없으면 기본 경로 쪽 내부망 주소."""
    candidates = [ip for ip in all_ipv4() if is_private_v4(ip)]
    if board_ip and is_private_v4(board_ip):
        prefix = board_ip.rsplit(".", 1)[0] + "."
        for ip in candidates:
            if ip.startswith(prefix):
                return ip
    d = _default_route_ip()
    if d and is_private_v4(d):
        return d
    for pref in ("192.168.", "10.", "172."):
        for ip in candidates:
            if ip.startswith(pref):
                return ip
    return None
