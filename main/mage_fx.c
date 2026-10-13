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
#define BG_Y        62
#define BG_H       188
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
    lv_obj_t *hero, *heroShadow, *cat, *catShadow;
    lv_obj_t *grassFg;         /* 前景草丛 */
    lv_obj_t *dim;             /* 活动带压暗层 */
    lv_obj_t *fx[16];          /* 粒子 */
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

static void mk_dot(lv_obj_t *parent, int size, int color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return;
}

/* ===================== 构建（只做一次） ===================== */
static void fx_build(void)
{
    if (F.built || !g_fx_parent) return;

    /* 滚动背景：宽 = 屏宽 + 屏宽（两屏宽），改 x 实现无缝滚动。
     * 素材 battle_bg 原图 240x132，这里按 2 屏宽贴两遍。
     * 注意：1:1 免缩放铁律 —— 所以用两个并排的 240 宽对象来铺。 */
    F.bg = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.bg);
    lv_obj_set_size(F.bg, SCR_W, BG_H);
    lv_obj_set_pos(F.bg, 0, BG_Y);
    lv_obj_set_style_bg_opa(F.bg, LV_OPA_TRANSP, 0);
    mage_draw_bg(F.bg, "battle_bg", 0, 0, SCR_W, BG_H);
    lv_obj_add_flag(F.bg, LV_OBJ_FLAG_HIDDEN);   /* 素材非 1:1 时隐藏，走纯色兜底 */

    /* 活动带压暗：HTML 的 gDim 渐变。用一层半透明黑近似（240x130） */
    F.dim = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.dim);
    lv_obj_set_size(F.dim, SCR_W, 130);
    lv_obj_set_pos(F.dim, 0, 140);
    lv_obj_set_style_bg_color(F.dim, lv_color_hex(0x060510), 0);
    lv_obj_set_style_bg_opa(F.dim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(F.dim, 0, 0);
    lv_obj_remove_flag(F.dim, LV_OBJ_FLAG_SCROLLABLE);

    /* 飘浮光尘 */
    for (int i = 0; i < 6; i++) {
        F.dust[i] = mk_dot(g_fx_parent, 2, 0xffd98a, 1);
        F.dust[i]->user.data = (void *)(intptr_t)(40 + i * 27);
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

    /* 伙伴猫 + 主角 */
    F.catShadow = mk_dot(g_fx_parent, 12, 0x000000, 6);
    lv_obj_set_style_bg_opa(F.catShadow, LV_OPA_50, 0);
    F.cat = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.cat);
    lv_obj_set_size(F.cat, 84, 76);
    mage_draw_bg(F.cat, "sp_cat", 0, 0, 84, 76);

    F.heroShadow = mk_dot(g_fx_parent, 14, 0x000000, 7);
    lv_obj_set_style_bg_opa(F.heroShadow, LV_OPA_50, 0);
    F.hero = lv_obj_create(g_fx_parent);
    lv_obj_remove_style_all(F.hero);
    lv_obj_set_size(F.hero, 84, 108);
    mage_draw_bg(F.hero, "sp_hero", 0, 0, 84, 108);

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
            g->user.data = (void *)(intptr_t)(i * 22 - 10);
        }
    }

    /* 粒子槽 */
    for (int i = 0; i < 16; i++) {
        F.fx[i] = mk_dot(g_fx_parent, 5, FX_WHITE, 2);
        lv_obj_add_flag(F.fx[i], LV_OBJ_FLAG_HIDDEN);
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

    /* 背景横向滚动：HTML 里 cam 由主循环累加，这里同义 */
    bt.cam += 28.0f * dt;

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

    /* ---- 滚动背景：两屏宽贴图横向移动做无缝循环 ----
     * 素材原图 240x132 与绘制区 1:1，F.bg 已建。若素材 1:1 则启用滚动。 */
    if (F.bg && !lv_obj_has_flag(F.bg, LV_OBJ_FLAG_HIDDEN)) {
        float off = fx_wrap(bt.cam, SCR_W, SCR_W);
        lv_obj_set_pos(F.bg, -(int)off + shakeX, BG_Y);
    }

    /* ---- 飘浮光尘 ---- */
    for (int i = 0; i < 6; i++) {
        int bx = (int)(intptr_t)F.dust[i]->user.data;
        float dy = fmodf((float)bx * 3.0f - t * 12.0f, 180.0f);
        if (dy < 0.0f) dy += 180.0f;
        float a = 0.25f + 0.4f * fabsf(sinf((float)bx + t * 2.0f));
        lv_obj_set_pos(F.dust[i], bx + (int)(sinf(t + (float)bx) * 6.0f), (int)dy + 60);
        lv_obj_set_style_bg_opa(F.dust[i], (lv_opa_t)(a * 255.0f), 0);
    }

    /* ---- 敌方立绘 ---- */
    const char *spr[FOE_MAX] = {"sp_goblin", "sp_slime", "sp_cat"};
    const int   sprH[FOE_MAX] = {80, 54, 76};
    const int   sprW[FOE_MAX] = {60, 84, 84};
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

        /* 首次进入该槽位才建对象，之后只改属性（防闪屏铁律） */
        if (!slot) {
            slot = lv_obj_create(g_fx_parent);
            lv_obj_remove_style_all(slot);
            lv_obj_set_size(slot, sprW[k], sprH[k]);
            mage_draw_bg(slot, spr[k], 0, 0, sprW[k], sprH[k]);
            F.foe[drawn] = slot;
        } else if (slot->user.data != (void *)(intptr_t)(k + 1)) {
            /* 换了怪种，重设贴图与尺寸 */
            lv_obj_set_size(slot, sprW[k], sprH[k]);
            mage_draw_bg(slot, spr[k], 0, 0, sprW[k], sprH[k]);
            slot->user.data = (void *)(intptr_t)(k + 1);
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

        /* 受击后仰 / 冲锋前倾 */
        float eTilt = (e->hurtT > 0.0f) ? -14.0f : lungeTilt * 57.3f;

        const int hPix = sprH[k];
        lv_obj_set_pos(slot, (int)sx + shakeX - hPix / 2,
                       (int)(GROUND_Y + hopY) - hPix);
        /* 压扁：纵向缩放，pivot 放脚底 */
        lv_obj_set_style_transform_pivot_x(slot, hPix / 2, 0);
        lv_obj_set_style_transform_pivot_y(slot, hPix, 0);
        lv_obj_set_style_transform_scale_y(slot, (int32_t)(sq * 256), 0);
        if (eTilt != 0.0f) {
            lv_obj_set_style_transform_angle(slot, (int32_t)(eTilt * 10), 0);
        }
        /* 受击闪红（HTML: 主角受击 px 红色半透明） */
        if (e->flashT > 0.0f) {
            lv_obj_set_style_bg_opa(slot, LV_OPA_20, 0);
        }

        /* 影子 */
        lv_obj_t *sh = F.foeShadow[drawn];
        if (sh) {
            lv_obj_remove_flag(sh, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(sh, (int)sx + shakeX - 11, GROUND_Y - 2);
        }
        drawn++;
    }
    /* 多余槽位隐藏 */
    for (int i = drawn; i < FOE_MAX; i++) {
        if (F.foe[i] && !lv_obj_has_flag(F.foe[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foe[i], LV_OBJ_FLAG_HIDDEN);
        if (F.foeShadow[i] && !lv_obj_has_flag(F.foeShadow[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(F.foeShadow[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 伙伴猫（HTML: psx = bt.px-cam-26，突击时冲出去再退回） ---- */
    float psx = (float)(70 - (int)bt.cam - 26);
    if (bt.catLungeT > 0.0f && bt.catLungeDur > 0.0f) {
        float cp = fclampf(1.0f - bt.catLungeT / bt.catLungeDur, 0.0f, 1.0f);
        psx += sinf(cp * 3.14159f) * 70.0f;
    }
    lv_obj_set_pos(F.cat, (int)psx, 250 - 76);
    lv_obj_set_pos(F.catShadow, (int)psx - 6, 250 - 2);

    /* ---- 主角（HTML: hBob/lunge/hTilt + 受击闪红） ---- */
    float hBob  = -fabsf(sinf(bt.walkF * 0.8f)) * 2.5f;
    float lunge = (bt.lungeT > 0.0f) ? (bt.lungeT / 0.22f) * 10.0f : 0.0f;
    float hTilt = (bt.lungeT > 0.0f) ? 0.1f : sinf(bt.walkF * 0.8f) * 0.05f;
    lv_obj_set_pos(F.hero, (int)(70 - (int)bt.cam + lunge) + shakeX - 42,
                   (int)(250.0f + hBob) - 108);
    lv_obj_set_style_transform_pivot_x(F.hero, 42, 0);
    lv_obj_set_style_transform_pivot_y(F.hero, 108, 0);
    lv_obj_set_style_transform_angle(F.hero, (int32_t)(hTilt * 57.3f * 10.0f), 0);
    lv_obj_set_pos(F.heroShadow, (int)(70 - (int)bt.cam + lunge) - 7, 250 - 2);

    /* ---- 前景草丛视差 1.3 ---- */
    if (F.grassFg) {
        lv_obj_t *c = lv_obj_get_child(F.grassFg, 0);
        (void)c;
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

    /* ---- BOSS 蓄力警告 ---- */
    bool big = false;
    for (int i = 0; i < bt.enemyCount; i++) {
        if (bt.enemies[i].hp > 0 && bt.enemies[i].boss && bt.enemies[i].lungeBig) big = true;
    }
    if (big && F.warn) {
        lv_obj_remove_flag(F.warn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(F.warn, 0, 178);
        lv_obj_set_style_text_opa(F.warn,
            (lv_opa_t)((0.45f + 0.55f * fabsf(sinf(t * 16.0f))) * 255.0f), 0);
    } else if (F.warn) {
        lv_obj_add_flag(F.warn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 生命周期 ===================== */
void mage_fx_attach(lv_obj_t *parent)
{
    g_fx_parent = parent;
    F.built = false;
    memset(F.foe, 0, sizeof(F.foe));
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
