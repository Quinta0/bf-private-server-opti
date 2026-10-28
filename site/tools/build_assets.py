#!/usr/bin/env python3
"""Build the images and font for the bfrevive promo site from the original game files.

Every picture on the page comes straight from Brave Frontier's own assets; this script only
crops, resizes and re-encodes them (WebP), plus two small composites: the logo with a
"REVIVE" plate across the blade, and the 1200x630 link-preview image.

Usage:
    python3 site/tools/build_assets.py

Needs Pillow and numpy. The font step also needs fontTools (pip install fonttools brotli);
without it the existing site/public/assets/rodin-eb.woff2 is kept.

Source folders (override with environment variables):
    BF_CONTENT  game archive "content" folder (default ~/bf-archive/assets/content)
    BF_APK      extracted APK assets (default <repo>/re/work/assets-sweep/apk/assets)
"""
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SITE = Path(__file__).resolve().parents[1]
REPO = SITE.parent
OUT = SITE / "public" / "assets"
PUBLIC = SITE / "public"
CONTENT = Path(os.environ.get("BF_CONTENT", Path.home() / "bf-archive/assets/content"))
APK = Path(os.environ.get("BF_APK", REPO / "re/work/assets-sweep/apk/assets"))
FONT_TTF = APK / "TT_RodinNTLG-EB.ttf"

# Unit illustrations (content/unit/img/unit_ills_full_<id>.png)
HERO_UNITS = {
    "phoenix": ("810798", (1000, 800, 640)),  # fire, phoenix wings: the centrepiece
    "earth": ("830148", (720, 460)),
    "water": ("820148", (720, 460)),
    "light": ("850437", (720, 460)),          # wide screens only
    "dark": ("860758", (720, 460)),           # wide screens only
}
# Unit art is detailed but its alpha edge is simple: alpha_quality 50 measured no visible loss.
UNIT_Q, UNIT_AQ = 72, 50
ORIGINAL_SIX = {  # the six starter heroes, 5-star forms
    "vargas": "10015", "selena": "20015", "lance": "30015",
    "eze": "40015", "atro": "50015", "magress": "60015",
}
FONT_UNICODES = ("U+0020-007E,U+00A0,U+00A9,U+00B7,U+2013,U+2014,U+2018,U+2019,"
                 "U+201C,U+201D,U+2026,U+2605,U+2606")


def src(rel):
    path = CONTENT / rel
    if not path.exists():
        sys.exit(f"missing source file: {path}")
    return path


def load(rel):
    return Image.open(src(rel)).convert("RGBA")


def fit_width(img, width):
    if img.width <= width:
        return img
    height = round(img.height * width / img.width)
    return img.resize((width, height), Image.LANCZOS)


def save_webp(img, name, width=None, quality=80, alpha_quality=90):
    out = fit_width(img, width) if width else img
    path = OUT / name
    out.save(path, "WEBP", quality=quality, method=6, alpha_quality=alpha_quality)
    print(f"  {name:28s} {out.width}x{out.height}  {path.stat().st_size / 1024:6.1f} KB")
    return out


def on_black(img):
    """Premultiply onto black. For layers drawn with mix-blend-mode: screen (black is
    screen's identity), this drops the alpha channel without changing the result."""
    a = np.asarray(img, dtype=float)
    return Image.fromarray(np.clip(a[..., :3] * a[..., 3:4] / 255, 0, 255).astype(np.uint8), "RGB")


# ---------------------------------------------------------------- ui pieces

def button_sprite():
    """The arena button, normal state above pressed state, 4px apart so neither bleeds
    into the other when the CSS background is scaled."""
    box = (7, 9, 633, 187)
    up = load("arena_top/arena_red_btn1.png").crop(box)
    down = load("arena_top/arena_red_btn2.png").crop(box)
    sprite = Image.new("RGBA", (up.width, up.height * 2 + 4), (0, 0, 0, 0))
    sprite.alpha_composite(up, (0, 0))
    sprite.alpha_composite(down, (0, up.height + 4))
    return sprite  # 626x360


FRAME_SIDE = 100  # corner slice width: keeps the small corner swirls inside the corners


def frame_pieces():
    """journalpoints_frame for a CSS 9-slice. Its top and bottom edges carry centre
    ornaments that a stretched edge slice would distort, so the edges between the
    corners are rebuilt from one plain column and the bottom ornament is returned
    separately, to be drawn on top at its own aspect ratio. (The top centre gets the
    section's name plate instead.)"""
    frame = load("summoner_journal/journalpoints_frame.png")  # 616x263, 9-slice 44 36 44 36
    edge = 44
    plain = frame.copy()
    for top in (0, frame.height - edge):
        column = frame.crop((FRAME_SIDE + 4, top, FRAME_SIDE + 5, top + edge))
        strip = column.resize((frame.width - 2 * FRAME_SIDE, edge), Image.NEAREST)
        plain.paste(strip, (FRAME_SIDE, top))
    ornament = frame.crop((250, frame.height - edge, 366, frame.height))
    return plain, ornament


# ---------------------------------------------------------------- logo

def rebuild_logo():
    """Full BF logo on transparency, from the Speed Rush minigame title.

    That asset is the only transparent copy of the full emblem + wordmark, but a
    'SPEED RUSH' banner hides the middle of the crystal blade (rows 241-299). Those
    rows are refilled by blending the clean blade rows just above and below
    (premultiplied alpha), which matches the blade's straight, even gradient.
    """
    a = np.array(load("minigame/speedrush/speed_rush_top_logo.png")).astype(float)
    out = np.zeros_like(a)
    top_end, bot_start = 241, 300
    x0, x1 = 290, 352
    out[:top_end] = a[:top_end]
    out[bot_start:, x0:x1] = a[bot_start:, x0:x1]

    def premul(row):
        p = row.copy()
        p[:, :3] *= p[:, 3:4] / 255.0
        return p

    above = premul(a[top_end - 6:top_end, x0:x1].mean(axis=0))
    below = premul(a[bot_start:bot_start + 4, x0:x1].mean(axis=0))
    for y in range(top_end, bot_start):
        t = (y - (top_end - 1)) / (bot_start - (top_end - 1))
        p = (1 - t) * above + t * below
        alpha = p[:, 3:4]
        out[y, x0:x1, :3] = np.where(alpha > 0, p[:, :3] * 255.0 / np.maximum(alpha, 1e-6), 0)
        out[y, x0:x1, 3] = alpha[:, 0]
    img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")
    return img.crop(img.getbbox())  # 578x355; blade gap now at y 235-293


def gold_text(text, size, tracking):
    """Rodin EB capitals in the game's gold style: gradient fill, dark outline, warm glow."""
    font = ImageFont.truetype(str(FONT_TTF), size)
    widths = [font.getbbox(ch)[2] - font.getbbox(ch)[0] for ch in text]
    asc, desc = font.getmetrics()
    pad = size // 2
    w = sum(widths) + tracking * (len(text) - 1) + pad * 2
    h = asc + desc + pad * 2
    mask = Image.new("L", (w, h), 0)
    stroke = Image.new("L", (w, h), 0)
    dm, ds = ImageDraw.Draw(mask), ImageDraw.Draw(stroke)
    x = pad
    for ch, cw in zip(text, widths):
        left = font.getbbox(ch)[0]
        dm.text((x - left, pad), ch, font=font, fill=255)
        ds.text((x - left, pad), ch, font=font, fill=255, stroke_width=max(3, size // 14))
        x += cw + tracking
    box = mask.getbbox()
    top, bottom = box[1], box[3]
    grad = np.zeros((h, w, 4), dtype=np.uint8)
    stops = [(0.0, (255, 252, 226)), (0.45, (255, 214, 102)), (0.55, (226, 160, 46)), (1.0, (255, 226, 140))]
    for y in range(h):
        t = min(max((y - top) / max(bottom - top, 1), 0), 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0)
                grad[y, :, :3] = [round(c0[i] + (c1[i] - c0[i]) * k) for i in range(3)]
                break
    grad[:, :, 3] = 255
    fill = Image.fromarray(grad, "RGBA")
    glow = Image.new("RGBA", (w, h), (255, 190, 80, 0))
    glow.putalpha(stroke.filter(ImageFilter.GaussianBlur(size // 6)).point(lambda v: v * 0.8))
    outline = Image.new("RGBA", (w, h), (46, 20, 0, 255))
    outline.putalpha(stroke)
    out = Image.alpha_composite(glow, outline)
    gold = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gold.paste(fill, (0, 0), mask)
    return Image.alpha_composite(out, gold)


def build_logo():
    """Logo at 1.5x with the game's gold name plate and 'REVIVE' across the blade,
    the way the game branded its minigames (Speed Rush, Smash Shot)."""
    scale = 1.5
    logo = rebuild_logo()
    logo = logo.resize((round(logo.width * scale), round(logo.height * scale)), Image.LANCZOS)
    canvas = Image.new("RGBA", logo.size, (0, 0, 0, 0))
    canvas.alpha_composite(logo)

    plate = load("common/handle_name_frame.png")  # 432x70, inner box x 44-388, y 10-60
    pw = 560
    ps = pw / plate.width
    plate = plate.resize((pw, round(plate.height * ps)), Image.LANCZOS)
    gap_mid = round((235 + 293) / 2 * scale)
    px, py = (canvas.width - pw) // 2, gap_mid - plate.height // 2

    backing = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(backing).rounded_rectangle(
        (px + round(46 * ps), py + round(12 * ps), px + round(386 * ps), py + round(58 * ps)),
        radius=round(20 * ps), fill=(12, 10, 26, 225))
    canvas.alpha_composite(backing)
    canvas.alpha_composite(plate, (px, py))

    word = gold_text("REVIVE", 50, 12)
    word = word.crop(word.getbbox())
    canvas.alpha_composite(word, ((canvas.width - word.width) // 2, gap_mid - word.height // 2 + 1))
    return canvas


# ---------------------------------------------------------------- backgrounds

def citadel():
    """grand_map803 is one painting split in four tiles: [_01|_03] over [_02|_04]."""
    t = {i: load(f"raid/raid_map/grand_map803_0{i}.jpg").convert("RGB") for i in range(1, 5)}
    w, h = t[1].width + t[3].width, t[1].height + t[2].height
    full = Image.new("RGB", (w, h))
    full.paste(t[1], (0, 0))
    full.paste(t[3], (t[1].width, 0))
    full.paste(t[2], (0, t[1].height))
    full.paste(t[4], (t[1].width, t[1].height))
    return full  # 1280x1440


# ---------------------------------------------------------------- favicon / og

def gem_icon():
    gem = load("shop_buy_coin/obtain_gem_icon.png").crop((14, 86, 408, 400))
    side = max(gem.size) + 24
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.alpha_composite(gem, ((side - gem.width) // 2, (side - gem.height) // 2))
    return sq


def build_icons():
    gem = gem_icon()
    for size, name in ((32, "favicon-32.png"), (192, "icon-192.png")):
        gem.resize((size, size), Image.LANCZOS).save(OUT / name, optimize=True)
        print(f"  {name:28s} {size}x{size}  {(OUT / name).stat().st_size / 1024:6.1f} KB")
    gem.save(PUBLIC / "favicon.ico", sizes=[(16, 16), (32, 32), (48, 48)])
    print(f"  {'../favicon.ico':28s} 16-48     {(PUBLIC / 'favicon.ico').stat().st_size / 1024:6.1f} KB")
    touch = Image.new("RGBA", (180, 180), (11, 15, 34, 255))
    g = gem.resize((150, 150), Image.LANCZOS)
    touch.alpha_composite(g, (15, 15))
    touch.convert("RGB").save(OUT / "apple-touch-icon.png", optimize=True)
    print(f"  {'apple-touch-icon.png':28s} 180x180  {(OUT / 'apple-touch-icon.png').stat().st_size / 1024:6.1f} KB")


def shade(img, top_alpha, bottom_alpha):
    """Darken an RGB image with a vertical gradient (alpha 0-255 at top and bottom)."""
    grad = np.linspace(top_alpha, bottom_alpha, img.height).astype(np.uint8)
    layer = Image.new("RGBA", img.size, (7, 9, 22, 0))
    layer.putalpha(Image.fromarray(np.repeat(grad[:, None], img.width, axis=1), "L"))
    return Image.alpha_composite(img.convert("RGBA"), layer)


def build_og(bg_wide, logo):
    w, h = 1200, 630
    bg = bg_wide.resize((w, round(bg_wide.height * w / bg_wide.width)), Image.LANCZOS).crop((0, 0, w, h))
    img = shade(bg, 30, 150)

    def unit(uid, width, flip=False):
        u = load(f"unit/img/unit_ills_full_{uid}.png")
        u = fit_width(u, width)
        return u.transpose(Image.FLIP_LEFT_RIGHT) if flip else u

    earth = unit("830148", 520, flip=True)
    water = unit("820148", 540)
    phoenix = unit("810798", 600)
    img.alpha_composite(earth, (-130, h - earth.height + 60))
    img.alpha_composite(water, (w - water.width + 150, h - water.height + 50))
    img.alpha_composite(phoenix, ((w - phoenix.width) // 2, h - phoenix.height + 150))
    img = shade(img.convert("RGB"), 0, 0)
    top = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    grad = np.linspace(170, 0, 300).astype(np.uint8)
    band = Image.fromarray(np.repeat(grad[:, None], w, axis=1), "L")
    top.paste(Image.new("RGBA", (w, 300), (7, 9, 22, 255)), (0, 0), band)
    img.alpha_composite(top)
    lg = fit_width(logo, 600)
    img.alpha_composite(lg, ((w - lg.width) // 2, 14))
    path = PUBLIC / "og.jpg"
    img.convert("RGB").save(path, "JPEG", quality=82, optimize=True, progressive=True)
    print(f"  {'../og.jpg':28s} {w}x{h}  {path.stat().st_size / 1024:6.1f} KB")


# ---------------------------------------------------------------- font

def build_font():
    target = OUT / "rodin-eb.woff2"
    try:
        import fontTools.subset  # noqa: F401
        import brotli  # noqa: F401
    except ImportError:
        print("  fontTools/brotli not installed: keeping existing rodin-eb.woff2" if target.exists()
              else "  WARNING: fontTools/brotli not installed and no rodin-eb.woff2 yet")
        return
    subprocess.run([sys.executable, "-m", "fontTools.subset", str(FONT_TTF),
                    f"--unicodes={FONT_UNICODES}", "--flavor=woff2",
                    "--layout-features=kern,liga", f"--output-file={target}"], check=True)
    print(f"  {'rodin-eb.woff2':28s}            {target.stat().st_size / 1024:6.1f} KB")


# ---------------------------------------------------------------- main

def main():
    for folder in (CONTENT, APK):
        if not folder.is_dir():
            sys.exit(f"source folder not found: {folder}")
    OUT.mkdir(parents=True, exist_ok=True)

    print("logo")
    logo = build_logo()
    save_webp(logo, "logo-860.webp", 860, quality=80, alpha_quality=60)
    save_webp(logo, "logo-560.webp", 560, quality=80, alpha_quality=60)

    print("backgrounds")
    full = citadel()
    bg_wide = full.crop((0, 0, 1280, 720))
    save_webp(bg_wide, "bg-wide.webp", quality=72)
    save_webp(full.crop((250, 0, 970, 1280)), "bg-tall.webp", quality=70)
    # the game's menu backdrop (sword-in-magic-circle emblem) behind the lower sections
    save_webp(shade(load("common/base2.jpg").convert("RGB"), 110, 150).convert("RGB"), "base.webp", quality=55)

    print("units")
    for key, (uid, widths) in HERO_UNITS.items():
        img = load(f"unit/img/unit_ills_full_{uid}.png")
        for width in widths:
            save_webp(img, f"unit-{key}-{width}.webp", width, quality=UNIT_Q, alpha_quality=UNIT_AQ)
    for name, uid in ORIGINAL_SIX.items():
        img = load(f"unit/img/unit_ills_full_{uid}.png")
        save_webp(img, f"six-{name}.webp", 360, quality=68, alpha_quality=UNIT_AQ)

    print("effects and ui")
    rays = fit_width(load("sam/BgCloudRainbowSkyAdd/BgCloudRainbowSkyAdd_782x634.png"), 480)
    save_webp(on_black(rays), "rays.webp", quality=70)
    save_webp(load("sam/GachaEndCircleReaGet/GachaEndCircleReaGet_645x644.png"), "circle.webp", 440, quality=55, alpha_quality=40)
    save_webp(load("sam/GachaSuperGekiReaAdd/GachaSuperGekiReaAdd_300x300.png"), "sparkle.webp", 128, quality=85)
    save_webp(button_sprite(), "button.webp", quality=80, alpha_quality=60)
    frame, ornament = frame_pieces()
    save_webp(frame, "frame.webp", quality=85)
    save_webp(ornament, "frame-ornament.webp", quality=88)
    save_webp(load("common/handle_name_frame.png"), "plate.webp", quality=90)
    # gold scroll line only; rows 30+ of the source are the translucent name-plate band
    save_webp(load("battle/battle_dungeon_name_deco.png").crop((0, 0, 640, 30)), "divider.webp", quality=90)

    print("icons")
    build_icons()

    print("link preview")
    build_og(bg_wide, logo)

    print("font")
    build_font()


if __name__ == "__main__":
    main()
