#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""submit_v2.py —— 组装并执行"竿影浮标 v2"新玩法的上架提交。

为什么单独写一个脚本：中英标题/简介里有中文标点与空格，直接在 shell 里拼参数
容易被打乱；这里用列表 argv 直接交给 publisher.py，一个字都不会错。

用法：
    python tools/submit_v2.py            # 只打印预览（不上传）
    python tools/submit_v2.py --auto     # 带 --auto 真正上传
"""
from __future__ import annotations

import os
import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUB = ROOT / "assets" / "publish" / "v2"
BASE_URL = "https://ai-passport.folotoy.cn"
REPO_URL = "https://github.com/lxh9988775/ipassport-fishing"

PUBLISHER = (
    Path.home() / ".workbuddy" / "skills" / "folotoy-ai-passport-publisher"
    / "scripts" / "publisher.py"
)


def read(name: str) -> str:
    return (PUB / name).read_text(encoding="utf-8").strip()


def build() -> list[str]:
    argv = [
        "publisher.py", "submit",
        "--kind", "firmware",
        "--title-zh", "竿影浮标 · 钓鱼图鉴",
        "--title-en", "Rod & Bobber: The Fishing Codex",
        "--description-zh", read("description_zh.txt"),
        "--description-en", read("description_en.txt"),
        "--instructions-zh-file", str(PUB / "instructions_zh.txt"),
        "--instructions-en-file", str(PUB / "instructions_en.txt"),
        "--category", "games",
        "--firmware", str(ROOT / "FoloToy-AI-Passport-full.bin"),
        "--cover", str(PUB / "fishing-cover-3x4.png"),
        "--image", str(PUB / "fishing-shot-codex-3x4.png"),
        "--image", str(PUB / "fishing-shot-scenes-3x4.png"),
        "--source-url", REPO_URL,
        "--tag", "family",
    ]
    if "--auto" in sys.argv:
        argv.append("--auto")
    return argv


def main() -> int:
    os.environ["FOLOTOY_AI_PASSPORT_URL"] = BASE_URL
    if not PUBLISHER.is_file():
        raise SystemExit("publisher.py not found: %s" % PUBLISHER)
    sys.argv = build()
    runpy.run_path(str(PUBLISHER), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
