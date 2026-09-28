import os

base = os.path.join(os.path.dirname(__file__), "..", "assets", "publish")
for name in ("instructions_zh.txt", "instructions_en.txt"):
    p = os.path.join(base, name)
    t = open(p, encoding="utf-8").read()
    if name.endswith("zh.txt"):
        n = len([c for c in t if "一" <= c <= "鿿"])
        print(f"{name}: chinese chars = {n} (target 150-300)")
    else:
        print(f"{name}: words = {len(t.split())}")
