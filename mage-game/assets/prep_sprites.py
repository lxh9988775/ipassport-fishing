# -*- coding: utf-8 -*-
# 离线预处理战斗素材：洋红底 -> 透明、软边、裁包围盒；背景图压暗+去水印
import math
from PIL import Image, ImageEnhance

def key_sprite(src, dst):
    im = Image.open(src).convert('RGB')
    w, h = im.size
    px = im.load()
    # 四角采样底色
    cs = [px[2, 2], px[w-3, 2], px[2, h-3], px[w-3, h-3]]
    r = sum(c[0] for c in cs) / 4
    g = sum(c[1] for c in cs) / 4
    b = sum(c[2] for c in cs) / 4
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    op = out.load()
    minX, minY, maxX, maxY = w, h, 0, 0
    for y in range(h):
        for x in range(w):
            pr, pg, pb = px[x, y]
            # 先把水印区直接当背景抹掉（右下角 28%x12%）
            if x >= w * 0.70 and y >= h * 0.87:
                continue
            d = math.sqrt((pr-r)**2 + (pg-g)**2 + (pb-b)**2)
            if d < 95:
                continue
            if d < 145:
                a = int(255 * (d - 95) / 50)
            else:
                a = 255
            op[x, y] = (pr, pg, pb, a)
            if a > 20:
                if x < minX: minX = x
                if x > maxX: maxX = x
                if y < minY: minY = y
                if y > maxY: maxY = y
    pad = 3
    minX = max(0, minX-pad); minY = max(0, minY-pad)
    maxX = min(w-1, maxX+pad); maxY = min(h-1, maxY+pad)
    out = out.crop((minX, minY, maxX+1, maxY+1))
    out.save(dst)
    print(dst, out.size)

def prep_bg(src, dst):
    im = Image.open(src).convert('RGB')
    w, h = im.size
    # 去水印：用左侧同尺寸区块覆盖右下角
    bw, bh = int(w*0.28), int(h*0.12)
    patch = im.crop((w - 2*bw, h - bh, w - bw, h))
    im.paste(patch, (w - bw, h - bh))
    # 压暗 + 降饱和，让角色更突出
    im = ImageEnhance.Color(im).enhance(0.82)
    im = ImageEnhance.Brightness(im).enhance(0.72)
    im.save(dst)
    print(dst, im.size)

key_sprite('sp_hero.png',   'sp_hero_k.png')
key_sprite('sp_goblin.png', 'sp_goblin_k.png')
key_sprite('sp_slime.png',  'sp_slime_k.png')
key_sprite('sp_cat.png',    'sp_cat_k.png')
prep_bg('battle_bg.png', 'battle_bg_k.png')
