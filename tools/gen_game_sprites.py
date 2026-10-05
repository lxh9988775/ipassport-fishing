#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_game_sprites.py — 生成 5 款小游戏的像素风道具精灵（ARGB8888 透明底）。

参考拓麻歌子/Tamagotchi/Pou 等经典像素宠物游戏的套路：
  全场景像素插画 + 精灵化游戏物件 + 三键极简操作。
把原来纯色矩形占位的道具全部换成像素小精灵（本地 PIL 逐像素画，不花积分）：

  接胡萝卜   spr_carrot        胡萝卜（掉落物）
  颜色配对   spr_egg_{r,b,y,g} 四色蛋（颜色必须与 pet.c COLORS_RGB 完全一致）
  记忆翻牌   spr_paw/heart/star/flower  卡面小图标
  躲猫猫     spr_bush          灌木丛（兔子藏身后）
             spr_arrow         红色选择箭头
  节奏蹦蹦   spr_drum          小鼓（拍点命中区）
             spr_note          音符（移动标记）

产物：
  assets/sprites/pet_game_sprites.c / .h
  assets/sprites/game_preview.png   （审阅图，不入固件）
"""
import os
import numpy as np
from PIL import Image, ImageFont, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "sprites")

# 与 pet.c COLORS_RGB 严格一致（gc(i) 的四个颜色）
EGG_COLORS = [
    (214, 40, 92),    # 0 红
    (40, 110, 210),   # 1 蓝
    (240, 200, 50),   # 2 黄
    (80, 190, 110),   # 3 绿
]
EGG_NAMES = ["spr_egg_red", "spr_egg_blue", "spr_egg_yellow", "spr_egg_green"]


def C(rgb, a=255):
    return (rgb[0], rgb[1], rgb[2], a)


def canvas(w, h):
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


# ---------------------------------------------------------------------------
# 胡萝卜 24x32：橙色锥形 + 绿缨
# ---------------------------------------------------------------------------
def draw_carrot():
    im = canvas(24, 32)
    d = ImageDraw.Draw(im)
    OR = C((240, 140, 50))
    OR_DK = C((206, 112, 34))
    LF = C((96, 178, 80))
    LF_DK = C((70, 142, 58))
    # 绿缨（三撮）
    d.line([(9, 8), (7, 1)], fill=LF, width=3)
    d.line([(12, 7), (12, 0)], fill=LF_DK, width=3)
    d.line([(15, 8), (17, 1)], fill=LF, width=3)
    # 身体（从宽到尖的阶梯三角）
    for i, (x0, x1) in enumerate(((4, 19), (5, 18), (5, 18), (6, 17), (6, 17),
                                  (7, 16), (7, 16), (8, 15), (8, 15), (9, 14),
                                  (9, 14), (10, 13), (10, 13), (11, 12), (11, 12),
                                  (11, 12), (11, 12))):
        y = 10 + i
        d.rectangle([x0, y, x1, y], fill=OR)
    # 暗侧 + 横纹
    d.line([(18, 11), (12, 26)], fill=OR_DK, width=1)
    for y in (14, 18, 22):
        d.line([(7, y), (11, y)], fill=OR_DK, width=1)
    return im


# ---------------------------------------------------------------------------
# 彩蛋 48x48：椭圆蛋 + 深色描边 + 高光
# ---------------------------------------------------------------------------
def draw_egg(rgb):
    im = canvas(48, 48)
    d = ImageDraw.Draw(im)
    body = C(rgb)
    dk = C(tuple(max(0, v - 60) for v in rgb))
    lt = C(tuple(min(255, v + 45) for v in rgb))
    # 干净的单椭圆蛋 + 深色描边 + 高光
    d.ellipse([9, 6, 39, 44], fill=body, outline=dk, width=2)
    d.ellipse([15, 11, 23, 21], fill=lt)     # 左上高光
    d.point((26, 12), fill=lt)
    return im


# ---------------------------------------------------------------------------
# 卡面图标 20x20
# ---------------------------------------------------------------------------
def draw_paw():
    im = canvas(20, 20)
    d = ImageDraw.Draw(im)
    PK = C((226, 140, 150))
    PK_DK = C((188, 104, 116))
    d.ellipse([5, 9, 15, 18], fill=PK, outline=PK_DK, width=1)   # 掌
    d.ellipse([3, 3, 8, 8], fill=PK, outline=PK_DK)              # 趾
    d.ellipse([8, 1, 13, 6], fill=PK, outline=PK_DK)
    d.ellipse([13, 3, 18, 8], fill=PK, outline=PK_DK)
    return im


def draw_heart():
    im = canvas(20, 20)
    d = ImageDraw.Draw(im)
    RD = C((222, 60, 90))
    d.ellipse([2, 3, 10, 11], fill=RD)
    d.ellipse([10, 3, 18, 11], fill=RD)
    d.polygon([(3, 8), (17, 8), (10, 18)], fill=RD)
    d.point((5, 5), fill=C((250, 180, 200)))
    return im


def draw_star():
    im = canvas(20, 20)
    d = ImageDraw.Draw(im)
    YL = C((250, 210, 70))
    YL_DK = C((210, 168, 40))
    d.polygon([(10, 1), (12, 7), (19, 7), (13, 12), (15, 19), (10, 15),
               (5, 19), (7, 12), (1, 7), (8, 7)], fill=YL, outline=YL_DK)
    return im


def draw_flower():
    im = canvas(20, 20)
    d = ImageDraw.Draw(im)
    PK = C((244, 130, 168))
    YL = C((250, 214, 100))
    for cx, cy in ((10, 3), (10, 13), (3, 8), (13, 8), (5, 3), (15, 3), (5, 13), (15, 13)):
        d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=PK)
    d.ellipse([7, 5, 13, 11], fill=YL)
    return im


# ---------------------------------------------------------------------------
# 灌木丛 60x84
# ---------------------------------------------------------------------------
def draw_bush():
    im = canvas(60, 84)
    d = ImageDraw.Draw(im)
    GR = C((92, 168, 92))
    GR_DK = C((62, 130, 64))
    GR_LT = C((128, 198, 118))
    # 三团圆灌木
    d.ellipse([2, 26, 38, 76], fill=GR, outline=GR_DK, width=2)
    d.ellipse([22, 12, 58, 70], fill=GR, outline=GR_DK, width=2)
    d.ellipse([10, 34, 52, 80], fill=GR)
    d.arc([2, 26, 38, 76], 90, 270, fill=GR_DK, width=2)
    d.arc([22, 12, 58, 70], 90, 270, fill=GR_DK, width=2)
    # 高光叶
    d.ellipse([12, 40, 22, 50], fill=GR_LT)
    d.ellipse([34, 30, 44, 40], fill=GR_LT)
    d.point((28, 60), fill=GR_LT)
    d.point((40, 58), fill=GR_LT)
    # 底部收口
    d.rectangle([8, 74, 52, 80], fill=GR)
    d.line([(8, 79), (52, 79)], fill=GR_DK, width=2)
    # 两颗小果子
    d.ellipse([14, 52, 20, 58], fill=C((226, 90, 110)))
    d.ellipse([42, 48, 48, 54], fill=C((226, 90, 110)))
    return im


# ---------------------------------------------------------------------------
# 选择箭头 14x10（红色下指）
# ---------------------------------------------------------------------------
def draw_arrow():
    im = canvas(14, 10)
    d = ImageDraw.Draw(im)
    RD = C((214, 40, 92))
    WH = C((255, 255, 255))
    d.polygon([(1, 1), (12, 1), (6, 9)], fill=RD, outline=WH)
    return im


# ---------------------------------------------------------------------------
# 音符 12x16
# ---------------------------------------------------------------------------
def draw_note():
    im = canvas(12, 16)
    d = ImageDraw.Draw(im)
    DK = C((70, 60, 70))
    d.ellipse([1, 10, 8, 16], fill=DK)          # 符头
    d.line([(7, 2), (7, 12)], fill=DK, width=2)  # 符杆
    d.polygon([(7, 1), (11, 4), (7, 6)], fill=DK)  # 符尾
    return im


# ---------------------------------------------------------------------------
# 小鼓 56x56
# ---------------------------------------------------------------------------
def draw_drum():
    im = canvas(56, 56)
    d = ImageDraw.Draw(im)
    RD = C((206, 60, 76))
    RD_DK = C((164, 40, 56))
    TM = C((244, 226, 186))   # 鼓皮
    TM_DK = C((210, 190, 150))
    GD = C((240, 200, 90))
    # 鼓身
    d.rectangle([6, 16, 49, 48], fill=RD, outline=RD_DK, width=2)
    # 鼓皮
    d.ellipse([4, 4, 51, 28], fill=TM, outline=TM_DK, width=2)
    d.ellipse([10, 9, 45, 24], outline=TM_DK, width=1)
    # 交叉绑绳
    d.line([(10, 20), (46, 44)], fill=GD, width=2)
    d.line([(46, 20), (10, 44)], fill=GD, width=2)
    d.line([(28, 18), (28, 48)], fill=GD, width=2)
    # 鼓身高光
    d.line([(9, 26), (9, 44)], fill=C((236, 120, 130)), width=2)
    return im


# ---------------------------------------------------------------------------
# ARGB8888 导出（与 gen_pet_sprites.py 同序：B,G,R,A）
# ---------------------------------------------------------------------------
def argb8888_bytes(img):
    a = np.asarray(img.convert("RGBA"))
    return a[..., [2, 1, 0, 3]].tobytes()


def fmt_bytes(data, per_line=16):
    return "\n".join(
        "    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ","
        for i in range(0, len(data), per_line)
    )


def emit_c(path, entries):
    parts = [
        "// 本文件由 tools/gen_game_sprites.py 自动生成，请勿手工编辑。",
        "#include \"lvgl.h\"",
        "#include \"pet_game_sprites.h\"",
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
        "// 本文件由 tools/gen_game_sprites.py 自动生成，请勿手工编辑。",
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


def build_preview(path, sprites):
    cols = 5
    cell_w, cell_h = 150, 190
    pad = 20
    rows = (len(sprites) + cols - 1) // cols
    W = cols * cell_w + pad * 2
    H = pad + 36 + rows * cell_h + pad
    im = Image.new("RGB", (W, H), (255, 244, 227))
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(os.path.join(ROOT, "assets/fonts/SourceHanSansCN-Normal.otf"), 16)
    except Exception:
        f = ImageFont.load_default()
    d.text((pad, pad - 6), "小游戏道具精灵审阅（像素风）", fill=(90, 60, 40), font=f)
    for i, (name, img) in enumerate(sprites):
        r, c = divmod(i, cols)
        x = pad + c * cell_w + (cell_w - img.size[0] * 2) // 2
        y = pad + 36 + r * cell_h
        big = img.resize((img.size[0] * 2, img.size[1] * 2), Image.NEAREST)
        bgc = Image.new("RGB", big.size, (214, 236, 200))
        bgc.paste(big, (0, 0), big)
        im.paste(bgc, (x, y))
        d.text((pad + c * cell_w, y + big.size[1] + 4), name, fill=(90, 60, 40), font=f)
    im.save(path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    built = []
    built.append(("spr_carrot", draw_carrot()))
    for name, rgb in zip(EGG_NAMES, EGG_COLORS):
        built.append((name, draw_egg(rgb)))
    built.append(("spr_paw", draw_paw()))
    built.append(("spr_heart", draw_heart()))
    built.append(("spr_star", draw_star()))
    built.append(("spr_flower", draw_flower()))
    built.append(("spr_bush", draw_bush()))
    built.append(("spr_arrow", draw_arrow()))
    built.append(("spr_note", draw_note()))
    built.append(("spr_drum", draw_drum()))

    entries = []
    names = []
    for key, img in built:
        entries.append((key, img.size[0], img.size[1], argb8888_bytes(img)))
        names.append(key)
        print("  %-16s %dx%d" % (key, img.size[0], img.size[1]))

    emit_c(os.path.join(OUT_DIR, "pet_game_sprites.c"), entries)
    emit_header(os.path.join(OUT_DIR, "pet_game_sprites.h"), names)
    build_preview(os.path.join(OUT_DIR, "game_preview.png"), built)
    n = sum(len(e[3]) for e in entries)
    print("[gen_game_sprites] sprites: %d, C bytes: %d (%.1f KB flash)" % (len(entries), n, n / 1024.0))


if __name__ == "__main__":
    main()
