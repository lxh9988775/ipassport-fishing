#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_pet_sprites.py — 把兔子电子宠物的像素原图（浅米色背景，ImageGen 产出）转成
LVGL 可用的精灵图 C 数组（ARGB8888，透明底）。

处理链（对齐 tools/gen_sprites.py 的约定）：
  裁水印边 → 角点取中位色做近似背景 → 距离阈值抠透明 →
  取主体 bbox → NEAREST 等比缩放居中到目标画布 → 量化 <=N 色 → 导出 C 数组。
  另外自动生成"朝右"镜像帧（行走/朝向用），不必再画一张。

用法:
  python tools/gen_pet_sprites.py
产物:
  assets/sprites/pet_sprites.c
  assets/sprites/pet_sprites.h
  assets/sprites/pet_preview.png   （本地审阅用，不入固件）
"""
import os
import numpy as np
from PIL import Image, ImageFont, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIXEL_PET = os.path.join(ROOT, "assets", "pixel_pet")
OUT_DIR = os.path.join(ROOT, "assets", "sprites")
PREVIEW = os.path.join(OUT_DIR, "pet_preview.png")

SPR_W, SPR_H = 72, 72          # 屏上 master 尺寸（240x320 屏上约占 1/3）
QUANT_COLORS = 24              # 调色板上限（保像素质感又省空间）
KEY_TOL = 45                   # 抠图容差：略激进，配合"补洞"把内部误抠的浅色填回

# (导出名, 中文, 源文件)  —— 文件名含中文，用 glob 匹配
SOURCES = [
    ("rabbit_front", "正面", "把这张毛绒兔子玩偶照片转换成16_bit像素风格的游戏角色精_2026-10-04T13-11-10.png"),
    ("rabbit_back",  "背面", "修改这张像素风兔子背面图_两处改动_1_两只长耳朵改成背面视_2026-10-04T13-19-28.png"),
    ("rabbit_side",  "左侧", "将这只白色毛绒兔子玩偶绘制成16位像素风电子宠物角色_朝向为_2026-10-04T13-13-56.png"),
]


def find_source(fname):
    p = os.path.join(PIXEL_PET, fname)
    if os.path.exists(p):
        return p
    # 兜底：按前缀匹配（文件名可能微调）
    for f in os.listdir(PIXEL_PET):
        if f.startswith(fname.split("_2026")[0]) or fname[:-4] in f:
            return os.path.join(PIXEL_PET, f)
    raise SystemExit("找不到兔子原图: %s" % fname)


def crop_watermark(img, bottom_frac=0.09):
    w, h = img.size
    return img.crop((0, 0, w, int(h * (1.0 - bottom_frac))))


def fill_holes(alpha):
    """把不与图像边界连通的透明区填回不透明（修复兔子内部浅色被误抠）。

    用从边界出发的 BFS 标记"外部透明区"，其余透明像素全部视为内部 → 填 255。
    纯 numpy/栈实现，不依赖 scipy。
    """
    h, w = alpha.shape
    transparent = alpha == 0
    outside = np.zeros_like(transparent, dtype=bool)
    stack = []
    # 把四边上的透明像素全部作为 BFS 起点
    for x in range(w):
        if transparent[0, x] and not outside[0, x]:
            outside[0, x] = True; stack.append((0, x))
        if transparent[h - 1, x] and not outside[h - 1, x]:
            outside[h - 1, x] = True; stack.append((h - 1, x))
    for y in range(h):
        if transparent[y, 0] and not outside[y, 0]:
            outside[y, 0] = True; stack.append((y, 0))
        if transparent[y, w - 1] and not outside[y, w - 1]:
            outside[y, w - 1] = True; stack.append((y, w - 1))
    while stack:
        y, x = stack.pop()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and transparent[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                stack.append((ny, nx))
    out = alpha.copy()
    out[transparent & ~outside] = 255
    return out


def key_out_bg(img, tol=KEY_TOL):
    """角点取中位色作近似背景 → 距离阈值抠透明（硬边）+ 内部补洞。"""
    a = np.asarray(img.convert("RGBA")).astype(np.int16)
    h, w, _ = a.shape
    rgb = a[..., :3]
    patches = [rgb[0:24, 0:24], rgb[0:24, w - 24:w],
               rgb[h - 24:h, 0:24], rgb[h - 24:h, w - 24:w]]
    bg = np.median(np.concatenate([p.reshape(-1, 3) for p in patches]), axis=0)
    d = np.sqrt(((rgb - bg) ** 2).sum(-1))
    alpha = np.where(d < tol, 0, 255).astype(np.uint8)
    alpha = fill_holes(alpha)
    out = np.dstack([a[..., :3].astype(np.uint8), alpha])
    return Image.fromarray(out, "RGBA"), bg


def extract_sprite(img, out_w, out_h, do_quant=True):
    a = np.asarray(img.convert("RGBA"))
    mask = a[..., 3] > 0
    if not mask.any():
        return None
    ys, xs = np.where(mask)
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    crop = img.crop((x0, y0, x1, y1))
    cw, ch = crop.size
    scale = min(out_w / cw, out_h / ch)
    nw, nh = max(1, int(round(cw * scale))), max(1, int(round(ch * scale)))
    resized = crop.resize((nw, nh), Image.NEAREST)
    canvas = Image.new("RGBA", (out_w, out_h), (0, 0, 0, 0))
    canvas.paste(resized, ((out_w - nw) // 2, (out_h - nh) // 2), resized)
    if do_quant:
        canvas = quantize_rgba(canvas)
    return canvas


def quantize_rgba(img, colors=QUANT_COLORS):
    a = np.asarray(img)
    if a[..., 3].max() == 0:
        return img
    rgb = Image.fromarray(a[..., :3], "RGB")
    q = rgb.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.NONE).convert("RGB")
    res = np.dstack([np.asarray(q), a[..., 3]])
    return Image.fromarray(res, "RGBA")


def mirror(img):
    return img.transpose(Image.FLIP_LEFT_RIGHT)


# ---------------------------------------------------------------------------
# C 数组导出
# ---------------------------------------------------------------------------
def argb8888_bytes(img):
    a = np.asarray(img.convert("RGBA"))
    bgra = a[..., [2, 1, 0, 3]]            # LVGL 内存序 = B,G,R,A
    return bgra.tobytes()


def fmt_bytes(data, per_line=16):
    return "\n".join(
        "    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ","
        for i in range(0, len(data), per_line)
    )


def emit_c(path, entries):
    parts = [
        "// 本文件由 tools/gen_pet_sprites.py 自动生成，请勿手工编辑。",
        '#include "lvgl.h"',
        '#include "pet_sprites.h"',
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
        f.write("\n".join(parts))
    return sum(len(e[3]) for e in entries)


def emit_header(path, names):
    parts = [
        "// 本文件由 tools/gen_pet_sprites.py 自动生成，请勿手工编辑。",
        "#pragma once",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for n in names:
        parts.append("extern const lv_image_dsc_t %s;" % n)
    parts += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))


def build_preview(path, sprites):
    cols = 4
    cell_w, cell_h = 200, 200
    pad = 24
    rows = (len(sprites) + cols - 1) // cols
    W = cols * cell_w + pad * 2
    H = pad + 40 + rows * cell_h + pad
    im = Image.new("RGB", (W, H), (28, 32, 40))
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(os.path.join(ROOT, "assets/fonts/SourceHanSansCN-Normal.otf"), 20)
    except Exception:
        f = ImageFont.load_default()
    d.text((pad, pad - 10), "兔子精灵图审阅（放大显示，透明底已铺灰）", fill=(230, 235, 242), font=f)
    for i, (name, zh, img) in enumerate(sprites):
        r, c = divmod(i, cols)
        x = pad + c * cell_w
        y = pad + 40 + r * cell_h
        sc = 3
        big = img.resize((img.size[0] * sc, img.size[1] * sc), Image.NEAREST)
        bgc = Image.new("RGB", big.size, (120, 124, 132))
        bgc.paste(big, (0, 0), big)
        im.paste(bgc, (x, y))
        d.text((x, y + big.size[1] + 4), "%s  %s" % (zh, name), fill=(230, 235, 242), font=f)
    im.save(path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    entries = []
    names = []
    preview_sprites = []
    for key, zh, fname in SOURCES:
        src = find_source(fname)
        img = crop_watermark(Image.open(src).convert("RGBA"))
        img, bg = key_out_bg(img, tol=KEY_TOL)
        spr = extract_sprite(img, SPR_W, SPR_H)
        if spr is None:
            print("  !! 空图: %s" % key)
            continue
        spr.save(os.path.join(OUT_DIR, "%s.png" % key))
        entries.append((key, SPR_W, SPR_H, argb8888_bytes(spr)))
        names.append(key)
        preview_sprites.append((key, zh, spr))
        # 自动生成朝右镜像帧（命名加 _r）
        if key in ("rabbit_side",):
            rkey = key + "_r"
            rspr = mirror(spr)
            rspr.save(os.path.join(OUT_DIR, "%s.png" % rkey))
            entries.append((rkey, SPR_W, SPR_H, argb8888_bytes(rspr)))
            names.append(rkey)
            preview_sprites.append((rkey, zh + "(右)", rspr))
        print("  %-14s %s" % (key, zh))

    n = emit_c(os.path.join(OUT_DIR, "pet_sprites.c"), entries)
    emit_header(os.path.join(OUT_DIR, "pet_sprites.h"), names)
    build_preview(PREVIEW, preview_sprites)
    print("[gen_pet_sprites] C bytes: %d (%.1f KB flash)" % (n, n / 1024.0))
    print("[gen_pet_sprites] wrote: assets/sprites/{pet_sprites.c,pet_sprites.h,pet_preview.png}")


if __name__ == "__main__":
    main()
