"""트레이·실행 파일 아이콘 (그림 파일 없이 코드로 그림)."""
from PIL import Image, ImageDraw

COLORS = {"ok": (34, 197, 94), "wait": (245, 158, 11), "error": (239, 68, 68), "idle": (148, 163, 184)}


def make_icon(state="ok", size=64):
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # 폰 모양 터치패드
    d.rounded_rectangle([s * .16, s * .04, s * .84, s * .96], radius=s * .14, fill=(37, 99, 235))
    d.rounded_rectangle([s * .24, s * .14, s * .76, s * .78], radius=s * .06, fill=(219, 234, 254))
    # 손가락 터치 점
    r = s * .09
    cx, cy = s * .5, s * .46
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(37, 99, 235))
    # 상태 점
    if state:
        c = COLORS.get(state, COLORS["idle"])
        r2 = s * .17
        x, y = s * .80, s * .80
        d.ellipse([x - r2 - s * .04, y - r2 - s * .04, x + r2 + s * .04, y + r2 + s * .04], fill=(255, 255, 255))
        d.ellipse([x - r2, y - r2, x + r2, y + r2], fill=c)
    return img


def save_ico(path):
    make_icon(None, 256).save(path, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    import sys
    save_ico(sys.argv[1] if len(sys.argv) > 1 else "app.ico")
