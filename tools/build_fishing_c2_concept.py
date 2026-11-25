# -*- coding: utf-8 -*-
"""钓鱼线 C+2 方案效果图：场内分标点 + 加2个新钓场（入海口/夜钓栈桥）。
4屏：5钓场选择 / 标点选择 / 主界面带标点 / 图鉴记录标点。
统一用像素硬边风（和用户已认可的精致像素版一致），真机240x320，真实背景缩略图。
仅电脑端效果图，不改固件/源码。
"""
from PIL import Image, ImageDraw, ImageFont
import os

ROOT = "c:/Users/8605464/Desktop/AIpassport"
IMG = ROOT + "/build/fish_ui"
OUT = ROOT + "/build/fish_ui/fishing_c2_concept.png"
W, H, S = 240, 320, 2

_F = {}
def font(size, bold=False):
    k = (size, bold)
    if k not in _F:
        _F[k] = ImageFont.truetype("C:/Windows/Fonts/simhei.ttf", size)
    return _F[k]

def load(n):
    return Image.open(os.path.join(IMG, n + ".png")).convert("RGBA")

BG = {k: load("bg_" + k) for k in ("pond", "river", "sea", "estuary", "pier")}
FISH_GUI = load("fish_guiyu").resize((72, 48), Image.NEAREST)

INK   = (8, 16, 30)
PANEL = (16, 32, 54)
PANEL2= (24, 46, 74)
HI    = (120, 156, 190)
SHADOW=(6, 12, 24)
GOLD  = (238, 190, 96)
TEAL  = (108, 190, 174)
WHITE = (232, 240, 248)
MUTED = (140, 164, 190)
RED   = (198, 76, 104)
GREEN = (112, 216, 140)

def key_tri(d,x,y,direction,color=MUTED,size=5):
    pts=[(x+size,y),(x+size,y+size*2),(x,y+size)] if direction=="l" else [(x,y),(x,y+size*2),(x+size,y+size)]
    d.polygon(pts,fill=color)

def px_panel(d, x0,y0,x1,y1, fill=PANEL, hi=True):
    d.rectangle([x0,y0,x1,y1], fill=INK)
    d.rectangle([x0+1,y0+1,x1-1,y1-1], fill=fill)
    if hi:
        d.line([x0+1,y0+1,x1-1,y0+1], fill=HI, width=1)
    d.line([x0+1,y1-1,x1-1,y1-1], fill=SHADOW, width=1)
    d.line([x0+1,y0+1,x0+1,y1-1], fill=HI, width=1)
    d.line([x1-1,y0+1,x1-1,y1-1], fill=SHADOW, width=1)

def hud(d, night=14, period="黄昏", weather="小雨"):
    d.rectangle([0,0,W,26],fill=INK)
    d.line([0,25,W,25],fill=(48,74,104),width=1)
    d.text((10,7),"第%d夜"%night,font=font(10,True),fill=GOLD)
    d.text((66,7),period,font=font(10),fill=WHITE)
    d.text((112,7),weather,font=font(10),fill=TEAL)
    key_tri(d,186,9,"l"); key_tri(d,198,9,"r")
    d.ellipse([212,8,222,18],outline=MUTED,width=1)

def bottom(d, txt="上下选 · 确认进入 · 长按返回"):
    d.rectangle([0,H-22,W,H],fill=INK)
    d.line([0,H-22,W,H-22],fill=(40,64,92),width=1)
    key_tri(d,8,H-17,"l"); key_tri(d,20,H-17,"r")
    d.ellipse([34,H-19,44,H-9],outline=MUTED,width=1)
    d.text((52,H-18),txt,font=font(10),fill=MUTED)

def thumb(d, im, img, x, y, w, h):
    iw,ih=img.size
    nw=w; nh=int(ih*w/iw)
    r=img.resize((nw,nh),Image.LANCZOS)
    if nh>h:
        top=(nh-h)//3; r=r.crop((0,top,nw,top+h))
    elif nh<h:
        cv=Image.new("RGBA",(w,h),(16,32,54,255)); cv.alpha_composite(r,(0,(h-nh)//2)); r=cv
    im.paste(r.convert("RGB"),(x,y))
    d.rectangle([x,y,x+w-1,y+h-1],outline=(70,100,130),width=1)

def new():
    im=Image.new("RGB",(W,H),(10,22,38)); return im, ImageDraw.Draw(im)

# ---------- 屏1：5个钓场（含2新场） ----------
def screen_spots():
    im,d=new(); hud(d)
    d.text((14,32),"选钓场",font=font(16,True),fill=WHITE)
    d.text((14,52),"天色为首，去哪下竿",font=font(9),fill=MUTED)
    spots=[
        ("pond","静水塘","2处标点","晨晴 · 白条成群",False),
        ("river","急流河","2处标点","昏雨 · 鳜鱼藏石缝",False),
        ("sea","深海","2处标点","夜晴 · 金枪旗鱼",False),
        ("estuary","入海口","2处标点 · 新","黄昏 · 鲈鱼洄游",True),
        ("pier","夜钓栈桥","2处标点 · 新","夜晴 · 灯光诱鱼",False),
    ]
    y=68
    for key,name,subs,lore,sel in spots:
        px_panel(d,10,y,230,y+42,fill=PANEL2 if sel else PANEL)
        if sel:
            d.rectangle([10,y,13,y+42],fill=RED)
        thumb(d,im,BG[key],16,y+5,34,32)
        d.text((58,y+4),name,font=font(12,True),fill=WHITE if sel else (200,214,230))
        d.text((58,y+20),subs,font=font(8),fill=TEAL)
        d.text((58,y+31),lore,font=font(8),fill=MUTED)
        if sel:
            d.text((202,y+20),"NEW",font=font(8,True),fill=GOLD)
        y+=47
    bottom(d,"上下选钓场 · 确认进标点")
    return im

# ---------- 屏2：标点选择（核心新概念） ----------
def screen_subspot():
    im,d=new()
    # 背景用入海口
    im.paste(BG["estuary"].convert("RGB"),(0,0))
    ov=Image.new("RGBA",(W,H),(6,14,28,180)); im=Image.alpha_composite(im.convert("RGBA"),ov).convert("RGB")
    d=ImageDraw.Draw(im)
    hud(d)
    d.text((14,32),"入海口 · 选标点",font=font(15,True),fill=WHITE)
    d.text((14,52),"同一片水，不同位置不一样的鱼",font=font(9),fill=TEAL)
    subs=[
        ("泥滩","鲈鱼 / 鲻鱼","退潮时鲻鱼翻泥",True),
        ("潮汐道","黑鲷 / 海鳗","涨潮黑鲷巡游",False),
    ]
    y=72
    for name,fish,lore,sel in subs:
        px_panel(d,14,y,226,y+74,fill=PANEL2 if sel else PANEL)
        if sel: d.rectangle([14,y,17,y+74],fill=RED)
        d.text((26,y+8),name,font=font(15,True),fill=WHITE if sel else (200,214,230))
        d.text((26,y+30),"出鱼："+fish,font=font(10),fill=GOLD)
        d.text((26,y+50),lore,font=font(9),fill=MUTED)
        if sel:
            d.text((186,y+8),"选中",font=font(9),fill=RED)
        y+=84
    # 提示权重非锁死
    px_panel(d,14,246,226,286,fill=PANEL)
    d.text((22,252),"标点只是提高概率，不锁死",font=font(9),fill=TEAL)
    d.text((22,268),"泥滩白天也能钓到黑鲷，只是少",font=font(8),fill=MUTED)
    bottom(d,"左右选标点 · 确认下竿")
    return im

# ---------- 屏3：主界面带标点 ----------
def screen_gameplay():
    im,d=new()
    im.paste(BG["estuary"].convert("RGB"),(0,0)); im=im.convert("RGBA")
    # 竿线浮漂
    rod=load("rod_sea"); flt=load("prop_float")
    d=ImageDraw.Draw(im)
    d.line([(62,122),(150,162)],fill=(240,246,252,255),width=1)
    im.alpha_composite(rod,(8,116)); im.alpha_composite(flt,(150,146))
    im=im.convert("RGB"); d=ImageDraw.Draw(im)
    hud(d,period="黄昏",weather="小雨")
    # 钓场+标点面板（两行）
    px_panel(d,10,34,150,72,fill=PANEL)
    d.text((18,37),"入海口",font=font(12,True),fill=WHITE)
    d.text((18,53),"泥滩 · 鲈鱼区",font=font(9),fill=TEAL)
    d.line([18,68,142,68],fill=(46,72,100),width=1)
    # 水下主角
    g=Image.new("RGBA",(W,H),(0,0,0,0))
    Fg=FISH_GUI.copy(); Fg.putalpha(Fg.getchannel("A").point(lambda a:int(a*0.5)))
    g.alpha_composite(Fg,(72,238)); im=Image.alpha_composite(im.convert("RGBA"),g).convert("RGB")
    d=ImageDraw.Draw(im)
    # 浮漂状态
    px_panel(d,126,180,230,206,fill=PANEL)
    d.text((134,186),"浮漂静着，等一口",font=font(9),fill=MUTED)
    bottom(d,"上下选 · 确认抛竿/收线")
    return im

# ---------- 屏4：图鉴记录标点 ----------
def screen_codex():
    im,d=new(); hud(d)
    d.text((14,32),"图鉴 · 钓获",font=font(16,True),fill=WHITE)
    d.text((14,52),"每一条都记着是在哪钓的",font=font(9),fill=MUTED)
    fish_imgs=[load("fish_jiyu"),load("fish_guiyu"),load("fish_luyu"),load("fish_jinqiang")]
    items=[
        ("鲫鱼","静水塘·芦苇荡","晨 · 蚯蚓","28cm"),
        ("鳜鱼","急流河·浅滩","昏雨 · 亮片","35cm"),
        ("鲈鱼","入海口·泥滩","黄昏 · 面团","41cm"),
        ("金枪鱼","深海·远洋","夜晴 · 亮片","92cm"),
    ]
    y=68
    for idx,(name,where,when,size) in enumerate(items):
        px_panel(d,12,y,228,y+52,fill=PANEL)
        # 鱼 sprite 图标位：等比缩放居中
        fi=fish_imgs[idx]; fi=fi.convert("RGBA")
        fw,fh=fi.size; sc=min(32/fw,32/fh)
        fi=fi.resize((max(1,int(fw*sc)),max(1,int(fh*sc))),Image.LANCZOS)
        ic=Image.new("RGBA",(34,36),(12,26,44,255))
        ic.alpha_composite(fi,((34-fi.width)//2,(36-fi.height)//2))
        im.paste(ic.convert("RGB"),(18,y+8))
        d.rectangle([18,y+8,52,y+44],outline=(70,100,130),width=1)
        d=ImageDraw.Draw(im)
        d.text((60,y+6),name,font=font(12,True),fill=WHITE)
        d.text((60,y+23),where,font=font(9),fill=GOLD)
        d.text((60,y+37),when,font=font(8),fill=MUTED)
        d.text((196,y+20),size,font=font(9),fill=TEAL)
        y+=58
    d.text((14,304),"已收录 27 种 · 标点 12 处",font=font(9),fill=MUTED)
    return im

def compose():
    panels=[
        ("屏1 · 选钓场（3→5，含2新场）",screen_spots(),TEAL),
        ("屏2 · 选标点（新增核心）",screen_subspot(),GOLD),
        ("屏3 · 钓鱼主界面（带标点）",screen_gameplay(),TEAL),
        ("屏4 · 图鉴（记录标点）",screen_codex(),TEAL),
    ]
    sw,sh=W*S,H*S; lab=34; gap=22; pad=26; cols=2; rows=2
    tw=pad*2+gap*(cols-1)+sw*cols
    th=pad*2+gap*(rows-1)+(lab+sh)*rows+48
    sheet=Image.new("RGB",(tw,th),(238,242,246)); d=ImageDraw.Draw(sheet)
    d.text((pad,12),"钓鱼线 C+2 方案效果图 · 场内分标点 + 加2新钓场",font=font(18,True),fill=(22,36,52))
    d.text((pad,36),"真机240x320 · 像素硬边风（与已认可主界面一致）· 真实背景 · 仅电脑端效果图，未改固件/源码",
           font=font(11),fill=(100,118,138))
    top=64
    for i,(label,im,accent) in enumerate(panels):
        cx=pad+(i%cols)*(sw+gap); cy=top+(i//cols)*(lab+sh+gap)
        d.rectangle([cx,cy,cx+sw,cy+lab-4],fill=(224,231,238))
        d.rectangle([cx,cy,cx+5,cy+lab-4],fill=accent)
        d.text((cx+14,cy+7),label,font=font(13,True),fill=(30,44,60))
        big=im.resize((sw,sh),Image.LANCZOS)
        sheet.paste(big,(cx,cy+lab))
        d.rectangle([cx,cy+lab,cx+sw-1,cy+lab+sh-1],outline=(154,170,188),width=1)
    sheet.save(OUT)
    print("saved:",OUT,sheet.size)

if __name__=="__main__":
    compose()
