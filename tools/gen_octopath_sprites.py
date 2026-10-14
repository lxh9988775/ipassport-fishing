#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_octopath_sprites.py
把 ImageGen 生成的「八方旅人 HD-2D 风格」怪物大图，后处理成实机可用的
真透明 sprite（RGB565A8 由 gen_mage_sprites.py 负责），并替换 mage-game/assets/sp_<type>.png。

背景三种情况都要能处理：
  A. 真透明 alpha 通道（background=transparent 生效时）
  B. 假透明：画成灰色棋盘格
  C. 纯色底（白/绿/洋红…）
策略：
  1. 若有真 alpha（>5% 像素 alpha<40）直接用；
  2. 否则取图片边缘环带的「高频色桶」，从边界做洪水填充抠底（不伤主体内部同色）；
  3. 前景只保留最大连通体（顺带丢掉右下角 AI生成 水印等小碎块）；
  4. 裁边 -> 限色 24 色（赛璐珞平涂感）-> 等比缩到目标高 -> 1px 粗黑描边。

输入：octa/ 下 sp_<type>_raw.png（由 ImageGen 结果改名而来，允许放 octa/<type>/ 子目录）
输出：覆盖 mage-game/assets/sp_<type>.png
"""
import os, argparse
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DEF = os.path.join(ROOT, "mage-game", "assets", "octa")
OUT_DEF = os.path.join(ROOT, "mage-game", "assets")
OUTLINE = (10, 8, 20)  # 暗蓝黑描边

# 每种怪目标显示高（来源：ENEMY/MORE_MOBS 的 h 字段，下限 32 保细节、上限 52 防图鉴盒溢出）
TARGET_H = {
    'goblin':40,'slime':32,'cat':46,'bat':32,'skel':40,'magma':34,'sala':40,'wolf':38,
    'frost':34,'golem':48,'ghost':40,'hornet':32,'mush':34,'boar':38,'treant':42,'frog':32,
    'deer':40,'snail':32,'fox':38,'spider':34,'kobold':38,'ghoul':42,'ooze':34,'glowfish':34,
    'stal':46,'weasel':32,'shade':38,'imp':34,'cinder':34,'lavadog':38,'obsid':48,'phoenix':40,
    'geo':34,'ashwisp':34,'slag':44,'yeti':48,'icefox':36,'peng':34,'hail':32,'mam':44,
    'owl':34,'sealf':34,'banshee':40,'rune':46,'mimic':38,'eyemon':38,'wraith':42,
    'sentinel':50,'drake':44,'sphinx':42,
}


def target_h_of(type_):
    if '_boss' in type_:
        return 64   # BOSS 大怪（战斗 2.1x 观感）
    return TARGET_H.get(type_, 44)


def has_real_alpha(im):
    a = im.getchannel("A")
    hist = a.histogram()
    return sum(hist[:40]) / (im.width * im.height) > 0.05


def border_buckets(im, ring=0.06, min_share=0.08, max_n=6):
    """边缘环带的高频色桶（/16 量化），容忍棋盘格两色/纯色底/轻微噪点。"""
    W, H = im.size
    r = max(2, int(W * ring))
    rgb = im.convert("RGB")
    px = rgb.load()
    cols = {}
    for y in range(H):
        ys = y < r or y >= H - r
        for x in range(W):
            if ys or x < r or x >= W - r:
                c = px[x, y]
                k = (c[0] // 16, c[1] // 16, c[2] // 16)
                cols[k] = cols.get(k, 0) + 1
    total = sum(cols.values())
    keep = [k for k, v in sorted(cols.items(), key=lambda kv: -kv[1])
            if v / total >= min_share][:max_n]
    return set(keep)


def magenta_key(im, thresh=90):
    """纯洋红底键控：ImageGen 画不出真透明时，指定纯色底最可靠。

    判据：洋红通道 R 高、B 高 而 G 显著低。命中即全透明，无需连通性判断，
    因此主体贴边也安全（这正是棋盘格/渐变底做不到的）。
    """
    px = im.convert("RGB").load()
    W, H = im.size
    out = im.copy()
    opx = out.load()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            if r > 150 and b > 150 and g < r - thresh and g < b - thresh:
                opx[x, y] = (r, g, b, 0)
    return out


def _near(k, buckets, tol=1):
    """k 是否落在背景桶集合内（含 tol 邻桶）。

    必须放宽到邻桶：ImageGen 常把背景画成**规则棋盘格**，深浅格子交错，
    4 连通洪水填充跨不过相邻色格子 -> 抠不掉（曾致 kobold 前景 69%，等于没抠）。
    放宽邻桶后深浅灰同属背景、彼此连通；主体内部不与图像边界连通，故不会被误吃。
    """
    if k in buckets:
        return True
    if tol <= 0:
        return False
    for b in buckets:
        if abs(k[0] - b[0]) <= tol and abs(k[1] - b[1]) <= tol and abs(k[2] - b[2]) <= tol:
            return True
    return False


def flood_bg(im, buckets, tol=1):
    """从边界洪水填充：只吃掉与边界连通的背景色，主体内部同色不受影响。"""
    W, H = im.size
    px = im.convert("RGB").load()
    bg = bytearray(W * H)
    stack = []
    for y in range(H):
        stack.append((0, y)); stack.append((W - 1, y))
    for x in range(W):
        stack.append((x, 0)); stack.append((x, H - 1))
    while stack:
        x, y = stack.pop()
        i = y * W + x
        if bg[i]:
            continue
        c = px[x, y]
        if not _near((c[0] // 16, c[1] // 16, c[2] // 16), buckets, tol):
            continue
        bg[i] = 1
        if x > 0: stack.append((x - 1, y))
        if x < W - 1: stack.append((x + 1, y))
        if y > 0: stack.append((x, y - 1))
        if y < H - 1: stack.append((x, y + 1))
    return bg


def largest_component(fg, W, H):
    """前景掩码里只留最大连通体（丢水印/碎屑）。"""
    seen = bytearray(W * H)
    best, best_size = None, 0
    for start in range(W * H):
        if fg[start] and not seen[start]:
            comp = []
            stack = [start]
            seen[start] = 1
            while stack:
                i = stack.pop()
                comp.append(i)
                x, y = i % W, i // W
                for nx, ny in ((x-1,y),(x+1,y),(x,y-1),(x,y+1)):
                    if 0 <= nx < W and 0 <= ny < H:
                        j = ny * W + nx
                        if fg[j] and not seen[j]:
                            seen[j] = 1
                            stack.append(j)
            if len(comp) > best_size:
                best, best_size = comp, len(comp)
    out = bytearray(W * H)
    if best:
        for i in best:
            out[i] = 1
    return out


def grow_bg(im, bg, buckets, rounds=4, tol=1):
    """从已抠出的背景区向外再扩张若干轮，吃掉残留的深色格子。

    棋盘格背景下，洪水填充只能吃掉与边界同色的那一层格子；另一层深色格子
    被它隔开成孤岛（kobold：浅灰吃掉后深灰仍围着主体一圈）。这里让背景
    从已有透明区向外扩散，吃掉相邻的同族背景色格子。
    """
    W, H = im.size
    px = im.convert("RGB").load()
    for _ in range(rounds):
        add = []
        for y in range(H):
            for x in range(W):
                i = y * W + x
                if bg[i]:
                    continue
                c = px[x, y]
                if not _near((c[0] // 16, c[1] // 16, c[2] // 16), buckets, tol):
                    continue
                for nx, ny in ((x-1,y),(x+1,y),(x,y-1),(x,y+1)):
                    if 0 <= nx < W and 0 <= ny < H and bg[ny * W + nx]:
                        add.append(i)
                        break
        if not add:
            break
        for i in add:
            bg[i] = 1
    return bg


def remove_bg(im):
    # 0) 洋红键控最可靠（提示词指定纯色底时走这条），命中才继续
    keyed = magenta_key(im)
    if keyed.getchannel("A").histogram()[0] > 0.05 * im.width * im.height:
        a = keyed.getchannel("A").point(lambda v: 255 if v >= 128 else 0)
        W, H = im.size
        fg = bytearray(W * H)
        apx = a.load()
        for y in range(H):
            for x in range(W):
                if apx[x, y]:
                    fg[y * W + x] = 1
        fg = largest_component(fg, W, H)
        out = keyed.copy()
        opx = out.load()
        for y in range(H):
            for x in range(W):
                r, g, b, _ = opx[x, y]
                opx[x, y] = (r, g, b, 255 if fg[y * W + x] else 0)
        return out
    if has_real_alpha(im):
        # 真透明：二值化 alpha，去掉小于最大体 5% 的碎块
        a = im.getchannel("A").point(lambda v: 255 if v >= 40 else 0)
        W, H = im.size
        fg = bytearray(W * H)
        px = a.load()
        for y in range(H):
            for x in range(W):
                if px[x, y]:
                    fg[y * W + x] = 1
        fg = largest_component(fg, W, H)
        out = im.copy()
        opx = out.load()
        for y in range(H):
            for x in range(W):
                r, g, b, _ = opx[x, y]
                opx[x, y] = (r, g, b, 255 if fg[y * W + x] else 0)
        return out
    buckets = border_buckets(im)
    W, H = im.size
    # 先用放宽邻桶（对付棋盘格背景），若前景占比异常低（主体被误吃）则退回严格模式
    bg = flood_bg(im, buckets, tol=1)
    bg = grow_bg(im, bg, buckets, rounds=4, tol=1)
    fg_ratio = sum(1 - b for b in bg) / (W * H)
    if fg_ratio < 0.40:
        bg2 = flood_bg(im, buckets, tol=0)
        r2 = sum(1 - b for b in bg2) / (W * H)
        if r2 > fg_ratio:
            bg = bg2
    fg = bytearray(1 - b for b in bg)
    fg = largest_component(fg, W, H)
    rgb = im.convert("RGB")
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rpx = rgb.load(); opx = out.load()
    for y in range(H):
        for x in range(W):
            if fg[y * W + x]:
                c = rpx[x, y]
                opx[x, y] = (c[0], c[1], c[2], 255)
    # 轻微羽化边缘
    a = out.getchannel("A").filter(ImageFilter.GaussianBlur(0.6))
    out.putalpha(a)
    return out


def trim(im):
    bbox = im.getbbox()
    return im.crop(bbox) if bbox else im


def add_outline(im, width=1):
    alpha = im.getchannel("A")
    dilated = alpha.filter(ImageFilter.MaxFilter(2 * width + 1))
    edge = Image.new("L", im.size, 0)
    epx = edge.load(); apx = alpha.load(); dpx = dilated.load()
    for y in range(im.height):
        for x in range(im.width):
            if dpx[x, y] > 10 and apx[x, y] < 10:
                epx[x, y] = 255
    rgb = im.convert("RGB")
    rpx = rgb.load()
    for y in range(im.height):
        for x in range(im.width):
            if epx[x, y] == 255:
                rpx[x, y] = OUTLINE
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def process(src, dst, target_h):
    im = Image.open(src).convert("RGBA")
    im = remove_bg(im)
    im = trim(im)
    if im.width < 2 or im.height < 2:
        print("SKIP(空)", src); return False
    w = max(1, int(round(im.width * target_h / im.height)))
    im = im.resize((w, target_h), Image.LANCZOS)
    qm = 2  # Fast Octree（RGBA 图唯一可用的快速量化法）
    im = im.quantize(colors=24, method=qm).convert("RGBA")
    im = add_outline(im, 1)
    if im.width / im.height < 0.6:
        print("WARN 比例过窄 %s -> %dx%d (ratio=%.2f)，屏幕上会看不清，请调大该type的目标高"
              % (os.path.basename(dst), im.width, im.height, im.width / im.height))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    im.save(dst)
    print("OK ", os.path.basename(dst), im.size)
    return True


def collect_raws(src_dir):
    """octa/ 及其一层子目录里找 sp_<type>_raw.png。"""
    found = []
    for root, _dirs, files in os.walk(src_dir):
        for f in files:
            if f.startswith("sp_") and f.endswith("_raw.png"):
                found.append(os.path.join(root, f))
    return sorted(found)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC_DEF)
    ap.add_argument("--out", default=OUT_DEF)
    ap.add_argument("--h", type=int, default=44)
    args = ap.parse_args()
    raws = collect_raws(args.src)
    if not raws:
        print("没有 sp_*_raw.png 源图，先跑 ImageGen 生成到", args.src)
        return
    ok = 0
    for p in raws:
        base = os.path.basename(p)
        type_ = base[len("sp_"):-len("_raw.png")]
        dst = os.path.join(args.out, "sp_%s.png" % type_)
        if process(p, dst, target_h_of(type_)):
            ok += 1
    print("完成 %d/%d -> %s" % (ok, len(raws), args.out))


if __name__ == "__main__":
    main()
