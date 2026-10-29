#!/usr/bin/env python3
"""把四阶主角精灵从高清源重烤到目标尺寸(28x36)，真透明(RGB565A8)。
- t0: sp_hero_k.png (干净 RGBA 高清源) 直接 LANCZOS 降采样。
- t1/t2/t3: generated-images 里的 AI 高清单图(RGB 带均匀灰底) -> 抠底 -> 裁 bbox -> 等比缩进 96x123 居中。
同时把出战伙伴 sp_mush 适度放大平滑(无高清源，仅 LANCZOS 上采样)。
运行期 mage_draw_bg_ret / mage_draw_sprite_ret 禁止缩放，故精灵必须等于显示尺寸。
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "mage-game", "assets")
GEN = os.path.join(ROOT, "generated-images")
TARGET_W, TARGET_H = 28, 36  # 主角显示尺寸 (脚底中心锚点)，≈图2参考比例：怪物(史莱姆32/哥布林40/BOSS64)大小不变，主角缩到≈普通怪


LO, HI = 50, 100  # 抠底阈值：距离背景 <LO 透明，>HI 不透明，中间羽化


def _component_cleanup(alpha_arr, padding=15):
    """连通域清理：保留最大连通块（角色本体）；面积达到最大块 10% 的大块（含被透明
    缝隙分裂出的上半身/法杖等）一律保留；仅丢弃「又小又远离主体」的孤立残影
    （如 AI 灰底水印文字、抠底噪点）。纯 numpy/Python 两遍标记 + 并查集，不依赖 scipy。"""
    import numpy as np
    H, W = alpha_arr.shape
    mask = (alpha_arr > 20)
    if not mask.any():
        return alpha_arr
    labels = np.zeros((H, W), dtype=np.int32)
    parent = {}

    def find(x):
        root = x
        while parent[root] != root:
            root = parent[root]
        while parent[x] != root:
            parent[x], x = root, parent[x]
        return root

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    nxt = 1
    m = mask
    for y in range(H):
        for x in range(W):
            if not m[y, x]:
                continue
            nb = []
            if y > 0 and m[y - 1, x]:
                nb.append(int(labels[y - 1, x]))
            if x > 0 and m[y, x - 1]:
                nb.append(int(labels[y, x - 1]))
            if not nb:
                labels[y, x] = nxt
                parent[nxt] = nxt
                nxt += 1
            else:
                mm = min(nb)
                labels[y, x] = mm
                for v in nb:
                    if v != mm:
                        union(v, mm)
    # 第二遍：解析到根标签
    for y in range(H):
        for x in range(W):
            v = labels[y, x]
            if v:
                labels[y, x] = find(v)
    uniq, counts = np.unique(labels[labels > 0], return_counts=True)
    areas = {int(u): int(c) for u, c in zip(uniq, counts)}
    largest = int(uniq[int(np.argmax(counts))])
    la = areas[largest]
    ys, xs = np.where(labels == largest)
    minx, maxx = xs.min() - padding, xs.max() + padding
    miny, maxy = ys.min() - padding, ys.max() + padding
    floor = max(80, la // 10)
    keep = np.zeros_like(labels, dtype=bool)
    for c in uniq:
        a = areas[int(c)]
        cy, cx = np.where(labels == c)
        in_box = (cx.min() >= minx and cx.max() <= maxx and
                  cy.min() >= miny and cy.max() <= maxy)
        if a >= floor or in_box:
            keep |= (labels == c)  # 大块（含最大块/分裂大块/远处法杖）或贴身小块 -> 保留
        # else: 小块且远离主体 -> 丢弃（水印/噪点）
    alpha_arr[~keep] = 0
    return alpha_arr


def fit_from_alpha(src_path, out_path):
    """t0 专用：源是干净 RGBA（透明背景已是正确 alpha），直接用其 alpha 抠图，
    绝不对其做灰底 key-out（否则会把暗色袍子/头发/阴影误判为背景而切碎角色）。"""
    im = Image.open(src_path).convert("RGBA")
    w, h = im.size
    import numpy as np
    a = np.array(im.split()[3], np.uint8)
    a = _component_cleanup(a)  # 仅去极小噪点，角色整块保留
    alpha = Image.fromarray(a, "L")
    bbox = alpha.getbbox()
    if bbox is None:
        print("WARN alpha 全空 %s" % src_path)
        return
    pad = 2
    l, t, r, b = bbox
    l = max(0, l - pad); t = max(0, t - pad)
    r = min(w, r + pad); b = min(h, b + pad)
    char = im.crop((l, t, r, b))
    cw, ch = char.size
    scale = TARGET_H / ch
    if cw * scale > TARGET_W:
        scale = TARGET_W / cw
    nw, nh = max(1, round(cw * scale)), max(1, round(ch * scale))
    char = char.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (TARGET_W, TARGET_H), (0, 0, 0, 0))
    ox, oy = (TARGET_W - nw) // 2, (TARGET_H - nh) // 2
    canvas.paste(char, (ox, oy), char)
    canvas.save(out_path)
    print("OK(alpha) %s -> %s (%dx%d, 内容 %dx%d @ %d,%d)" %
          (os.path.basename(src_path), os.path.basename(out_path), TARGET_W, TARGET_H, nw, nh, ox, oy))


def keyout_and_fit(src_path, out_path):
    im = Image.open(src_path).convert("RGB")
    w, h = im.size
    # 采样背景色：避开右下角（AI 水印常驻区会污染背景均值），取其余干净点
    corners = [im.getpixel((3, 3)), im.getpixel((w - 4, 3)), im.getpixel((3, h - 4)),
               im.getpixel((w // 2, 3)), im.getpixel((w // 4, h // 4)),
               im.getpixel((3 * w // 4, h // 4))]
    bg = tuple(sum(c[i] for c in corners) // len(corners) for i in range(3))
    px = im.load()
    import numpy as np
    alpha_arr = np.zeros((h, w), dtype=np.uint8)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            d = ((r - bg[0]) ** 2 + (g - bg[1]) ** 2 + (b - bg[2]) ** 2) ** 0.5
            if d < LO:
                a = 0
            elif d > HI:
                a = 255
            else:
                a = int((d - LO) / (HI - LO) * 255)
            alpha_arr[y, x] = a
    # 连通域清理：去掉脱离角色的孤立残影（水印文字等）
    alpha_arr = _component_cleanup(alpha_arr)
    alpha = Image.fromarray(alpha_arr, "L")
    # 裁 bbox
    bbox = alpha.getbbox()
    if bbox is None:
        print("WARN 抠底失败(全透明) %s" % src_path)
        return
    pad = 2
    l, t, r, b = bbox
    l = max(0, l - pad); t = max(0, t - pad)
    r = min(w, r + pad); b = min(h, b + pad)
    char = im.crop((l, t, r, b)).convert("RGBA")
    ca = alpha.crop((l, t, r, b))
    char.putalpha(ca)
    cw, ch = char.size
    # 等比缩到高=123；若宽>96 则改缩到宽=96
    scale = TARGET_H / ch
    if cw * scale > TARGET_W:
        scale = TARGET_W / cw
    nw, nh = max(1, round(cw * scale)), max(1, round(ch * scale))
    char = char.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (TARGET_W, TARGET_H), (0, 0, 0, 0))
    ox, oy = (TARGET_W - nw) // 2, (TARGET_H - nh) // 2
    canvas.paste(char, (ox, oy), char)
    canvas.save(out_path)
    print("OK %s -> %s (%dx%d, 内容 %dx%d @ %d,%d)" %
          (os.path.basename(src_path), os.path.basename(out_path), TARGET_W, TARGET_H, nw, nh, ox, oy))


def resize_existing(src_path, out_path, tw, th):
    """已有低清精灵：LANCZOS 放大到目标尺寸(内容等比居中)。用于出战伙伴等无高清源的情况。"""
    im = Image.open(src_path).convert("RGBA")
    w, h = im.size
    scale = min(tw / w, th / h)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    canvas.paste(im, ((tw - nw) // 2, (th - nh) // 2), im)
    canvas.save(out_path)
    print("OK(enlarge) %s -> %s (%dx%d)" % (os.path.basename(src_path), os.path.basename(out_path), tw, th))


def main():
    # t0: 干净 RGBA 高清源 -> 直接用其 alpha（绝不能 key-out，否则暗色部位被切碎）
    fit_from_alpha(os.path.join(SRC, "sp_hero_k.png"), os.path.join(SRC, "sp_hero_t0.png"))
    # t1/t2/t3: AI 高清单图(带灰底)
    keyout_and_fit(os.path.join(GEN, "A_single_centered_full_body_ch_2026-10-28T01-18-25.png"),
                   os.path.join(SRC, "sp_hero_t1.png"))
    keyout_and_fit(os.path.join(GEN, "A_single_centered_full_body_ch_2026-10-28T01-18-41.png"),
                   os.path.join(SRC, "sp_hero_t2.png"))
    keyout_and_fit(os.path.join(GEN, "A_single_centered_full_body_ch_2026-10-28T01-18-50.png"),
                   os.path.join(SRC, "sp_hero_t3.png"))
    # 出战伙伴 mush: 无高清源，从当前 48x50 LANCZOS 缩到 24x25，
    # 保持「主角是大哥、伙伴是小弟」的层级（主角 28x36，伙伴约为其 0.67 倍，对齐图2）
    resize_existing(os.path.join(SRC, "sp_mush.png"), os.path.join(SRC, "sp_mush.png"), 24, 25)


if __name__ == "__main__":
    main()
