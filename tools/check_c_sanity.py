"""fishing.c 的轻量结构自检（本机没有 C 编译器，推云端前先兜一道）。

只做静态检查，不解析 C 语法：
1) 括号配平（去掉注释与字符串字面量后统计）
2) 所有使用到的 g_* / s_* 静态变量都有声明
3) 每个 static 函数都有定义且在文件内被调用
4) 没有残留的旧标识符（防止改一半）
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "fishing.c")

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
    s = strip(raw)
    bad = []

    for a, b, name in [("{", "}", "花括号"), ("(", ")", "圆括号"), ("[", "]", "方括号")]:
        na, nb = s.count(a), s.count(b)
        if na != nb:
            bad.append("%s不配平：%d 个 %s vs %d 个 %s" % (name, na, a, nb, b))
        else:
            print("  %-6s %4d 对  OK" % (name, na))

    # 静态变量：声明 vs 使用
    decl = set(re.findall(r"\bstatic\s+\w+\s*\*?\s*(\w+)\s*(?:=|\[|;)", s))
    used = set(re.findall(r"\b([gs]_\w+)\b", s))
    miss = sorted(u for u in used if u not in decl)
    if miss:
        bad.append("未声明的静态变量：%s" % ", ".join(miss))
    else:
        print("  静态变量 %d 个声明，%d 个引用，无未声明项  OK" % (len(decl), len(used)))

    # static 函数：定义 + 是否被调用
    # 注意：任务入口是 xTaskCreate(game_task, ...) 这种「按名字传函数指针」，
    # 所以不能只数 `fn(` 的调用形式，裸名出现即算被引用。
    defs = set(re.findall(r"\bstatic\s+\w+\s+(\w+)\s*\(", s))
    unused = []
    for fn in sorted(defs):
        n = len(re.findall(r"\b%s\b" % re.escape(fn), s))
        if n <= 1:
            unused.append(fn)
    if unused:
        bad.append("函数定义了但从未被引用（可能改漏了）：%s" % ", ".join(unused))
    else:
        print("  static 函数 %d 个，全部有引用点  OK" % len(defs))

    # 残留旧标识符（v2 HUD 改造后不应再出现的东西）
    for old in ['"电量 %d"', '"电量 --"', '"电量 %d%%"']:
        if old in raw:
            bad.append("仍有旧文案残留：%s" % old)

    if bad:
        print("\n[FAIL]")
        for b in bad:
            print("  -", b)
        return 1
    print("\n结构自检通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
