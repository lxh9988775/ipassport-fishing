#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_sprites.py — 把 ImageGen 产出的像素原图（品红背景拼图）转成 LVGL C 数组 sprite。

处理链：
  裁掉水印边 → 品红抠底(硬边二值化) → 按网格切格 → 取每格主体 bbox →
  裁边→等比缩放(NEAREST)居中到目标画布 → 量化到 <=N 色 → 导出 C 数组 →
  程序化生成"未捕剪影" → 产出预览 PNG 与 preview.html。

用法:
  python tools/gen_sprites.py [--raw assets/raw] [--out assets/sprites]

参数化：新增一条鱼/道具，只需在下面的 FISH / PROPS / BACKGROUNDS 清单加一行，
重跑本脚本即可（sprite 与索引自动扩，无需手工改 C 数组）。
"""
import os
import sys
import base64
import argparse
from io import BytesIO

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ----------------------------------------------------------------------------
# 资产清单（改这里即可扩内容）
# ----------------------------------------------------------------------------
FISH_W, FISH_H = 48, 32          # 游戏内/图鉴 master 尺寸（图鉴用整数 2× 放大）
QUANT_COLORS = 24                # 调色板量化上限（保真又不失像素质感）

# 鱼：三张栖息地拼图，每张 4 列 × 2 行，行优先
FISH_SHEETS = {
    "pond": [
        ("baitiao",  "白条"), ("jiyu",     "鲫鱼"), ("luofei", "罗非鱼"), ("lianyu", "鲢鱼"),
        ("bianyu",   "鳊鱼"), ("caoyu",    "草鱼"), ("liyu",   "鲤鱼"),   ("qingyu", "青鱼"),
    ],
    "river": [
        ("makou",     "马口鱼"), ("huangsang", "黄颡鱼"), ("qiaozui", "翘嘴"), ("luyu",  "鲈鱼"),
        ("guiyu",     "鳜鱼"),   ("nianyu",    "鲶鱼"),   ("junyu",   "军鱼"), ("ganyu", "鳡鱼"),
    ],
    "sea": [
        ("xiaohuang", "小黄鱼"), ("daiyu",   "带鱼"),   ("bayu",   "鲅鱼"),   ("shiban",  "石斑鱼"),
        ("bimu",      "比目鱼"), ("jinqiang","金枪鱼"), ("qiyu",   "旗鱼"),   ("xiaosha", "小鲨鱼"),
    ],
}

# 道具/饵/竿/图标：一张 4×4 拼图，行优先
# 字段：(key, 中文, 行, 列, 横向跨格数, 输出宽, 输出高, 分类[, 旋转角度°])
#   —— 海竿那张图很宽，横跨了 (1,1)(1,2) 两格，所以要 colspan=2；
#      浮漂在 (1,3)，(1,2) 是空的（之前错填成 (1,2)，导致整列错位：
#      浮漂变成空图、鱼钩拿到浮漂、图鉴拿到水波……）
#   —— 竿/漂/饵的输出尺寸是"游戏里实际显示的大小"，不是美术原稿大小：
#      原稿只有 12~24px，直接摆到 240×320 的钓场里像一粒沙。这里先 NEAREST
#      放大再旋转，旋转放在放大之后（先生成 3× 方块再转，边缘是 3px 台阶，
#      符合像素风；反过来的话旋转锯齿会被放大 3 倍）。
PROPS_SHEET = "props_sheet.png"
PROPS = [
    ("bait_worm",     "蚯蚓",   0, 0, 1, 32, 32, "bait"),
    ("bait_dough",    "面团",   0, 1, 1, 32, 32, "bait"),
    ("bait_spinner",  "亮片",   0, 2, 1, 32, 32, "bait"),
    ("rod_hand",      "手竿",   0, 3, 1, 76, 76, "rod", 40),
    ("rod_lure",      "路亚竿", 1, 0, 1, 76, 76, "rod", 40),
    ("rod_sea",       "海竿",   1, 1, 2, 76, 76, "rod", 40),
    ("prop_float",    "浮漂",   1, 3, 1, 24, 32, "prop"),
    ("prop_hook",     "鱼钩",   2, 0, 1, 12, 12, "prop"),
    ("prop_bubble",   "气泡",   2, 1, 1,  8,  8, "prop"),
    ("prop_wave",     "水波",   2, 2, 1, 24,  4, "prop"),
    ("icon_codex",    "图鉴",   2, 3, 1, 16, 16, "icon"),
    ("icon_star",     "稀有",   3, 0, 1,  8,  8, "icon"),
    ("icon_perfect",  "完美",   3, 1, 1, 12, 12, "icon"),
]

# 鱼竿的卷线器原稿是"白圈 + 深色轴"，在浅蓝天空上会糊成一块发光白斑。
# 这里把竿上的近白像素压成中灰蓝（阈值, 目标色），卷线器读成金属轮且保住中间的轴。
# 只对 cat == "rod" 生效，鱼/饵/道具/背景都不动。
ROD_DARKEN = (110, (88, 98, 115))

# 场景背景：全屏 240×320（RGB565 不透明，省一半空间）
BG_W, BG_H = 240, 320
BACKGROUNDS = [
    ("bg_pond",  "静水塘", "bg_pond.png"),
    ("bg_river", "急流河", "bg_river.png"),
    ("bg_sea",   "深海",   "bg_sea.png"),
]

# ----------------------------------------------------------------------------
# 图像处理
# ----------------------------------------------------------------------------

def crop_watermark(img, bottom_frac=0.09):
    """裁掉底部水印带（ImageGen 右下角 'AI生成' 水印）。"""
    w, h = img.size
    return img.crop((0, 0, w, int(h * (1.0 - bottom_frac))))


def key_out_magenta(img, tol=95):
    """品红背景(四角取中位色) → alpha；硬边二值化。"""
    a = np.asarray(img.convert("RGBA")).astype(np.int16)
    h, w, _ = a.shape
    rgb = a[..., :3]
    patches = [rgb[0:24, 0:24], rgb[0:24, w - 24:w], rgb[h - 24:h, 0:24], rgb[h - 24:h, w - 24:w]]
    bg = np.median(np.concatenate([p.reshape(-1, 3) for p in patches]), axis=0)
    d = np.sqrt(((rgb - bg) ** 2).sum(-1))
    alpha = np.where(d < tol, 0, 255).astype(np.uint8)
    out = np.dstack([a[..., :3].astype(np.uint8), alpha])
    return Image.fromarray(out, "RGBA"), bg


def slice_cell(img, rows, cols, r, c, cspan=1):
    w, h = img.size
    cw, ch = w // cols, h // rows
    return img.crop((c * cw, r * ch, (c + cspan) * cw, (r + 1) * ch))


def blank_corner_watermark(img, x_frac=0.82, y_frac=0.90):
    """把右下角水印区涂成背景色（不改尺寸，格子仍按原始网格对齐）。

    裁掉底部一条会让每格高度变短、整列内容错位，所以这里改成"原地抹掉"。
    """
    w, h = img.size
    bg = img.convert("RGB").getpixel((3, 3))
    img = img.copy()
    box = (int(w * x_frac), int(h * y_frac), w, h)
    region = Image.new("RGBA", (box[2] - box[0], box[3] - box[1]), bg + (255,))
    img.paste(region, box[:2])
    return img


def quantize_rgba(img, colors=QUANT_COLORS):
    a = np.asarray(img)
    if a[..., 3].max() == 0:
        return img
    rgb = Image.fromarray(a[..., :3], "RGB")
    q = rgb.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.NONE).convert("RGB")
    res = np.dstack([np.asarray(q), a[..., 3]])
    return Image.fromarray(res, "RGBA")


def extract_sprite(cell, out_w, out_h, do_quant=True):
    """取单元格主体(非透明 bbox) → 等比 NEAREST 缩放 → 居中到 out_w×out_h 透明画布。"""
    a = np.asarray(cell.convert("RGBA"))
    mask = a[..., 3] > 0
    if not mask.any():
        return None
    ys, xs = np.where(mask)
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    crop = cell.crop((x0, y0, x1, y1))
    cw, ch = crop.size
    scale = min(out_w / cw, out_h / ch)
    nw, nh = max(1, int(round(cw * scale))), max(1, int(round(ch * scale)))
    resized = crop.resize((nw, nh), Image.NEAREST)
    canvas = Image.new("RGBA", (out_w, out_h), (0, 0, 0, 0))
    canvas.paste(resized, ((out_w - nw) // 2, (out_h - nh) // 2), resized)
    if do_quant:
        canvas = quantize_rgba(canvas)
    return canvas


def make_silhouette(img, color=(18, 34, 56)):
    a = np.asarray(img).copy()
    m = a[..., 3] > 0
    a[m, 0], a[m, 1], a[m, 2] = color
    a[m, 3] = 255
    return Image.fromarray(a, "RGBA")


def darken_light(img, thresh=110, tone=(88, 98, 115)):
    """把精灵里的近白像素压成中灰蓝。

    鱼竿原稿的卷线器是"白圈 + 深色轴"，摆到浅蓝天空上会糊成一块发光白斑，
    放大 3× 再旋转后更像一块渲染噪点。压暗后卷线器读成金属轮，还保住了中间
    的轴。只作用于竿（见 ROD_DARKEN），鱼/饵/道具/背景都不动。
    """
    a = np.asarray(img).copy()
    m = a[..., 3] > 0
    lum = a[..., :3].mean(-1)
    hit = m & (lum > thresh)
    a[hit, 0], a[hit, 1], a[hit, 2] = tone
    return Image.fromarray(a, "RGBA")


def fit_background(img, out_w, out_h):
    """源方图中心裁成 out 比例 → NEAREST 缩放到 out 尺寸（保持比例不拉伸）。"""
    w, h = img.size
    target_ratio = out_w / out_h
    cur = w / h
    if cur > target_ratio:      # 太宽 → 裁两边
        nw = int(h * target_ratio)
        left = (w - nw) // 2
        img = img.crop((left, 0, left + nw, h))
    else:                        # 太高 → 裁上下
        nh = int(w / target_ratio)
        top = (h - nh) // 2
        img = img.crop((0, top, w, top + nh))
    return img.convert("RGB").resize((out_w, out_h), Image.NEAREST)


# ----------------------------------------------------------------------------
# C 数组导出
# ----------------------------------------------------------------------------

def argb8888_bytes(img):
    a = np.asarray(img.convert("RGBA"))          # R,G,B,A
    bgra = a[..., [2, 1, 0, 3]]                  # LVGL 内存序 = B,G,R,A
    return bgra.tobytes()


def rgb565_bytes(img):
    a = np.asarray(img.convert("RGB")).astype(np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return v.astype("<u2").tobytes()


def fmt_bytes(data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    return "\n".join(lines)


def emit_c(path, banner, entries):
    """entries: list of (name, cf_macro, w, h, data_bytes)"""
    parts = [
        "// 本文件由 tools/gen_sprites.py 自动生成，请勿手工编辑。",
        "// %s" % banner,
        '#include "lvgl.h"',
        '#include "sprites.h"',
        "",
    ]
    for name, cf, w, h, data in entries:
        bpp = 2 if cf == "LV_COLOR_FORMAT_RGB565" else 4
        stride = w * bpp
        parts.append("static const uint8_t %s_map[] = {" % name)
        parts.append(fmt_bytes(data))
        parts.append("};")
        parts.append("")
        parts.append("const lv_image_dsc_t %s = {" % name)
        parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        parts.append("    .header.cf = %s," % cf)
        parts.append("    .header.w = %d," % w)
        parts.append("    .header.h = %d," % h)
        parts.append("    .header.stride = %d," % stride)
        parts.append("    .data_size = sizeof(%s_map)," % name)
        parts.append("    .data = %s_map," % name)
        parts.append("};")
        parts.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))
    return sum(len(e[4]) for e in entries)


def emit_header(path, externs):
    parts = [
        "// 本文件由 tools/gen_sprites.py 自动生成，请勿手工编辑。",
        "#pragma once",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for name in externs:
        parts.append("extern const lv_image_dsc_t %s;" % name)
    parts += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))


# ----------------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------------

def sheet_name_for(key):
    return {"pond": "pond_sheet.png", "river": "river_sheet.png", "sea": "sea_sheet.png"}[key]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default="assets/raw")
    ap.add_argument("--out", default="assets/sprites")
    ap.add_argument("--pngdir", default=None, help="导出单图 PNG 的目录（默认 <out>/png）")
    ap.add_argument("--font", default="assets/fonts/SourceHanSansCN-Normal.otf")
    args = ap.parse_args()

    raw = args.raw
    out = args.out
    pngdir = args.pngdir or os.path.join(out, "png")
    os.makedirs(out, exist_ok=True)
    os.makedirs(pngdir, exist_ok=True)

    fish_c, fish_hdr = [], []
    fish_pngs = []          # (key, 中文, habitat, png_path)
    sil_pngs = []
    prop_c, prop_hdr = [], []
    prop_pngs = []
    bg_c, bg_hdr = [], []
    bg_pngs = []

    # ---- 鱼 ----
    for habitat, items in FISH_SHEETS.items():
        src = os.path.join(raw, sheet_name_for(habitat))
        sheet = crop_watermark(Image.open(src).convert("RGBA"))
        sheet, bg = key_out_magenta(sheet)
        for idx, (key, zh) in enumerate(items):
            r, c = divmod(idx, 4)
            cell = slice_cell(sheet, 2, 4, r, c)
            spr = extract_sprite(cell, FISH_W, FISH_H)
            if spr is None:
                print("  !! empty cell: %s (%s r%d c%d)" % (key, habitat, r, c))
                continue
            p = os.path.join(pngdir, "fish_%s.png" % key)
            spr.save(p)
            fish_pngs.append((key, zh, habitat, p))
            fish_c.append(("fish_%s" % key, "LV_COLOR_FORMAT_ARGB8888", FISH_W, FISH_H, argb8888_bytes(spr)))
            fish_hdr.append("fish_%s" % key)
            # 剪影
            sil = make_silhouette(spr)
            ps = os.path.join(pngdir, "sil_%s.png" % key)
            sil.save(ps)
            sil_pngs.append((key, zh, habitat, ps))
            fish_c.append(("sil_%s" % key, "LV_COLOR_FORMAT_ARGB8888", FISH_W, FISH_H, argb8888_bytes(sil)))
            fish_hdr.append("sil_%s" % key)

    # ---- 道具/饵/竿/图标 ----
    # 注意：水印用"原地抹掉"而不是裁掉底部，否则 4x4 格子高度变短、整列错位。
    psrc = os.path.join(raw, PROPS_SHEET)
    psheet = blank_corner_watermark(Image.open(psrc).convert("RGBA"))
    psheet, _ = key_out_magenta(psheet)
    for entry in PROPS:
        key, zh, r, c, cspan, w, h, cat = entry[:8]
        rot = float(entry[8]) if len(entry) > 8 else 0.0
        cell = slice_cell(psheet, 4, 4, r, c, cspan)
        spr = extract_sprite(cell, w, h)
        if spr is None:
            print("  !! empty cell: %s (%s r%d c%d)" % (key, cat, r, c))
            continue
        if rot:
            # 放大之后才旋转：先有 3× 方块，再整体转，得到 3px 台阶的像素斜边
            spr = spr.rotate(rot, resample=Image.NEAREST, expand=True)
            bb = spr.getbbox()
            if bb:
                spr = spr.crop(bb)
        if cat == "rod":
            spr = darken_light(spr, ROD_DARKEN[0], ROD_DARKEN[1])
        w, h = spr.size          # 旋转后画布跟着变大，C 数组尺寸以实际为准
        p = os.path.join(pngdir, "%s.png" % key)
        spr.save(p)
        prop_pngs.append((key, zh, cat, p, w, h))
        prop_c.append((key, "LV_COLOR_FORMAT_ARGB8888", w, h, argb8888_bytes(spr)))
        prop_hdr.append(key)

    # ---- 背景 ----
    for (key, zh, fn) in BACKGROUNDS:
        img = Image.open(os.path.join(raw, fn)).convert("RGB")
        img = fit_background(img, BG_W, BG_H)
        p = os.path.join(pngdir, "%s.png" % key)
        img.save(p)
        bg_pngs.append((key, zh, p))
        bg_c.append((key, "LV_COLOR_FORMAT_RGB565", BG_W, BG_H, rgb565_bytes(img)))
        bg_hdr.append(key)

    # ---- 写 C ----
    n1 = emit_c(os.path.join(out, "fish_sprites.c"), "鱼 sprite + 未捕剪影", fish_c)
    n2 = emit_c(os.path.join(out, "prop_sprites.c"), "饵/竿/道具/图标 sprite", prop_c)
    n3 = emit_c(os.path.join(out, "bg_sprites.c"), "场景背景（RGB565）", bg_c)
    emit_header(os.path.join(out, "sprites.h"), fish_hdr + prop_hdr + bg_hdr)

    # ---- 预览 ----
    build_preview_png(os.path.join(out, "preview.png"), fish_pngs, sil_pngs, prop_pngs, bg_pngs, args.font)
    build_preview_html(os.path.join(out, "preview.html"), fish_pngs, sil_pngs, prop_pngs, bg_pngs)

    total = n1 + n2 + n3
    print("[gen_sprites] fish=%d props=%d bg=%d" % (len(fish_pngs), len(prop_pngs), len(bg_pngs)))
    print("[gen_sprites] C bytes: fish=%d prop=%d bg=%d total=%.1f KB (flash)" % (n1, n2, n3, total / 1024.0))
    print("[gen_sprites] wrote: %s/{fish_sprites.c,prop_sprites.c,bg_sprites.c,sprites.h,preview.png,preview.html}" % out)


def _font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()


def build_preview_png(path, fish, sils, props, bgs, font_path):
    cols = 4
    cell_w, cell_h = 290, 170
    rows = (len(fish) + cols - 1) // cols
    pad = 20
    W = cols * cell_w + pad * 2
    H = pad + 40 + rows * cell_h + 60 + 200 + 60 + 220 + pad
    im = Image.new("RGB", (W, H), (245, 247, 250))
    d = ImageDraw.Draw(im)
    f_title = _font(font_path, 26)
    f_lab = _font(font_path, 18)
    f_small = _font(font_path, 14)
    y = pad
    d.text((pad, y), "钓鱼 v2 · 像素 sprite 审阅（放大 4×）", fill=(20, 30, 50), font=f_title)
    y += 40
    # 鱼
    for i, (key, zh, habitat, p) in enumerate(fish):
        r, c = divmod(i, cols)
        x = pad + c * cell_w
        yy = y + r * cell_h
        img = Image.open(p).convert("RGBA").resize((48 * 4, 32 * 4), Image.NEAREST)
        bgc = Image.new("RGB", img.size, (210, 230, 240))
        bgc.paste(img, (0, 0), img)
        im.paste(bgc, (x, yy))
        d.text((x, yy + img.size[1] + 4), "%s  %s" % (zh, key), fill=(20, 30, 50), font=f_lab)
    y += rows * cell_h + 60
    d.text((pad, y), "未捕剪影", fill=(20, 30, 50), font=f_title)
    y += 36
    for i, (key, zh, habitat, p) in enumerate(sils):
        r, c = divmod(i, 8)
        x = pad + c * 120
        yy = y + r * 70
        img = Image.open(p).convert("RGBA").resize((48 * 2, 32 * 2), Image.NEAREST)
        bgc = Image.new("RGB", img.size, (235, 238, 242))
        bgc.paste(img, (0, 0), img)
        im.paste(bgc, (x, yy))
    y += ((len(sils) + 7) // 8) * 70 + 40
    d.text((pad, y), "饵 / 竿 / 道具 / 图标", fill=(20, 30, 50), font=f_title)
    y += 36
    for i, (key, zh, cat, p, w, h) in enumerate(props):
        r, c = divmod(i, 7)
        x = pad + c * 150
        yy = y + r * 90
        sc = max(1, 64 // max(w, h))
        img = Image.open(p).convert("RGBA").resize((w * sc, h * sc), Image.NEAREST)
        bgc = Image.new("RGB", img.size, (235, 238, 242))
        bgc.paste(img, (0, 0), img)
        im.paste(bgc, (x, yy))
        d.text((x, yy + img.size[1] + 4), "%s" % zh, fill=(20, 30, 50), font=f_small)
    y += 2 * 90 + 40
    d.text((pad, y), "场景背景（240×320，缩略）", fill=(20, 30, 50), font=f_title)
    y += 36
    for i, (key, zh, p) in enumerate(bgs):
        img = Image.open(p).convert("RGB").resize((160, 213), Image.NEAREST)
        im.paste(img, (pad + i * 180, y))
        d.text((pad + i * 180, y + 216), zh, fill=(20, 30, 50), font=f_lab)
    im.save(path)


def build_preview_html(path, fish, sils, props, bgs):
    def b64(p):
        with open(p, "rb") as f:
            return base64.b64encode(f.read()).decode()

    habitats = {"pond": "静水塘 POND", "river": "急流河 RIVER", "sea": "深海 SEA"}

    def fish_card(key, zh, habitat, p):
        return ('<div class="card"><div class="imgbox"><img class="px" src="data:image/png;base64,%s"></div>'
                '<div class="zh">%s</div><div class="k">%s</div></div>' % (b64(p), zh, key))

    sections = []
    for hk, hz in habitats.items():
        items = [f for f in fish if f[2] == hk]
        sections.append('<h2>%s（%d）</h2><div class="grid">%s</div>'
                        % (hz, len(items), "".join(fish_card(*f) for f in items)))
    sil_cards = "".join(
        '<div class="card"><div class="imgbox" style="background:#eef1f5"><img class="px small" src="data:image/png;base64,%s"></div>'
        '<div class="k">%s</div></div>' % (b64(p), zh) for (k, zh, h, p) in sils)
    prop_cards = "".join(
        '<div class="card"><div class="imgbox" style="background:#eef1f5"><img class="px" src="data:image/png;base64,%s"></div>'
        '<div class="zh" style="font-size:13px">%s</div><div class="k">%sx%s</div></div>' % (b64(p), zh, w, h)
        for (k, zh, cat, p, w, h) in props)
    bg_cards = "".join(
        '<div class="card"><img class="bg" src="data:image/png;base64,%s"><div class="zh">%s</div></div>' % (b64(p), zh)
        for (k, zh, p) in bgs)

    html = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>钓鱼 v2 像素 sprite 审阅</title>
<style>
 body{margin:0;background:#0f1419;color:#e8edf2;font:15px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
 .wrap{max-width:1080px;margin:0 auto;padding:24px 16px 60px}
 h1{font-size:22px;margin:0 0 4px}
 .sub{color:#8fa3b8;font-size:13px;margin-bottom:20px}
 h2{font-size:16px;margin:28px 0 12px;padding-left:10px;border-left:4px solid #38bdf8}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}
 .card{background:#182029;border:1px solid #26313d;border-radius:10px;padding:10px;text-align:center}
 .imgbox{background:#cfe3ef;border-radius:6px;padding:8px;display:flex;align-items:center;justify-content:center}
 .px{image-rendering:pixelated;image-rendering:crisp-edges;width:100%;height:auto;max-width:200px}
 .px.small{max-width:120px}
 .bg{width:100%;border-radius:6px;image-rendering:pixelated}
 .zh{font-weight:600;margin-top:8px}
 .k{color:#7d8fa3;font-size:12px}
 .note{background:#1b2531;border:1px solid #2a3a4a;border-radius:10px;padding:14px 16px;margin:20px 0;color:#b9c9d9;font-size:13px}
</style></head><body><div class="wrap">
 <h1>钓鱼 v2 · 像素 sprite 审阅</h1>
 <div class="sub">24 条鱼按真实外观像素化（朝左 / 硬边 / 透明底，放大 4× 显示）＋ 未捕剪影 ＋ 饵/竿/道具/图标 ＋ 场景背景</div>
 <div class="note">怎么用：扫一眼 24 条鱼，把<b>不像 / 不满意</b>的记下来告诉我就行（例如「带鱼太粗」「金枪鱼颜色不对」），我会逐条单独重生成替换。看清楚像素可放大浏览器。</div>
 @@SECTIONS@@
 <h2>未捕剪影（程序化生成，24）</h2><div class="grid">@@SIL@@</div>
 <h2>饵 / 竿 / 道具 / 图标</h2><div class="grid">@@PROPS@@</div>
 <h2>场景背景（240×320）</h2><div class="grid">@@BG@@</div>
</div></body></html>"""
    html = (html.replace("@@SECTIONS@@", "".join(sections))
                .replace("@@SIL@@", sil_cards)
                .replace("@@PROPS@@", prop_cards)
                .replace("@@BG@@", bg_cards))

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)


if __name__ == "__main__":
    main()
