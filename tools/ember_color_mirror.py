# ember_color_mirror.py — 用 Python 复刻 ember_color.c 的同款算法，
# 与真实 JS 引擎导出的 tools/ember_ref.json 对拍，验证 C 移植的算法正确性。
# 用法：node tools/ember_ref_dump.js && python tools/ember_color_mirror.py
import json, math

DIMS = [(208.0,.52,.33),(44.0,.80,.64),(6.0,.82,.52),(158.0,.42,.50)]
BASE=(32.7,.61,.64); NEUTRAL=(36.0,.26,.62); BORROWED=(220.0,.62,.24)
STOPS=[[0,0],[5,.4],[10,.7],[15,1.0]]
GREEN_LO,GREEN_HI,COLD_LO,COLD_HI=75.0,155.0,185.0,330.0
RAMP_X=[0,.15,.32,.5,.7,.88,1.0]; RAMP_L=[.05,.14,.31,.5,.68,.8,.86]; RAMP_S=[1,1,.96,.9,.8,.62,.48]
BAND_HI=[0,10,24,34,52,70,100,140,182,205,238,268,310,342,361]
BAND_NAME=["朱绯","绯橙","暮橙","暖金","蜜金","秋禾","苔金","苔青","烟青","墨蓝","夜蓝","暮紫","绯紫","绛红","朱绯"]

def norm(h):
    r=math.fmod(h,360.0)
    if r<0: r+=360.0
    return r
def arc(a,b):
    return math.fmod(b-a+540.0,360.0)-180.0
def greenHits(frm,d):
    c=0
    for k in range(1,13):
        if GREEN_LO<=norm(frm+d*k/12)<=GREEN_HI: c+=1
    return c
def travelArc(frm,to):
    d1=arc(frm,to); t=norm(to)
    if not(COLD_LO<=t<=COLD_HI): return d1
    d2=d1-360.0 if d1>=0 else d1+360.0
    return d1 if greenHits(frm,d1)<=greenHits(frm,d2) else d2
def progressFor(W):
    if W<=0: return 0.0
    if W>=STOPS[3][0]: return 1.0
    for i in range(3):
        a,b=STOPS[i][0],STOPS[i+1][0]
        if W<=b:
            pa,pb=STOPS[i][1],STOPS[i+1][1]
            return pa+(pb-pa)*(W-a)/(b-a)
    return 1.0
def analyze(tot):
    w=sum(tot); active=sum(1 for v in tot if v>0)
    order=list(range(4))
    for i in range(3):
        for j in range(i+1,4):
            if tot[order[j]]>tot[order[i]] or (tot[order[j]]==tot[order[i]] and order[j]<order[i]):
                order[i],order[j]=order[j],order[i]
    m=order[0] if w>0 else -1
    sc=order[1] if (w>0 and tot[order[1]]>0) else -1
    wm=tot[m]/w if m>=0 else 0.0
    ws=tot[sc]/w if sc>=0 else 0.0
    bal=(active>=3 and wm<0.38)
    tipW=(min(0.20,0.30*ws/wm) if (not bal and sc>=0) else 0.0)
    return w,m,sc,wm,ws,bal,tipW
def hslToRgb(h,s,l):
    h=norm(h)/360.0
    s=max(0.0,min(1.0,s)); l=max(0.0,min(1.0,l))
    if s==0.0:
        v=round(l*255.0); return (v,v,v)
    q=(l*(1+s)) if l<0.5 else (l+s-l*s)
    p=2*l-q
    fpt=[h+1/3,h,h-1/3]; ch=[]
    for t in fpt:
        t=t-math.floor(t)
        if t<1/6: v=p+(q-p)*6*t
        elif t<1/2: v=q
        elif t<2/3: v=p+(q-p)*(2/3-t)*6
        else: v=p
        ch.append(round(v*255.0))
    return tuple(ch)
def colorFor(tot,borrowed):
    W,m,sc,wm,ws,bal,tipW=analyze(tot)
    p=progressFor(W)
    if borrowed:
        rgb=hslToRgb(*BORROWED); h,s,l=BORROWED
        return dict(h=h,s=s,l=l,rgb=rgb,hex=rgb2hex(rgb),balanced=bal,main=m,second=sc,tipWeight=0.0)
    if W==0: target=BASE
    elif bal: target=NEUTRAL
    else:
        D=DIMS[m]; h=D[0]; 
        if sc>=0:
            pull=0.08*ws/wm; h=norm(D[0]+travelArc(D[0],DIMS[sc][0])*pull)
        target=(h,D[1],D[2])
    dh=travelArc(BASE[0],target[0])
    risky=(abs(dh)>90.0) or (greenHits(BASE[0],dh)>0)
    pHue=min(1.0,p*2.2) if risky else p
    hh=norm(BASE[0]+dh*pHue)
    dip=(0.34*math.sin(math.pi*min(1.0,pHue))) if risky else 0.0
    ss=max(0.05,BASE[1]+(target[1]-BASE[1])*p-dip)
    ll=BASE[2]+(target[2]-BASE[2])*p
    rgb=hslToRgb(hh,ss,ll)
    return dict(h=hh,s=ss,l=ll,rgb=rgb,hex=rgb2hex(rgb),balanced=bal,main=m,second=sc,tipWeight=tipW)
def rgb2hex(c):
    return "#%02X%02X%02X"%tuple(max(0,min(255,v)) for v in c)
def ramp_val(arr,x):
    for i in range(6):
        if x<=RAMP_X[i+1]:
            t=(x-RAMP_X[i])/(RAMP_X[i+1]-RAMP_X[i])
            return arr[i]+(arr[i+1]-arr[i])*t
    return arr[6]
def relightPixel(l0,s0,hDeg,sat,tip,tipWeight):
    L=ramp_val(RAMP_L,l0)*0.86+l0*0.14; L=max(0,min(1,L))
    S=ramp_val(RAMP_S,l0)*sat*(0.82+0.28*s0); S=max(0,min(0.95,S))
    rgb=list(hslToRgb(hDeg,S,L))
    if tip and tipWeight>0.0 and l0<0.38:
        w=tipWeight*0.5*(1-l0/0.38)
        for i in range(3): rgb[i]=round(rgb[i]*(1-w)+tip[i]*w)
    return tuple(rgb)

TIP=hslToRgb(44.0,0.80,0.64)  # 关系蜜金，与 ember_ref_dump.js 同款 tip 色
ref=json.load(open("tools/ember_ref.json"))
fail=0; total=0
for c in ref["color"]:
    g=colorFor(c["tot"],c["borrowed"])
    total+=1
    dh=abs(g["h"]-c["h"]); ds=abs(g["s"]-c["s"]); dl=abs(g["l"]-c["l"])
    # hex 容差 ±1/通道（float vs double 取整差异，设计已声明末位 ±1 正常）
    gh=tuple(int(g["hex"][i:i+2],16) for i in (1,3,5))
    rh=tuple(int(c["hex"][i:i+2],16) for i in (1,3,5))
    hexok=all(abs(a-b)<=1 for a,b in zip(gh,rh))
    if max(dh,ds,dl)>1e-2 or not hexok:
        fail+=1
        if fail<=20:
            print("COLOR MISMATCH tot=%s bor=%s  h %.3f vs %.3f  s %.3f vs %.3f  l %.3f vs %.3f  hex %s vs %s"
                  %(c["tot"],c["borrowed"],g["h"],c["h"],g["s"],c["s"],g["l"],c["l"],g["hex"],c["hex"]))
for r in ref["relight"]:
    a=relightPixel(r["l0"],r["s0"],r["h"],r["sat"],None,0)
    b=relightPixel(r["l0"],r["s0"],r["h"],r["sat"],TIP,0.15 if r["l0"]<0.38 else 0)
    # 用 JS 基准的 tip rgb（关系蜜金）比对 base；tip 行单独比对
    base=r["base"]; tipc=r["tip"]
    for calc,refv in ((a,base),(b,tipc)):
        total+=1
        if any(abs(calc[i]-refv[i])>2 for i in range(3)):
            fail+=1
            if fail<=40:
                print("RELIGHT MISMATCH l0=%.2f s0=%.2f h=%.0f sat=%.2f  got %s ref %s"
                      %(r["l0"],r["s0"],r["h"],r["sat"],calc,tuple(int(x) for x in refv)))

print("\nCOLOR+RELIGHT: %d checked, %d mismatch (tol hsl 1e-2, hex ±1, rgb ±2)"%(total,fail))
print("RESULT:", "PASS ✅" if fail==0 else "FAIL ❌")
