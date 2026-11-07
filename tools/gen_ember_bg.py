# gen_ember_bg.py — 生成《余烬》夜森林静态背景 (LVGL v9.5 ARGB8888 常量图)
#
# 设计约束（见 余烬_设计案V1.html 第八节）：
#   - 夜森林是静态舞台，任何火色下都不变色、不参与混色；
#   - 火是唯一的光源，也是唯一会变色的东西（精灵层单独上色，背景一像素不碰）；
#   - 屏 240x320，无 PSRAM；背景走 RGB565A8/ARGB8888 常量图放 Flash，不占 LV_MEM。
#
# 字节序严格对齐本工程 pet_sprites.c 的已验证格式：
#   .header.magic   = LV_IMAGE_HEADER_MAGIC
#   .header.cf      = LV_COLOR_FORMAT_ARGB8888
#   .header.stride  = W * 4
#   每像素 4 字节：[R, G, B, A]，A=255 不透明。
#
# 用法：python tools/gen_ember_bg.py  ->  写 assets/ember/ember_bg.c
import struct, os, random
from PIL import Image, ImageDraw, ImageFilter

W, H = 240, 320
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "ember", "ember_bg.c")

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

# 夜空三段渐变（冷暗，让暖色火苗跳出来）
SKY_TOP   = (8, 12, 30)      # 深蓝近黑
SKY_MID   = (18, 28, 58)     # 夜蓝
SKY_LOW   = (26, 34, 52)     # 地平线略暖灰蓝
TREE      = (6, 9, 20)       # 树/栅栏剪影（比天空更暗）
GROUND    = (12, 14, 24)     # 地面
WINDOW    = (232, 170, 86)   # 树屋暖窗（唯一暖光源，弱）
FIREFLY   = (220, 200, 120)  # 萤火（冷调黄，避免与火苗暖橙混淆）

random.seed(20261104)  # 固定，保证可复现

img = Image.new("RGB", (W, H), SKY_TOP)
d = ImageDraw.Draw(img)

# --- 天空竖直渐变 ---
for y in range(H):
    if y < 200:
        t = y / 200.0
        if t < 0.5:
            col = lerp(SKY_TOP, SKY_MID, t / 0.5)
        else:
            col = lerp(SKY_MID, SKY_LOW, (t - 0.5) / 0.5)
    else:
        col = SKY_LOW
    d.line([(0, y), (W - 1, y)], fill=col)

# --- 远山轮廓（最暗一层，地平线 y≈205）---
horizon = 205
for x in range(0, W, 1):
    # 用几条正弦叠出缓坡
    h = horizon - int(10 + 6 * __import__("math").sin(x / 40.0) + 4 * __import__("math").sin(x / 17.0))
    d.line([(x, h), (x, H - 1)], fill=GROUND)

# --- 树屋暖窗（x≈185, y≈120 的小屋 + 光晕）---
# 先在独立层画径向光晕，再合成，保证柔和
glow = Image.new("RGB", (W, H), (0, 0, 0))
gd = ImageDraw.Draw(glow)
wx, wy = 186, 122
for r in range(46, 0, -2):
    a = int(70 * (1 - r / 46.0) ** 1.6)
    gd.ellipse([wx - r, wy - r, wx + r, wy + r], fill=(WINDOW[0], WINDOW[1], WINDOW[2]))
# 模糊光晕
glow = glow.filter(ImageFilter.GaussianBlur(10))
# 合成（additive-ish：取较亮者）
pix = img.load(); gp = glow.load()
for y in range(H):
    for x in range(W):
        r = max(pix[x, y][0], gp[x, y][0]); g = max(pix[x, y][1], gp[x, y][1]); b = max(pix[x, y][2], gp[x, y][2])
        pix[x, y] = (r, g, b)
# 窗框实体（小暖方块）
d.rectangle([wx - 7, wy - 8, wx + 7, wy + 8], fill=WINDOW)
d.rectangle([wx - 7, wy - 8, wx + 7, wy + 8], outline=(120, 80, 30))

# --- 树（剪影三角，分布在地平线上）---
def tree(cx, base, th, tw):
    d.polygon([(cx, base - th), (cx - tw, base), (cx + tw, base)], fill=TREE)
    d.rectangle([cx - 2, base - 6, cx + 2, base + 4], fill=TREE)

tree(40, horizon + 4, 70, 26)
tree(95, horizon + 2, 58, 22)
tree(150, horizon + 5, 78, 28)
tree(208, horizon, 64, 24)
# 一棵前景大剪影（左下，压暗边框）
d.polygon([(0, 250), (20, 250), (8, 120), (0, 120)], fill=TREE)

# --- 栅栏（地平线附近几根竖线）---
for fx in range(20, 220, 26):
    d.rectangle([fx, horizon - 2, fx + 3, horizon + 14], fill=TREE)
d.line([(0, horizon + 4), (W, horizon + 4)], fill=TREE)

# --- 萤火（固定位置的小暖点 + 微光晕）---
ff = [(60, 150), (120, 175), (170, 200), (95, 135), (200, 165), (45, 195)]
for (fx, fy) in ff:
    for r in range(6, 0, -1):
        a = int(120 * (1 - r / 6.0))
        d.ellipse([fx - r, fy - r, fx + r, fy + r], fill=FIREFLY)
    d.ellipse([fx - 1, fy - 1, fx + 1, fy + 1], fill=(255, 240, 180))

# --- 导出 ARGB8888 字节流 [R,G,B,A] ---
data = bytearray()
px = img.load()
for y in range(H):
    for x in range(W):
        r, g, b = px[x, y]
        data += bytes((r, g, b, 255))

# --- 写出 C 文件 ---
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    f.write("/* 本文件由 tools/gen_ember_bg.py 自动生成，请勿手工编辑。 */\n")
    f.write('#include "lvgl.h"\n\n')
    f.write("static const uint8_t ember_bg_map[] = {\n")
    n = len(data)
    for i in range(0, n, 12):
        chunk = data[i:i + 12]
        f.write("    " + ", ".join("0x%02X" % b for b in chunk) + ",\n")
    f.write("};\n\n")
    f.write("const lv_img_dsc_t ember_bg_dsc = {\n")
    f.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
    f.write("    .header.cf = LV_COLOR_FORMAT_ARGB8888,\n")
    f.write("    .header.w = %d,\n" % W)
    f.write("    .header.h = %d,\n" % H)
    f.write("    .header.stride = %d,\n" % (W * 4))
    f.write("    .data_size = sizeof(ember_bg_map),\n")
    f.write("    .data = ember_bg_map,\n")
    f.write("};\n")

print("wrote", OUT, "(%d bytes image data, %d px)" % (len(data), W * H))
