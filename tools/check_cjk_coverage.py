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
FONT_C = os.path.join(ROOT, "assets", "fonts", "fishing_cjk_16.c")


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def needed_chars():
    with open(SRC_C, "r", encoding="utf-8") as f:
        src = strip_comments(f.read())
    need = {}
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
        for ch in lit:
            if ord(ch) > 0x7F:
                need.setdefault(ch, set()).add(lit)
    return need


def font_chars():
    with open(FONT_C, "r", encoding="utf-8", errors="replace") as f:
        body = f.read()
    have = {chr(int(m, 16)) for m in re.findall(r"/\* U\+([0-9A-Fa-f]+) ", body)}
    have |= {chr(c) for c in range(0x20, 0x7F)}   # 可打印 ASCII
    return have


def main():
    if not os.path.exists(SRC_C) or not os.path.exists(FONT_C):
        print("[FAIL] 找不到 %s 或 %s" % (SRC_C, FONT_C))
        return 2

    need = needed_chars()
    have = font_chars()
    missing = {c: v for c, v in need.items() if c not in have}

    print("源文案中的非 ASCII 字符：%d 个" % len(need))
    print("子集字库覆盖字形：%d 个（含 ASCII）" % len(have))

    if missing:
        print("\n[FAIL] 以下字符字库里没有，屏上会显示方框：")
        for ch, lits in sorted(missing.items()):
            print("  %s  U+%04X   出现在：%s" % (ch, ord(ch), " / ".join(sorted(lits))))
        print("\n修法：把缺字补进 tools/gen_font.py 的 TEXTS，再跑 python tools/gen_font.py")
        return 1

    print("\n[PASS] 屏上所有中文文案，子集字库均覆盖，无缺字。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
