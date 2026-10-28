#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""切片 4 套人物等级立绘 -> mage-game/assets/sp_hero_t0..t3.png (36x46 透明底，
对齐现有 sp_hero 的显示尺寸，gen_mage_sprites 的 discover_monsters 会自动烘焙)。

等级映射（与 hero_tiers 概念一致，对齐 RANKS 职级）：
  t0 见习学徒   rank 0-1
  t1 魔法师     rank 2-3
  t2 大魔法师   rank 4-5
  t3 魔法帝     rank 6

【2026-10-28 修复】源图 A_single_centered_full_body_ch_*.png 是 RGB 灰底（无透明通道），
原脚本直接贴图把灰底带进了精灵，设备上显示成「英雄立绘后一大块浅色矩形（用户说的白块）」。
改用「中心主体连通域」去背：
  - 四角取样得背景参考色 bg_ref（灰底纹理波动约 ±10）。
  - 距 bg_ref 近（<TOL_LO）判为灰底，远（>TOL_HI）判为角色。
  - 从「角色种子」向四邻漫水，凡非灰底且连到主体的区域都保留；
    灰底（连到四边）被排除；右下角 AI 水印不连主体也被排除。
  - 这样灰袍（与灰底近色，但连着身体）被保留，灰底/水印被透明化。
"""
import os
from PIL import Image
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.join(ROOT, "generated-images")
DEST = os.path.join(ROOT, "mage-game", "assets")
SIZE = (36, 46)

HERO = [
    ("sp_hero_t0", "A_single_centered_full_body_ch_2026-10-28T01-17-19.png"),  # 见习学徒
    ("sp_hero_t1", "A_single_centered_full_body_ch_2026-10-28T01-18-25.png"),  # 魔法师
    ("sp_hero_t2", "A_single_centered_full_body_ch_2026-10-28T01-18-41.png"),  # 大魔法师
    ("sp_hero_t3", "A_single_centered_full_body_ch_2026-10-28T01-18-50.png"),  # 魔法帝
]

TOL_LO = 48   # <TOL_LO 判为灰底
TOL_HI = 80   # >TOL_HI 判为角色种子


def remove_bg(rgb):
    """返回去背后的 RGBA 图（背景透明）。rgb: HxWx3 uint8。"""
    h, w, _ = rgb.shape
    bg_patches = [rgb[5:20, 5:20], rgb[5:20, w-20:w-5],
                  rgb[h-20:h-5, 5:20], rgb[h-20:h-5, w-20:w-5]]
    bg_ref = np.mean([p.reshape(-1, 3).mean(0) for p in bg_patches], axis=0)
    d = np.abs(rgb.astype(int) - bg_ref).sum(2)  # 三通道绝对差之和
    def_bg = d < TOL_LO
    seeds = d > TOL_HI

    # 从角色种子向四邻漫水，凡「非灰底」且连到主体的区域都保留
    mask = seeds.copy()
    while True:
        grow = np.zeros_like(mask)
        grow[1:, :] |= mask[:-1, :]
        grow[:-1, :] |= mask[1:, :]
        grow[:, 1:] |= mask[:, :-1]
        grow[:, :-1] |= mask[:, 1:]
        grow &= ~def_bg
        if not (grow & ~mask).any():
            break
        mask |= grow

    out = np.zeros((h, w, 4), dtype=np.uint8)
    out[..., :3] = rgb
    out[..., 3] = np.where(mask, 255, 0)
    return out, (bg_ref, d.mean())


def main():
    os.makedirs(DEST, exist_ok=True)
    for name, src in HERO:
        im = np.array(Image.open(os.path.join(BASE, src)).convert("RGB"))
        rgba, info = remove_bg(im)
        # 缩放到目标尺寸（保持宽高比，居中）
        h, w, _ = rgba.shape
        scale = min(SIZE[0] / w, SIZE[1] / h)
        tw, th = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
        pim = Image.fromarray(rgba, "RGBA").resize((tw, th), Image.LANCZOS)
        canvas = Image.new("RGBA", SIZE, (0, 0, 0, 0))
        canvas.paste(pim, ((SIZE[0] - tw) // 2, (SIZE[1] - th) // 2), pim)
        out = os.path.join(DEST, name + ".png")
        canvas.save(out)
        print("saved", out, "bg_ref", np.round(info[0], 1), "avgdist", round(info[1], 1))
    print("DONE 4 hero sprites (transparent bg)")


if __name__ == "__main__":
    main()
