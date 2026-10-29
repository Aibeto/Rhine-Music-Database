"""纸张层：ACES + 纸洗 + 界面合成。数值全部来自 Qt/shaders/enhancement/*.frag。"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080

PAPER = {
    "light": (0.871, 0.859, 0.843),   # #DEDBD7
    "dark":  (0.067, 0.094, 0.106),   # #11181B
}
INK = {
    "light": (0.08, 0.085, 0.075),
    "dark":  (0.92, 0.92, 0.92),
}

_ACES_IN = np.array([[0.59719, 0.07600, 0.02840],
                     [0.35458, 0.90834, 0.13383],
                     [0.04823, 0.01566, 0.83777]])
_ACES_OUT = np.array([[1.60475, -0.10208, -0.00327],
                      [-0.53108, 1.10813, -0.07276],
                      [-0.07367, -0.00605, 1.07602]])


def srgb2lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin2srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def aces(x, exposure=0.92):
    x = np.atleast_2d(x) * exposure
    x = x @ _ACES_IN
    x = (x * (x + 0.0245786) - 0.000090537) / (x * (0.983729 * x + 0.4329510) + 0.238081)
    x = x @ _ACES_OUT
    return np.clip(x, 0, 1)


def wash(img, theme="light", strength=1.0):
    """img: HxWx3 sRGB float. 四层纸洗：顶 / 底 / 椭圆 / 右缘。"""
    h, w, _ = img.shape
    yy, xx = np.mgrid[0:h, 0:w]
    x = xx / (w - 1)
    y = yy / (h - 1)
    paper = np.array(PAPER[theme])
    if theme == "light":
        top = np.clip(0.72 * (1 - y / 0.50), 0, 1) * (y < 0.5)
        bot = np.clip(0.72 * (1 - (1 - y) / 0.35), 0, 1) * (y > 0.65)
        cx, cy, rx, ry, edge = 0.42, 0.47, 0.58, 0.53, 0.19
    else:
        top = np.clip(0.82 * (1 - y / 0.36), 0, 1) * (y < 0.36)
        bot = np.clip(0.84 * (1 - (1 - y) / 0.35), 0, 1) * (y > 0.65)
        cx, cy, rx, ry, edge = 0.48, 0.48, 0.58, 0.53, 0.48
    d = np.sqrt(((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2)
    ell = edge * np.clip((d - 0.72) / (1.30 - 0.72), 0, 1) ** 1.3
    right = np.where(x < 0.59, 0.0, np.where(x < 0.68, (x - 0.59) / 0.09 * 0.92, 1.0))
    amt = np.maximum.reduce([top, bot, ell, right])[..., None] * strength
    return img * (1 - amt) + paper * amt


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64) / 255.0


def grade(rgb_srgb, theme="light", exposure=0.92):
    """Blender 的 Standard 视图变换输出（已 sRGB 编码）→ ACES → 纸洗。"""
    lin = srgb2lin(rgb_srgb)
    return wash(lin2srgb(aces(lin, exposure)), theme)


def save(arr, path):
    Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8)).save(path)


def paper_image(theme="light", size=(W, H)):
    a = np.zeros((size[1], size[0], 3)) + np.array(PAPER[theme])
    return a


# ---------- 排印 ----------
FONT_DIR = "/home/aibeto/code/Rhine-Music-Database/Assets/fonts/"
FONTS = {k: FONT_DIR + "RhineMiSans-%s.otf" % k for k in ("light", "regular", "demibold", "bold")}
SIZES = (10, 11, 12, 13, 14, 15, 22, 32, 50.75)

_cache = {}


def font(weight, size):
    key = (weight, size)
    if key not in _cache:
        _cache[key] = ImageFont.truetype(FONTS[weight], size=int(round(size)))
    return _cache[key]


def draw_text(d, xy, s, weight="regular", size=14, color=(0, 0, 0), tracking=0.0, anchor="lt"):
    """tracking 单位 = px（设计稿 1080 高下的像素）。"""
    f = font(weight, size)
    if not tracking:
        d.text(xy, s, font=f, fill=color, anchor=anchor)
        return
    x, y = xy
    for ch in s:
        d.text((x, y), ch, font=f, fill=color, anchor=anchor)
        x += d.textlength(ch, font=f) + tracking


def text_width(d, s, weight="regular", size=14, tracking=0.0):
    f = font(weight, size)
    if not tracking:
        return d.textlength(s, font=f)
    return sum(d.textlength(c, font=f) for c in s) + tracking * (len(s) - 1)


def canvas(theme="light"):
    img = Image.new("RGB", (W, H), tuple(int(v * 255) for v in PAPER[theme]))
    return img, ImageDraw.Draw(img)


def hairline(d, p0, p1, width=0.15, color=None, theme="light"):
    c = color or tuple(int(v * 255) for v in INK[theme])
    d.line([p0, p1], fill=c, width=max(1, int(round(width))))
