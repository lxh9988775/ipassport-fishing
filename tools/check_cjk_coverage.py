#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_cjk_coverage.py - 端到端校验：屏上文案的每个字，子集字库里是否都有字形。

做法（不依赖任何手工维护的清单，改完代码直接跑就行）：
  1. 从 main/fishing.c 抽掉注释后，取所有字符串字面量里的**非 ASCII 字符**；
  2. 从 assets/fonts/fishing_cjk_16.c 抽出真正生成的字形（bitmap 注释 `/* U+XXXX "字" */`）；
  3. 差集非空 = 屏上会出现方框，直接 FAIL 并列出缺字与所在文案。

为什么需要它：LVGL 不会因为缺字而报错，只会在屏上画个方块。
拿真机/模拟器才能发现，成本高；本地几秒钟即可拦住。

用法：
    python tools/check_cjk_coverage.py
返回 0 = 全覆盖；1 = 有缺字（需重跑 tools/gen_font.py 补字）。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_C = os.path.join(ROOT, "main", "fishing.c")
# v2：鱼名/简介放在纯逻辑层，缺字会让图鉴页出方框，必须一并校验
SRC_LOGIC = os.path.join(ROOT, "main", "fishing_logic.c")
# 电子宠物应用（与钓鱼共用同一份子集字库）的屏上文案也要校验，否则宠物的中文会出方框
SRC_PET = os.path.join(ROOT, "main", "pet.c")
SRC_PET_LOGIC = os.path.join(ROOT, "main", "pet_logic.c")
SRC_MAIN = os.path.join(ROOT, "main", "main.c")
SRC_FILES = [SRC_C, SRC_LOGIC, SRC_PET, SRC_PET_LOGIC, SRC_MAIN]
FONT_C = os.path.join(ROOT, "assets", "fonts", "fishing_cjk_16.c")


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def needed_chars():
    lits = []
    for p in SRC_FILES:
        with open(p, "r", encoding="utf-8") as f:
            src = strip_comments(f.read())
        lits += re.findall(r'"((?:[^"\\]|\\.)*)"', src)
    need = {}
    for lit in lits:
        for ch in lit:
            if ord(ch) > 0x7F:
                need.setdefault(ch, set()).add(lit)
    return need, lits


def font_chars():
    with open(FONT_C, "r", encoding="utf-8", errors="replace") as f:
        body = f.read()
    have = {chr(int(m, 16)) for m in re.findall(r"/\* U\+([0-9A-Fa-f]+) ", body)}
    have |= {chr(c) for c in range(0x20, 0x7F)}   # 可打印 ASCII
    return have


def width_check():
    """文案宽度护栏：单行超过屏宽 240px 就会被裁掉，这类问题编译期发现不了。

    注意：只对「不换行」的 UI 文案做校验 —— 也就是 main/fishing.c 里的 HUD/提示串。
    逻辑层的鱼种简介在图鉴详情页是 WRAP 换行显示的，单行超宽属于正常行为，
    若一并检查会误报一大堆。
    """
    otf = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
    if not os.path.exists(otf):
        print("\n[跳过] 宽度检查：本地没有源字体 assets/fonts/SourceHanSansCN-Normal.otf")
        return 0
    try:
        from PIL import ImageFont
    except ImportError:
        print("\n[跳过] 宽度检查：未安装 Pillow")
        return 0

    SCREEN_W = 240          # 屏宽
    MAX_W = SCREEN_W - 8    # 两侧各留 4px
    font = ImageFont.truetype(otf, 16)
    with open(SRC_C, "r", encoding="utf-8") as f:
        ui_src = strip_comments(f.read())
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', ui_src)
    over = []
    for lit in lits:
        probe = lit.replace("\\n", "\n").replace("%d", "0").replace("%s", "X").replace("%%", "%")
        widest = max((font.getlength(line) for line in probe.split("\n")), default=0)
        if widest > MAX_W:
            over.append((widest, lit))

    print("\n宽度检查（16px 字号，上限 %dpx / 屏宽 %dpx）" % (MAX_W, SCREEN_W))
    if over:
        print("[FAIL] 以下文案超出屏宽，会被裁掉：")
        for w, lit in sorted(over, reverse=True):
            print("  %.1fpx  %s" % (w, lit))
        return 1
    print("[PASS] 所有文案单行宽度均在屏宽内。")
    return 0


def main():
    if not os.path.exists(SRC_C) or not os.path.exists(FONT_C):
        print("[FAIL] 找不到 %s 或 %s" % (SRC_C, FONT_C))
        return 2

    need, lits = needed_chars()
    have = font_chars()
    missing = {c: v for c, v in need.items() if c not in have}

    print("源文案中的非 ASCII 字符：%d 个" % len(need))
    print("子集字库覆盖字形：%d 个（含 ASCII）" % len(have))

    rc = 0
    if missing:
        print("\n[FAIL] 以下字符字库里没有，屏上会显示方框：")
        for ch, lits_ in sorted(missing.items()):
            print("  %s  U+%04X   出现在：%s" % (ch, ord(ch), " / ".join(sorted(lits_))))
        print("\n修法：把缺字补进屏上文案，再跑 python tools/gen_font.py 重新生成字库")
        rc = 1
    else:
        print("\n[PASS] 屏上所有中文文案，子集字库均覆盖，无缺字。")

    if width_check() != 0:
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
