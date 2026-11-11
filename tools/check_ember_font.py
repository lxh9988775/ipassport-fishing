#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_ember_font.py - 《余烬》端到端字库覆盖校验（手册 X-08 的验收工具）。

钓鱼线的 check_cjk_coverage.py 硬编码了 fishing.c，不能用在《余烬》上，
所以这里单独一份：从固件源码抽屏上文案的非 ASCII 字符，对比字库里真正
生成的字形（bitmap 注释 /* U+XXXX "字" */），差集非空即 FAIL。

LVGL 缺字不会报错，只会在屏上画方块，真机才发现成本太高，本地几秒拦住。

用法： python tools/check_ember_font.py      返回 0 = 全覆盖
改完中文文案重跑流程：
    python tools/gen_ember_font.py && python tools/check_ember_font.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "assets", "fonts", "ember_cjk_16.c")
SRC_FILES = ["main/ember.c", "main/ember_color.c",
             "main/ember_color.h", "main/ember_main.c"]
# 键提示的三键符号，必须逐个确认有字形
REQUIRED = "　◁〇▷"


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def main():
    used = set()
    for rel in SRC_FILES:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            print("[FAIL] 找不到 %s" % p)
            return 2
        with open(p, "r", encoding="utf-8") as f:
            src = strip_comments(f.read())
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in lit:
                if ord(ch) > 0x7F:
                    used.add(ch)

    with open(FONT, "r", encoding="utf-8", errors="replace") as f:
        font = f.read()
    have = set(re.findall(r'/\* U\+[0-9A-Fa-f]+ "(.)" \*/', font))

    missing = sorted(used - have)
    print("源码用字: %d | 字库字形: %d | 缺失: %d"
          % (len(used), len(have), len(missing)))
    if missing:
        print("缺字: %s" % "".join(missing))
        for ch in missing:
            for rel in SRC_FILES:
                with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
                    for n, line in enumerate(f, 1):
                        if ch in line:
                            print("  -> %s:%d  %s" % (rel, n, line.strip()[:90]))
                            break

    for sym in REQUIRED:
        ok = sym in have
        print("  U+%04X %s : %s" % (ord(sym), repr(sym), "✅" if ok else "❌"))
        if not ok:
            missing.append(sym)

    ok = not missing
    print("RESULT: %s" % ("PASS ✅ 0 缺字" if ok else "FAIL ❌ 需重跑 gen_ember_font.py"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
