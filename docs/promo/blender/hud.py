"""程序界面层：按 NativeHud.h:Layout（1080 设计稿）等比缩放到目标高度。"""
from PIL import ImageDraw
import paper as P

ROWS = [
    ("01", "LONETRAIL", "孤星"),
    ("02", "Control's Wishes", ""),
    ("03", "Rhine Lab. LLC.", ""),
    ("04", "Silent Corridor", ""),
    ("05", "Access Permission", ""),
    ("06", "Archive Light", ""),
    ("07", "Optical Surface", ""),
    ("08", "Return To Zero", ""),
    ("09", "Database", ""),
]


def ink(theme):
    return tuple(int(v * 255) for v in P.INK[theme])


def draw(d, W, H, theme="light", kind="browse", u=0.0, sel=1):
    k = H / 1080.0 * 1.22
    c = ink(theme)
    X = int(40 * k)          # left
    S = int(250 * k)         # sidebar
    CX = int(326 * k)        # content
    TY = int(260 * k)
    RY = int(434 * k)
    RH = int(54 * k)
    N = 9

    # 顶部标识
    P.draw_text(d, (X, int(150 * k)), "RHINE  MUSIC  DATABASE", "regular", 11 * k, c, 4.0 * k)
    P.draw_text(d, (X, int(190 * k)), "TRACK / SELECT", "light", 10 * k, c, 3.0 * k)
    d.line([(X, TY - int(18 * k)), (X + int(560 * k), TY - int(18 * k))], fill=c, width=1)

    if kind in ("browse", "list"):
        for i, (no, t1, t2) in enumerate(ROWS):
            y = RY + i * RH
            a = 1.0
            if i > sel:
                a = max(0.0, 1.0 - (u * 1.4 - (i - sel) * 0.05))
            if a <= 0.02:
                continue
            col = c if a > 0.5 else tuple(int(v * 0.5 + 255 * 0.5) for v in c)
            P.draw_text(d, (X, y), no, "light", 10 * k, col, 2.0 * k)
            P.draw_text(d, (X + int(42 * k), y), t1, "regular", 15 * k, col, 1.0 * k)
            if t2:
                P.draw_text(d, (X + int(42 * k) + P.text_width(d, t1, "regular", 15 * k, 1.0 * k) + int(10 * k), y),
                            t2, "light", 11 * k, col, 1.0 * k)
            if i == sel:
                d.line([(X, y + int(26 * k)), (X + int(200 * k), y + int(26 * k))], fill=(143, 227, 187), width=1)
        d.line([(CX - int(14 * k), RY - int(10 * k)), (CX - int(14 * k), RY + N * RH)], fill=c, width=1)

    if kind == "detail":
        RX = W - int(430 * k)
        y0 = int(470 * k)
        d.line([(RX, y0), (W - X, y0)], fill=c, width=1)
        for i, (lab, val) in enumerate([("SONG ID /", "R-0114"), ("ARTIST / 歌手", "Erik Castro"),
                                        ("ALBUM / 专辑", "Rhine Lab. LLC."), ("LENGTH / 时长", "1:17")]):
            y = y0 + int(34 * k) + i * int(52 * k)
            P.draw_text(d, (RX, y), lab, "light", 10 * k, c, 2.0 * k)
            P.draw_text(d, (RX, y + int(15 * k)), val, "regular", 13 * k, c, 0.6 * k)
        d.rectangle([RX + int(300 * k), y0 + int(250 * k), RX + int(410 * k), y0 + int(276 * k)],
                    outline=c, width=1)
        P.draw_text(d, (RX + int(312 * k), y0 + int(256 * k)), "面板内播放", "regular", 10 * k, c, 1.0 * k)

    if kind == "title":
        P.draw_text(d, (X, int(430 * k)), "Control's Wishes", "demibold", 50.75 * k, c, -0.4 * k)
        P.draw_text(d, (X, int(500 * k)), "LONETRAIL  /  孤星", "light", 12 * k, c, 2.4 * k)

    # 底部播放条
    by = H - int(96 * k)
    d.line([(X, by), (W - X, by)], fill=c, width=1)
    d.rectangle([X, by + int(26 * k), X + int(10 * k), by + int(40 * k)], outline=c, width=1)
    d.rectangle([X + int(22 * k), by + int(26 * k), X + int(32 * k), by + int(40 * k)], outline=c, width=1)
    P.draw_text(d, (X + int(52 * k), by + int(22 * k)), "02 / 10", "light", 11 * k, c, 2.0 * k)
    P.draw_text(d, (W - X - int(120 * k), by + int(22 * k)), "01:17.6", "light", 11 * k, c, 2.0 * k)
    d.line([(X + int(140 * k), by + int(33 * k)), (W - X - int(140 * k), by + int(33 * k))], fill=c, width=1)
    px = X + int(140 * k) + int((W - 2 * X - int(280 * k)) * 0.34)
    d.line([(px, by + int(29 * k)), (px, by + int(37 * k))], fill=c, width=1)
