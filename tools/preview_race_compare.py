#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""赛跑对战 S3 对照图：试玩页视觉（左） vs 固件实现（右），240×320 同坐标。

试玩侧严格按 build_pet_race_playable.py drawRace() 公式重绘（含 AI 房间底图+遮罩，
那是对试玩的如实呈现；固件侧按红线 6 用不透明深底）。
固件侧严格按 main/pet.c game_init/refresh_game case 0 的坐标常量重绘。
过程脚本，不进 commit。
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
BG_ROOM = r"C:\Users\8605464\Desktop\AIpassport\build\pet_ui\ai_bg\16_bit_pixel_art_game_backgrou_2026-11-27T02-46-44.png"
RABBIT_WALK = os.path.join(ROOT, "assets", "sprites", "png", "rabbit_front_walk.png")

LEN = 120000
ROCK = [26000, 55000, 86000]
CAR = [18000, 44000, 73000]


def F(px_size):
    return ImageFont.truetype(FONT_PATH, px_size)


def px(d_mm):
    return 26 + d_mm * 188 // LEN


# ---------------- 试玩页侧（照抄 drawRace 公式） ----------------

def draw_playable(st):
    im = Image.new("RGB", (240, 320))
    d = ImageDraw.Draw(im)
    # 背景：房间图 + 42% 深色遮罩（试玩原样）
    try:
        bg = Image.open(BG_ROOM).convert("RGB").resize((240, 360)).crop((0, 0, 240, 320))
        im.paste(bg, (0, 0))
    except Exception:
        pass
    ov = Image.new("RGBA", (240, 320), (24, 32, 40, 107))
    im = Image.alpha_composite(im.convert("RGBA"), ov).convert("RGB")
    d = ImageDraw.Draw(im)
    # 顶部战况 rgba(24,32,40,.9)
    d.rectangle([0, 0, 239, 37], fill=(26, 34, 44))
    d.text((8, 5), "你 VS " + st["opp"], font=F(12), fill=(255, 243, 220))
    d.text((8, 22), "%.1fs" % st["t"], font=F(9), fill=(216, 230, 240))
    lead = st["me"] - st["rb"]
    ltxt = "并驾齐驱" if abs(lead) < 2000 else ("你领先 %dm" % (lead // 1000) if lead > 0 else "落后 %dm" % (-lead // 1000))
    lcol = (245, 215, 122) if abs(lead) < 2000 else ((123, 224, 160) if lead > 0 else (255, 154, 154))
    d.text((160, 22), ltxt, font=F(9), fill=lcol)

    def track(y, label):
        d.rectangle([16, y, 223, y + 45], fill=(107, 88, 68))
        d.rectangle([18, y + 2, 221, y + 43], fill=(140, 117, 88))
        x = 20
        while x < 220:
            d.line([(x, y + 23), (min(x + 5, 219), y + 23)], fill=(160, 139, 106))
            x += 11
        for i in range(13):
            d.line([(20 + i * 17, y + 2), (20 + i * 17, y + 43)], fill=(92, 70, 50))
        d.rectangle([20, y - 15, 65, y - 4], fill=(24, 34, 46))
        d.text((24, y - 13), label, font=F(8), fill=(255, 243, 220))
    track(84, "兔子")
    track(174, "你")
    # 石头/胡萝卜（试玩内联画法）
    for y, is_me in ((84, False), (174, True)):
        for i, od in enumerate(ROCK):
            hit = st["obs_hit"][i] if is_me else st["obs_rhit"][i]
            if hit:
                continue
            x = px(od)
            d.polygon([(x - 6, y + 42), (x - 3, y + 30), (x + 4, y + 30), (x + 7, y + 42)], fill=(78, 66, 54))
            d.rectangle([x - 3, y + 30, x + 3, y + 32], fill=(107, 90, 71))
        for i, cd in enumerate(CAR):
            got = st["car_got"][i] if is_me else st["car_rgot"][i]
            if got:
                continue
            x, cy = px(cd), y + 38
            d.polygon([(x - 4, cy - 2), (x + 4, cy - 2), (x, cy + 6)], fill=(232, 137, 60))
            d.polygon([(x - 4, cy - 2), (x - 1, cy - 7), (x, cy - 2)], fill=(109, 190, 94))
            d.polygon([(x + 4, cy - 2), (x + 1, cy - 7), (x, cy - 2)], fill=(109, 190, 94))
    # 角色
    def draw_rabbit(cx, cy, flip=True):
        try:
            rb_im = Image.open(RABBIT_WALK).convert("RGBA").resize((48, 48), Image.NEAREST)
        except Exception:
            return
        if flip:
            rb_im = rb_im.transpose(Image.FLIP_LEFT_RIGHT)
        im.paste(rb_im, (int(cx) - 24, int(cy) - 24), rb_im)
    rby = 84 + 44 - (10 if st["rjump"] else 0)
    draw_rabbit(px(st["rb"]), rby - 16)
    # 玩家跑者（试玩 drawMe 等效：32×40 剪影）
    my = 174 + 44 - (11 if st["jump"] else 0)
    yy = my - 44 - 5
    mx = int(px(st["me"]))
    rd = ImageDraw.Draw(im)
    rd.rectangle([mx - 16, yy, mx + 15, yy + 39], fill=(0, 0, 0, 0))
    rd.rectangle([mx - 7, yy + 27, mx - 2, yy + 37], fill=(92, 70, 50))
    rd.rectangle([mx + 2, yy + 27, mx + 7, yy + 37], fill=(92, 70, 50))
    rd.rectangle([mx - 9, yy + 36, mx + 0, yy + 38], fill=(47, 40, 34))
    rd.rectangle([mx + 1, yy + 36, mx + 10, yy + 38], fill=(47, 40, 34))
    rd.rectangle([mx - 10, yy + 13, mx + 9, yy + 28], fill=(91, 144, 196))
    rd.rectangle([mx - 16, yy + 15, mx - 11, yy + 24], fill=(138, 106, 66))
    rd.rectangle([mx + 10, yy + 13, mx + 15, yy + 24], fill=(91, 144, 196))
    rd.rectangle([mx - 7, yy + 8, mx + 6, yy + 19], fill=(232, 195, 158))
    rd.rectangle([mx - 9, yy + 5, mx + 8, yy + 9], fill=(179, 58, 90))
    rd.rectangle([mx - 15, yy + 9, mx - 8, yy + 11], fill=(179, 58, 90))
    # 底部冲刺条 rgba(24,32,40,.75)
    d.rectangle([0, 266, 239, 319], fill=(33, 43, 55))
    d.text((6, 271), "冲刺节奏", font=F(9), fill=(216, 230, 240))
    d.rectangle([66, 274, 175, 281], fill=(59, 74, 86))
    bw = int(110 * st["mom"])
    col = (245, 215, 122) if st["mom"] > 0.75 else ((123, 224, 160) if st["mom"] > 0.4 else (224, 162, 76))
    if bw:
        d.rectangle([66, 274, 65 + bw, 281], fill=col)
    d.text((6, 287), "← → 交替迈腿　空格跳", font=F(8), fill=(159, 180, 196))
    d.text((6, 301), "真机：上/下迈腿 · OK 跳", font=F(8), fill=(159, 180, 196))
    d.text((200, 287), "%dm" % (st["me"] // 1000), font=F(8), fill=(255, 243, 220))
    d.text((176, 301), "%s %dm" % (st["opp"], st["rb"] // 1000), font=F(8), fill=(216, 230, 240))
    # 倒计时 / 闪字 / 结算
    if st["count"] > 0:
        d.rectangle([80, 116, 159, 203], fill=(28, 36, 46))
        txt = str(math.ceil(st["count"] - 0.2)) if st["count"] > 0.6 else "GO!"
        tb = d.textbbox((0, 0), txt, font=F(40))
        d.text(((240 - (tb[2] - tb[0])) // 2, 130), txt, font=F(40), fill=(245, 215, 122))
    if st["flash"]:
        d.text((90, 148), st["flash"], font=F(13), fill=(255, 154, 154) if "石头" in st["flash"] else (245, 215, 122))
    if st["over"]:
        d.rectangle([20, 92, 219, 239], fill=(24, 30, 38))
        d.text((120 - 44, 116), "你 赢 了！" if st["win"] else "兔 子 赢 了", font=F(22),
               fill=(245, 215, 122) if st["win"] else (255, 154, 154))
        d.text((52, 148), "你 %dm　%s %dm" % (st["me"] // 1000, st["opp"], st["rb"] // 1000), font=F(11), fill=(255, 243, 220))
        d.text((52, 172), "用时 %.1f 秒　总战绩 %d 胜 %d 负" % (st["t"], st["wins"], st["losses"]), font=F(10), fill=(216, 230, 240))
        d.text((60, 194), st["say"], font=F(10), fill=(201, 185, 159))
        d.text((68, 214), "空格 / OK 再来一局", font=F(10), fill=(245, 215, 122))
    return im


# ---------------- 固件侧（照抄 pet.c 坐标常量） ----------------

def draw_firmware(st):
    import gen_game_sprites as G
    im = G.draw_race_bg().convert("RGB")   # 固件烘焙底图（含顶条/赛道/标签区/冲刺条）
    d = ImageDraw.Draw(im)
    # 标签牌：LVGL panel 46×20 + 16px 字（比试玩 8px 位图大，固件矢量字裁量）
    d.rectangle([20, 62, 65, 81], fill=(24, 34, 46))
    d.text((27, 63), "兔子", font=F(16), fill=(255, 243, 220))
    d.rectangle([20, 152, 65, 171], fill=(24, 34, 46))
    d.text((35, 153), "你", font=F(16), fill=(255, 243, 220))
    # 石头 spr_rock 14×13 @ (px-6, ty+30)
    rock = G.draw_rock()
    for i, od in enumerate(ROCK):
        if not st["obs_rhit"][i]:
            im.paste(rock, (px(od) - 6, 114), rock)
        if not st["obs_hit"][i]:
            im.paste(rock, (px(od) - 6, 204), rock)
    # 胡萝卜 spr_carrot_s 10×15（母版小萝卜）@ (px-5, ty+30)
    carrot = G.draw_race_carrot()
    for i, cd in enumerate(CAR):
        if not st["car_rgot"][i]:
            im.paste(carrot, (px(cd) - 5, 84 + 30), carrot)
        if not st["car_got"][i]:
            im.paste(carrot, (px(cd) - 5, 174 + 30), carrot)
    # 兔子 48×48 @ (px-24, 88-跳10)
    try:
        rab = Image.open(RABBIT_WALK).convert("RGBA").resize((48, 48), Image.NEAREST)
        im.paste(rab, (px(st["rb"]) - 24, 88 - (10 if st["rjump"] else 0)), rab)
    except Exception:
        pass
    # 跑者 32×40 @ (px-16, 159-跳11)
    runner = G.draw_runner(st["frame"])
    im.paste(runner, (px(st["me"]) - 16, 159 - (11 if st["jump"] else 0)), runner)
    # 顶部战况（固件 16px 字单行）
    d.text((8, 9), "你 VS %s %d.%ds" % (st["opp"], int(st["t"]), int(st["t"] * 10) % 10),
           font=F(16), fill=(255, 243, 220))
    lead = st["me"] - st["rb"]
    ltxt = "并驾齐驱" if abs(lead) < 2000 else ("你领先 %dm" % (lead // 1000) if lead > 0 else "落后 %dm" % (-lead // 1000))
    lcol = (245, 215, 122) if abs(lead) < 2000 else ((123, 224, 160) if lead > 0 else (255, 154, 154))
    tb = d.textbbox((0, 0), ltxt, font=F(16))
    d.text((232 - (tb[2] - tb[0]), 9), ltxt, font=F(16), fill=lcol)
    # 动量条填充
    bw = max(1, int(110 * st["mom"]))
    col = (245, 215, 122) if st["mom"] > 0.75 else ((123, 224, 160) if st["mom"] > 0.4 else (224, 162, 76))
    d.rectangle([66, 274, 65 + bw, 281], fill=col)
    # 右下米数（固件右对齐 16px；G_INTRO 阶段 label 为空）
    hud_rows = [] if not st.get("hud", True) else [
        (280, "你 %dm" % (st["me"] // 1000), (255, 243, 220)),
        (300, "%s %dm" % (st["opp"], st["rb"] // 1000), (216, 230, 240)),
    ]
    for y, txt, c in hud_rows:
        tb = d.textbbox((0, 0), txt, font=F(16))
        d.text((232 - (tb[2] - tb[0]), y), txt, font=F(16), fill=c)
    # 反馈词
    if st["flash"]:
        d.text((66, 148), st["flash"], font=F(16), fill=(255, 154, 154) if "石头" in st["flash"] else (245, 215, 122))
    # 倒计时贴图 80×88 @ (80,116)
    if st["count"] > 0:
        txt = "3" if st["count"] > 2.6 else ("2" if st["count"] > 1.6 else ("1" if st["count"] > 0.6 else "GO!"))
        cd = G.draw_countdown(txt)
        im.paste(cd, (80, 116))
    # 底部 G_INTRO/G_PLAY msg（固件 g_g_msg y286，米白）
    if st.get("msg"):
        tb = d.textbbox((0, 0), st["msg"], font=F(16))
        d.text((120 - (tb[2] - tb[0]) // 2, 284), st["msg"], font=F(16), fill=(216, 230, 240))
    # 结算面板 200×148 @ (20,92)，6 行 16px
    if st["over"]:
        d.rectangle([20, 92, 219, 239], fill=(18, 24, 32))
        rows = [
            ("你 赢 了！" if st["win"] else "兔 子 赢 了", (245, 215, 122) if st["win"] else (255, 154, 154)),
            ("你 %dm　%s %dm" % (st["me"] // 1000, st["opp"], st["rb"] // 1000), (255, 243, 220)),
            ("用时 %d.%d 秒" % (int(st["t"]), int(st["t"] * 10) % 10), (255, 243, 220)),
            ("总战绩 %d 胜 %d 负" % (st["wins"], st["losses"]), (255, 243, 220)),
            (st["say"], (201, 185, 159)),
            ("OK 再来 · 长按退出", (245, 215, 122)),
        ]
        for i, (txt, c) in enumerate(rows):
            y = 92 + 8 + i * 22
            tb = d.textbbox((0, 0), txt, font=F(16))
            d.text((120 - (tb[2] - tb[0]) // 2, y), txt, font=F(16), fill=c)
    return im


def states():
    return [
        ("倒计时（count=2.2s）", dict(
            opp="粘人精", t=0.0, me=0, rb=0, mom=0.0, jump=False, rjump=False,
            count=2.2, over=False, win=False, wins=0, losses=0, say="", flash="",
            obs_hit=[0, 0, 0], obs_rhit=[0, 0, 0], car_got=[0, 0, 0], car_rgot=[0, 0, 0],
            frame=0, msg="按 OK 开始 · 长按 OK 返回", hud=False)),
        ("赛中途（6.4s，mom=0.82）", dict(
            opp="粘人精", t=6.4, me=41000, rb=53000, mom=0.82, jump=False, rjump=True,
            count=0, over=False, win=False, wins=2, losses=1, say="", flash="胡萝卜！加速",
            obs_hit=[1, 0, 0], obs_rhit=[1, 0, 0], car_got=[1, 0, 0], car_rgot=[1, 0, 0],
            frame=1, msg="加油！")),
        ("结算（你赢 · 3胜1负）", dict(
            opp="粘人精", t=23.8, me=120000, rb=94000, mom=0.0, jump=False, rjump=False,
            count=0, over=True, win=True, wins=3, losses=1, say="你好快…再来一次嘛！",
            flash="", obs_hit=[1, 1, 1], obs_rhit=[1, 1, 1], car_got=[1, 1, 1], car_rgot=[1, 1, 1],
            frame=0, msg="")),
    ]


def main():
    import gen_game_sprites as G  # noqa: F401（触发审阅资源即可）
    rows = states()
    W = 240 * 2 + 60
    H = 40 + len(rows) * (320 + 46) + 10
    sheet = Image.new("RGB", (W, H), (247, 242, 234))
    d = ImageDraw.Draw(sheet)
    title = F(15)
    d.text((14, 8), "赛跑对战 S3 对照：左=试玩页（母版） · 右=固件实现（240×320 同坐标）", font=title, fill=(90, 62, 43))
    y = 40
    for name, st in rows:
        left = draw_playable(st)
        right = draw_firmware(st)
        sheet.paste(left, (20, y))
        sheet.paste(right, (20 + 240 + 20, y))
        d.text((20, y + 322), "试玩页 · " + name, font=F(12), fill=(122, 92, 62))
        d.text((20 + 260, y + 322), "固件 · " + name, font=F(12), fill=(122, 92, 62))
        y += 320 + 46
    out = os.path.join(ROOT, "race_compare_s3.png")
    sheet.save(out)
    print("saved:", out, sheet.size)


if __name__ == "__main__":
    main()
