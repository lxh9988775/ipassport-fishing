#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
poll_ci.py - 查询云端 GitHub Actions 对本仓库最新提交的编译结果。

本机没装 ESP-IDF / gcc，固件能不能编译过只能靠云端 CI 说话。
用法：
    python tools/poll_ci.py            # 只看最新一次 workflow run 的结论
    python tools/poll_ci.py --wait     # 轮询到结束（默认最多 10 分钟）

查询的分支默认跟着「当前 git 分支」走（本仓有两条构建线：main=电子宠物、
fishing-app=钓鱼 play）。要指定别的分支用环境变量 POLL_BRANCH。
"""

import json
import os
import subprocess
import sys
import time
import urllib.request

REPO = "lxh9988775/ipassport-fishing"


def git_head():
    out = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True)
    return out.strip()


def git_branch():
    env = os.environ.get("POLL_BRANCH")
    if env:
        return env
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], text=True)
        name = out.strip()
        return name if name and name != "HEAD" else "main"
    except Exception:  # noqa: BLE001
        return "main"


def http_json(url):
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "ipassport-fishing-ci-poller",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def find_run(head, branch):
    data = http_json(
        f"https://api.github.com/repos/{REPO}/actions/runs?branch={branch}&per_page=20"
    )
    for run in data.get("workflow_runs", []):
        if run.get("head_sha") == head:
            return run
    return None


def main():
    wait = "--wait" in sys.argv
    head = git_head()
    branch = git_branch()
    print("本地 HEAD:", head)
    print("查询分支:", branch)
    deadline = time.time() + 600
    while True:
        try:
            run = find_run(head, branch)
        except Exception as e:
            print("查询失败:", e)
            return 2
        if not run:
            print("CI 还没为这个提交建 run，稍后再试…")
        else:
            name = run.get("name")
            status = run.get("status")
            conclusion = run.get("conclusion")
            print(f"[{name}] status={status} conclusion={conclusion}")
            print("  ", run.get("html_url"))
            if status == "completed":
                if conclusion != "success":
                    print("\n编译/校验未通过，拉取失败步骤日志：")
                    try:
                        jobs = http_json(run["jobs_url"])
                        for j in jobs.get("jobs", []):
                            if j.get("conclusion") != "success":
                                print(f"  --- job: {j.get('name')} ({j.get('conclusion')})")
                                steps = j.get("steps", [])
                                for s in steps:
                                    if s.get("conclusion") not in (None, "success", "skipped"):
                                        print(f"      step: {s.get('name')} -> {s.get('conclusion')}")
                        print("\n  完整日志请打开上面的 html_url 查看。")
                    except Exception as e:
                        print("  取 job 明细失败:", e)
                return 0 if conclusion == "success" else 1
        if not wait or time.time() > deadline:
            return 3
        print("  等待 30s 再查…")
        time.sleep(30)


if __name__ == "__main__":
    sys.exit(main())
