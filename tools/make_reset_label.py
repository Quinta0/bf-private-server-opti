"""Draw the "Reset Account" label for the Menu button that tools/patch_apk.py repurposes.

The Menu labels (content/menu/menu_*_btn1.png, *_btn2.png when pressed) are 180x156
grey+alpha images drawn over the shared button base: white bold text with a soft black
glow; the pressed copy has grey text (L 153) two pixels lower. DIN Alternate Bold with a
1 px stroke is the closest system font: 35 px matches "Credits" in size and weight.
Two-line labels ("Item Guide", "Player Info") span y 48-106, so "Reset Account" uses
34 px there. --check redraws "Credits" and reports how well its text overlaps the
original, so the style stays honest.

Usage:
  python3 -I tools/make_reset_label.py --out tools/apk-assets
  python3 -I tools/make_reset_label.py --check ".../assets/content/menu/menu_credit_btn1.png"
"""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT = '/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf'
SIZE = (180, 156)
STROKE = 1
GLOW_GROW = 2          # px the glow mask is widened before blurring
GLOW_BLUR = 5.0
GLOW_GAIN = 1.3
PRESSED_GREY = 153
PRESSED_DROP = 2
TEXT_ALPHA = 200       # alpha above this counts as text (not glow) in --check


def text_mask(lines: list[str], font_px: int, line_gap: int, center_y: int) -> Image.Image:
    font = ImageFont.truetype(FONT, font_px)
    boxes = [font.getbbox(line, stroke_width=STROKE) for line in lines]
    heights = [b[3] - b[1] for b in boxes]
    total = sum(heights) + line_gap * (len(lines) - 1)
    mask = Image.new('L', SIZE, 0)
    draw = ImageDraw.Draw(mask)
    y = center_y - total // 2
    for line, box, height in zip(lines, boxes, heights):
        x = (SIZE[0] - (box[2] - box[0])) // 2 - box[0]
        draw.text((x, y - box[1]), line, font=font, fill=255, stroke_width=STROKE, stroke_fill=255)
        y += height + line_gap
    return mask


def label(lines: list[str], font_px: int, line_gap: int, center_y: int, pressed: bool) -> Image.Image:
    mask = text_mask(lines, font_px, line_gap, center_y + (PRESSED_DROP if pressed else 0))
    grown = mask.filter(ImageFilter.MaxFilter(GLOW_GROW * 2 + 1))
    glow = grown.filter(ImageFilter.GaussianBlur(GLOW_BLUR)).point(lambda a: min(255, int(a * GLOW_GAIN)))
    alpha = ImageChops.lighter(glow, mask)
    grey = mask.point(lambda a: PRESSED_GREY * a // 255 if pressed else a)
    return Image.merge('LA', (grey, alpha))


def check(original: Path) -> None:
    def text(im: Image.Image) -> Image.Image:
        return im.getchannel('A').point(lambda a: 255 if a > TEXT_ALPHA else 0)
    ours = text(label(['Credits'], 35, 0, 76, pressed=False))
    theirs = text(Image.open(original).convert('LA'))
    both = ImageChops.multiply(ours, theirs).histogram()[255]
    either = ImageChops.lighter(ours, theirs).histogram()[255]
    print(f'"Credits" text overlap with the original: {both / either:.2f} '
          f'(ours {ours.getbbox()}, original {theirs.getbbox()})')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path)
    ap.add_argument('--check', type=Path, help='original menu_credit_btn1.png to compare a redrawn "Credits" with')
    args = ap.parse_args()
    if args.check:
        check(args.check)
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        for n, pressed in ((1, False), (2, True)):
            path = args.out / f'menu_reset_btn_{n}.png'
            label(['Reset', 'Account'], 34, 4, 77, pressed).save(path, optimize=True)
            print(f'wrote {path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
