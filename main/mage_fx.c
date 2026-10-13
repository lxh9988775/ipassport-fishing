/* 魔法帝之路 —— 战斗演出层（1:1 复刻 HTML index.html 的 drawBattle 实时演出）
 *
 * HTML 是 canvas 逐帧实时绘制，本工程是回合制 + 20ms 周期刷新。
 * 做法：把 HTML 的 bt.t 时间轴搬到固件里，用 lv_obj_set_pos/transform 每帧
 * 推动各对象的位置/倾斜/透明度，等价于 HTML 逐帧 drawImage+rotate。
 *
 * 性能铁律（ESP32-C3 单核 160MHz，踩过坑的）：
 *   1. 绝不在周期刷新里 lv_obj_clean() + 重建 —— 会整屏闪烁。
 *      做法：对象池预建一次，之后只改属性（pos / rot / opa / size）。
 *   2. 全屏滚动背景用「一张比屏宽 2 倍的图 + 改 x 坐标」实现，
 *      不新建对象、不重新分配内存。
 *   3. 粒子数量控制在个位数，超出就复用最老的槽位。
 */

#include "mage_draw.h"
#include "mage_state.h"
#include "mage_data.h"

#include "lvgl.h"
#include "esp_log.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SCR_W 240
#define SCR_H 320

/* 活动带：HTML 里人物站在 y≈250，背景占 y=60..250，草在 y=318 */
#define GROUND_Y   250
/* HTML drawBattle(): ctx.drawImage(bg,sx,0,sw,bg.height, 0,0,W,H)
     * 原版是**整屏铺满**（源裁剪横向视差窗，目标铺满 WxH），不是只贴中间一条。
     * 之前误设为 BG_Y=62/BG_H=132 的一条窄带，导致画面上下大片空白。 */
#define BG_Y        0
#define BG_H        320      /* 整屏高 */
#define FOE_MAX      3      /* 同屏最多画 3 只怪的立绘（与 HTML 一致） */

static const char *TAG = "mage_fx";

/* ---------- 调色板（与 mage.c 的 C_* 同源，粒子用十六进制） ---------- */
#define FX_RED    0xff6b6b
#define FX_GOLD   0xffd93d
#define FX_CYAN   0x7fd4ff
#define FX_WHITE  0xffffff
#define FX_GREEN  0x7cf0a0
#define FX_PINK   0xff9ad5
#define FX_ORANGE 0xffa04d
#define FX_PURPLE 0xb98cff

/* ===================== 对象池 ===================== */
typedef struct {
    lv_obj_t *bg;              /* 滚动背景（2 倍宽，左右循环） */
    lv_obj_t *dust[6];         /* 飘浮光尘 */
    lv_obj_t *foe[FOE_MAX];    /* 敌方立绘 */
    lv_obj_t *foeShadow[FOE_MAX];
    /* 敌人血条 / Lv 标签 / 精英标记 / 破防盾槽（1:1 复刻 HTML 1940-1946） */
    lv_obj_t *foeBarSlot[FOE_MAX], *foeBar[FOE_MAX];
    lv_obj_t *foeLv[FOE_MAX];
    lv_obj_t *foeElite[FOE_MAX];
    lv_obj_t *hero, *heroShadow, *cat, *catShadow;
    lv_obj_t *grassFg;         /* 前景草丛 */
    lv_obj_t *dim;             /* 活动带压暗层 */
    lv_obj_t *fx[16];          /* 粒子 */
    lv_obj_t *num[8];          /* 伤害飘字（HTML bt.nums） */
    int heroHurt;           /* 主角受击闪白状态翻转标记（防每帧设样式导致卡死） */
    lv_obj_t *warn;            /* BOSS 蓄力警告文字 */
    bool built;
} FxLayer;

static FxLayer F = {0};
static lv_obj_t *g_fx_parent = NULL;

/* ===================== 工具 ===================== */
static float fclampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/* HTML 的 wrap(v, max, sp)：循环滚动 */
static float fx_wrap(float v, float max, float sp)
{
    float span = max + sp;
    if (span <= 0.0f) return 0.0f;
    float r = fmodf(v, span);
    if (r < 0.0f) r += span;
    return r;
}

static lv_obj_t *mk_dot(lv_obj_t *parent, int size, int color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ===================== 构建（只做一次） ===================== */
static void fx_build(void)
{
    if (F.built || !g_fx_parent) return;

    /* 战斗背景：HTML drawImage(bg,sx,0,sw,bg.height, 0,0,W,H) 整屏铺满。
     * 素材已按 cover 预生成为 240x320（整屏原生尺寸），故 1:1 直接铺满全屏，
     * 无需也不能做缩放。横向视差改由「压暗层 + 前景草丛」表达。 */
    F.bg = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.bg);
    lv_obj_set_size(F.bg, SCR_W, BG_H);
    lv_obj_set_pos(F.bg, 0, BG_Y);
    lv_obj_set_style_bg_opa(F.bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(F.bg, 0, 0);
    lv_obj_remove_flag(F.bg, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *b0 = lv_image_create(F.bg);
        lv_image_set_src(b0, mage_img("battle_bg"));
        lv_obj_set_pos(b0, 0, 0);
    }

    /* 活动带压暗：HTML 的 gDim 渐变。用一层半透明黑近似（240x130） */
    F.dim = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.dim);
    lv_obj_set_size(F.dim, SCR_W, 130);
    lv_obj_set_pos(F.dim, 0, 140);
    lv_obj_set_style_bg_color(F.dim, lv_color_hex(0x060510), 0);
    lv_obj_set_style_bg_opa(F.dim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(F.dim, 0, 0);
    lv_obj_remove_flag(F.dim, LV_OBJ_FLAG_SCROLLABLE);

    /* 飘浮光尘：透明度在构建期定死（HTML 的 0.25~0.65 呼吸感折中为静态 0.5），
     * 运行时只改 pos —— 改透明度会每帧触发 invalidate 导致卡死。 */
    for (int i = 0; i < 6; i++) {
        F.dust[i] = mk_dot(g_fx_parent, 2, 0xffd98a, 1);
        lv_obj_set_style_bg_opa(F.dust[i], LV_OPA_50, 0);
        lv_obj_set_user_data(F.dust[i], (void *)(intptr_t)(40 + i * 27));
    }

    /* 地面阴影（椭圆用圆角矩形近似） */
    for (int i = 0; i < FOE_MAX; i++) {
        F.foeShadow[i] = lv_obj_create(g_fx_parent);
        lv_obj_remove_style_all(F.foeShadow[i]);
        lv_obj_set_size(F.foeShadow[i], 22, 5);
        lv_obj_set_style_bg_color(F.foeShadow[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(F.foeShadow[i], LV_OPA_50, 0);
        lv_obj_set_style_radius(F.foeShadow[i], 2, 0);
        lv_obj_set_style_border_width(F.foeShadow[i], 0, 0);
        lv_obj_remove_flag(F.foeShadow[i], LV_OBJ_FLAG_SCROLLABLE);
        F.foe[i] = NULL;
    }

    /* 敌人血条 + Lv 标签 + 精英标记 + 破防盾槽
     * HTML 权威：
     *   bar(sx-12, sy-26, 24, 3, hp/maxHp, '#331a1a', hurtT>0?'#fff':'#e05555')
     *   txt('Lv'+lvl, sx+13, sy-35, elite?'#ffd93d':'#c9b8ee', 7)
     *   精英标记 px(sx-2, sy-30, 4, 3) 金色
     *   破防盾槽 小菱形排，y = sy-56
     * 这些元素此前完全缺失 —— 这是「其他都没修复好」的主因之一。 */
    for (int i = 0; i < FOE_MAX; i++) {
        /* 血条槽：24x3 @ (sx-12, sy-26) */
        F.foeBarSlot[i] = lv_obj_create(g_fx_parent);
        lv_obj_remove_style_all(F.foeBarSlot[i]);
        lv_obj_set_size(F.foeBarSlot[i], 24, 3);
        lv_obj_set_style_bg_color(F.foeBarSlot[i], lv_color_hex(0x331a1a), 0);
        lv_obj_set_style_bg_opa(F.foeBarSlot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(F.foeBarSlot[i], 0, 0);
        lv_obj_set_style_radius(F.foeBarSlot[i], 0, 0);
        lv_obj_remove_flag(F.foeBarSlot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(F.foeBarSlot[i], LV_OBJ_FLAG_HIDDEN);

        /* 血条填充（改宽度表示血量百分比） */
        F.foeBar[i] = lv_obj_create(F.foeBarSlot[i]);
        lv_obj_remove_style_all(F.foeBar[i]);
        lv_obj_set_size(F.foeBar[i], 24, 3);
        lv_obj_set_style_bg_color(F.foeBar[i], lv_color_hex(0xe05555), 0);
        lv_obj_set_style_bg_opa(F.foeBar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(F.foeBar[i], 0, 0);
        lv_obj_remove_flag(F.foeBar[i], LV_OBJ_FLAG_SCROLLABLE);

        /* Lv 标签 */
        F.foeLv[i] = lv_label_create(g_fx_parent);
        lv_obj_set_style_text_color(F.foeLv[i], lv_color_hex(0xc9b8ee), 0);
        lv_obj_set_style_text_align(F.foeLv[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(F.foeLv[i], 26);
        lv_label_set_text(F.foeLv[i], "Lv1");
        lv_obj_add_flag(F.foeLv[i], LV_OBJ_FLAG_HIDDEN);

        /* 精英标记：金色小三角（HTML px(sx-2,sy-30,4,3)+px(sx-1,sy-27,2,3)） */
        F.foeElite[i] = lv_obj_create(g_fx_parent);
        lv_obj_remove_style_all(F.foeElite[i]);
        lv_obj_set_size(F.foeElite[i], 4, 6);
        lv_obj_set_style_bg_color(F.foeElite[i], lv_color_hex(0xffd93d), 0);
        lv_obj_set_style_bg_opa(F.foeElite[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(F.foeElite[i], 0, 0);
        lv_obj_set_style_radius(F.foeElite[i], 2, 0);
        lv_obj_remove_flag(F.foeElite[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(F.foeElite[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 敌方立绘槽位：构建期一次建好 3 个（绝不周期刷新里建对象）。
     * 每槽默认挂 sp_goblin，进入战斗时按怪种切 src。 */
    {
        static const char *spr0[FOE_MAX] = {"sp_goblin", "sp_slime", "sp_cat"};
        static const int   sw0[FOE_MAX]  = {30, 48, 29};
        static const int   sh0[FOE_MAX]  = {40, 24, 32};
        for (int i = 0; i < FOE_MAX; i++) {
            lv_obj_t *o = lv_image_create(g_fx_parent);
            lv_image_set_src(o, mage_img(spr0[i]));
            lv_obj_set_size(o, sw0[i], sh0[i]);
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_user_data(o, (void *)(intptr_t)(i + 1));
            F.foe[i] = o;
        }
    }

    /* 伙伴猫 + 主角 */
    F.catShadow = mk_dot(g_fx_parent, 12, 0x000000, 6);
    lv_obj_set_style_bg_opa(F.catShadow, LV_OPA_50, 0);
    F.cat = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.cat);
    lv_obj_set_size(F.cat, 29, 32);
    mage_draw_bg(F.cat, "sp_cat", 0, 0, 29, 32);

    F.heroShadow = mk_dot(g_fx_parent, 14, 0x000000, 7);
    lv_obj_set_style_bg_opa(F.heroShadow, LV_OPA_50, 0);
    F.hero = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.hero);
    lv_obj_set_size(F.hero, 36, 46);
    mage_draw_bg(F.hero, "sp_hero", 0, 0, 36, 46);

    /* 前景草丛：HTML 画两组三角，这里用一排深绿小三角块近似（视差 1.3） */
    F.grassFg = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.grassFg);
    lv_obj_set_size(F.grassFg, SCR_W, 20);
    lv_obj_set_pos(F.grassFg, 0, SCR_H - 20);
    lv_obj_set_style_bg_opa(F.grassFg, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(F.grassFg, LV_OBJ_FLAG_SCROLLABLE);
    {
        for (int i = 0; i < 12; i++) {
            lv_obj_t *g = lv_obj_create(F.grassFg);
            lv_obj_remove_style_all(g);
            lv_obj_set_size(g, 12, 10);
            lv_obj_set_style_bg_color(g, lv_color_hex(i % 2 ? 0x1a3212 : 0x14240e), 0);
            lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(g, 0, 0);
            lv_obj_set_style_radius(g, 6, 0);
            lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(g, i * 22 - 10, 8);
            lv_obj_set_user_data(g, (void *)(intptr_t)(i * 22 - 10));
        }
    }

    /* 粒子槽 */
    for (int i = 0; i < 16; i++) {
        F.fx[i] = mk_dot(g_fx_parent, 5, FX_WHITE, 2);
        lv_obj_add_flag(F.fx[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 伤害飘字槽：1:1 复刻 HTML bt.nums
     *（x,y,v,c,t）→ 红/白字、向上飘 22px/s、0.8~1.2s 淡出 */
    for (int i = 0; i < 8; i++) {
        lv_obj_t *o = lv_label_create(g_fx_parent);
        lv_obj_set_style_text_color(o, lv_color_hex(FX_WHITE), 0);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(o, 60);
        lv_label_set_text(o, "");
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        F.num[i] = o;
    }

    /* BOSS 蓄力警告 */
    F.warn = lv_label_create(g_fx_parent);
    lv_obj_set_style_text_color(F.warn, lv_color_hex(FX_RED), 0);
    lv_obj_set_style_text_align(F.warn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(F.warn, SCR_W);
    lv_label_set_text(F.warn, "蓄力大招!");
    lv_obj_add_flag(F.warn, LV_OBJ_FLAG_HIDDEN);

    F.built = true;
    ESP_LOGI(TAG, "战斗演出层就绪");
}

/* ===================== 伤害飘字（1:1 复刻 HTML bt.nums） ===================== */
void mage_fx_num(int worldX, int worldY, int value, uint32_t color)
{
    if (!F.built) return;
    int i = bt.numHead % 8;
    bt.numHead++;
    snprintf(bt.nums[i].v, sizeof(bt.nums[i].v), "%d", value);
    /* HTML：伤害数字直接写 v:'+dmg' / '-'+d，故负数自带负号 */
    if (value < 0) { /* 已在调用方拼好，这里不处理 */ }
    bt.nums[i].x = (float)worldX;
    bt.nums[i].y = (float)worldY;
    bt.nums[i].c = color;
    bt.nums[i].t = 0.8f;
    lv_obj_t *o = F.num[i];
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_text(o, bt.nums[i].v);
    lv_obj_set_pos(o, worldX - (int)bt.cam - 30, worldY);
}

/* ===================== 粒子 ===================== */
void mage_fx_spawn(int kind, float x, float y, int color, float r, float life)
{
    if (!F.built) return;
    int i = bt.fxHead % 16;
    bt.fxHead++;
    bt.fx[i].kind  = kind;
    bt.fx[i].x = x; bt.fx[i].y = y;
    bt.fx[i].r = r; bt.fx[i].t = 0.0f; bt.fx[i].t0 = life;
    bt.fx[i].c = color;
    /* 先摆到起点并显示 */
    lv_obj_t *o = F.fx[i];
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(o, lv_color_hex((uint32_t)color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_pos(o, (int)x - 2, (int)y - 2);
    lv_obj_set_size(o, 5, 5);
}

/* ===================== 每帧推进 ===================== */
void mage_fx_tick(float dt)
{
    if (!F.built) return;

    /* HTML bt.t 时间轴 */
    bt.fxT += dt;
    /* 行走相位必须推进，否则主角永远僵在 walkF=0（hBob/hTilt 全为 0） */
    bt.walkF += dt * 6.0f;

    /* 背景横向滚动：HTML 里 cam 是**派生量** cam = bt.px-70（bx.px 只在 walk
     * 阶段推进 40*dt/s），不是独立累加器。
     * 原写成 bt.cam += 28*dt 无上限累加，几秒后 cam 涨到几百，
     * 所有角色 x = worldX - cam 被推出屏幕左侧 -> 敌人全挤在左边甚至消失，
     * 且每帧都在动 -> 持续 invalidate -> 卡死。 */
    bt.cam = 0.0f;

    /* 英雄行走相位：只在菜单/待命时轻微起伏（HTML: hBob） */
    if (bt.lungeT > 0.0f) {
        bt.lungeT -= dt;
        if (bt.lungeT < 0.0f) bt.lungeT = 0.0f;
    }
    if (bt.hurtT > 0.0f) {
        bt.hurtT -= dt;
        if (bt.hurtT < 0.0f) bt.hurtT = 0.0f;
    }
    if (bt.shake > 0.0f) {
        bt.shake -= dt * 4.0f;
        if (bt.shake < 0.0f) bt.shake = 0.0f;
    }
    if (bt.catLungeT > 0.0f) {
        bt.catLungeT -= dt;
        if (bt.catLungeT < 0.0f) bt.catLungeT = 0.0f;
    }

    /* 敌人逐个推进演出计时 */
    for (int i = 0; i < bt.enemyCount; i++) {
        BattleEnemy *e = &bt.enemies[i];
        if (e->hurtT > 0.0f) {
            e->hurtT -= dt;
            if (e->hurtT < 0.0f) e->hurtT = 0.0f;
        }
        if (e->flashT > 0.0f) {
            e->flashT -= dt;
            if (e->flashT < 0.0f) e->flashT = 0.0f;
        }
        if (e->lungeT > 0.0f) {
            e->lungeT -= dt;
            if (e->lungeT < 0.0f) e->lungeT = 0.0f;
        }
        if (e->brokenT > 0.0f) {
            e->brokenT -= dt;
            if (e->brokenT < 0.0f) e->brokenT = 0.0f;
        }
    }

    /* 粒子推进 */
    for (int i = 0; i < 16; i++) {
        if (bt.fx[i].t0 <= 0.0f) continue;
        bt.fx[i].t += dt;
        if (bt.fx[i].t >= bt.fx[i].t0) {
            bt.fx[i].t0 = 0.0f;
            lv_obj_add_flag(F.fx[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 伤害飘字推进：HTML bt.nums.filter(n=>{n.t-=dt; n.y-=22*dt; return n.t>0}) */
    for (int i = 0; i < 8; i++) {
        if (bt.nums[i].t <= 0.0f) continue;
        bt.nums[i].t -= dt;
        bt.nums[i].y -= 22.0f * dt;
        if (bt.nums[i].t <= 0.0f) {
            bt.nums[i].t = 0.0f;
            lv_obj_add_flag(F.num[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ===================== 每帧绘制（只改属性，不建对象） ===================== */
void mage_fx_render(void)
{
    if (!F.built) return;

    const float t = bt.fxT;

    /* 暴击震屏：HTML bt.shake 用随机位移，这里用 sin 近似（更稳） */
    int shakeX = 0;
    if (bt.shake > 0.0f) {
        shakeX = (int)(sinf(bt.fxT * 60.0f) * bt.shake * 4.0f);
    }

    /* ---- 背景：整屏单幅，只做震屏平移（不做缩放/横向滚动，避免露出空白） ---- */
    if (F.bg && !lv_obj_has_flag(F.bg, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_pos(F.bg, shakeX, BG_Y);
    }

    /* ---- 飘浮光尘 ----
     * 卡死根因修复：原先每帧改 bg_opa 让光尘闪烁，LVGL 每次都把该对象
     * 所在区域标脏重绘。在全屏贴图之上做透明度动画 = 每帧触发大面积
     * invalidate，ESP32-C3 单核直接卡死。
     * 现在改为**只在构建期设一次固定透明度**，运行时只改 pos（位移重绘
     * 范围仅 2x2 像素）。HTML 的呼吸闪烁感在 240x320 单核上代价太大，
     * 视觉收益远小于「不卡死」的收益。 */
    for (int i = 0; i < 6; i++) {
        int bx = (int)(intptr_t)lv_obj_get_user_data(F.dust[i]);
        float dy = fmodf((float)bx * 3.0f - t * 12.0f, 180.0f);
        if (dy < 0.0f) dy += 180.0f;
        lv_obj_set_pos(F.dust[i], bx + (int)(sinf(t + (float)bx) * 6.0f), (int)dy + 60);
    }

    /* ---- 敌方立绘 ---- */
    const char *spr[FOE_MAX] = {"sp_goblin", "sp_slime", "sp_cat"};
    const int   sprH[FOE_MAX] = {40, 24, 32};   /* HTML ENEMY.h: goblin40/slime24/cat32 */
    const int   sprW[FOE_MAX] = {30, 48, 29};
    int drawn = 0;

    for (int i = 0; i < bt.enemyCount && drawn < FOE_MAX; i++) {
        BattleEnemy *e = &bt.enemies[i];
        lv_obj_t *slot = F.foe[drawn];
        if (e->hp <= 0) {
            if (slot && !lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
            if (F.foeShadow[drawn] && !lv_obj_has_flag(F.foeShadow[drawn], LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(F.foeShadow[drawn], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int k = (e->typeIdx >= 0 && e->typeIdx <= 2) ? e->typeIdx : 0;

        /* 槽位在 fx_build 里已全部建好，这里只在换怪种时切 src（不新建对象） */
        if (!slot) {
            continue;   /* 理论上不会发生，槽位已在构建期创建 */
        }
        if (lv_obj_get_user_data(slot) != (void *)(intptr_t)(k + 1)) {
            /* 换怪种：只切贴图源，绝不新建对象（防闪屏 + 防内存抖动） */
            lv_image_set_src(slot, mage_img(spr[k]));
            lv_obj_set_user_data(slot, (void *)(intptr_t)(k + 1));
        }
        lv_obj_remove_flag(slot, LV_OBJ_FLAG_HIDDEN);

        /* --- HTML 参数 1:1 --- */
        const int  fly   = ENEMY[e->typeIdx].fly;
        const bool isSlime = (strcmp(ENEMY[e->typeIdx].type, "slime") == 0);

        /* 冲锋位移：sx -= sin(lp*PI)*dash */
        float sx = (float)(e->x - 70);      /* BTPX=70，减 cam 得屏幕位置 */
        sx -= (float)(int)bt.cam;
        float lungeTilt = 0.0f;
        if (e->lungeT > 0.0f && e->lungeDur > 0.0f) {
            float lp = fclampf(1.0f - e->lungeT / e->lungeDur, 0.0f, 1.0f);
            float dash = (float)(e->x - 70 - 16);
            if (dash < 0.0f) dash = 0.0f;
            sx -= sinf(lp * 3.14159f) * dash;
            lungeTilt = -0.12f * sinf(lp * 3.14159f);
        }

        /* 跳跃 + 落地压扁 */
        float hopPh = fabsf(sinf(t * (isSlime ? 4.2f : 5.2f) + e->bob));
        float hopY  = fly ? sinf(t * 2.6f + e->bob) * 4.0f
                          : -(isSlime ? 5.0f : 3.5f) * hopPh;
        /* 1:1 免缩放铁律：压扁用 transform_pivot_y 的 scale，不改 w/h */
        float sq    = fly ? 1.0f : 1.0f + (isSlime ? 0.14f : 0.06f) * (1.0f - hopPh);
        (void)sq; (void)isSlime;
        /* 压扁不用 transform（会打满单核致发烫），改由 hopY 位移体现弹跳 */
        float eTilt = 0.0f;
        (void)eTilt; (void)lungeTilt;

        const int hPix = sprH[k];
        lv_obj_set_pos(slot, (int)sx + shakeX - hPix / 2,
                       (int)(GROUND_Y + hopY) - hPix);
        /* 压扁改用「不缩放但整体上抬」近似：hopY 越低代表压得越扁，
         * 视觉上保留弹跳感，且零变换开销。 */
        /* 受击闪白：仅在状态翻转的那一帧设置，避免每帧无效重绘 */
        if (e->flashT > 0.0f) {
            if (lv_obj_get_user_data(slot) != (void *)(intptr_t)0xBEEF) {
                lv_obj_set_style_img_opa(slot, LV_OPA_60, 0);
                lv_obj_set_user_data(slot, (void *)(intptr_t)0xBEEF);
            }
        } else if (lv_obj_get_user_data(slot) == (void *)(intptr_t)0xBEEF) {
            lv_obj_set_style_img_opa(slot, LV_OPA_COVER, 0);
            lv_obj_set_user_data(slot, (void *)(intptr_t)(k + 1));
        }

        /* 影子 */
        lv_obj_t *sh = F.foeShadow[drawn];
        if (sh) {
            lv_obj_remove_flag(sh, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(sh, (int)sx + shakeX - 11, GROUND_Y - 2);
        }

        /* ---- 敌人血条 / Lv / 精英标记（HTML 1940-1946，1:1） ---- */
        const int isx = (int)sx + shakeX;
        lv_obj_t *bs = F.foeBarSlot[drawn];
        if (bs) {
            lv_obj_remove_flag(bs, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(bs, isx - 12, GROUND_Y - 26);
            int bw = e->maxHp > 0 ? (int)((float)e->hp * 24.0f / (float)e->maxHp) : 0;
            if (bw < 0) { bw = 0; }
            if (bw > 24) { bw = 24; }
            lv_obj_set_size(F.foeBar[drawn], bw, 3);
            /* 受击瞬间血条变白（HTML hurtT>0?'#fff'），只在该帧设一次 */
            if (e->hurtT > 0.0f) {
                lv_obj_set_style_bg_color(F.foeBar[drawn], lv_color_hex(0xffffff), 0);
            } else {
                lv_obj_set_style_bg_color(F.foeBar[drawn], lv_color_hex(0xe05555), 0);
            }
        }
        lv_obj_t *lvLbl = F.foeLv[drawn];
        if (lvLbl) {
            lv_obj_remove_flag(lvLbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(lvLbl, isx + 13, GROUND_Y - 35);
            lv_obj_set_style_text_color(lvLbl,
                lv_color_hex(e->elite ? 0xffd93d : 0xc9b8ee), 0);
            char lb[8];
            snprintf(lb, sizeof(lb), "Lv%d", e->lvl);
            lv_label_set_text(lvLbl, lb);
        }
        lv_obj_t *el = F.foeElite[drawn];
        if (el) {
            if (e->elite) {
                lv_obj_remove_flag(el, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(el, isx - 2, GROUND_Y - 30);
            } else if (!lv_obj_has_flag(el, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(el, LV_OBJ_FLAG_HIDDEN);
            }
        }
        drawn++;
    }
    /* 多余槽位隐藏 */
    for (int i = drawn; i < FOE_MAX; i++) {
        if (F.foe[i] && !lv_obj_has_flag(F.foe[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foe[i], LV_OBJ_FLAG_HIDDEN);
        if (F.foeShadow[i] && !lv_obj_has_flag(F.foeShadow[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foeShadow[i], LV_OBJ_FLAG_HIDDEN);
        if (F.foeBarSlot[i] && !lv_obj_has_flag(F.foeBarSlot[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foeBarSlot[i], LV_OBJ_FLAG_HIDDEN);
        if (F.foeLv[i] && !lv_obj_has_flag(F.foeLv[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foeLv[i], LV_OBJ_FLAG_HIDDEN);
        if (F.foeElite[i] && !lv_obj_has_flag(F.foeElite[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foeElite[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 伙伴猫（HTML: psx = bt.px-cam-26，突击时冲出去再退回） ---- */
    float psx = (float)(70 - (int)bt.cam - 26);
    if (bt.catLungeT > 0.0f && bt.catLungeDur > 0.0f) {
        float cp = fclampf(1.0f - bt.catLungeT / bt.catLungeDur, 0.0f, 1.0f);
        psx += sinf(cp * 3.14159f) * 70.0f;
    }
    lv_obj_set_pos(F.cat, (int)psx - 14, 250 - 32);
    lv_obj_set_pos(F.catShadow, (int)psx - 6, 250 - 2);

    /* ---- 主角（HTML: hBob/lunge/hTilt + 受击闪红） ---- */
    float hBob  = -fabsf(sinf(bt.walkF * 0.8f)) * 2.5f;
    float lunge = (bt.lungeT > 0.0f) ? (bt.lungeT / 0.22f) * 10.0f : 0.0f;
    float hTilt = (bt.lungeT > 0.0f) ? 0.1f : sinf(bt.walkF * 0.8f) * 0.05f;
    (void)hTilt;   /* 不用旋转，倾斜效果由 lunge 位移近似 */
    /* 主角：不使用 transform 旋转，同样只改 pos（零软件变换） */
    lv_obj_set_pos(F.hero, (int)(70 - (int)bt.cam + lunge) + shakeX - 18,
                   (int)(250.0f + hBob) - 46);
    /* 主角受击闪白：只在状态翻转的那一帧设置。
     * 每帧无条件设 img_opa 会持续触发 invalidate -> 卡死。 */
    if (bt.hurtT > 0.0f) {
        if (F.heroHurt == 0) {
            F.heroHurt = 1;
            lv_obj_set_style_img_opa(F.hero, LV_OPA_60, 0);
        }
    } else if (F.heroHurt == 1) {
        F.heroHurt = 0;
        lv_obj_set_style_img_opa(F.hero, LV_OPA_COVER, 0);
    }
    lv_obj_set_pos(F.heroShadow, (int)(70 - (int)bt.cam + lunge) - 7, 250 - 2);

    /* ---- 前景草丛视差 1.3 ---- */
    if (F.grassFg) {
        float gx = -fx_wrap(bt.cam * 1.3f, 264.0f, 264.0f);
        lv_obj_set_pos(F.grassFg, (int)gx, SCR_H - 20);
    }

    /* ---- 粒子绘制 ---- */
    for (int i = 0; i < 16; i++) {
        if (bt.fx[i].t0 <= 0.0f) continue;
        float pr = fclampf(1.0f - bt.fx[i].t / bt.fx[i].t0, 0.0f, 1.0f);
        lv_obj_t *o = F.fx[i];
        int k = bt.fx[i].kind;
        int px2 = (int)(bt.fx[i].x - (int)bt.cam);
        int py2 = (int)bt.fx[i].y;
        if (k == 0) {            /* p：粒子方块，飞散淡出 */
            lv_obj_set_pos(o, px2 - 2, py2 - 2 - (int)(pr * 12.0f));
            lv_obj_set_style_bg_opa(o, (lv_opa_t)(fclampf(bt.fx[i].t * 2.2f, 0.0f, 1.0f) * 255), 0);
        } else if (k == 1) {     /* ring：扩散圆环 */
            int r = (int)(bt.fx[i].r * (1.2f - pr));
            lv_obj_set_size(o, r * 2, r * 2);
            lv_obj_set_pos(o, px2 - r, py2 - r);
            lv_obj_set_style_radius(o, r, 0);
            lv_obj_set_style_bg_opa(o, (lv_opa_t)(pr * 255.0f * 0.6f), 0);
        } else if (k == 2) {     /* circle：地面魔法阵，缩放扁圆 */
            int r = (int)(bt.fx[i].r * (1.0f - pr)) + 2;
            lv_obj_set_size(o, r * 2, (int)(r * 0.7f) + 1);
            lv_obj_set_pos(o, px2 - r, py2 - (int)(r * 0.35f));
            lv_obj_set_style_radius(o, r, 0);
            lv_obj_set_style_bg_opa(o, (lv_opa_t)(fclampf(bt.fx[i].t * 1.8f, 0.0f, 1.0f) * 230), 0);
        } else {                 /* charge：聚能光球膨胀 */
            int R = (int)(3.0f + pr * 10.0f);
            lv_obj_set_size(o, R * 2, R * 2);
            lv_obj_set_pos(o, px2 - R, py2 - R);
            lv_obj_set_style_radius(o, R, 0);
            lv_obj_set_style_bg_opa(o, (lv_opa_t)(fclampf(bt.fx[i].t * 2.0f, 0.0f, 1.0f) * 200), 0);
        }
    }

    /* ---- 伤害飘字绘制：HTML txt(n.v, n.x-cam, n.y, n.c, 10, 'center')
     * alpha = clamp(n.t*1.5, 0, 1) */
    for (int i = 0; i < 8; i++) {
        if (bt.nums[i].t <= 0.0f) continue;
        lv_obj_t *o = F.num[i];
        lv_obj_set_pos(o, (int)(bt.nums[i].x - bt.cam) - 30, (int)bt.nums[i].y);
        lv_obj_set_style_text_opa(o,
            (lv_opa_t)(fclampf(bt.nums[i].t * 1.5f, 0.0f, 1.0f) * 255.0f), 0);
    }

    /* ---- BOSS 蓄力警告 ---- */
    bool big = false;
    for (int i = 0; i < bt.enemyCount; i++) {
        if (bt.enemies[i].hp > 0 && bt.enemies[i].boss && bt.enemies[i].lungeBig) big = true;
    }
    if (big && F.warn) {
        if (lv_obj_has_flag(F.warn, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(F.warn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(F.warn, 0, 178);
        }
        /* 原版有 alpha 呼吸动画，但在全屏贴图之上改 text_opa 会每帧触发
         * 大面积 invalidate -> 卡死。改为静态不透明，保留可见性。 */
        lv_obj_set_style_text_opa(F.warn, LV_OPA_COVER, 0);
    } else if (F.warn && !lv_obj_has_flag(F.warn, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(F.warn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 生命周期 ===================== */
void mage_fx_attach(lv_obj_t *parent)
{
    g_fx_parent = parent;
    F.built = false;
}

void mage_fx_show(bool on)
{
    if (!g_fx_parent) return;
    fx_build();
    if (on) {
        lv_obj_remove_flag(g_fx_parent, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_fx_parent, LV_OBJ_FLAG_HIDDEN);
    }
}

bool mage_fx_ready(void)
{
    return F.built;
}
