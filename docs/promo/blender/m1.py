"""M1「访问」：程序自己的授权序列(25 fps)重采样到 12 fps，纸层绘制。"""
import os, math, json
import numpy as np
from PIL import Image, ImageDraw
import paper as P

W, H = P.W, P.H
INK = tuple(int(v * 255) for v in P.INK["light"])
PAPER = tuple(int(v * 255) for v in P.PAPER["light"])
OUT = "/tmp/m1/frames"
os.makedirs(OUT, exist_ok=True)

TEXT = {
    "access": "ACCESS PERMISSION REQUIRED",
    "brand": "RHINE·LAB",
    "identity": "ID CONFIRMED : JOYCE MOORE",
    "request": "REQUEST RECEIVED",
    "processing": "START PROCESSING...",
    "glitch": "...SING...",
    "permission": "PERMISSION AUTHORIZED",
    "company": "RHINE LAB",
    "database": "DATABASE",
}
COUNTS = [1, 1, 3, 4, 5, 6, 9, 11, 12, 14, 17, 18, 19, 20, 22, 23, 25, 26]
FLASH = {479, 485, 486}
GLITCH = {525, 526, 528, 529}


def typed(text, frame, start, end):
    if frame < start:
        return ""
    if frame >= end:
        return text
    n = 1 + int(math.floor((frame - start) * (len(text) - 1) / (end - start)))
    return text[:min(len(text), n)]


def at(frame, s):
    return int(frame) in s



def draw_grid(img, f, rows=26, lanes=5, alpha=0.22, scan_f=None):
    """一点透视的档案栅格：竖线收敛到灭点，横线按透视压缩。"""
    ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    vx, vy = 1560, 452
    near_y = 1046
    near_x0, near_x1 = 96, 1836
    a = int(255 * alpha)
    # 竖线
    for i in range(lanes + 2):
        x = near_x0 + (near_x1 - near_x0) * i / (lanes + 1)
        d.line([(vx, vy), (x, near_y)], fill=INK + (a,), width=1)
    # 横线（透视压缩）
    for i in range(rows):
        k = (1 - (i / float(rows)) ** 1.0) ** 2.05
        y = vy + (near_y - vy) * k
        ink = a
        if scan_f is not None:
            dist = abs(i - scan_f)
            ink = int(min(255, a + 210 * math.exp(-(dist * dist) / 2.2)))
        d.line([(0, y), (W, y)], fill=INK + (ink,), width=1)
    img.paste(Image.composite(Image.new("RGB", (W, H), INK), img, ov.split()[3]), (0, 0))

def draw_frame(f):
    """f = 25 fps 帧号（0..630）"""
    img, d = P.canvas()
    # ---- 顶部固定标识 ----
    P.draw_text(d, (40, 88), "RHINE LAB / MUSIC DATABASE", "regular", 11, INK, 1.6)
    P.draw_text(d, (1880, 88), "ACCESS", "regular", 11, INK, 1.6, anchor="rt")

    # ---- 起手细线：0→170 帧长满 ----
    if f < 170:
        grow = min(1.0, f / 150.0)
        x1 = 40 + grow * 1180
        d.line([(40, 548), (x1, 548)], fill=INK, width=1)

    # ---- 档案栅格：120 帧起渐入，540 帧起被扫描波点亮 ----
    if f >= 120:
        ga = 0.10 * min(1.0, (f - 120) / 90.0)
        if f >= 540:
            ga = 0.10 + 0.10 * min(1.0, (f - 540) / 40.0)
            draw_grid(img, f, alpha=ga, scan_f=3 + 19 * ((f - 540) / 25.0))
        else:
            draw_grid(img, f, alpha=ga)

    # ---- access 逐字 ----
    if 170 <= f < 227:
        n = COUNTS[min(17, f - 170)]
        s = TEXT["access"][:n]
        P.draw_text(d, (40, 452), s, "light", 46, INK, 7.5)
        tw = P.text_width(d, TEXT["access"], "light", 46, 7.5)
        d.line([(40, 548), (40 + max(tw * n / 26.0, 40), 548)], fill=INK, width=1)

    # ---- brand 逐字 ----
    if 232 <= f < 282:
        s = typed(TEXT["brand"], f, 232, 255)
        P.draw_text(d, (40, 440), s, "demibold", 50.75, INK, 3.2)

    # ---- identity / request / processing ----
    if 282 <= f < 363:
        s = typed("ID CONFIRMED", f, 282, 295)
        if f >= 320:
            s = TEXT["identity"][:len("ID CONFIRMED")] + " : " + typed("JOYCE MOORE", f, 320, 340)[:0] + "JOYCE MOORE"
            s = "ID CONFIRMED : JOYCE MOORE"
        P.draw_text(d, (40, 470), s, "light", 22, INK, 4.0)
    elif 363 <= f < 421:
        P.draw_text(d, (40, 470), TEXT["identity"], "light", 22, INK, 4.0)
        P.draw_text(d, (40, 512), typed(TEXT["request"], f, 367, 389), "light", 22, INK, 4.0)
    elif 421 <= f < 479:
        P.draw_text(d, (40, 470), TEXT["identity"], "light", 22, INK, 4.0)
        P.draw_text(d, (40, 512), TEXT["request"], "light", 22, INK, 4.0)
        s = typed("START PROCESSING", f, 423, 440)
        if f >= 449:
            s = "START PROCESSING" + "." * min(3, 1 + (f - 449) // 4)
        P.draw_text(d, (40, 554), s, "light", 22, INK, 4.0)

    # ---- glitch 闪现 ----
    if at(f, FLASH):
        P.draw_text(d, (40, 554), TEXT["glitch"], "light", 22, INK, 4.0)

    # ---- 扫描环 + 六卫星（纸层：环 + 点）----
    if 540 <= f < 588:
        t = (f - 540) / 48.0
        cx, cy = 1320, 540
        r = 60 + 430 * t
        if r < 470:
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=INK, width=1)
        for i in range(6):
            a = math.radians(i * 60 + t * 300)
            rr = r + 30
            px, py = cx + rr * math.cos(a), cy + rr * math.sin(a)
            d.ellipse([px - 2, py - 2, px + 2, py + 2], fill=INK)
        P.draw_text(d, (40, 470), typed(TEXT["permission"], f - 540, 0, 40), "light", 22, INK, 4.0)

    # ---- 公司字标 ----
    if f >= 588:
        s = TEXT["company"]
        if at(f, {594, 595}):
            s = ""
        sc = 1.0 - 0.46 * max(0.0, min(1.0, (f - 600) / 26.0))
        P.draw_text(d, (40, 430 + (1 - sc) * 120), s, "demibold", 50.75 * sc, INK, 3.2 * sc)
        if f >= 626 and not at(f, {628, 629, 631, 634}):
            P.draw_text(d, (40, 520), TEXT["database"], "light", 22, INK, 6.0)

    # ---- glitch 横向错位 ----
    if at(f, GLITCH):
        a = np.asarray(img).copy()
        band = a[430:600]
        a[430:600] = np.roll(band, 23, axis=1)
        img = Image.fromarray(a)

    # ---- 白场 ----
    if f >= 626:
        k = min(1.0, (f - 626) / 4.0)
        a = np.asarray(img).astype(np.float64) / 255.0
        a = a * (1 - k) + 1.0 * k
        img = Image.fromarray((a * 255).astype(np.uint8))
    # ---- 缓慢推镜（0.0 → 4.5%）----
    z = 1.0 + 0.030 * min(1.0, f / 630.0)
    if z > 1.0005:
        cw, ch = W / z, H / z
        ax, ay = 300.0, 820.0
        left = min(max(0.0, ax - ax / z), W - cw)
        top = min(max(0.0, ay - ay / z), H - ch)
        img = img.resize((W, H), Image.LANCZOS, box=(left, top, left + cw, top + ch))
    return img


def main():
    FPS = 12
    dur = 25.71
    n = int(round(dur * FPS))
    for i in range(n):
        t = i / FPS
        f25 = int(math.floor(t * 25 + 1e-6))
        draw_frame(f25).save("%s/f_%04d.png" % (OUT, i))
    print("M1 frames", n)


if __name__ == "__main__":
    main()
