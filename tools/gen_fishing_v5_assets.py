#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""竿影浮标 v5 · C+2 新增美术资产（自有像素画，不改已有 sprite）。

产出：
  assets/raw/estuary_sheet.png      3 条新鱼拼图（品红底，4x2 网格，仅前 3 格有鱼）
  assets/raw/bg_estuary.png         入海口背景（从已确认效果图复制）
  assets/raw/bg_pier.png            夜钓栈桥背景
  assets/raw/bg_estuary_dark.png    标点选择页用的预压暗背景（不靠运行时半透明层）
  assets/raw/thumb_*.png            5 张钓场缩略图（34x32）
然后由 tools/gen_sprites.py 统一烘焙成 C 数组。
"""
import os
import shutil
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "assets", "raw")
UI = os.path.join(ROOT, "build", "fish_ui")
MAGENTA = (233, 11, 195, 255)
SHEET = 1024
CELL_W, CELL_H = SHEET // 4, SHEET // 2


def new_fish_canvas():
    return Image.new("RGBA", (48, 32), (0, 0, 0, 0))


def draw_mullet():
    """鲻鱼：银灰长身 + 暗色纵纹，常见档，比白条更壮一点。"""
    im = new_fish_canvas()
    d = ImageDraw.Draw(im)
    d.polygon([(40, 16), (47, 9), (47, 23)], fill=(88, 108, 126, 255))     # 尾鳍
    d.polygon([(8, 14), (38, 10), (44, 16), (38, 23), (8, 19)], fill=(168, 184, 196, 255))
    d.polygon([(8, 14), (16, 9), (34, 10), (38, 14)], fill=(198, 210, 218, 255))
    d.polygon([(14, 19), (34, 20), (30, 25), (18, 24)], fill=(136, 154, 170, 255))
    d.polygon([(20, 10), (28, 5), (34, 10)], fill=(120, 142, 160, 255))     # 背鳍
    d.polygon([(20, 21), (27, 26), (32, 21)], fill=(126, 146, 164, 255))
    for y in (13, 16, 19):
        d.line([(12, y), (36, y)], fill=(108, 128, 146, 255), width=1)
    d.point((11, 15), fill=(16, 24, 32, 255))
    d.point((12, 15), fill=(240, 244, 248, 255))
    return im


def draw_black_bream():
    """黑鲷：深灰高身、金眼、谨慎感，少见档。"""
    im = new_fish_canvas()
    d = ImageDraw.Draw(im)
    d.polygon([(39, 16), (47, 10), (46, 23)], fill=(42, 52, 66, 255))
    d.polygon([(7, 16), (15, 8), (30, 8), (40, 16), (31, 25), (15, 25)], fill=(60, 72, 88, 255))
    d.polygon([(12, 12), (24, 6), (34, 11)], fill=(82, 96, 112, 255))
    d.polygon([(14, 22), (25, 27), (33, 22)], fill=(48, 60, 74, 255))
    d.polygon([(10, 16), (36, 16)], fill=(92, 104, 120, 255))              # 侧线
    for x in (18, 24, 30):
        d.line([(x, 11), (x - 2, 22)], fill=(44, 54, 68, 255), width=1)
    d.point((11, 14), fill=(238, 188, 84, 255))
    d.point((12, 14), fill=(20, 24, 30, 255))
    d.polygon([(7, 16), (2, 13), (2, 19)], fill=(70, 82, 96, 255))          # 唇
    return im


def draw_eel():
    """海鳗：细长蛇形、深橄榄背 + 浅腹，稀有档，力量高。"""
    im = new_fish_canvas()
    d = ImageDraw.Draw(im)
    # 身体加厚到 6~9px，缩到 48x32 后仍读得出"蛇形鱼"，不是一根草棍
    d.polygon([(1, 24), (8, 20), (16, 18), (24, 15), (32, 11), (42, 7), (47, 9),
               (45, 15), (36, 20), (28, 24), (20, 27), (12, 29), (4, 29)],
              fill=(58, 72, 62, 255))
    d.polygon([(4, 27), (12, 25), (20, 23), (28, 20), (36, 16), (43, 12),
               (36, 19), (26, 24), (16, 27), (7, 29)],
              fill=(126, 140, 116, 255))
    d.polygon([(36, 9), (40, 3), (45, 8)], fill=(78, 92, 76, 255))   # 背鳍
    d.point((42, 11), fill=(236, 196, 92, 255))                       # 金眼
    d.point((43, 11), fill=(12, 18, 16, 255))
    for x, y in ((10, 26), (18, 23), (27, 20)):
        d.point((x, y), fill=(38, 50, 42, 255))
    return im


def make_estuary_sheet():
    sheet = Image.new("RGBA", (SHEET, SHEET), MAGENTA)
    for idx, fish in enumerate((draw_mullet(), draw_black_bream(), draw_eel())):
        r, c = divmod(idx, 4)
        big = fish.resize((192, 128), Image.NEAREST)
        # 拼图会被裁掉底部 9%，鱼放在格子靠上位置，避免被裁
        x = c * CELL_W + (CELL_W - 192) // 2
        y = r * CELL_H + 120
        sheet.alpha_composite(big, (x, y))
    path = os.path.join(RAW, "estuary_sheet.png")
    sheet.convert("RGB").save(path)
    print("wrote", path)


def copy_backgrounds():
    for name in ("bg_estuary", "bg_pier"):
        src = os.path.join(UI, name + ".png")
        dst = os.path.join(RAW, name + ".png")
        shutil.copyfile(src, dst)
        print("copied", dst)
    # 标点页用预压暗图：运行时不再叠半透明全屏层（C3 单核扛不住逐像素混合）
    im = Image.open(os.path.join(RAW, "bg_estuary.png")).convert("RGB")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = px[x, y]
            px[x, y] = (int(r * 0.45), int(g * 0.50), int(b * 0.58))
    dark = os.path.join(RAW, "bg_estuary_dark.png")
    im.save(dark)
    print("wrote", dark)


def make_thumbs():
    for key in ("pond", "river", "sea", "estuary", "pier"):
        im = Image.open(os.path.join(RAW, "bg_%s.png" % key)).convert("RGB")
        # 先裁成 34:32 的横条，再 NEAREST 缩到目标，保像素硬边
        target_ratio = 34 / 32
        w, h = im.size
        nw = int(h * target_ratio)
        if nw <= w:
            left = (w - nw) // 2
            im = im.crop((left, 0, left + nw, h))
        else:
            nh = int(w / target_ratio)
            top = (h - nh) // 3
            im = im.crop((0, top, w, top + nh))
        im = im.resize((34, 32), Image.NEAREST)
        path = os.path.join(RAW, "thumb_%s.png" % key)
        im.save(path)
        print("wrote", path)


if __name__ == "__main__":
    os.makedirs(RAW, exist_ok=True)
    make_estuary_sheet()
    copy_backgrounds()
    make_thumbs()
