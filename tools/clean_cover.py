#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
clean_cover.py - 去掉 AI 生成封面图右下角的生成水印。

水印区落在纯色水面上，所以用同行左侧的像素向右延展覆盖即可，肉眼无痕。
（比起裁剪/重生成，这种方式不改变构图。）

用法：
    python tools/clean_cover.py <输入图> <输出图>
"""
import sys
from PIL import Image


def clean(src, dst):
    im = Image.open(src).convert("RGB")
    W, H = im.size
    px = im.load()

    # 只处理画面右下角：底部 20%、右侧 28% 的区域
    SRC_X = int(W * 0.66)      # 取样列
    Y0 = int(H * 0.80)
    X0 = int(W * 0.72)

    for y in range(Y0, H):
        r = sum(px[SRC_X + d, y][0] for d in (-2, 0, 2)) // 3
        g = sum(px[SRC_X + d, y][1] for d in (-2, 0, 2)) // 3
        b = sum(px[SRC_X + d, y][2] for d in (-2, 0, 2)) // 3
        for x in range(X0, W):
            px[x, y] = (r, g, b)

    im.save(dst)
    print("size=%dx%d  saved: %s" % (W, H, dst))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    clean(sys.argv[1], sys.argv[2])
