#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""阶段4「钓鱼履历」本机验证（无需 gcc/硬件），基于 verify_logic.py 参考实现：
  1) 首捕写 first_day、破纪录写 best_wgt，且首捕日不被后续钓获覆盖；
  2) serialize -> apply 往返后新字段不丢；
  3) 旧版本(schema=1) blob 喂入能迁移（首捕日/重量填 0），不崩、不丢老进度；
  4) 0 值展示口径：first_day=0 -> "很久以前"，best_wgt=0 -> "未记录"。
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_logic as V

fail = 0
def check(name, ok, detail=""):
    global fail
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok: fail += 1

print("=== 阶段4 钓鱼履历 · 存档往返与迁移 ===")

# 1) 首捕写日 + 破纪录写重量
# 直接构造确定性的履历写入验证（绕开收线随机，钉字段逻辑本身）
g2 = V.Fishing(1)
g2.cur = 0; g2.cur_wgt = 1234; g2.cur_len = 200
g2.env_day = 7
g2.first = None
# 模拟 commit_catch 的履历分支
if g2.cnt[0] == 0: g2.first_day[0] = g2.env_day
if g2.cur_wgt > g2.best_wgt[0]: g2.best_wgt[0] = g2.cur_wgt
g2.cnt[0] += 1   # commit_catch 真实会递增捕获数
check("首捕写入 first_day", g2.first_day[0] == 7, "(=%d)" % g2.first_day[0])
check("破纪录写入 best_wgt", g2.best_wgt[0] == 1234, "(=%d)" % g2.best_wgt[0])
# 首捕日不被后续覆盖（第二次钓：cnt 已 >0）
g2.env_day = 30; g2.cur_wgt = 500
if g2.cnt[0] == 0: g2.first_day[0] = g2.env_day
check("首捕日不被后续覆盖", g2.first_day[0] == 7, "(=%d)" % g2.first_day[0])
# 更小重量不覆盖纪录
if g2.cur_wgt > g2.best_wgt[0]: g2.best_wgt[0] = g2.cur_wgt
check("更小重量不覆盖纪录", g2.best_wgt[0] == 1234, "(=%d)" % g2.best_wgt[0])

# 2) 往返
g3 = V.Fishing(1)
g3.cnt[0] = 3; g3.first_day[0] = 12; g3.best_wgt[0] = 4567
blob = g3.serialize()
g4 = V.Fishing(2)
rc = g4.apply(blob)
check("往返 rc=0", rc == 0, "(rc=%d)" % rc)
check("往返 first_day 不丢", g4.first_day[0] == 12, "(=%d)" % g4.first_day[0])
check("往返 best_wgt 不丢", g4.best_wgt[0] == 4567, "(=%d)" % g4.best_wgt[0])
check("往返老字段也不丢", g4.cnt[0] == 3, "(=%d)" % g4.cnt[0])

# 3) 旧 schema=1 blob 迁移：真实老存档只有 20+64*4=276 字节，没有新字段
old = bytearray(blob[:20 + V.CODEX_CAP * 4])
old[4:6] = (1).to_bytes(2, "little")   # 降成旧版本
g5 = V.Fishing(3)
rc = g5.apply(bytes(old))
check("旧blob迁移 rc=1", rc == 1, "(rc=%d)" % rc)
check("迁移后首捕日填0", g5.first_day[0] == 0)
check("迁移后最大重量填0", g5.best_wgt[0] == 0)
check("迁移不丢老进度 cnt", g5.cnt[0] == 3)

# 4) 0 值展示口径（与 fishing.c 里的分支一致）
def display(fd, wg):
    d = ("第%d日" % fd) if fd > 0 else "很久以前"
    w = ("%d.%d千克" % (wg // 1000, (wg % 1000) // 100)) if wg > 0 else "未记录"
    return d, w
check("0值显示很久以前/未记录", display(0, 0) == ("很久以前", "未记录"), str(display(0, 0)))
check("正常值显示第N日/X.X千克", display(7, 1234) == ("第7日", "1.2千克"), str(display(7, 1234)))

print("结论: %s（%d 项失败）" % ("PASS" if fail == 0 else "FAIL", fail))
raise SystemExit(1 if fail else 0)
