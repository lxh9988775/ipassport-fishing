"""Compose the 3:4 portrait community cover for the pet play.

Uses the approved AI-generated pixel rabbit (front view) as the hero art.
Pure PIL composition - no AI image generation, no watermark of any tool.
Output: assets/publish/pet/cover-3x4.png (1152x1536, PNG < 10 MiB).
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "assets" / "pixel_pet" / (
    "把这张毛绒兔子玩偶照片转换成16_bit像素风格的游戏角色精_2026-10-04T13-11-10.png"
)
OUT_DIR = ROOT / "assets" / "publish" / "pet"
OUT = OUT_DIR / "cover-3x4.png"

W, H = 1152, 1536


def load_font(size: int, bold: bool = True) -> ImageFont.FreeTypeFont:
    candidates = [
        r"C:\Windows\Fonts\msyhbd.ttc" if bold else r"C:\Windows\Fonts\msyh.ttc",
        r"C:\Windows\Fonts\simhei.ttf",
        r"C:\Windows\Fonts\arial.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    src = Image.open(SRC).convert("RGB")
    bg = src.getpixel((8, 8))

    # Erase the generator watermark (bottom-right flat background area).
    # Use the LOCAL background color there, not the global one - the source
    # has a subtle gradient and a global fill leaves a visible patch.
    clean = src.copy()
    d = ImageDraw.Draw(clean)
    d.rectangle((812, 944, src.width, src.height), fill=src.getpixel((790, 960)))

    canvas = Image.new("RGB", (W, H), bg)
    draw = ImageDraw.Draw(canvas)

    # Soft vignette band behind the rabbit for depth.
    band = Image.new("RGB", (W, H), bg)
    band_draw = ImageDraw.Draw(band)
    for i in range(140):
        alpha_color = tuple(
            max(0, c - int(14 * (1 - i / 140))) for c in bg
        )
        band_draw.rectangle((0, i, W, i + 1), fill=alpha_color)
    canvas.paste(band, (0, 0))

    # Hero rabbit, large and centered (bottom third stays clean: the
    # community card overlay covers the lower part of the cover, and any
    # text there gets occluded -> review rejection. Keep it text-free).
    rabbit_w = 940
    rabbit = clean.resize((rabbit_w, rabbit_w), Image.LANCZOS)
    rabbit_x = (W - rabbit_w) // 2
    rabbit_y = 430
    canvas.paste(rabbit, (rabbit_x, rabbit_y))

    ink = (58, 58, 72)

    # Title.
    f_title = load_font(160)
    title = "口袋兔"
    tw = draw.textlength(title, font=f_title)
    draw.text(((W - tw) / 2 + 4, 84 + 4), title, font=f_title, fill=(226, 200, 168))
    draw.text(((W - tw) / 2, 84), title, font=f_title, fill=ink)

    # Subtitle.
    f_sub = load_font(56, bold=False)
    sub = "随 身 电 子 宠 物"
    sw = draw.textlength(sub, font=f_sub)
    draw.text(((W - sw) / 2, 296), sub, font=f_sub, fill=(150, 120, 110))

    # A few pink hearts.
    heart = Image.new("RGBA", (80, 74), (0, 0, 0, 0))
    hd = ImageDraw.Draw(heart)
    pink = (233, 130, 155, 255)
    hd.ellipse((0, 8, 38, 46), fill=pink)
    hd.ellipse((42, 8, 80, 46), fill=pink)
    hd.polygon([(4, 30), (76, 30), (40, 74)], fill=pink)
    for (hx, hy, s) in [(96, 300, 1.0), (1010, 360, 0.8), (90, 1330, 0.7), (1000, 1310, 0.9)]:
        hs = heart.resize((int(80 * s), int(74 * s)), Image.LANCZOS)
        canvas.paste(hs, (hx, hy), hs)

    canvas.save(OUT, "PNG", optimize=True)
    print(f"saved: {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
