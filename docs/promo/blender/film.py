"""全片合成：3D 底板 → ACES → 纸洗 → 排印。1280x720 / 12fps / 70.714s。"""
import os, math, numpy as np
from PIL import Image, ImageDraw
import paper as P
import hud as HD

W, H, FPS = 1280, 720, 12
DUR = 70.7143
NF = int(round(DUR * FPS))
OUT = "/tmp/film/frames"
PL = "/tmp/plates"
os.makedirs(OUT, exist_ok=True)

INK = tuple(int(v * 255) for v in P.INK["light"])
INKD = tuple(int(v * 255) for v in P.INK["dark"])
_cache = {}


def plate(name):
    if name not in _cache:
        _cache[name] = Image.open("%s/%s.png" % (PL, name)).convert("RGBA").resize((W, H), Image.LANCZOS)
    return _cache[name]


def img_of(name, zoom=1.0, dx=0.0, dy=0.0):
    zoom = min(3.0, zoom * 1.55)
    dx = dx + 0.055
    dy = dy + 0.030
    im = plate(name)
    if abs(zoom - 1) < 1e-4 and abs(dx) < 1e-4 and abs(dy) < 1e-4:
        return im
    cw, ch = W / zoom, H / zoom
    left = min(max(0.0, W / 2 - dx * W - cw / 2), W - cw)
    top = min(max(0.0, H / 2 - dy * H - ch / 2), H - ch)
    return im.resize((W, H), Image.BILINEAR, box=(left, top, left + cw, top + ch))


def over(im, theme="light", exposure=0.92, gain=1.0):
    a = np.asarray(im).astype(np.float64) / 255.0
    rgb, al = a[..., :3], a[..., 3:4]
    toned = P.lin2srgb(P.aces(P.srgb2lin(rgb) * gain, exposure))
    toned = np.clip((toned - 0.5) * 1.16 + 0.48, 0, 1)
    paper = np.array(P.PAPER[theme])
    return toned * al + paper * (1 - al)


def act_frame(name, u):
    d = "/tmp/acts/" + name
    fs = sorted(os.listdir(d))
    i = min(len(fs) - 1, max(0, int(round(u * (len(fs) - 1)))))
    return Image.open(os.path.join(d, fs[i])).convert("RGBA").resize((W, H), Image.LANCZOS)


def frame_plate_act(name, u, theme, exposure, gain):
    c = over(act_frame(name, u), theme, exposure, gain)
    if theme == "light":
        c = P.wash(c, "light", 0.50)
    return Image.fromarray((np.clip(c, 0, 1) * 255).astype(np.uint8))


def frame_plate(name, zoom=1.0, dx=0.0, dy=0.0, theme="light", exposure=0.92, gain=1.0, wash_amt=1.0):
    c = over(img_of(name, zoom, dx, dy), theme, exposure, gain)
    if theme == "light":
        c = P.wash(c, "light", 0.50)
    return Image.fromarray((np.clip(c, 0, 1) * 255).astype(np.uint8))


# ---------- 叠层 ----------
def scan_lines(img, phase, n=7, ink=INK):
    d = ImageDraw.Draw(img, "RGBA")
    for i in range(n):
        y = (phase * H * 1.4 + i * H / 9.0) % (H * 1.2) - H * 0.1
        d.line([(0, y), (W, y)], fill=ink + (70,), width=1)
    return img


def ripple(img, cy, rr, ink=INK, alpha=90):
    d = ImageDraw.Draw(img, "RGBA")
    for k in range(3):
        r = rr - k * 60
        if r > 4:
            d.ellipse([W / 2 - r, cy - r * 0.42, W / 2 + r, cy + r * 0.42], outline=ink + (alpha - k * 25,), width=1)
    return img


def hairline(img, p0, p1, ink=INK, w=1):
    ImageDraw.Draw(img).line([p0, p1], fill=ink, width=w)
    return img


def darken(img, k):
    a = np.asarray(img).astype(np.float64)
    return Image.fromarray((a * k).astype(np.uint8))


def shot_at(t):
    for s in SHOTS:
        if s["t0"] <= t < s["t1"]:
            return s, (t - s["t0"]) / max(1e-6, s["t1"] - s["t0"])
    return SHOTS[-1], 1.0


def ease(u):
    return u * u * (3 - 2 * u)


SHOTS = [
    dict(t0=25.71, t1=28.60, p="p1_corridor", z0=1.00, z1=1.10, th="light", ui="browse"),
    dict(t0=28.60, t1=31.10, p="ACT:a_extract", th="light"),
    dict(t0=31.10, t1=33.00, p="p7_dark",     z0=1.00, z1=1.08, th="light", ui="detail"),
    dict(t0=33.00, t1=34.83, p="ACT:a_hover", th="light"),
    dict(t0=34.83, t1=37.60, p="p4_top",      z0=1.10, z1=1.18, th="light"),
    dict(t0=37.60, t1=40.10, p="ACT:a_scan",  th="light"),
    dict(t0=40.10, t1=42.00, p="p1_corridor", z0=1.55, z1=1.35, th="light", ui="title"),
    dict(t0=42.00, t1=44.50, p="ACT:a_ripple", th="light"),
    dict(t0=44.50, t1=46.40, p="p6_far",      z0=1.10, z1=1.25, th="light"),
    dict(t0=46.40, t1=48.23, p="ACT:a_return", th="dark"),
    dict(t0=48.23, t1=51.00, p="p5_lane",     z0=1.20, z1=1.02, th="dark", ui="browse"),
    dict(t0=51.00, t1=52.90, p="p7_dark",     z0=1.05, z1=1.22, th="dark"),
    dict(t0=52.90, t1=53.60, p="p7_dark",     z0=1.22, z1=1.26, th="light", ov="white"),
    dict(t0=53.60, t1=55.20, p="p7_dark",     z0=1.22, z1=1.02, th="light", ov="decrypt", ui="detail"),
    dict(t0=55.20, t1=56.25, p="p1_corridor", z0=1.02, z1=1.10, th="light"),
    dict(t0=56.25, t1=58.50, p="p1_corridor", z0=1.10, z1=1.00, th="light", ov="fadein"),
    dict(t0=58.50, t1=60.80, p="p5_lane",     z0=1.06, z1=1.00, th="light", ui="browse"),
    dict(t0=60.80, t1=63.00, p="p6_far",      z0=1.20, z1=1.70, th="light", ov="point"),
    dict(t0=63.00, t1=65.50, p="p6_far",      z0=1.70, z1=1.74, th="light", ov="line"),
    dict(t0=65.50, t1=68.50, p="p6_far",      z0=1.74, z1=1.74, th="light", ov="word"),
    dict(t0=68.50, t1=70.7143, p="p6_far",    z0=1.74, z1=1.76, th="light", ov="vanish"),
]


def build(t):
    if t < 25.71:
        i = min(308, int(round(t * FPS)))
        im = Image.open("/tmp/m1/frames/f_%04d.png" % i).convert("RGB").resize((W, H), Image.LANCZOS)
        if t >= 24.9:                      # M1 末尾接白
            k = min(1.0, (t - 24.9) / 0.8)
            im = Image.fromarray((np.asarray(im).astype(np.float64) * (1 - k) + 255 * k).astype(np.uint8))
        return im

    s, u = shot_at(t)
    e = ease(u)
    z = s.get("z0", 1.0) + (s.get("z1", 1.0) - s.get("z0", 1.0)) * e
    th = s["th"]
    
    gain = 0.55 if th == "dark" else 1.0
    exposure = 0.80 if th == "dark" else 0.86
    if s["p"].startswith("ACT:"):
        im = frame_plate_act(s["p"][4:], u, th, exposure, gain)
    else:
        im = frame_plate(s["p"], z, 0, 0, th, exposure, gain)
    ink = INKD if th == "dark" else INK

    ov = s.get("ov")
    if ov == "scan":
        scan_lines(im, u, 9, ink)
    elif ov == "ripple":
        ripple(im, H * 0.62, 90 + 420 * u, ink)
    elif ov == "white":
        a = np.asarray(im).astype(np.float64)
        k = min(1.0, u * 1.6)
        im = Image.fromarray((a * (1 - k) + 255 * k).astype(np.uint8))
    elif ov == "decrypt":
        d = ImageDraw.Draw(im, "RGBA")
        for i in range(9):
            y = H * 0.18 + i * H * 0.075
            wpx = int(W * min(1.0, max(0.0, (u * 1.5 - i * 0.06))))
            d.line([(W * 0.1, y), (W * 0.1 + wpx, y)], fill=ink + (150,), width=1)
    elif ov == "fadein":
        a = np.asarray(im).astype(np.float64) / 255.0
        paper = np.array(P.PAPER["light"])
        k = 1 - min(1.0, u * 1.25)
        im = Image.fromarray((np.clip(a * (1 - k) + paper * k, 0, 1) * 255).astype(np.uint8))
    elif ov == "point":
        im = darken_paper(im, 1 - 0.55 * min(1.0, u * 1.1) * 0)
        r = 26 * (1 - min(1.0, u * 1.2)) + 2
        d = ImageDraw.Draw(im)
        d.ellipse([W / 2 - r, H / 2 - r, W / 2 + r, H / 2 + r], fill=ink)
    elif ov == "line":
        im = blank_paper(im)
        d = ImageDraw.Draw(im)
        g = min(1.0, u * 1.4)
        x0, x1 = W / 2 - (W * 0.30) * g, W / 2 + (W * 0.30) * g
        d.line([(x0, H / 2), (x1, H / 2)], fill=ink, width=1)
    elif ov == "word":
        im = blank_paper(im)
        d = ImageDraw.Draw(im)
        P.draw_text(d, (W / 2, H / 2 - 26), "RHINE  MUSIC  DATABASE", "regular", 21, ink, 7.0, anchor="mt")
        d.line([(W / 2 - W * 0.30, H / 2), (W / 2 + W * 0.30, H / 2)], fill=ink, width=1)
    elif ov == "vanish":
        im = blank_paper(im)
        d = ImageDraw.Draw(im)
        P.draw_text(d, (W / 2, H / 2 - 26), "RHINE  MUSIC  DATABASE", "regular", 21, ink, 7.0, anchor="mt")
        g = max(0.0, 1 - u * 1.5)
        d.line([(W / 2 - W * 0.30 * g, H / 2), (W / 2 + W * 0.30 * g, H / 2)], fill=ink, width=1)

    if s.get("ui"):
        HD.draw(ImageDraw.Draw(im), W, H, th, s["ui"], u)

    # 橙色：全片仅 3 帧（M3 线左端）
    if s.get("ov") in ("line",) and 0.62 <= u < 0.66:
        d = ImageDraw.Draw(im)
        c = (232, 98, 42)
        x0 = W / 2 - W * 0.30
        d.rectangle([x0 - 1, H / 2 - 7, x0 + 9, H / 2 + 7], fill=c)
    return im


def blank_paper(im):
    a = np.zeros((H, W, 3)) + np.array(P.PAPER["light"])
    return Image.fromarray((a * 255).astype(np.uint8))


def darken_paper(im, k):
    return im


def main():
    for i in range(NF):
        build(i / FPS).save("%s/f_%04d.png" % (OUT, i))
    print("FILM frames", NF)


if __name__ == "__main__":
    main()
