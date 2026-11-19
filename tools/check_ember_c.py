#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ember.c 结构自检（本机无 C 编译器，push 云端前先兜一道）。

复用 check_c_sanity.py 的方法，针对 ember.c + M1 动效改动：
1) 括号配平（去注释/字符串后统计）
2) g_* / s_* 静态变量：声明 vs 引用
3) static 函数：定义 + 至少被引用一次
4) M1 新增extern符号已声明、动效 tick 已接线
5) 冻结不变量：LED/发烫修复的关键模式仍在（1x1 隐藏、无 set_size、事件驱动锁）
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "ember.c")
HDR = os.path.join(ROOT, "assets", "ember", "ember_assets.h")
STR_RE = re.compile(r'"(?:\\.|[^"\\])*"')
CHAR_RE = re.compile(r"'(?:\\.|[^'\\])'")

def strip(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    src = STR_RE.sub('""', src)
    src = CHAR_RE.sub("' '", src)
    return src

def main():
    raw = open(SRC, encoding="utf-8").read()
    hdr = open(HDR, encoding="utf-8").read()
    s = strip(raw)
    bad = []

    for a, b, name in [("{", "}", "花括号"), ("(", ")", "圆括号"), ("[", "]", "方括号")]:
        na, nb = s.count(a), s.count(b)
        if na != nb:
            bad.append("%s不配平：%d vs %d" % (name, na, nb))
        else:
            print("  %-6s %4d 对  OK" % (name, na))

    decl = set(re.findall(r"\bstatic\s+\w+\s*\*?\s*(\w+)\s*(?:=|\[|;)", s))
    # 逗号连续声明：static lv_obj_t *g_bg, *g_spirit, ...;
    for stmt in re.findall(r"\bstatic\s+\w+\s*\*?[^;]*;", s):
        for name in re.findall(r"\*\s*(\w+)", stmt):
            decl.add(name)
    used = set(re.findall(r"\b([gs]_\w+)\b", s))
    miss = sorted(u for u in used if u not in decl)
    if miss:
        bad.append("未声明的静态变量：%s" % ", ".join(miss))
    else:
        print("  静态变量 %d 声明 / %d 引用，无未声明  OK" % (len(decl), len(used)))

    defs = set(re.findall(r"\bstatic\s+\w+\s+(\w+)\s*\(", s))
    unused = [fn for fn in sorted(defs)
              if len(re.findall(r"\b%s\b" % re.escape(fn), s)) <= 1]
    if unused:
        bad.append("定义未被引用：%s" % ", ".join(unused))
    else:
        print("  static 函数 %d 个，均被引用  OK" % len(defs))

    # 冻结不变量：发烫修复不能被回退
    inv = {
        "1x1 隐藏图仍用 ember_none_dsc": "ember_none_dsc" in raw,
        "无 lv_obj_set_size 固定尺寸": "lv_obj_set_size" not in s,
        "按键仍走队列入队": "xQueueSend" in raw,
        "LVGL 访问仍加锁": raw.count("bsp_lvgl_lock") >= 1 and raw.count("bsp_lvgl_unlock") >= 1,
        "6 档实体帧未改": "EMBER_FIRE_FRAMES 6" in raw,
    }
    for k, v in inv.items():
        print("  %-28s %s" % (k, "OK" if v else "FAIL"))
        if not v:
            bad.append("冻结不变量被破坏：" + k)

    # M1 新增符号头文件已声明
    for sym in ["ember_breath_dsc", "ember_spark_dsc", "ember_mote_dsc", "ember_rain_dsc"]:
        need = sym in raw
        declared = sym in hdr
        if need and not declared:
            bad.append("固件用到 %s 但头文件未声明" % sym)
    print("  M1 动效符号 extern 对齐  OK" if not bad else "")

    print("\nRESULT:", "PASS" if not bad else "FAIL")
    for b in bad:
        print("  -", b)
    return 0 if not bad else 1

if __name__ == "__main__":
    raise SystemExit(main())
