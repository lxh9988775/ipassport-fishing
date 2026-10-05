#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_costumes.py — 生成「口袋兔」换装装扮精灵（5 件，ARGB8888 透明底 72x72）。

坐标约定：与兔子精灵同画布（72x72），直接叠在兔子图上（同位置同缩放）即可
对齐头部：耳朵顶部 y≈4，头顶 y≈12，眼睛 y≈30，脖子 y≈44。

装扮（像素风、粗描边、高饱和点缀，72x72 下 2~4px 一笔）：
  0 草帽  costume_hat     红带草编帽（戴头顶两耳之间）
  1 皇冠  costume_crown   金色三尖皇冠 + 宝石
  2 眼镜  costume_glasses 圆框眼镜（眼睛位置）
  3 围巾  costume_scarf   红围巾（脖子 + 垂尾）
  4 花环  costume_flower  小花环（额头一圈彩色小花）

产物：
  assets/sprites/pet_costumes.c / .h
  assets/sprites/costume_preview.png （合成审阅图，不入固件）
"""
import os
import numpy as np
from PIL import Image, ImageFont, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "sprites")

SIZE = 72

# 名字（屏上显示，gen_font 会自动收字）
COSTUMES = [
    ("costume_hat",     "草帽"),
    ("costume_crown",   "皇冠"),
    ("costume_glasses", "眼镜"),
    ("costume_scarf",   "围巾"),
    ("costume_flower",  "花环"),
]

# 调色板
HAT_BRIM = (232, 198, 118, 255)
HAT_DOME = (220, 182, 102, 255)
HAT_DK   = (192, 154, 82, 255)
HAT_BAND = (196, 30, 80, 255)
CROWN_GD = (250, 206, 84, 255)
CROWN_DK = (214, 168, 56, 255)
GEM_R    = (222, 60, 90, 255)
GEM_B    = (80, 140, 220, 255)
GLS_FR   = (78, 56, 44, 255)
GLS_LN   = (214, 238, 248, 255)
SCARF_R  = (214, 64, 88, 255)
SCARF_LT = (238, 122, 140, 255)
FLW_ST   = (110, 172, 96, 255)
FLW_P    = (244, 130, 168, 255)
FLW_Y    = (250, 214, 100, 255)
FLW_W    = (250, 250, 246, 255)


def new_canvas():
    return Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))


def rect(d, x0, y0, x1, y1, color):
    d.rectangle([x0, y0, x1, y1], fill=color)


def ellipse(d, x0, y0, x1, y1, color):
    d.ellipse([x0, y0, x1, y1], fill=color)


def draw_hat():
    """草帽：帽冠 + 宽帽檐 + 红带，戴在头顶（两耳之间）。"""
    im = new_canvas()
    d = ImageDraw.Draw(im)
    # 帽冠（圆顶）
    ellipse(d, 26, 2, 46, 18, HAT_DOME)
    rect(d, 26, 10, 46, 14, HAT_DOME)
    # 帽檐（宽椭圆）
    ellipse(d, 12, 12, 60, 22, HAT_BRIM)
    rect(d, 12, 16, 60, 19, HAT_BRIM)
    # 红帽带
    rect(d, 26, 12, 46, 15, HAT_BAND)
    # 草编纹理
    for x in range(30, 46, 6):
        d.point((x, 6), fill=HAT_DK)
        d.point((x, 7), fill=HAT_DK)
    return im


def draw_crown():
    """皇冠：金底三尖 + 红/蓝宝石。"""
    im = new_canvas()
    d = ImageDraw.Draw(im)
    # 底带
    rect(d, 24, 14, 48, 20, CROWN_GD)
    rect(d, 24, 19, 48, 20, CROWN_DK)
    # 三个尖
    for cx in (28, 36, 44):
        rect(d, cx - 3, 6, cx + 3, 15, CROWN_GD)
        d.point((cx, 4), fill=CROWN_GD)
        d.point((cx, 5), fill=CROWN_GD)
    # 尖顶圆珠
    for cx in (28, 36, 44):
        d.point((cx, 4), fill=CROWN_DK)
    # 宝石
    rect(d, 34, 15, 38, 18, GEM_R)
    d.point((29, 16), fill=GEM_B)
    d.point((43, 16), fill=GEM_B)
    return im


def draw_glasses():
    """圆框眼镜：两个圆框 + 鼻梁 + 腿。位置对齐眼睛（y≈30）。"""
    im = new_canvas()
    d = ImageDraw.Draw(im)
    ey = 30
    # 镜片（先浅色再描框）
    ellipse(d, 17, ey - 7, 33, ey + 7, GLS_LN)
    ellipse(d, 39, ey - 7, 55, ey + 7, GLS_LN)
    # 框（空心圆用两圈近似）
    ellipse(d, 16, ey - 8, 34, ey + 8, GLS_FR)
    ellipse(d, 18, ey - 6, 32, ey + 6, GLS_LN)
    ellipse(d, 38, ey - 8, 56, ey + 8, GLS_FR)
    ellipse(d, 40, ey - 6, 54, ey + 6, GLS_LN)
    # 鼻梁 + 腿
    rect(d, 33, ey - 2, 39, ey - 1, GLS_FR)
    rect(d, 12, ey - 2, 16, ey, GLS_FR)
    rect(d, 56, ey - 2, 60, ey, GLS_FR)
    return im


def draw_scarf():
    """围巾：脖子一圈 + 垂下的尾巴（脖子在脸下方 y≈52）。"""
    im = new_canvas()
    d = ImageDraw.Draw(im)
    # 脖子一圈（肩颈交界 y≈54）
    rect(d, 22, 54, 52, 60, SCARF_R)
    rect(d, 22, 58, 52, 60, SCARF_LT)
    # 垂尾
    rect(d, 40, 60, 50, 69, SCARF_R)
    rect(d, 40, 67, 50, 69, SCARF_LT)
    rect(d, 38, 69, 52, 71, SCARF_LT)
    # 流苏
    d.point((39, 71), fill=SCARF_R)
    d.point((45, 71), fill=SCARF_R)
    d.point((51, 71), fill=SCARF_R)
    return im


def draw_flower():
    """花环：额头一圈绿色藤 + 彩色小花。"""
    im = new_canvas()
    d = ImageDraw.Draw(im)
    # 藤带（弧形近似：三段）
    rect(d, 18, 12, 54, 15, FLW_ST)
    rect(d, 16, 14, 20, 18, FLW_ST)
    rect(d, 52, 14, 56, 18, FLW_ST)
    # 小花（每朵 4 瓣 + 芯）
    for cx, cy, petal, core in (
        (20, 14, FLW_P, FLW_Y),
        (30, 12, FLW_W, FLW_Y),
        (40, 12, FLW_P, FLW_Y),
        (50, 14, FLW_W, FLW_Y),
    ):
        d.point((cx, cy - 2), fill=petal)
        d.point((cx, cy + 2), fill=petal)
        d.point((cx - 2, cy), fill=petal)
        d.point((cx + 2, cy), fill=petal)
        d.point((cx, cy), fill=core)
        d.point((cx - 1, cy - 1), fill=petal)
        d.point((cx + 1, cy - 1), fill=petal)
        d.point((cx - 1, cy + 1), fill=petal)
        d.point((cx + 1, cy + 1), fill=petal)
    return im


DRAWERS = [draw_hat, draw_crown, draw_glasses, draw_scarf, draw_flower]


# ---------------------------------------------------------------------------
# ARGB8888 导出（与 gen_pet_sprites.py 同序：B,G,R,A）
# ---------------------------------------------------------------------------
def argb8888_bytes(img):
    a = np.asarray(img.convert("RGBA"))
    bgra = a[..., [2, 1, 0, 3]]
    return bgra.tobytes()


def fmt_bytes(data, per_line=16):
    return "\n".join(
        "    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ","
        for i in range(0, len(data), per_line)
    )


def emit_c(path, entries):
    parts = [
        "// 本文件由 tools/gen_costumes.py 自动生成，请勿手工编辑。",
        "#include \"lvgl.h\"",
        "#include \"pet_costumes.h\"",
        "",
    ]
    for name, w, h, data in entries:
        parts.append("static const uint8_t %s_map[] = {" % name)
        parts.append(fmt_bytes(data))
        parts.append("};")
        parts.append("")
        parts.append("const lv_image_dsc_t %s = {" % name)
        parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        parts.append("    .header.cf = LV_COLOR_FORMAT_ARGB8888,")
        parts.append("    .header.w = %d," % w)
        parts.append("    .header.h = %d," % h)
        parts.append("    .header.stride = %d," % (w * 4))
        parts.append("    .data_size = sizeof(%s_map)," % name)
        parts.append("    .data = %s_map," % name)
        parts.append("};")
        parts.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts) + "\n")


def emit_header(path, names):
    parts = [
        "// 本文件由 tools/gen_costumes.py 自动生成，请勿手工编辑。",
        "#pragma once",
        "#include \"lvgl.h\"",
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
    ]
    for n in names:
        parts.append("extern const lv_image_dsc_t %s;" % n)
    parts += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))


def build_preview(path, rabbit, pairs):
    """合成审阅：每件装扮叠在正面兔上。"""
    cols = 3
    cell_w, cell_h = 200, 216
    pad = 20
    rows = (len(pairs) + cols - 1) // cols
    W = cols * cell_w + pad * 2
    H = pad + 36 + rows * cell_h + pad
    im = Image.new("RGB", (W, H), (255, 244, 227))
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(os.path.join(ROOT, "assets/fonts/SourceHanSansCN-Normal.otf"), 18)
    except Exception:
        f = ImageFont.load_default()
    d.text((pad, pad - 8), "换装审阅（装扮叠加在正面兔上）", fill=(90, 60, 40), font=f)
    sc = 2
    for i, (zh, img) in enumerate(pairs):
        r, c = divmod(i, cols)
        x = pad + c * cell_w
        y = pad + 36 + r * cell_h
        base = rabbit.convert("RGBA")
        comp = Image.alpha_composite(base, img)
        big = comp.resize((comp.size[0] * sc, comp.size[1] * sc), Image.NEAREST)
        bgc = Image.new("RGB", big.size, (214, 236, 200))
        bgc.paste(big, (0, 0), big)
        im.paste(bgc, (x, y))
        d.text((x, y + big.size[1] + 4), zh, fill=(90, 60, 40), font=f)
    im.save(path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    rabbit = Image.open(os.path.join(OUT_DIR, "rabbit_front.png")).convert("RGBA")

    entries = []
    names = []
    pairs = []
    for (key, zh), drawer in zip(COSTUMES, DRAWERS):
        img = drawer()
        img.save(os.path.join(OUT_DIR, "%s.png" % key))
        entries.append((key, SIZE, SIZE, argb8888_bytes(img)))
        names.append(key)
        pairs.append((zh, img))
        print("  %-18s %s" % (key, zh))

    emit_c(os.path.join(OUT_DIR, "pet_costumes.c"), entries)
    emit_header(os.path.join(OUT_DIR, "pet_costumes.h"), names)
    build_preview(os.path.join(OUT_DIR, "costume_preview.png"), rabbit, pairs)
    n = sum(len(e[3]) for e in entries)
    print("[gen_costumes] costumes: %d, C bytes: %d (%.1f KB flash)" % (len(entries), n, n / 1024.0))
    print("[gen_costumes] wrote: assets/sprites/{pet_costumes.c,pet_costumes.h,costume_preview.png}")


if __name__ == "__main__":
    main()
