"""把 ImageGen 生成的 v2 封面裁成严格 3:4 并去掉右下角 AI 水印。

源图 1024x1536 (2:3)，水印在右下角 (y≈1440..1500)。
做法：保留顶部 1024x1365（= 1024*4/3，恰为 3:4，且裁掉底部含水印区），
再放大到 1152x1536（平台上架规格，1152*4 == 1536*3）。
"""
from PIL import Image

SRC = "assets/cover/Healing_style_2D_cartoon_fishi_2026-09-29T03-20-57.png"
OUT = "assets/cover/fishing-cover-v2-3x4.png"

src = Image.open(SRC).convert("RGB")
w, h = src.size
print("source", w, h)

# 3:4 最大内接矩形（宽度受限，保留顶部 -> 裁掉底部水印区）
cw = w
ch = (w * 4) // 3          # 1024 -> 1365
ch = ch - (ch % 3)         # 保证后续缩放无误差
crop = src.crop((0, 0, cw, ch))
print("cropped", crop.size, "is3x4", cw * 4 == ch * 3)

out = crop.resize((1152, 1536), Image.LANCZOS)
out.save(OUT)
nw, nh = out.size
print("saved", OUT, out.size, "is3x4", nw * 4 == nh * 3)
