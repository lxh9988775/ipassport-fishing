"""Create an exact portrait 3:4 cover from the existing square gameplay cover.

Center-crops the square source to a 3:4 portrait region (keeps full height,
trims equal margins left/right), then upscales to 1152x1536 (the publisher's
preferred cover size). Output satisfies the API's strict width*4 == height*3.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "cover" / "fishing-cover.png"
OUT = ROOT / "assets" / "publish" / "fishing-cover-3x4.png"


def main() -> int:
    src = Image.open(SRC).convert("RGB")
    width, height = src.size
    crop_width = height * 3 // 4  # 3:4 portrait => width = height * 3/4
    left = (width - crop_width) // 2
    cropped = src.crop((left, 0, left + crop_width, height))
    out = cropped.resize((1152, 1536), Image.LANCZOS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    out.save(OUT)
    ok = out.size[0] * 4 == out.size[1] * 3
    print(f"src={width}x{height} crop={cropped.size} -> out={out.size} 3:4={ok} path={OUT}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
