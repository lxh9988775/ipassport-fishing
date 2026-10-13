/*
 * mage.c —— 《魔法帝之路》固件入口 + 全部非战斗场景（1:1 复刻 index.html 玩法逻辑）
 *
 * 里程碑说明：
 *   M1 —— 地基 + 标题/命名 + 场景框架（已编译通过）。
 *   M2 —— 本文件：用统一的「标题/正文/提示」三标签 + 选择高亮，1:1 复刻
 *         CITY 枢纽、STATUS、BAG、DEX、BOOK、PARTNERS、MENU、HOME、RESEARCH、
 *         FORGE、MARKET、ALCHEMY、ACAD、PROMOTE、CRAFT、EQUIP、GUILD、WILD，
 *         以及 BATTLE 占位（战斗引擎在 M3 实装，目前进入战斗显示「下个里程碑」）。
 *   美术层（立绘/精灵/粒子）因硬件无图源简化为文字 + 描金框，玩法/数值/文案不变。
 *
 * 按键映射（3 键 → ADC 单引脚，BSP 已去抖）：
 *   OK 短按 = A 确定/进入；DOWN = B 下一项；UP = 上一项；OK 长按 = C 返回。
 */
#include "bsp_display.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include "mage_state.h"
#include "mage_data.h"

static const char *TAG = "mage";

/* ===================== 字体 ===================== */
LV_FONT_DECLARE(mage_cjk_16);
static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &mage_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ===================== 屏幕常量 ===================== */
#define SCR_W 240
#define SCR_H 320
#define HUD_H  16
#define UI_REFRESH_MS 60

/* ===================== 场景枚举 ===================== */
enum {
    SC_TITLE = 0, SC_NAME, SC_CITY, SC_STATUS, SC_PARTNERS,
    SC_BOOK, SC_ALCHEMY, SC_BAG, SC_EQUIP, SC_ACAD,
    SC_WILD, SC_GUILD, SC_HOME, SC_FORGE, SC_MARKET,
    SC_DEX, SC_MUPGRADE, SC_BATTLE, SC_MENU, SC_PROMOTE,
    SC_RESEARCH, SC_CRAFT, SC_COUNT
};
static const char *SCENE_NAME[SC_COUNT] = {
    "标题", "命名", "魔法小镇", "角色", "伙伴", "魔法书", "炼金", "背包",
    "装备", "魔法学院", "野外试炼", "冒险者公会", "住宅", "锻造房",
    "魔法黑市", "魔物图鉴", "魔法升级", "战斗", "系统", "晋升仪式",
    "魔法研究", "研究魔法"
};

/* ===================== 动作枚举 ===================== */
enum { ACT_OK = 0, ACT_NEXT, ACT_PREV, ACT_BACK };

/* ===================== 颜色 ===================== */
#define C_GOLD  lv_color_hex(0xffd93d)
#define C_GRAY  lv_color_hex(0x9a93b5)
#define C_WHITE lv_color_hex(0xf2eefb)
#define C_CYAN  lv_color_hex(0x5ec8f2)
#define C_RED   lv_color_hex(0xff6b6b)
#define C_GREEN lv_color_hex(0x7fdc8a)
#define C_DIM   lv_color_hex(0x6b6480)
#define C_BG    lv_color_hex(0x0d0b1a)

/* ===================== 全局 ===================== */
static QueueHandle_t s_btn_q = NULL;
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_title_grp = NULL, *g_name_grp = NULL, *g_scene_grp = NULL, *g_hud = NULL;
static lv_obj_t *g_title_main = NULL, *g_title_sub = NULL, *g_title_tag = NULL;
static lv_obj_t *g_menu0 = NULL, *g_menu1 = NULL, *g_title_foot = NULL, *g_title_hint = NULL;
static lv_obj_t *g_name_prompt = NULL, *g_name_val = NULL, *g_name_hint = NULL;
static lv_obj_t *g_scene_title = NULL, *g_scene_body = NULL, *g_scene_hint = NULL;
static lv_obj_t *g_toast = NULL;
static lv_obj_t *g_stars[14];

/* 选择游标（与 HTML 各场景的 *Sel 对应） */
static int g_titleSel = 0, g_nameIdx = 0;
static int locSel = 0, funcSel = 0;
static int bagIdx = 0, dexIdx = 0, bookIdx = 0, partIdx = 0;
static int partMenuOpen = 0, partMenu = 0, partRelConf = 0;
static int menuIdx = 0, homeSel = 0, resIdx = 0, forgeSel = 0, marketIdx = 0, alcIdx = 0;
static int acadIdx = 0, muIdx = 0, craftSlot = 0, eqSel = 0, guildIdx = 0, wildIdx = 0;

static int  g_scene = SC_TITLE;
static int  g_need_refresh = 1;
static int  g_last_scene = -1;
static uint32_t g_last_ui_ms = 0;
static uint32_t g_last_save_ms = 0;

/* toast */
static char g_toast_buf[64];
static uint32_t g_toast_until = 0;

/* 正文缓冲 */
static char s_b[1100];
static int  s_o;
static char s_ttl[40];

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* 地点/功能 → 场景 映射（1:1 对应 HTML LOCATIONS/FUNCS） */
static const int LOC_SCENE[7] = {SC_ACAD, SC_WILD, SC_GUILD, SC_HOME, SC_FORGE, SC_MARKET, SC_DEX};
static const int FUNC_SCENE[8] = {SC_STATUS, SC_PARTNERS, SC_BOOK, SC_ALCHEMY, SC_BAG, SC_EQUIP, SC_MUPGRADE, SC_MENU};
static const int WILD_REQ[5] = {1, 3, 5, 7, 9};

/* ===================== UI 构件 ===================== */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, int x, int y, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    use_cjk(l);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, SCR_W);
    lv_obj_set_pos(l, x, y);
    return l;
}
static void toast(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_toast_buf, sizeof(g_toast_buf), fmt, ap);
    va_end(ap);
    g_toast_until = now_ms() + 1700;
}
static void s_init(void) { s_o = 0; }
static void s_add(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(s_b + s_o, sizeof(s_b) - (size_t)s_o, fmt, ap);
    va_end(ap);
    if (k > 0) { int rem = (int)(sizeof(s_b) - s_o); s_o += (k > rem) ? rem : k; }
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ===================== 前置声明 / 小工具 ===================== */
static int  g_str_field(const char *name);
static void bagItemAt(int idx, char *cat, char *nm, int *cnt, int *col);
static int  researchRateFor(int key);
static int  spellModLen(int slot);
static void set_hidden(lv_obj_t *o, bool h) {
    if (!o) return;
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static int spellModLen(int slot) { return slot == 0 ? 3 : slot == 1 ? 9 : 5; }

/* ===================== 战斗引擎（M3，1:1 复刻 HTML 战斗逻辑）前向声明 ===================== */
static void battle_render(void);
static void battle_handle(int act);
static void battle_tick(void);

/* ===================== 建 UI ===================== */
static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    g_root = lv_obj_create(scr);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, SCR_W, SCR_H);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, C_BG, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(g_root, LV_SCROLLBAR_MODE_OFF);

    /* 顶栏 HUD */
    g_hud = lv_label_create(g_root);
    use_cjk(g_hud);
    lv_obj_set_style_text_color(g_hud, C_CYAN, 0);
    lv_obj_set_style_text_align(g_hud, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_hud, SCR_W);
    lv_obj_set_pos(g_hud, 0, 2);

    /* 标题组 */
    g_title_grp = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_title_grp);
    lv_obj_set_size(g_title_grp, SCR_W, SCR_H);
    lv_obj_set_pos(g_title_grp, 0, 0);
    lv_obj_set_style_bg_opa(g_title_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_title_grp, LV_SCROLLBAR_MODE_OFF);
    g_title_main = mk_label(g_title_grp, "魔法帝之路", 0, 42, C_GOLD);
    g_title_sub  = mk_label(g_title_grp, "— MAGE OF THE MAGIC EMPEROR —", 0, 74, C_DIM);
    g_title_tag  = mk_label(g_title_grp, "在试炼中成长，于魔法中登顶", 0, 100, C_WHITE);
    g_menu0      = mk_label(g_title_grp, "  继续冒险", 0, 156, C_GRAY);
    g_menu1      = mk_label(g_title_grp, "  新的旅程", 0, 190, C_GRAY);
    g_title_foot = mk_label(g_title_grp, "v6.4 · 魔法帝之路", 0, 272, C_DIM);
    g_title_hint = mk_label(g_title_grp, "↑↓ 选择 · OK 确定", 0, 296, C_DIM);

    /* 命名组 */
    g_name_grp = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_name_grp);
    lv_obj_set_size(g_name_grp, SCR_W, SCR_H);
    lv_obj_set_pos(g_name_grp, 0, 0);
    lv_obj_set_style_bg_opa(g_name_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_name_grp, LV_SCROLLBAR_MODE_OFF);
    g_name_prompt = mk_label(g_name_grp, "为你的魔法师起名：", 0, 70, C_WHITE);
    g_name_val    = mk_label(g_name_grp, "", 0, 130, C_GOLD);
    g_name_hint   = mk_label(g_name_grp, "↑↓ 换名 · OK 确定 · 长按返回", 0, 214, C_DIM);

    /* 通用场景组（标题/正文/提示） */
    g_scene_grp = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_scene_grp);
    lv_obj_set_size(g_scene_grp, SCR_W, SCR_H);
    lv_obj_set_pos(g_scene_grp, 0, 0);
    lv_obj_set_style_bg_opa(g_scene_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_scene_grp, LV_SCROLLBAR_MODE_OFF);

    g_scene_title = lv_label_create(g_scene_grp);
    use_cjk(g_scene_title);
    lv_obj_set_style_text_color(g_scene_title, C_GOLD, 0);
    lv_obj_set_style_text_align(g_scene_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_scene_title, SCR_W);
    lv_obj_set_pos(g_scene_title, 0, 18);

    g_scene_body = lv_label_create(g_scene_grp);
    use_cjk(g_scene_body);
    lv_obj_set_style_text_color(g_scene_body, C_WHITE, 0);
    lv_obj_set_style_text_align(g_scene_body, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(g_scene_body, SCR_W - 16);
    lv_label_set_long_mode(g_scene_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_scene_body, 10, 38);

    g_scene_hint = lv_label_create(g_scene_grp);
    use_cjk(g_scene_hint);
    lv_obj_set_style_text_color(g_scene_hint, C_DIM, 0);
    lv_obj_set_style_text_align(g_scene_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_scene_hint, SCR_W);
    lv_obj_set_pos(g_scene_hint, 0, SCR_H - 18);

    /* toast */
    g_toast = lv_label_create(g_root);
    use_cjk(g_toast);
    lv_obj_set_style_text_color(g_toast, C_GOLD, 0);
    lv_obj_set_style_text_align(g_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_toast, SCR_W - 16);
    lv_label_set_long_mode(g_toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x1a1430), 0);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_toast, 4, 0);
    lv_obj_set_pos(g_toast, 8, SCR_H - 56);
    lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);

    /* 星点 */
    static const int sx[14] = {18,46,70,96,120,150,176,200,30,64,110,140,188,214};
    static const int sy[14] = {30,58,22,44,60,28,52,36,300,288,300,270,296,282};
    for (int i = 0; i < 14; i++) {
        lv_obj_t *s = lv_obj_create(g_root);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 2, 2);
        lv_obj_set_pos(s, sx[i], sy[i]);
        lv_obj_set_style_bg_color(s, C_WHITE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s, 1, 0);
        g_stars[i] = s;
    }
}

/* ===================== 通用小工具 ===================== */
static void mage_set_scene(int s) { g_scene = s; g_need_refresh = 1; }

static void ensureDungeonOffers(void) {
    if (g.ndoffers > 0) return;
    int n = mage_ri(1, 2);
        while (g.ndoffers < n) {
        int id = mage_ri(0, 29);
        int dup = 0;
        for (int i = 0; i < g.ndoffers; i++) if (g.dungeonOffers[i] == id) dup = 1;
        if (!dup) g.dungeonOffers[g.ndoffers++] = id;
    }
}

/* ===================== 刷新（按场景拼装正文） ===================== */
static void refresh_ui(void) {
    bool title  = (g_scene == SC_TITLE);
    bool naming = (g_scene == SC_NAME);
    bool hud_on = !title && !naming;

    set_hidden(g_title_grp, !title);
    set_hidden(g_name_grp, !naming);
    set_hidden(g_scene_grp, title || naming);
    set_hidden(g_hud, !hud_on);
    for (int i = 0; i < 14; i++) set_hidden(g_stars[i], !title);

    if (title) {
        uint32_t t = now_ms();
        for (int i = 0; i < 14; i++) {
            int on = ((t / 400) + i * 3) % 5;
            lv_obj_set_style_bg_opa(g_stars[i], on < 3 ? LV_OPA_COVER : LV_OPA_30, 0);
        }
        lv_obj_set_style_text_color(g_menu0, g_titleSel == 0 ? C_GOLD : C_GRAY, 0);
        lv_obj_set_style_text_color(g_menu1, g_titleSel == 1 ? C_GOLD : C_GRAY, 0);
        lv_label_set_text(g_menu0, g_titleSel == 0 ? "▶ 继续冒险" : "  继续冒险");
        lv_label_set_text(g_menu1, g_titleSel == 1 ? "▶ 新的旅程" : "  新的旅程");
        return;
    }
    if (naming) {
        lv_label_set_text(g_name_val, NAMES[g_nameIdx % 7]);
        return;
    }

    /* HUD */
    char hb[64];
    snprintf(hb, sizeof(hb), "%s Lv%d  金%d  HP %d/%d  MP %d/%d",
             RANKS[g.rank], g.level, g.gold, g.hp, g.maxHp, g.mp, g.maxMp);
    lv_label_set_text(g_hud, hb);

    s_init();
    s_ttl[0] = '\0';

    switch (g_scene) {
    case SC_CITY: {
        s_ttl[0] = '\0';
        if (g.cityMode == 0) {
            s_add("· 魔法小镇 ·\n");
            for (int i = 0; i < 7; i++)
                s_add("%s%s\n", i == locSel ? "▶ " : "  ", LOCATIONS[i].n);
            lv_label_set_text(g_scene_hint, "↑↓ 选地点 · OK 进入 · 长按 功能栏");
        } else {
            s_add("· 功能面板 ·\n");
            for (int i = 0; i < 8; i++)
                s_add("%s%s\n", i == funcSel ? "▶ " : "  ", FUNCS[i].n);
            lv_label_set_text(g_scene_hint, "↑↓ 选功能 · OK 打开 · 长按 回地图");
        }
        break;
    }
    case SC_STATUS: {
        int atk = 7 + g.level * 2 + g.rank * 3 + g.atkBonus + eqAtk();
        s_add("%s\n", g.name);
        s_add("%s · Lv%d\n", RANKS[g.rank], g.level);
        s_add("经验 %d/%d\n", g.exp, expNeed());
        s_add("生命 %d/%d\n", g.hp, g.maxHp);
        s_add("魔力 %d/%d\n", g.mp, g.maxMp);
        s_add("攻击 %d（装+%d）\n", atk, eqAtk());
        s_add("护甲减伤 %d\n", eqDef());
        s_add("金币 %d · 击杀 %d\n", g.gold, g.kills);
        s_add("伙伴 %d 只\n", g.cats);
        int lo[4]; int n = activeLoadoutKeys(lo, 4);
        s_add("出战 %d/4：", n);
        for (int i = 0; i < n; i++) s_add("%s ", spellName(lo[i]));
        s_add("\n");
        lv_label_set_text(g_scene_hint, "OK/长按 回城 · ↓ 管理装备");
        break;
    }
    case SC_BAG: {
        char cat[12], nm[24]; int cnt = 0;
        int nP = 0, nB = 0;
        for (int id = 0; id < 20; id++) if (g.pbag[id] > 0) nP++;
        for (int id = 0; id < 40; id++) if (g.mbag[id] > 0) nB++;
        int listN = 0;
        for (int id = 0; id < 20; id++) if (g.pbag[id] > 0) listN++;
        for (int id = 0; id < 40; id++) if (g.mbag[id] > 0) listN++;
        for (int i = 1; i <= 4; i++) if (stoneN(i) > 0) listN++;
        const char *props[6] = {"orbs","seals","herbs","mats","crystals","potions"};
        for (int i = 0; i < 6; i++) if (g_str_field(props[i]) > 0) listN++;
        if (listN == 0) {
            s_add("背包空空如也\n");
            lv_label_set_text(g_scene_hint, "长按 返回");
            break;
        }
        bagIdx = clampi(bagIdx, 0, listN - 1);
        bagItemAt(bagIdx, cat, nm, &cnt, NULL);
        s_add("分类：%s\n", cat);
        s_add("[ %s ]\n", nm);
        s_add("数量 ×%d\n", cnt);
        s_add("已收集 药%d/20 材%d/40\n", nP, nB);
        s_add("(%d / %d)\n", bagIdx + 1, listN);
        lv_label_set_text(g_scene_hint, "OK/↓ 切换 · 长按 返回");
        break;
    }
    case SC_DEX: {
        int seen = 0, caught = 0;
        for (int i = 0; i < DEXORDER_COUNT; i++) { if (g.dexSeen[i]) seen++; if (g.dexCaught[i]) caught++; }
        dexIdx = clampi(dexIdx, 0, DEXORDER_COUNT - 1);
        s_add("已收录 %d/%d · 收服 %d\n", seen, DEXORDER_COUNT, caught);
        int t = enemyIndex(DEXORDER[dexIdx]);
        MobDef *bd = &ENEMY[t];
        int seenF = g.dexSeen[dexIdx];
        s_add(seenF ? "%s\n" : "？？？（未发现）\n", seenF ? bd->n : "");
        if (seenF) {
            s_add(bd->capturable ? "可捕捉" : "不可捕捉");
            if (bd->capturable) {
                const char *tool = bd->cap.tool && strcmp(bd->cap.tool, "seal") == 0 ? "封印卡" : "捕捉球";
                if (bd->hasCap && bd->cap.hp > 0) s_add(" · 用%s（血<%d%%）", tool, (int)(bd->cap.hp * 100));
                else s_add(" · 用%s", tool);
            }
            s_add("\n");
            s_add("出没：%s\n", speciesZones(dexIdx));
            s_add("天赋：");
            for (int i = 0; i < bd->nSkills; i++) s_add("%s ", SKILLS[skillIndex(bd->skills[i])].n);
            s_add("\n特性：");
            for (int i = 0; i < bd->nTraits; i++) s_add("%s ", TRAITS[traitIndex(bd->traits[i])].n);
            s_add("\n");
            if (bd->hasEvo) s_add("进化：Lv%d → %s%s\n", bd->evo.lvl, bd->n, bd->evo.title);
            else s_add("进化：无\n");
            if (bd->dex) s_add("%s\n", bd->dex);
            if (g.dexCaught[dexIdx]) s_add("已收服 %d 只\n", g.dexCaught[dexIdx]);
        }
        s_add("(%d / %d)\n", dexIdx + 1, DEXORDER_COUNT);
        lv_label_set_text(g_scene_hint, "OK 捕捉条件 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_BOOK: {
        int n = discoveredCount();
        int total = n + 1;  /* +1 = 研究新魔法 */
        bookIdx = clampi(bookIdx, 0, total - 1);
        s_add("已习得 %d/135\n", n);
        int win = 5, start = clampi(bookIdx - 2, 0, total - win);
        if (start < 0) start = 0;
        for (int i = start; i < start + win && i < total; i++) {
            if (i < n) {
                int k = discoveredKeyAt(i);
                int inlo = 0; for (int j = 0; j < g.nload; j++) if (g.loadout[j] == k) inlo = 1;
                s_add("%s%s%s\n", i == bookIdx ? "▶ " : "  ", spellName(k), inlo ? " ★" : "");
            } else {
                s_add("%s✚ 研究新魔法\n", i == bookIdx ? "▶ " : "  ");
            }
        }
        s_add("———\n");
        if (bookIdx < n) {
            int k = discoveredKeyAt(bookIdx);
            int a, e, m; spell_split(k, &a, &e, &m);
            int inlo = 0; for (int j = 0; j < g.nload; j++) if (g.loadout[j] == k) inlo = 1;
            s_add("%s · %s魔法\n", spellName(k), TIER_NAMES[m]);
            s_add("Lv%d 威力%d 耗魔%d\n", slv(k), spellPower(k), spellCost(k));
            s_add(inlo ? "★已在出战编队\n" : "A 加入出战\n");
            s_add("系：%s\n", ELEMS[e].n);
        } else {
            s_add("✚ 三槽自由拼装\n组合独门魔法\n");
        }
        int lo[4]; int nl = activeLoadoutKeys(lo, 4);
        s_add("出战 %d/4\n", nl);
        lv_label_set_text(g_scene_hint, "OK 加入/移出 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_PARTNERS: {
        if (g.npartners == 0) {
            s_add("还没有伙伴\n");
            s_add("去野外把魔物打到虚弱\n再用球/卡收服\n");
            lv_label_set_text(g_scene_hint, "长按 返回");
            break;
        }
        partIdx = clampi(partIdx, 0, g.npartners - 1);
        Partner *p = &g.partners[partIdx];
        MobDef *bd = &ENEMY[p->typeIdx];
        int ivs0 = p->ivs[0] < 0 ? 0 : p->ivs[0];
        int ivs1 = p->ivs[1] < 0 ? 0 : p->ivs[1];
        int ivs2 = p->ivs[2] < 0 ? 0 : p->ivs[2];
        int total = ivs0 + ivs1 + ivs2;
        const char *grade = total >= 40 ? "S" : total >= 33 ? "A" : total >= 26 ? "B" : "C";
        if (g.activePartner == partIdx) s_add("%s ★\n", p->name);
        else s_add("%s\n", p->name);
        s_add("%s Lv%d 资质%s(%d/45)\n", bd->n, p->level, grade, total);
        s_add("技能 %s\n", SKILLS[p->skillIdx < 7 ? p->skillIdx : 0].n);
        s_add("特性 %s\n", TRAITS[p->traitIdx < 7 ? p->traitIdx : 0].n);
        if (bd->hasEvo) s_add(bd->evo.lvl <= p->level ? "★ Lv%d可进化%s\n" : "Lv%d 可进化%s\n", bd->evo.lvl, bd->evo.title);
        s_add("攻%d 防%d 速%d\n", ivs0, ivs1, ivs2);
        if (partMenuOpen) {
            s_add("———菜单———\n");
            int nopts = 0;
            if (bd->hasEvo && !p->evolved && p->level >= bd->evo.lvl) nopts++;
            nopts += 3; /* release/city/close */
            int oi = 0;
            if (bd->hasEvo && !p->evolved && p->level >= bd->evo.lvl) { s_add("%s进化→%s%s\n", oi++ == partMenu ? "▶" : " ", p->name, bd->evo.title); }
            s_add("%s放生\n", oi++ == partMenu ? "▶" : " ");
            s_add("%s返回城镇\n", oi++ == partMenu ? "▶" : " ");
            s_add("%s关闭菜单\n", oi++ == partMenu ? "▶" : " ");
        } else if (partRelConf) {
            s_add("确认放生 %s？\nA 放生 · 长按 取消\n", p->name);
        } else {
            s_add("(%d/%d)\n", partIdx + 1, g.npartners);
        }
        lv_label_set_text(g_scene_hint, partMenuOpen || partRelConf ? "↓ 选 · OK 确定" : "OK 出战 · ↓ 切换 · 长按 菜单");
        break;
    }
    case SC_MENU: {
        menuIdx = clampi(menuIdx, 0, 6);
        const char *items[7];
        items[0] = g.autoMode ? "自动战斗：开" : "自动战斗：关";
        items[1] = g.autoCapture ? "自动捕捉：开" : "自动捕捉：关";
        items[2] = g.sound ? "音效：开" : "音效：关";
        items[3] = diffC()->n;
        items[4] = "导出存档";
        items[5] = "导入存档";
        items[6] = "重新开始";
        for (int i = 0; i < 7; i++)
            s_add("%s%s\n", i == menuIdx ? "▶ " : "  ", items[i]);
        lv_label_set_text(g_scene_hint, "OK 执行 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_HOME: {
        homeSel = clampi(homeSel, 0, 2);
        const char *it[3] = {"休息", "炼金台", "魔法研究"};
        const char *sub[3] = {"恢复全部状态", "炼制药剂与捕捉球", "消耗材料升魔法等级"};
        for (int i = 0; i < 3; i++) s_add("%s%s\n  %s\n", i == homeSel ? "▶ " : "  ", it[i], sub[i]);
        lv_label_set_text(g_scene_hint, "OK 选择 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_RESEARCH: {
        int n = discoveredCount();
        resIdx = clampi(resIdx, 0, n == 0 ? 0 : n - 1);
        int win = 5, start = clampi(resIdx - 2, 0, n - win);
        if (n > 0) {
            for (int i = start; i < start + win && i < n; i++) {
                int k = discoveredKeyAt(i);
                s_add("%s%s Lv%d\n", i == resIdx ? "▶ " : "  ", spellName(k), slv(k));
            }
        }
        if (n > 0) {
            int k = discoveredKeyAt(resIdx);
            int lv = slv(k);
            int rate = researchRateFor(k);
            s_add("———\n%s Lv%d→Lv%d\n威力%d→%d\n成功率%d%%\n", spellName(k), lv, lv + 1, spellPower(k), spellPower2(k, lv + 1), rate);
            s_add("耗 月光草x2 兽骨x2\n持有 %d/%d\n", g.herbs, g.mats);
        } else {
            s_add("魔法书还是空的\n");
        }
        lv_label_set_text(g_scene_hint, "OK 研究 · ↓ 切换 · 长按 返回住宅");
        break;
    }
    case SC_FORGE: {
        forgeSel = clampi(forgeSel, 0, 1);
        const char *nm[2] = {"锻造武器", "锻造法袍"};
        for (int i = 0; i < 2; i++) {
            int lv = i == 0 ? g.weaponLv : g.armorLv;
            int gg = 30 * (lv + 1), mm = 2 * (lv + 1);
            s_add("%s %d级 · %d金+兽骨x%d\n  %s\n", i == forgeSel ? "▶" : " ", lv + 1, gg, mm,
                  i == 0 ? "攻击+2" : "生命上限+8");
        }
        lv_label_set_text(g_scene_hint, "OK 锻造 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_MARKET: {
        marketIdx = clampi(marketIdx, 0, 4);
        for (int i = 0; i < 5; i++) {
            int hold = 0;
            switch (MARKET[i].holdField) {
                case 0: hold = g.herbs; break;
                case 1: hold = g.mats; break;
                case 2: hold = g.crystals; break;
                case 3: hold = g.seals; break;
                default: hold = -1; break;
            }
            s_add("%s%s · %d金\n", i == marketIdx ? "▶ " : "  ", MARKET[i].n, MARKET[i].price);
            if (hold >= 0) s_add("  持有 %d\n", hold);
            else s_add("  随机习得魔法\n");
        }
        lv_label_set_text(g_scene_hint, "OK 购买 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_ALCHEMY: {
        alcIdx = clampi(alcIdx, 0, 6);
        int win = 6, start = clampi(alcIdx - 2, 0, 7 - win);
        for (int i = start; i < start + win && i < 7; i++) {
            RecipeDef *r = &RECIPES[i];
            s_add("%s%s\n", i == alcIdx ? "▶ " : "  ", r->n);
            s_add("  耗 ");
            if (r->cH) s_add("草%d ", r->cH);
            if (r->cM) s_add("骨%d ", r->cM);
            if (r->cC) s_add("晶%d", r->cC);
            if (r->makeStone) s_add("→%s", STONE_NAMES[r->makeStone]);
            s_add("\n");
        }
        s_add("草%d 骨%d 晶%d\n", g.herbs, g.mats, g.crystals);
        lv_label_set_text(g_scene_hint, "OK 炼制 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_ACAD: {
        acadIdx = clampi(acadIdx, 0, 2);
        int tc = 20 + g.studyCnt * 15;
        const char *rows[3];
        char buf0[48], buf1[48], buf2[48];
        snprintf(buf0, sizeof(buf0), "魔力修行 · %d金（上限+5）", tc);
        snprintf(buf1, sizeof(buf1), "学属性魔法 · 45金（已学%d/135）", discoveredCount());
        snprintf(buf2, sizeof(buf2), "职业晋升考核%s", promoteReady() ? "（可参加）" : "");
        rows[0] = buf0; rows[1] = buf1; rows[2] = buf2;
        for (int i = 0; i < 3; i++) s_add("%s%s\n", i == acadIdx ? "▶ " : "  ", rows[i]);
        lv_label_set_text(g_scene_hint, "OK 确定 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_PROMOTE: {
        s_add("晋 升 仪 式\n\n");
        s_add("%s\n", g.name);
        s_add("%s →\n", RANKS[g.rank]);
        if (g.rank < 6) s_add("%s\n", RANKS[g.rank + 1]);
        else s_add("魔法帝！\n");
        s_add("「试炼已足」\n");
        lv_label_set_text(g_scene_hint, "OK 接受晋升 · 长按 返回");
        break;
    }
    case SC_CRAFT: {
        craftSlot = clampi(craftSlot, 0, 2);
        const char *slots[3] = {"形态", "元素", "修饰"};
        s_add("%s%s\n", craftSlot == 0 ? "▶" : " ", slots[0]);
        s_add("  %s\n", FORMS[g.spell[0]].n);
        s_add("%s%s\n", craftSlot == 1 ? "▶" : " ", slots[1]);
        s_add("  %s\n", ELEMS[g.spell[1]].n);
        s_add("%s%s\n", craftSlot == 2 ? "▶" : " ", slots[2]);
        s_add("  %s\n", MODS[g.spell[2]].n);
        int key = spell_key(g.spell[0], g.spell[1], g.spell[2]);
        s_add("———\n预览：%s\n", spellName(key));
        s_add("威力%d 耗魔%d\n", spellPower(key), spellCost(key));
        lv_label_set_text(g_scene_hint, "OK 切槽 · ↓ 换项 · 长按 装配");
        break;
    }
    case SC_EQUIP: {
        if (g.nebag == 0) {
            s_add("装备背包是空的\n去锻造/战斗获取\n");
        } else {
            eqSel = clampi(eqSel, 0, g.nebag - 1);
            int win = 6, start = clampi(eqSel - 2, 0, g.nebag - win);
            for (int i = start; i < start + win && i < g.nebag; i++) {
                EbagItem *it = &g.ebag[i];
                s_add("%s[%c]%s", i == eqSel ? "▶" : " ", it->slot, it->name);
                if (it->atk) s_add(" 攻+%d", it->atk);
                else if (it->def) s_add(" 减伤%d", it->def);
                else if (it->pow > 0) s_add(" 法强+%d%%", (int)((it->pow - 1) * 100));
                s_add("\n");
            }
        }
        s_add("———\n");
        const char *sl[3] = {"w", "a", "t"};
        for (int i = 0; i < 3; i++) {
            int idx = (sl[i][0] == 'w') ? 0 : (sl[i][0] == 'a') ? 1 : 2;
            EquipItem *it = &g.equip[idx];
            s_add("%s：%s\n", EQ_SLOTS[idx].n, it->rar >= 0 ? it->name : "空");
        }
        lv_label_set_text(g_scene_hint, "OK 装备/卸下 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_GUILD: {
        guildIdx = clampi(guildIdx, 0, 3);
        for (int i = 0; i < 4; i++) {
            QuestDef *q = &QUESTS[i];
            int lock = g.arank < q->rank;
            int acc = g.accepted == i;
            int prog = questProg(q);
            s_add("%s%s%s\n", i == guildIdx ? "▶ " : "  ", acc ? "✓" : "", q->n);
            if (lock) s_add("  %s级解锁\n", ARANKS[q->rank]);
            else { s_add("  %s · %d金\n", q->sub, q->gold); if (acc) s_add("  进度 %d/%d\n", prog, q->need); }
        }
        s_add("冒险者 %s级 · 完成%d单\n", ARANKS[g.arank], g.questDone);
        lv_label_set_text(g_scene_hint, "OK 接取/交付 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_WILD: {
        ensureDungeonOffers();
        int n = 5 + 1 + g.ndoffers;
        wildIdx = clampi(wildIdx, 0, n - 1);
        for (int i = 0; i < 5; i++) {
            int lock = (i > 0 && !g.bossKilled[i - 1]) || g.level < WILD_REQ[i];
            s_add("%s%s%s\n", i == wildIdx ? "▶ " : "  ", lock ? "🔒" : "", CHAPTERS[i]);
        }
        s_add("%s遗迹秘境\n", wildIdx == 5 ? "▶ " : "  ");
        for (int i = 0; i < g.ndoffers; i++) {
            int id = g.dungeonOffers[i];
            s_add("%s%s · %s\n", (wildIdx == 6 + i) ? "▶ " : "  ", DUNGEONS[id].name, DUNGEONS[id].kind);
        }
        lv_label_set_text(g_scene_hint, "OK 出发/进入 · ↓ 切换 · 长按 返回");
        break;
    }
    case SC_BATTLE: {
        battle_render();
        break;
    }
    case SC_MUPGRADE: {
        int n = discoveredCount();
        if (n == 0) { s_add("魔法书还是空的\n"); lv_label_set_text(g_scene_hint, "长按 返回"); break; }
        muIdx = clampi(muIdx, 0, n - 1);
        int win = 5, start = clampi(muIdx - 2, 0, n - win);
        for (int i = start; i < start + win && i < n; i++) {
            int k = discoveredKeyAt(i);
            int a, e, m; spell_split(k, &a, &e, &m);
            s_add("%s%s(%s) Lv%d\n", i == muIdx ? "▶ " : "  ", spellName(k), TIER_NAMES[m], slv(k));
        }
        if (n > 0) {
            int k = discoveredKeyAt(muIdx);
            int a, e, m; spell_split(k, &a, &e, &m);
            int lv = slv(k), gold = 50 * (lv + 1);
            s_add("———\n%s Lv%d→Lv%d\n威力%d→%d\n", spellName(k), lv, lv + 1, spellPower(k), spellPower2(k, lv + 1));
            if (m > 0) s_add("需%s×1 + %d金\n", STONE_NAMES[m], gold);
            else s_add("需 %d金\n", gold);
        }
        lv_label_set_text(g_scene_hint, "OK 升级 · ↓ 切换 · 长按 返回");
        break;
    }
    default:
        break;
    }

    if (s_ttl[0]) lv_label_set_text(g_scene_title, s_ttl);
    else lv_label_set_text(g_scene_title, SCENE_NAME[g_scene]);
    lv_label_set_text(g_scene_body, s_b);

    /* toast 显隐 */
    if (now_ms() < g_toast_until) {
        lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_toast, g_toast_buf);
    } else {
        lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 辅助：背包/魔法计数 ===================== */
static int g_str_field(const char *name) {
    if (!strcmp(name, "orbs")) return g.orbs;
    if (!strcmp(name, "seals")) return g.seals;
    if (!strcmp(name, "herbs")) return g.herbs;
    if (!strcmp(name, "mats")) return g.mats;
    if (!strcmp(name, "crystals")) return g.crystals;
    if (!strcmp(name, "potions")) return g.potions;
    return 0;
}
static void bagItemAt(int idx, char *cat, char *nm, int *cnt, int *col) {
    int cur = 0; int found = 0;
    for (int id = 0; id < 20 && !found; id++) if (g.pbag[id] > 0) {
        if (cur++ == idx) { strncpy(cat, "药品", 12); strncpy(nm, POTIONS[id].n, 24); *cnt = g.pbag[id]; found = 1; break; }
    }
    for (int id = 0; id < 40 && !found; id++) if (g.mbag[id] > 0) {
        if (cur++ == idx) { strncpy(cat, "材料", 12); strncpy(nm, MATERIALS[id].n, 24); *cnt = g.mbag[id]; found = 1; break; }
    }
    for (int i = 1; i <= 4 && !found; i++) if (stoneN(i) > 0) {
        if (cur++ == idx) { strncpy(cat, "魔石", 12); strncpy(nm, STONE_NAMES[i], 24); *cnt = stoneN(i); found = 1; break; }
    }
    const char *props[6] = {"orbs","seals","herbs","mats","crystals","potions"};
    for (int i = 0; i < 6 && !found; i++) if (g_str_field(props[i]) > 0) {
        if (cur++ == idx) {
            strncpy(cat, "道具", 12);
            const char *nms[6] = {"捕捉球","封印卡","月光草","兽骨","魔晶","体力药水"};
            strncpy(nm, nms[i], 24); *cnt = g_str_field(props[i]); found = 1; break;
        }
    }
}
static int researchRateFor(int key) {
    int lv = slv(key);
    int r = 85 - 15 * lv; if (r < 25) r = 25; return r;
}

/* ===================== 按键处理 ===================== */
static void doEvolve(Partner *p) {
    MobDef *bd = &ENEMY[p->typeIdx];
    if (!bd->hasEvo) { toast("这只伙伴无法进化"); return; }
    p->evolved = 1;
    int nt = bd->evo.rarityTo; if (nt <= 0) nt = 4;
    p->rar = (p->rar + 1 <= nt) ? p->rar + 1 : nt;
    strncat(p->name, bd->evo.title, sizeof(p->name) - strlen(p->name) - 1);
    for (int i = 0; i < 3; i++) { int v = p->ivs[i]; if (v < 0) v = 11; if (v < 15) v++; p->ivs[i] = v; }
    mage_persist();
    toast("进化！%s 资质提升", p->name);
}

static void scene_handle(int act) {
    switch (g_scene) {
    case SC_TITLE:
        if (act == ACT_PREV || act == ACT_NEXT) { g_titleSel = g_titleSel ? 0 : 1; g_need_refresh = 1; }
        else if (act == ACT_OK) {
            if (g_titleSel == 1 || !g_hasSave) { mage_new_game(); mage_set_scene(SC_NAME); }
            else { mage_load_from_nvs(); mage_set_scene(SC_CITY); g.cityMode = 0; toast("欢迎回来，%s", g.name); }
        }
        break;
    case SC_NAME:
        if (act == ACT_PREV || act == ACT_NEXT) { g_nameIdx = (g_nameIdx + (act == ACT_NEXT ? 1 : 6)) % 7; g_need_refresh = 1; }
        else if (act == ACT_OK) {
            strncpy(g.name, NAMES[g_nameIdx % 7], sizeof(g.name) - 1); g.name[sizeof(g.name) - 1] = '\0';
            mage_persist(); mage_set_scene(SC_CITY); g.cityMode = 0; toast("%s，开始魔法之旅！", g.name);
        } else if (act == ACT_BACK) { mage_set_scene(SC_TITLE); }
        break;
    case SC_CITY:
        if (g.cityMode == 0) {
            if (act == ACT_NEXT) { locSel = (locSel + 1) % 7; g_need_refresh = 1; }
            else if (act == ACT_PREV) { locSel = (locSel + 6) % 7; g_need_refresh = 1; }
            else if (act == ACT_OK) { mage_set_scene(LOC_SCENE[locSel]); }
            else if (act == ACT_BACK) { g.cityMode = 1; g_need_refresh = 1; }
        } else {
            if (act == ACT_NEXT) { funcSel = (funcSel + 1) % 8; g_need_refresh = 1; }
            else if (act == ACT_PREV) { funcSel = (funcSel + 7) % 8; g_need_refresh = 1; }
            else if (act == ACT_OK) { mage_set_scene(FUNC_SCENE[funcSel]); }
            else if (act == ACT_BACK) { g.cityMode = 0; g_need_refresh = 1; }
        }
        break;
    case SC_STATUS:
        if (act == ACT_NEXT) { mage_set_scene(SC_EQUIP); }
        else if (act == ACT_OK || act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        break;
    case SC_BAG:
        if (act == ACT_OK || act == ACT_NEXT || act == ACT_PREV) { bagIdx++; g_need_refresh = 1; }
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }
        break;
    case SC_DEX:
        if (act == ACT_NEXT) { dexIdx = (dexIdx + 1) % DEXORDER_COUNT; g_need_refresh = 1; }
        else if (act == ACT_PREV) { dexIdx = (dexIdx + DEXORDER_COUNT - 1) % DEXORDER_COUNT; g_need_refresh = 1; }
        else if (act == ACT_OK) {
            if (!g.dexSeen[dexIdx]) { toast("尚未发现该魔物"); break; }
            MobDef *bd = &ENEMY[enemyIndex(DEXORDER[dexIdx])];
            if (bd->capturable) {
                const char *tool = bd->cap.tool && strcmp(bd->cap.tool, "seal") == 0 ? "魔法封印卡" : "捕捉球";
                if (bd->hasCap && bd->cap.hp > 0) toast("出没：%s｜用%s（血<%d%%）", speciesZones(dexIdx), tool, (int)(bd->cap.hp * 100));
                else toast("出没：%s｜用%s收服", speciesZones(dexIdx), tool);
            } else toast("该魔物无法被收服");
        } else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }
        break;
    case SC_BOOK: {
        int n = discoveredCount(), total = n + 1;
        if (act == ACT_NEXT) bookIdx = (bookIdx + 1) % total;
        else if (act == ACT_PREV) bookIdx = (bookIdx + total - 1) % total;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }
        else if (act == ACT_OK) {
            if (bookIdx < n) {
                int k = discoveredKeyAt(bookIdx);
                int inlo = 0, pos = -1;
                for (int i = 0; i < g.nload; i++) if (g.loadout[i] == k) { inlo = 1; pos = i; }
                if (inlo) { for (int i = pos; i < g.nload - 1; i++) g.loadout[i] = g.loadout[i + 1]; g.nload--; if (g.equipped == k) g.equipped = g.nload ? g.loadout[0] : k; toast("移出出战：%s", spellName(k)); }
                else { if (g.nload >= 4) { toast("出战已满4个"); break; } g.loadout[g.nload++] = k; g.equipped = k; toast("加入出战：%s", spellName(k)); }
                mage_persist();
            } else { mage_set_scene(SC_CRAFT); }
        }
        g_need_refresh = 1;
        break;
    }
    case SC_PARTNERS: {
        if (g.npartners == 0) { if (act == ACT_BACK || act == ACT_OK) { mage_set_scene(SC_CITY); g.cityMode = 1; } break; }
        partIdx = clampi(partIdx, 0, g.npartners - 1);
        Partner *p = &g.partners[partIdx];
        MobDef *bd = &ENEMY[p->typeIdx];
        if (partRelConf) {
            if (act == ACT_OK) {
                /* 收敛出战位 */
                if (g.activePartner >= g.npartners) g.activePartner = 0;
                /* 删除伙伴 */
                for (int i = partIdx; i < g.npartners - 1; i++) g.partners[i] = g.partners[i + 1];
                g.npartners--; g.cats = g.npartners;
                if (g.activePartner >= g.npartners) g.activePartner = 0;
                if (partIdx >= g.npartners) partIdx = 0;
                partRelConf = 0; partMenuOpen = 0; mage_persist();
                toast("已放生 %s", p->name);
            } else if (act == ACT_BACK) { partRelConf = 0; }
            g_need_refresh = 1; break;
        }
        if (partMenuOpen) {
            int nopts = 0;
            if (bd->hasEvo && !p->evolved && p->level >= bd->evo.lvl) nopts++;
            nopts += 3;
            if (act == ACT_NEXT) partMenu = (partMenu + 1) % nopts;
            else if (act == ACT_PREV) partMenu = (partMenu + nopts - 1) % nopts;
            else if (act == ACT_OK) {
                int oi = 0; int evoOpt = -1;
                if (bd->hasEvo && !p->evolved && p->level >= bd->evo.lvl) { evoOpt = oi++; }
                if (partMenu == evoOpt) { doEvolve(p); partMenuOpen = 0; }
                else if (partMenu == evoOpt + 1) { partRelConf = 1; }
                else if (partMenu == evoOpt + 2) { partMenuOpen = 0; mage_set_scene(SC_CITY); g.cityMode = 1; }
                else { partMenuOpen = 0; }
            } else if (act == ACT_BACK) { partMenuOpen = 0; }
            g_need_refresh = 1; break;
        }
        if (act == ACT_OK) { g.activePartner = partIdx; mage_persist(); toast("出战：%s", p->name); }
        else if (act == ACT_NEXT) { partIdx = (partIdx + 1) % g.npartners; }
        else if (act == ACT_PREV) { partIdx = (partIdx + g.npartners - 1) % g.npartners; }
        else if (act == ACT_BACK) { partMenuOpen = 1; partMenu = 0; }
        g_need_refresh = 1;
        break;
    }
    case SC_MENU:
        if (act == ACT_NEXT) menuIdx = (menuIdx + 1) % 7;
        else if (act == ACT_PREV) menuIdx = (menuIdx + 6) % 7;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }
        else if (act == ACT_OK) {
            switch (menuIdx) {
                case 0: g.autoMode = !g.autoMode; toast("自动战斗 %s", g.autoMode ? "开" : "关"); break;
                case 1: g.autoCapture = !g.autoCapture; toast("挂机自动捕捉 %s", g.autoCapture ? "开" : "关"); break;
                case 2: g.sound = !g.sound; toast("音效 %s", g.sound ? "开" : "关"); break;
                case 3: g.diff = (g.diff + 1) % 4; toast("难度→%s", diffC()->n); break;
                case 4: toast("设备端暂不支持导入导出"); break;
                case 5: toast("设备端暂不支持导入导出"); break;
                case 6: mage_new_game(); mage_set_scene(SC_NAME); toast("已重置"); break;
            }
            mage_persist();
        }
        g_need_refresh = 1;
        break;
    case SC_HOME:
        if (act == ACT_NEXT) homeSel = (homeSel + 1) % 3;
        else if (act == ACT_PREV) homeSel = (homeSel + 2) % 3;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            if (homeSel == 0) { g.hp = g.maxHp; g.mp = g.maxMp; mage_persist(); toast("睡了一觉，状态全满"); }
            else if (homeSel == 1) { alcIdx = 0; mage_set_scene(SC_ALCHEMY); }
            else { resIdx = 0; mage_set_scene(SC_RESEARCH); }
        }
        g_need_refresh = 1;
        break;
    case SC_RESEARCH: {
        int n = discoveredCount();
        if (n == 0) { if (act == ACT_BACK) { mage_set_scene(SC_HOME); } break; }
        if (act == ACT_NEXT) resIdx = (resIdx + 1) % n;
        else if (act == ACT_PREV) resIdx = (resIdx + n - 1) % n;
        else if (act == ACT_BACK) { mage_set_scene(SC_HOME); }
        else if (act == ACT_OK) {
            if (g.herbs < 2 || g.mats < 2) { toast("需要 月光草x2 + 兽骨x2"); break; }
            int k = discoveredKeyAt(resIdx); int lv = slv(k);
            g.herbs -= 2; g.mats -= 2;
            if (mage_ri(0, 99) < researchRateFor(k)) { slvSet(k, lv + 1); toast("研究成功！%s→Lv%d", spellName(k), lv + 1); }
            else { toast("研究失败…材料已消耗"); }
            mage_persist();
        }
        g_need_refresh = 1;
        break;
    }
    case SC_FORGE:
        if (act == ACT_NEXT) forgeSel = (forgeSel + 1) % 2;
        else if (act == ACT_PREV) forgeSel = (forgeSel + 1) % 2;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            int lv = forgeSel == 0 ? g.weaponLv : g.armorLv;
            int gg = 30 * (lv + 1), mm = 2 * (lv + 1);
            if (g.gold < gg || g.mats < mm) { toast("材料不足"); break; }
            g.gold -= gg; g.mats -= mm;
            if (forgeSel == 0) { g.weaponLv++; g.atkBonus += 2; toast("锻造成功！攻击+%d", g.atkBonus); }
            else { g.armorLv++; g.maxHp += 8; g.hp += 8; toast("锻造成功！生命上限%d", g.maxHp); }
            mage_persist();
        }
        g_need_refresh = 1;
        break;
    case SC_MARKET:
        if (act == ACT_NEXT) marketIdx = (marketIdx + 1) % 5;
        else if (act == ACT_PREV) marketIdx = (marketIdx + 4) % 5;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            MarketDef *m = &MARKET[marketIdx];
            if (g.gold < m->price) { toast("金币不足"); break; }
            g.gold -= m->price;
            if (m->holdField == 0) g.herbs++;
            else if (m->holdField == 1) g.mats++;
            else if (m->holdField == 2) g.crystals++;
            else if (m->holdField == 3) g.seals++;
            else {
                int k; do { k = spell_key(mage_ri(0,2), mage_ri(0,8), mage_ri(0,4)); } while (g.discovered[k]);
                g.discovered[k] = 1; mage_persist(); toast("习得：%s", spellName(k));
            }
            if (m->holdField >= 0 && m->holdField <= 3) mage_persist();
        }
        g_need_refresh = 1;
        break;
    case SC_ALCHEMY:
        if (act == ACT_NEXT) alcIdx = (alcIdx + 1) % 7;
        else if (act == ACT_PREV) alcIdx = (alcIdx + 6) % 7;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            RecipeDef *r = &RECIPES[alcIdx];
            if (!mage_can_craft(r)) { toast("材料不足"); break; }
            g.herbs -= r->cH; g.mats -= r->cM; g.crystals -= r->cC;
            if (r->makeStone > 0) { stoneAdd(r->makeStone, 1); toast("炼成：%s", r->n); }
            else if (!strcmp(r->outName, "potions")) { g.potions++; toast("炼成：体力药水"); }
            else if (!strcmp(r->outName, "orbs")) { g.orbs++; toast("炼成：捕捉球"); }
            else if (!strcmp(r->outName, "seals")) { g.seals++; toast("炼成：魔法封印卡"); }
            mage_persist();
        }
        g_need_refresh = 1;
        break;
    case SC_ACAD:
        if (act == ACT_NEXT) acadIdx = (acadIdx + 1) % 3;
        else if (act == ACT_PREV) acadIdx = (acadIdx + 2) % 3;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            if (acadIdx == 0) {
                int tc = 20 + g.studyCnt * 15;
                if (g.gold < tc) { toast("金币不足"); break; }
                g.gold -= tc; g.studyCnt++; g.maxMp += 5; g.mp = g.maxMp; mage_persist(); toast("魔力上限%d", g.maxMp);
            } else if (acadIdx == 1) {
                if (g.gold < 45) { toast("金币不足"); break; }
                if (discoveredCount() >= 135) { toast("已学完全部魔法"); break; }
                g.gold -= 45;
                int k; do { k = spell_key(mage_ri(0,2), mage_ri(0,8), mage_ri(0,4)); } while (g.discovered[k]);
                g.discovered[k] = 1; mage_persist(); toast("习得新魔法：%s", spellName(k));
            } else {
                if (promoteReady()) { mage_set_scene(SC_PROMOTE); }
                else toast("需击杀%d/%d 且委托%d/%d", g.kills, RANK_KILLS[g.rank], g.questDone, g.rank + 1);
            }
        }
        g_need_refresh = 1;
        break;
    case SC_PROMOTE:
        if (act == ACT_OK) {
            if (g.rank < 6) { g.rank++; g.kills = 0; g.hp = g.maxHp; g.mp = g.maxMp; mage_persist(); toast("晋升为 %s！", RANKS[g.rank]); }
            mage_set_scene(SC_CITY); g.cityMode = 0;
        } else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        break;
    case SC_CRAFT:
        if (act == ACT_NEXT) { g.spell[craftSlot] = (g.spell[craftSlot] + 1) % spellModLen(craftSlot); g_need_refresh = 1; }
        else if (act == ACT_PREV) { g.spell[craftSlot] = (g.spell[craftSlot] + spellModLen(craftSlot) - 1) % spellModLen(craftSlot); g_need_refresh = 1; }
        else if (act == ACT_OK) { craftSlot = (craftSlot + 1) % 3; g_need_refresh = 1; }
        else if (act == ACT_BACK) {
            int key = spell_key(g.spell[0], g.spell[1], g.spell[2]);
            g.discovered[key] = 1; if (g.equipped < 0 || !g.discovered[g.equipped]) g.equipped = key;
            mage_persist();
            bookIdx = 0; int n = discoveredCount(); for (int i = 0; i < n; i++) if (discoveredKeyAt(i) == key) bookIdx = i;
            mage_set_scene(SC_BOOK); toast("录入魔法书：%s", spellName(key));
        }
        break;
    case SC_EQUIP:
        if (g.nebag == 0) { if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; } break; }
        if (act == ACT_NEXT) eqSel = (eqSel + 1) % g.nebag;
        else if (act == ACT_PREV) eqSel = (eqSel + g.nebag - 1) % g.nebag;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }
        else if (act == ACT_OK) {
            EbagItem *it = &g.ebag[eqSel];
            int idx = (it->slot == 'w') ? 0 : (it->slot == 'a') ? 1 : 2;
            EquipItem old = g.equip[idx];
            g.equip[idx] = (EquipItem){ it->slot, it->rar, it->atk, it->def, it->pow, "" };
            strncpy(g.equip[idx].name, it->name, sizeof(g.equip[idx].name) - 1);
            /* 从背包移除 it，旧装备放回 */
            for (int i = eqSel; i < g.nebag - 1; i++) g.ebag[i] = g.ebag[i + 1];
            g.nebag--;
            if (old.rar >= 0) {
                EbagItem e; e.slot = old.slot; e.rar = old.rar; e.atk = old.atk; e.def = old.def; e.pow = old.pow;
                strncpy(e.name, old.name, sizeof(e.name) - 1);
                if (g.nebag < 24) g.ebag[g.nebag++] = e;
            }
            if (eqSel >= g.nebag) eqSel = 0;
            mage_persist(); toast("已装备 %s", it->name);
        }
        g_need_refresh = 1;
        break;
    case SC_GUILD:
        if (act == ACT_NEXT) guildIdx = (guildIdx + 1) % 4;
        else if (act == ACT_PREV) guildIdx = (guildIdx + 3) % 4;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            QuestDef *q = &QUESTS[guildIdx];
            if (g.arank < q->rank) { toast("需冒险者 %s级", ARANKS[q->rank]); break; }
            if (g.accepted < 0) { g.accepted = guildIdx; g.qBase = g.kills; mage_persist(); toast("接取：%s", q->n); break; }
            if (g.accepted != guildIdx) { toast("先完成进行中委托"); break; }
            if (questProg(q) < q->need) { toast("委托未完成"); break; }
            if (q->type[0] == 'h') g.herbs -= q->need;
            g.gold += q->gold;
            if (q->orbs) g.orbs += q->orbs;
            if (q->potions) g.potions += q->potions;
            if (q->crystals) g.crystals += q->crystals;
            g.accepted = -1; g.questDone++;
            if (g.questDone % 2 == 0 && g.arank < 6) { g.arank++; toast("冒险者等级→%s", ARANKS[g.arank]); }
            mage_persist(); toast("委托完成！+%d金", q->gold);
        }
        g_need_refresh = 1;
        break;
    case SC_WILD: {
        ensureDungeonOffers();
        int n = 5 + 1 + g.ndoffers;
        if (act == ACT_NEXT) wildIdx = (wildIdx + 1) % n;
        else if (act == ACT_PREV) wildIdx = (wildIdx + n - 1) % n;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 0; }
        else if (act == ACT_OK) {
            if (wildIdx < 5) {
                if (wildIdx > 0 && !g.bossKilled[wildIdx - 1]) { toast("先击败%s的BOSS", CHAPTERS[wildIdx - 1]); break; }
                if (g.level < WILD_REQ[wildIdx]) { toast("建议Lv%d以上", WILD_REQ[wildIdx]); break; }
                if (!g.storySeen[wildIdx]) { g.storySeen[wildIdx] = 1; toast("%s", STORY_ENTER[wildIdx]); }
                g.mapChapter = wildIdx; mage_persist();
                battle_start_wild(wildIdx);
            } else if (wildIdx == 5) {
                if (g.rank < 1) { toast("晋升初级魔法师后开放"); break; }
                battle_start_dungeon(-1);
            } else {
                int id = g.dungeonOffers[wildIdx - 6];
                battle_start_dungeon(id);
            }
        }
        g_need_refresh = 1;
        break;
    }
    case SC_BATTLE:
        battle_handle(act);
        break;
    case SC_MUPGRADE: {
        int n = discoveredCount();
        if (n == 0) { if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; } break; }
        if (act == ACT_NEXT) muIdx = (muIdx + 1) % n;
        else if (act == ACT_PREV) muIdx = (muIdx + n - 1) % n;
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }
        else if (act == ACT_OK) {
            int k = discoveredKeyAt(muIdx); int a, e, m; spell_split(k, &a, &e, &m);
            int lv = slv(k);
            if (lv >= 10) { toast("%s 已达Lv10", spellName(k)); break; }
            int gold = 50 * (lv + 1);
            if (g.gold < gold) { toast("金币不足（需%d）", gold); break; }
            if (m > 0 && stoneN(m) < 1) { toast("缺少%s×1", STONE_NAMES[m]); break; }
            g.gold -= gold; if (m > 0) stoneAdd(m, -1);
            slvSet(k, lv + 1); mage_persist(); toast("%s→Lv%d", spellName(k), lv + 1);
        }
        g_need_refresh = 1;
        break;
    }
    default:
        break;
    }
}

/* ===================== 主循环 ===================== */
static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    int act = -1;
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_OK) act = ACT_OK;
        else if (btn == BSP_BTN_DOWN) act = ACT_NEXT;
        else if (btn == BSP_BTN_UP) act = ACT_PREV;
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) act = ACT_BACK;
    }
    if (act < 0) return;
    scene_handle(act);
}

static void game_task(void *arg) {
    (void)arg;
    for (;;) {
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);

        uint32_t now = now_ms();
        if (bt.active) battle_tick();
        if (now - g_last_save_ms > 30000) { mage_persist(); g_last_save_ms = now; }

        if (g_need_refresh || g_scene != g_last_scene || now - g_last_ui_ms >= UI_REFRESH_MS) {
            g_last_ui_ms = now;
            g_last_scene = g_scene;
            g_need_refresh = 0;
            if (bsp_lvgl_lock(100)) { refresh_ui(); bsp_lvgl_unlock(); }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void mage_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_btn_q) return;
    const btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ===================== 战斗引擎（M3，1:1 复刻 HTML 战斗逻辑） ===================== */
#define BP_MENU 0
#define BP_MAGIC 1
#define BP_ITEM  2
#define BP_ENEMY 3
#define BP_RESULT 4
#define BP_EVENT 5
#define BP_FAINT 6
#define BTPX 70

static float fclamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static void set_bphase(int p) { bt.phase = p; bt.tickMs = now_ms(); }

/* 取最近敌人（模拟横坐标判定，与 HTML engage/range 一致） */
static BattleEnemy *nearest_enemy(int range) {
    BattleEnemy *best = NULL; int bd = 2000000000;
    for (int i = 0; i < bt.enemyCount; i++) {
        BattleEnemy *e = &bt.enemies[i];
        if (e->hp <= 0) continue;
        int d = e->x - BTPX;
        if (d > -6 && d < range && d < bd) { bd = d; best = e; }
    }
    return best;
}

/* ---------- 捕捉判定 ---------- */
static int can_capture(BattleEnemy *e) {
    if (e->boss) return 0;
    const MobDef *bd = &ENEMY[e->typeIdx];
    if (!bd->hasCap) return 0;
    float ratio = (float)e->hp / (float)e->maxHp;
    if (bd->cap.hp > 0 && ratio > bd->cap.hp) return 0;        /* 需压到指定血线以下 */
    if (strcmp(bd->cap.tool, "seal") == 0) { if (g.seals <= 0) return 0; }
    else { if (g.orbs <= 0) return 0; }
    return 1;
}
static BattleEnemy *capture_target(void) {
    BattleEnemy *best = NULL; int bestScore = 2000000000;
    for (int i = 0; i < bt.enemyCount; i++) {
        BattleEnemy *e = &bt.enemies[i];
        if (e->hp <= 0 || !can_capture(e)) continue;
        int uncaught = g.dexCaught[e->typeIdx] ? 0 : 1;
        int score = (uncaught ? 0 : 100)
                   + (strcmp(ENEMY[e->typeIdx].type, "cat") == 0 ? 12 : 0)
                   + (int)((float)e->hp / (float)e->maxHp * 100);
        if (score < bestScore) { bestScore = score; best = e; }
    }
    return best;
}
static int capture_chance(BattleEnemy *e, int seal) {
    float ratio = (float)e->hp / (float)e->maxHp;
    float p;
    if (seal) p = fclamp(0.75f + (1 - ratio) * 0.2f, 0.75f, 0.95f);
    else      p = fclamp(RARITY[e->rarity].cap * (1.45f - ratio) / 1.15f, 0.05f, 0.95f);
    Partner *pr = NULL;
    if (g.npartners > 0 && g.activePartner >= 0 && g.activePartner < g.npartners) pr = &g.partners[g.activePartner];
    if (pr) {
        if (pr->traitIdx == traitIndex("lucky")) p += 0.10f;
        if (pr->traitIdx == traitIndex("flex"))  p += 0.15f;
    }
    if (p < 0.02f) p = 0.02f; if (p > 0.98f) p = 0.98f;
    return (int)(p * 100);
}

/* ---------- 前向声明（战斗函数互相引用，集中声明避免隐式声明报错） ---------- */
static void kill_award(BattleEnemy *e);
static void reap_dead(void);
static void after_player_action(void);
static void win_battle(void);
static void spawn_boss(void);
static void finish_crawl(void);
static void finish_expedition(void);
static void throw_orb(int seal);

/* ---------- 伤害/击杀 ---------- */
static void deal_to(BattleEnemy *e, int dmg) {
    if (e->brokenTurns > 0) dmg = (int)(dmg * 1.4f + 0.5f);   /* 破防易伤 ×1.4 */
    e->hp -= dmg; if (e->hp < 0) e->hp = 0;
    if (e->boss && !e->enraged && e->hp > 0 && e->hp <= (int)(e->maxHp * 0.65f)) {
        e->enraged = 1; e->atk = (int)(e->atk * 1.6f + 0.5f);
        e->bossCD = e->bossCD < 2 ? e->bossCD : 2;
        toast("🔥 %s 狂暴了！攻击大幅提升", ENEMY[e->typeIdx].n);
    }
    if (ENEMY[e->typeIdx].hasCap && can_capture(e)) {           /* 手下留情锁血 */
        int fl = (int)(e->maxHp * 0.08f + 0.5f); if (fl < 1) fl = 1;
        if (e->hp <= fl) { e->hp = fl; if (!e->mercy) { e->mercy = 1; e->mercyTurns = 0; toast("手下留情！%s 留了残血，可捕捉", ENEMY[e->typeIdx].n); } }
    }
    /* 击杀回收统一在 reap_dead() 中进行（deal_to 只管扣血，避免在伤害循环里移动数组导致悬垂指针） */
}
static void partner_gain(int gxp) {
    if (g.npartners <= 0 || g.activePartner < 0 || g.activePartner >= g.npartners) return;
    Partner *p = &g.partners[g.activePartner];
    p->exp += gxp;
    int need = 10 + p->level * 4;
    while (p->exp >= need) {
        p->exp -= need; p->level++;
        if (mage_rf(0, 1) < 0.6f) { int i = mage_ri(0, 2); if (p->ivs[i] < 15) p->ivs[i]++; }
        need = 10 + p->level * 4;
    }
}
static void check_level(void) {
    while (g.exp >= expNeed()) {
        g.exp -= expNeed(); g.level++;
        g.maxHp += 6; g.maxMp += 3; g.hp = g.maxHp; g.mp = g.maxMp;
        toast("升级！Lv%d", g.level);
    }
}
static void kill_award(BattleEnemy *e) {
    const MobDef *bd = &ENEMY[e->typeIdx];
    int m = e->boss ? 9 : (e->elite ? 3 : 1);
    if (e->boss) {
        EbagItem it; rollEquip(1, &it);
        if (g.nebag < 24) { g.ebag[g.nebag] = it; g.nebag++; }
        g.crystals += 2; g.orbs += 2; g.seals += 1;
        strncpy(bt.bossEquip, it.name, sizeof(bt.bossEquip) - 1); bt.bossEquip[sizeof(bt.bossEquip) - 1] = '\0';
        bt.bossCrystal = 2; bt.bossOrb = 2; bt.bossSeal = 1;
        int ck = g.mapChapter; if (ck < 0) ck = 0; if (ck > 4) ck = 4;
        g.bossKilled[ck] = 1;
        if (!bt.storyShown && g.mapChapter < 4) { bt.storyShown = 1; toast("主线推进：%s 已解锁！", CHAPTERS[g.mapChapter + 1]); }
        mage_persist();
    }
    int gd = mage_ri(bd->gold[0], bd->gold[1]) * m;
    int ed = bd->exp * m;
    Partner *ap = NULL;
    if (g.npartners > 0 && g.activePartner >= 0 && g.activePartner < g.npartners) ap = &g.partners[g.activePartner];
    if (ap && ap->traitIdx == traitIndex("greedy")) gd = (int)(gd * 1.2f + 0.5f);
    g.gold += gd; bt.goldGain += gd; g.exp += ed; bt.expGain += ed;
    bt.killCnt++; g.kills++;
    if (mage_rf(0, 1) < 0.18) { int h = mage_ri(1, 2) * m; g.herbs += h; bt.herbGain += h; }
    if (mage_rf(0, 1) < 0.14) { int mm = mage_ri(1, 2) * m; g.mats += mm; bt.matGain += mm; }
    if (mage_rf(0, 1) < 0.10) { g.potions++; bt.potGain++; }
    partner_gain((int)(ed * 0.4f));
    check_level();
}
/* 一次性回收所有 HP<=0 的敌人（在整套伤害结算完后调用，避免伤害循环中移动数组导致悬垂指针） */
static void reap_dead(void) {
    for (int i = 0; i < bt.enemyCount; ) {
        if (bt.enemies[i].hp <= 0) {
            kill_award(&bt.enemies[i]);
            for (int j = i; j < bt.enemyCount - 1; j++) bt.enemies[j] = bt.enemies[j + 1];
            bt.enemyCount--;
        } else i++;
    }
}

/* ---------- 玩家行动 ---------- */
static void hero_strike(void) {
    BattleEnemy *e = nearest_enemy(175);
    if (!e) {
        if (bt.enemyCount > 0) toast("敌人还没走近…");
        else win_battle();
        return;
    }
    int dmg = (int)((9 + g.level * 3 + g.rank * 3 + g.atkBonus + eqAtk()) * mage_rf(0.9f, 1.1f) + 0.5f);
    if (dmg < 1) dmg = 1;
    deal_to(e, dmg);
    reap_dead();
    after_player_action();
}
static void partner_strike(void) {
    if (g.npartners <= 0 || g.activePartner < 0 || g.activePartner >= g.npartners) return;
    Partner *p = &g.partners[g.activePartner];
    BattleEnemy *target = nearest_enemy(175);
    if (!target) return;
    int hits = 1;
    if (p->skillIdx == skillIndex("swift") && mage_rf(0, 1) < 0.5f) hits = 2;
    if (p->traitIdx == traitIndex("nimble") && mage_rf(0, 1) < 0.2f) hits = 2;
    float mul = 1.0f;
    if (p->skillIdx == skillIndex("claw"))  mul *= 1.15f;
    if (p->skillIdx == skillIndex("ember")) mul *= 1.10f;
    if (p->skillIdx == skillIndex("frost")) mul *= 1.05f;
    if (p->traitIdx == traitIndex("fierce")) mul *= 1.12f;
    if (p->skillIdx == skillIndex("guard")) { bt.guardBlock += 0.3f; if (bt.guardBlock > 0.6f) bt.guardBlock = 0.6f; }
    for (int h = 0; h < hits; h++) {
        int a = (int)((8 + p->ivs[0] + p->level * 2) * mul * mage_rf(0.9f, 1.1f) + 0.5f);
        if (a < 1) a = 1;
        deal_to(target, a);
        if (p->skillIdx == skillIndex("venom") && target->hp > 0) { if (target->poisonTurns < 4) target->poisonTurns = 4; }
        if (p->skillIdx == skillIndex("leech")) {
            int heal = (int)((8 + p->ivs[0]) * 0.2f + 0.5f);
            g.hp += heal; if (g.hp > g.maxHp) g.hp = g.maxHp;
        }
    }
    reap_dead();
}
static void magic_cast(void) {
    int key = g.equipped;
    int cost = spellCost(key);
    if (g.mp < cost) { toast("魔力不足"); return; }
    g.mp -= cost;
    int a, e2, m; spell_split(key, &a, &e2, &m);
    int range = FORMS[a].range;
    int pow = (int)(spellPower(key) * eqPow() + 0.5f);
    BattleEnemy *hits[6]; int nh = 0;
    for (int i = 0; i < bt.enemyCount; i++) {
        BattleEnemy *en = &bt.enemies[i];
        if (en->hp <= 0) continue;
        int d = en->x - BTPX;
        if (d > -8 && d < range) hits[nh++] = en;       /* 已按 x 升序 */
    }
    int take = nh;
    if (a == 0) take = nh > 0 ? 1 : 0;                 /* 弹：单体 */
    else if (m == 3) take = nh > 3 ? 3 : nh;           /* 古代：最多3 */
    if (take == 0) { toast("空放了…"); after_player_action(); return; }
    for (int i = 0; i < take; i++) {
        BattleEnemy *en = hits[i];
        int wk = 0; for (int j = 0; j < ENEMY[en->typeIdx].nWeak; j++) if (ENEMY[en->typeIdx].weakOf[j] == e2) wk = 1;
        int resisted = (en->resist[0] >= 0 && (en->resist[0] == e2 || en->resist[1] == e2)) ? 1 : 0;
        int dmg = (int)(pow * mage_rf(0.9f, 1.15f) + 0.5f); if (dmg < 1) dmg = 1;
        if (resisted) { dmg = (int)(dmg * 0.45f + 0.5f); en->resistKnown = 1; toast("抗性!"); }
        if (wk) {
            dmg = (int)(dmg * 1.5f + 0.5f);
            if (en->shield > 0 && en->brokenTurns <= 0) {
                en->shield--;
                if (en->shield <= 0) { en->brokenTurns = 3; toast("破防！%s 硬直，受伤×1.4", ENEMY[en->typeIdx].n); }  /* HTML 原文 brokenT=4.5s（实时引擎）；回合制下等价取 3 个敌方回合 */
            }
        }
        deal_to(en, dmg);
    }
    reap_dead();
    after_player_action();
}
static int item_opt_count(void) {
    int c = 0;
    if (g.potions > 0) c++;
    if (g.orbs > 0) c++;
    if (g.seals > 0) c++;
    return c;
}
static int item_opt_kind(int i) {   /* 0=药水 1=球 2=封印卡（按可用顺序） */
    int k = 0;
    if (g.potions > 0) { if (k == i) return 0; k++; }
    if (g.orbs > 0)    { if (k == i) return 1; k++; }
    if (g.seals > 0)   { if (k == i) return 2; k++; }
    return -1;
}
static void use_item(void) {
    int kind = item_opt_kind(bt.itemIdx);
    if (kind == 0) {
        if (g.potions <= 0) { toast("没有药水了"); return; }
        if (g.hp >= g.maxHp) { toast("状态很好"); return; }
        g.potions--;
        g.hp += (int)(g.maxHp * 0.4f + 0.5f); if (g.hp > g.maxHp) g.hp = g.maxHp;
        g.mp += (int)(g.maxMp * 0.3f + 0.5f); if (g.mp > g.maxMp) g.mp = g.maxMp;
        after_player_action();
    } else if (kind == 1 || kind == 2) {
        throw_orb(kind == 2 ? 1 : 0);
        after_player_action();
    }
}
static void throw_orb(int seal) {   /* seal: 1=封印卡 0=球 -1=auto */
    BattleEnemy *e = capture_target();
    if (!e) { toast("没有可捕捉的魔物"); return; }
    if (seal < 0) {
        const MobDef *bd = &ENEMY[e->typeIdx];
        seal = (strcmp(bd->cap.tool, "seal") == 0) ? 1 : 0;
    }
    if (seal) { if (g.seals <= 0) { toast("没有魔法封印卡！"); return; } g.seals--; }
    else      { if (g.orbs <= 0)  { toast("没有捕捉球！"); return; } g.orbs--; }
    int p = capture_chance(e, seal);
    if (mage_rf(0, 1) * 100 < p) {                                 /* 成功 */
        int idx = (int)(e - &bt.enemies[0]);
        for (int i = idx; i < bt.enemyCount - 1; i++) bt.enemies[i] = bt.enemies[i + 1];
        bt.enemyCount--;
        const MobDef *bd = &ENEMY[e->typeIdx];
        char nm[16];
        if (bd->nNames > 0) { strncpy(nm, bd->names[mage_ri(0, bd->nNames - 1)], sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0'; }
        else { strncpy(nm, CAT_NAMES[mage_ri(0, 7)], sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0'; }
        int sk = bd->nSkills > 0 ? skillIndex(bd->skills[mage_ri(0, bd->nSkills - 1)]) : 0;
        int tr = bd->nTraits > 0 ? traitIndex(bd->traits[mage_ri(0, bd->nTraits - 1)]) : 0;
        if (g.npartners < 6) {
            Partner *p2 = &g.partners[g.npartners++];
            p2->typeIdx = e->typeIdx;
            strncpy(p2->name, nm, sizeof(p2->name) - 1); p2->name[sizeof(p2->name) - 1] = '\0';
            p2->level = g.level > 0 ? g.level : 1; p2->exp = 0;
            p2->skillIdx = sk; p2->traitIdx = tr; p2->evolved = 0; p2->rar = e->rarity;
            p2->ivs[0] = e->ivs[0]; p2->ivs[1] = e->ivs[1]; p2->ivs[2] = e->ivs[2];
            p2->hp = 0; p2->maxhp = 0; p2->atk = 0;
        }
        g.cats++;
        g.dexCaught[e->typeIdx]++;
        toast("%s 收服成功！(%d%%)", nm, p);
        mage_persist();
        if (bt.enemyCount == 0) win_battle();
    } else {
        int heal = (int)(e->maxHp * 0.08f + 0.5f);
        e->hp += heal; if (e->hp > e->maxHp) e->hp = e->maxHp;
        toast("%s 挣脱了！(概率%d%%)", ENEMY[e->typeIdx].n, p);
    }
}

/* ---------- 敌方回合 ---------- */
static void apply_enemy_hit(BattleEnemy *e, int big) {
    int d = (int)(e->atk + mage_ri(-1, 2)); if (d < 1) d = 1;
    if (big) { d = (int)(d * 2.6f + 0.5f); toast("💥 %s 大招命中！", ENEMY[e->typeIdx].n); }
    d = (int)(d * diffC()->dmgTaken + 0.5f); if (d < 1) d = 1;
    d -= eqDef(); if (d < 1) d = 1;
    Partner *pr = NULL;
    if (g.npartners > 0 && g.activePartner >= 0 && g.activePartner < g.npartners) pr = &g.partners[g.activePartner];
    if (pr && pr->traitIdx == traitIndex("tough")) d = (int)(d * 0.9f + 0.5f);
    if (bt.guardBlock > 0) { int cut = (int)(d * bt.guardBlock + 0.5f); d -= cut; if (d < 1) d = 1; bt.guardBlock -= 0.15f; if (bt.guardBlock < 0) bt.guardBlock = 0; }
    g.hp -= d; if (g.hp < 0) g.hp = 0;
    toast("-%d HP", d);
}
static void enemy_phase(void) {
    /* 中毒结算 */
    int i = 0;
    while (i < bt.enemyCount) {
        BattleEnemy *e = &bt.enemies[i];
        if (e->hp <= 0) { i++; continue; }
        if (e->poisonTurns > 0) {
            int pd = (int)(e->maxHp * 0.02f + 0.5f); if (pd < 1) pd = 1;
            deal_to(e, pd); e->poisonTurns--;
            if (e->hp <= 0) continue;       /* 死亡敌人留给下方 reap_dead 统一回收 */
        }
        i++;
    }
    reap_dead();
    if (bt.enemyCount == 0) { win_battle(); return; }
    /* 逐个攻击 */
    for (int k = 0; k < bt.enemyCount; k++) {
        BattleEnemy *e = &bt.enemies[k];
        if (e->hp <= 0 || e->brokenTurns > 0) continue;   /* 硬直跳过 */
        if (e->boss) {
            if (e->hp < e->maxHp) {
                int hg = (int)(e->maxHp * (e->enraged ? 0.045f : 0.03f) + 0.5f); if (hg < 1) hg = 1;
                e->hp += hg; if (e->hp > e->maxHp) e->hp = e->maxHp;
            }
            if (e->bossCD > 0) e->bossCD--;
            if (e->bossCD <= 0) { apply_enemy_hit(e, 1); e->bossCD = e->enraged ? 2 : 3; }
            else apply_enemy_hit(e, 0);
        } else {
            apply_enemy_hit(e, 0);
        }
        if (g.hp <= 0) { set_bphase(BP_FAINT); return; }
    }
    /* 破防恢复 */
    for (int k = 0; k < bt.enemyCount; k++) {
        BattleEnemy *e = &bt.enemies[k];
        if (e->brokenTurns > 0) { e->brokenTurns--; if (e->brokenTurns <= 0) { e->brokenTurns = 0; e->shield = e->shieldMax; } }
    }
    /* 手下留情锁血兜底逃走 */
    for (int k = 0; k < bt.enemyCount; k++) {
        BattleEnemy *e = &bt.enemies[k];
        if (e->mercy) {
            e->mercyTurns++;
            if (e->mercyTurns >= 5) {
                int idx = (int)(e - &bt.enemies[0]);
                for (int j = idx; j < bt.enemyCount - 1; j++) bt.enemies[j] = bt.enemies[j + 1];
                bt.enemyCount--;
                toast("%s 恢复力气逃走了…", ENEMY[e->typeIdx].n);
                k--;
            }
        }
    }
    if (bt.enemyCount == 0) { win_battle(); return; }
    bt.turn++;
    set_bphase(BP_MENU);
}
static void after_player_action(void) {
    partner_strike();
    if (bt.enemyCount == 0) { win_battle(); return; }
    enemy_phase();
}

/* ---------- 刷怪 ---------- */
static void spawn_group(void) {
    if (bt.mode == 1 && bt.wave >= bt.dungeonRounds) { finish_crawl(); return; }
    if (bt.mode != 1 && bt.wave >= 10) { spawn_boss(); return; }
    int cnt = mage_ri(1, 3);
    int elite = (bt.mode == 1) ? 1 : (bt.wave == 5 ? 1 : (mage_rf(0, 1) < (bt.mode == 2 ? 0.55f : 0.12f) ? 1 : 0));
    const char **pool; int npool;
    static const char *dungeonPool[8] = {"bat","skel","magma","sala","wolf","frost","golem","ghost"};
    if (bt.mode == 1) {
        const char *pl[8]; int pn = 0;
        for (int i = 0; i < 5; i++) if (DUNGEONS[bt.dungeonId].pool[i]) pl[pn++] = DUNGEONS[bt.dungeonId].pool[i];
        pool = pl; npool = pn;
    } else if (bt.mode == 2) { pool = dungeonPool; npool = 8; }
    else {
        const char *pl[10]; int pn = 0;
        for (int i = 0; i < ZONE_MOBS_N[g.mapChapter]; i++) pl[pn++] = ZONE_MOBS[g.mapChapter][i];
        pool = pl; npool = pn;
    }
    if (npool <= 0) npool = 1;
    for (int i = 0; i < cnt && bt.enemyCount < BATTLE_MAX_ENEMIES; i++) {
        int typeIdx;
        if (mage_rf(0, 1) < 0.26f) typeIdx = enemyIndex("cat");
        else typeIdx = enemyIndex(pool[mage_ri(0, npool - 1)]);
        if (typeIdx < 0) continue;
        const MobDef *bd = &ENEMY[typeIdx];
        g.dexSeen[typeIdx]++;
        int lvl = (g.mapChapter - 1) * 2 + bt.wave + mage_ri(0, 1);
        if (elite && i == 0) lvl += 2;
        const DiffDef *dc = diffC();
        float mult = 1 + (lvl - 1) * 0.14f;
        float MB = 1.05f;
        int hp = (int)(bd->hp * mult * dc->hp * MB + 0.5f);
        int atk = (int)((bd->atk + mult * 0.6f) * dc->atk * MB + 0.5f);
        BattleEnemy *e = &bt.enemies[bt.enemyCount++];
        e->typeIdx = typeIdx; e->lvl = lvl;
        e->hp = hp; e->maxHp = hp; e->atk = atk;
        e->shield = elite ? 2 : 1; e->shieldMax = elite ? 2 : 1; e->brokenTurns = 0;
        e->boss = 0; e->elite = (elite && i == 0) ? 1 : 0; e->enraged = 0; e->bossCD = 0;
        e->mercy = 0; e->mercyTurns = 0; e->poisonTurns = 0;
        e->resist[0] = -1; e->resist[1] = -1; e->resistKnown = 0;
        e->rarity = rollRarity();
        e->ivs[0] = mage_ri(6, 15); e->ivs[1] = mage_ri(6, 15); e->ivs[2] = mage_ri(6, 15);
        e->x = BTPX + 78 + i * 16;
        if (e->elite) { e->hp = (int)(e->hp * 2.2f + 0.5f); e->maxHp = e->hp; e->atk = (int)(e->atk * 1.5f + 0.5f); }
    }
}
static void spawn_boss(void) {
    int bi = g.mapChapter; if (bi > 4) bi = 4; if (bi < 0) bi = 0;
    const BossDef *B = &BOSSES[bi];
    int typeIdx = enemyIndex(B->type); if (typeIdx < 0) typeIdx = 0;
    const MobDef *bd = &ENEMY[typeIdx];
    g.dexSeen[typeIdx]++;
    int lvl = (g.mapChapter - 1) * 2 + 10 + 2;
    const DiffDef *dc = diffC();
    float mult = (1 + (lvl - 1) * 0.14f) * 7.5f * dc->hp;
    int hp = (int)(bd->hp * mult + 0.5f);
    int atk = (int)((bd->atk + mult * 0.6f) * 1.85f * dc->bossAtk + 0.5f);
    bt.enemyCount = 1;
    BattleEnemy *e = &bt.enemies[0];
    e->typeIdx = typeIdx; e->lvl = lvl; e->hp = hp; e->maxHp = hp; e->atk = atk;
    e->shield = 5; e->shieldMax = 5; e->brokenTurns = 0;
    e->boss = 1; e->elite = 1; e->enraged = 0; e->bossCD = 3;
    e->mercy = 0; e->mercyTurns = 0; e->poisonTurns = 0;
    int rr = rollRarity(); e->rarity = (rr + 1) < 3 ? (rr + 1) : 3;
    e->ivs[0] = mage_ri(10, 15); e->ivs[1] = mage_ri(10, 15); e->ivs[2] = mage_ri(10, 15);
    e->x = BTPX + 150;
    int pl[9]; int np = 0;
    for (int i = 0; i < 9; i++) { int wk = 0; for (int j = 0; j < bd->nWeak; j++) if (bd->weakOf[j] == i) wk = 1; if (!wk) pl[np++] = i; }
    e->resist[0] = -1; e->resist[1] = -1;
    for (int k = 0; k < 2 && np > 0; k++) {
        int pi = mage_ri(0, np - 1); e->resist[k] = pl[pi];
        for (int j = pi; j < np - 1; j++) pl[j] = pl[j + 1]; np--;
    }
    e->resistKnown = 0;
    bt.storyShown = 0;
    toast("⚠ %s 出现！用弱点破盾，蓄力时防御！", B->n);
}
static void spawn_and_menu(void) {
    spawn_group();
    bt.turn = 1;
    set_bphase(BP_MENU);
    g_need_refresh = 1;
}

/* ---------- 自动战斗 ---------- */
static void auto_act(void) {
    if (bt.enemyCount == 0) { win_battle(); return; }
    BattleEnemy *cap = capture_target();
    if (cap && bt.autoCapture) {
        throw_orb(-1);
        if (bt.active && bt.phase == BP_MENU) after_player_action();
        return;
    }
    if (!bt.autoCapture) {                       /* 无自动捕：虚弱怪主动逃走清场 */
        for (int i = 0; i < bt.enemyCount; i++) {
            BattleEnemy *e = &bt.enemies[i];
            if (!ENEMY[e->typeIdx].hasCap) continue;
            int f = (int)(e->maxHp * 0.08f + 0.5f); if (f < 1) f = 1;
            if (e->hp <= f && e->mercy) {
                int idx = (int)(e - &bt.enemies[0]);
                for (int j = idx; j < bt.enemyCount - 1; j++) bt.enemies[j] = bt.enemies[j + 1];
                bt.enemyCount--; i--;
                toast("虚弱的魔物逃走了…");
            }
        }
        if (bt.enemyCount == 0) { win_battle(); return; }
    }
    if (g.hp * 1.0f / g.maxHp < 0.35f && g.potions > 0) { bt.itemIdx = 0; use_item(); return; }
    int lo[16]; int nl = activeLoadoutKeys(lo, 16);
    int chosen = -1;
    for (int i = 1; i <= nl; i++) {
        int k = lo[(bt.autoSpell + i) % nl];
        if (g.mp >= spellCost(k)) { chosen = k; bt.autoSpell = (bt.autoSpell + i) % nl; break; }
    }
    if (chosen >= 0) { g.equipped = chosen; magic_cast(); }
    else hero_strike();
}

/* ---------- 胜负/事件 ---------- */
static void win_battle(void) {
    if (bt.phase == BP_RESULT) return;
    bt.phase = BP_RESULT; bt.tickMs = now_ms();
    mage_persist();
}
static void next_wave(void) {
    if (bt.done) { end_battle(); return; }
    if (bt.mode == 1 && bt.wave >= bt.dungeonRounds) { finish_crawl(); return; }
    if (bt.mode == 2 && bt.wave >= 3) {
        int gd = 100 + 50 * g.mapChapter; g.gold += gd; g.crystals++; g.orbs++; g.seals++;
        toast("遗迹探索成功！+%d金 +魔晶 +球 +卡", gd);
        end_battle(); return;
    }
    if (bt.mode == 0 && bt.wave >= 10) { finish_expedition(); return; }
    bt.wave++;
    int forceEvent = (!bt.eventSeen && bt.wave >= 4);
    if (bt.wave < 10 && (mage_rf(0, 1) < 0.45f || forceEvent)) {
        bt.eventSeen = 1;
        float r = mage_rf(0, 1);
        bt.eventKind = (r < 0.32f) ? 0 : (r < 0.55f ? 1 : (r < 0.78f ? 2 : 3));
        set_bphase(BP_EVENT); g_need_refresh = 1; return;
    }
    spawn_and_menu();
}
static void finish_crawl(void) {
    const DungeonDef *D = &DUNGEONS[bt.dungeonId];
    int gd = 120 + bt.dungeonRounds * 4 + g.mapChapter * 40; g.gold += gd; bt.goldGain += gd;
    EbagItem it; rollEquip(D->dropRar > 0 ? D->dropRar : 2, &it);
    if (g.nebag < 24) { g.ebag[g.nebag] = it; g.nebag++; }
    int st = 1 + mage_ri(0, 3); if (st > 4) st = 4;
    int stn = mage_ri(1, 3); g.stones[st] += stn;
    if (mage_rf(0, 1) < 0.8f) { int id = mage_ri(0, 39); g.mbag[id] += mage_ri(1, 2); }
    if (mage_rf(0, 1) < 0.6f) { int id = mage_ri(0, 19); g.pbag[id] += 1; }
    for (int i = 0; i < 4; i++) g.dungeonOffers[i] = -1; g.ndoffers = 0;
    toast("【%s】通关！+%d金 +%s", D->name, gd, it.name);
    mage_persist(); end_battle();
}
static void finish_expedition(void) {
    bt.done = 1; bt.phase = BP_RESULT; bt.tickMs = now_ms();
    int ck = g.mapChapter; if (ck > 4) ck = 4; if (ck < 0) ck = 0;
    toast("远征完成！%s 肃清，回城休整", CHAPTERS[ck]);
    mage_persist();
}
static void apply_event(void) {
    if (bt.eventKind < 0) { spawn_and_menu(); return; }
    int k = bt.eventKind, ch = g.mapChapter;
    if (k == 0) {                                   /* 宝箱 */
        int gd = mage_ri(30, 60) + 40 * ch; g.gold += gd; bt.goldGain += gd;
        int n = mage_ri(1, 3);
        for (int i = 0; i < n; i++) {
            if (mage_rf(0, 1) < 0.45f) { int id = mage_ri(0, 19); g.pbag[id]++; }
            else { int id = mage_ri(0, 39); g.mbag[id]++; }
        }
        if (mage_rf(0, 1) < 0.30f) { int st = 1 + mage_ri(0, 3); if (st > 4) st = 4; g.stones[st] += mage_ri(1, 2); }
        if (mage_rf(0, 1) < 0.20f) g.orbs += 2;
        if (mage_rf(0, 1) < 0.12f) g.crystals++;
        toast("宝箱开启！+%d金", gd);
    } else if (k == 1) {                            /* 陷阱 */
        int d = (int)(g.maxHp * mage_rf(0.10f, 0.18f) + 0.5f); g.hp -= d; if (g.hp < 1) g.hp = 1;
        if (mage_rf(0, 1) < 0.5f) { int lost = mage_ri(15, 35); if (lost > g.gold) lost = g.gold; g.gold -= lost; }
        toast("踩中陷阱！-%d HP", d);
    } else if (k == 2) {                            /* 泉水 */
        g.hp += (int)(g.maxHp * 0.5f + 0.5f); if (g.hp > g.maxHp) g.hp = g.maxHp;
        g.mp += (int)(g.maxMp * 0.5f + 0.5f); if (g.mp > g.maxMp) g.mp = g.maxMp;
        toast("泉水恢复了一半 HP/MP");
    } else {                                        /* 药草 */
        int h = mage_ri(3, 6); g.herbs += h;
        if (mage_rf(0, 1) < 0.35f) g.crystals++;
        int mid = mage_ri(0, 39); g.mbag[mid]++;
        toast("采集到月光草 x%d", h);
    }
    mage_persist();
    bt.eventKind = -1;
    spawn_and_menu();
}
static void end_battle(void) {
    bt.active = 0; bt.enemyCount = 0;
    mage_set_scene(SC_CITY); g.cityMode = 0;
    mage_persist();
    g_need_refresh = 1;
}

/* ---------- 启动入口 ---------- */
void battle_start_wild(int chapter) {
    memset(&bt, 0, sizeof(bt));
    g.mapChapter = chapter;
    bt.active = 1; bt.mode = 0; bt.wave = 1;
    bt.autoBattle = g.autoMode; bt.autoCapture = g.autoCapture;
    bt.lastSpell = -1; bt.eventKind = -1; bt.itemIdx = 0; bt.magicIdx = 0;
    bt.tickMs = now_ms();
    spawn_and_menu();
    mage_set_scene(SC_BATTLE); g_need_refresh = 1;
}
void battle_start_dungeon(int dungeonId) {
    memset(&bt, 0, sizeof(bt));
    bt.active = 1;
    if (dungeonId < 0) { bt.mode = 2; }            /* 遗迹秘境 */
    else {
        bt.mode = 1; bt.dungeonId = dungeonId;
        const DungeonDef *D = &DUNGEONS[dungeonId];
        bt.dungeonRounds = mage_ri(D->minR, D->maxR);
        if (g.level < D->reqLv) toast("%s 推荐Lv%d，强行进入会很苦", D->name, D->reqLv);
    }
    bt.wave = 1; bt.autoBattle = g.autoMode; bt.autoCapture = g.autoCapture;
    bt.lastSpell = -1; bt.eventKind = -1; bt.itemIdx = 0; bt.magicIdx = 0;
    bt.tickMs = now_ms();
    spawn_and_menu();
    mage_set_scene(SC_BATTLE); g_need_refresh = 1;
}
void battle_start_boss(int chapter) {
    memset(&bt, 0, sizeof(bt));
    g.mapChapter = chapter;
    bt.active = 1; bt.mode = 0; bt.wave = 10;      /* 直接打 BOSS 波 */
    bt.autoBattle = g.autoMode; bt.autoCapture = g.autoCapture;
    bt.lastSpell = -1; bt.eventKind = -1; bt.itemIdx = 0; bt.magicIdx = 0;
    bt.tickMs = now_ms();
    spawn_group(); set_bphase(BP_MENU);
    mage_set_scene(SC_BATTLE); g_need_refresh = 1;
}

/* ---------- 渲染 / 输入 / 自动推进 ---------- */
static void battle_render(void) {
    char hud[64];
    snprintf(hud, sizeof(hud), "Lv%d 金%d 球%d 卡%d 药%d", g.level, g.gold, g.orbs, g.seals, g.potions);
    lv_label_set_text(g_hud, hud);
    s_init();
    if (bt.mode == 1)      s_add("⚔ 副本 %s [%d/%d]\n", DUNGEONS[bt.dungeonId].name, bt.wave, bt.dungeonRounds);
    else if (bt.mode == 2) s_add("⚔ 遗迹秘境 [%d/3]\n", bt.wave);
    else                   s_add("⚔ 远征 [%d/10]\n", bt.wave);
    s_add("你 Lv%d  HP %d/%d  MP %d/%d\n", g.level, g.hp, g.maxHp, g.mp, g.maxMp);
    for (int i = 0; i < bt.enemyCount; i++) {
        BattleEnemy *e = &bt.enemies[i];
        if (e->hp <= 0) continue;
        const MobDef *bd = &ENEMY[e->typeIdx];
        s_add("%s Lv%d %d/%d", bd->n, e->lvl, e->hp, e->maxHp);
        if (e->shield > 0) s_add(" 盾%d", e->shield);
        if (e->brokenTurns > 0) s_add(" 破防");
        if (e->boss) s_add(" [BOSS]"); else if (e->elite) s_add(" [精英]");
        s_add("\n");
    }
    if (bt.enemyCount == 0) s_add("（无敌人）\n");
    if (bt.phase == BP_MENU) {
        s_add("———\nOK 攻击 · ↓魔法 · ↑道具\n");
        if (bt.autoBattle) s_add("（自动战斗中，按任意键接管）\n");
    } else if (bt.phase == BP_MAGIC) {
        int lo[16]; int nl = activeLoadoutKeys(lo, 16);
        int k = (bt.magicIdx < nl) ? lo[bt.magicIdx] : g.equipped;
        s_add("———\n施法：%s\n威力%d 耗%d\n", spellName(k), spellPower(k), spellCost(k));
        s_add("OK 释放 · ↓切换 · ↑返回\n");
    } else if (bt.phase == BP_ITEM) {
        const char *kn[3] = {"药水", "捕捉球", "封印卡"};
        int kind = item_opt_kind(bt.itemIdx);
        s_add("———\n道具：%s\n", kind >= 0 ? kn[kind] : "（空）");
        s_add("OK 使用 · ↓切换 · ↑返回\n");
    } else if (bt.phase == BP_RESULT) {
        s_add("———\n【本场战果】\n金币+%d 经验+%d\n", bt.goldGain, bt.expGain);
        if (bt.herbGain) s_add("月光草+%d ", bt.herbGain);
        if (bt.matGain)  s_add("兽骨+%d\n", bt.matGain);
        if (bt.potGain)  s_add("药水+%d\n", bt.potGain);
        if (bt.done) s_add("远征完成！\n");
        if (bt.bossEquip[0]) s_add("BOSS掉落：%s\n魔晶+%d 球+%d 卡+%d\n", bt.bossEquip, bt.bossCrystal, bt.bossOrb, bt.bossSeal);
        s_add("OK 继续 · 长按回城\n");
    } else if (bt.phase == BP_EVENT) {
        const char *ev[4] = {"宝箱", "陷阱", "泉水", "稀有药草"};
        s_add("———\n遭遇：%s\n", bt.eventKind >= 0 ? ev[bt.eventKind] : "?");
        s_add("OK 探索 · 长按绕开\n");
    } else if (bt.phase == BP_FAINT) {
        s_add("———\n魔力耗尽，力竭…\nOK 回城休息（无惩罚）\n");
    }
    lv_label_set_text(g_scene_hint, "OK确认 · ↓切换 · ↑道具 · 长按返回");
}
static void battle_handle(int act) {
    switch (bt.phase) {
    case BP_MENU:
        if (bt.autoBattle) { bt.autoBattle = 0; toast("手动接管"); break; }
        if (act == ACT_OK) hero_strike();
        else if (act == ACT_NEXT) {
            int lo[16]; int nl = activeLoadoutKeys(lo, 16); if (nl == 0) { toast("没有魔法"); break; }
            bt.magicIdx = 0; g.equipped = lo[0]; set_bphase(BP_MAGIC);
        } else if (act == ACT_PREV) {
            if (item_opt_count() == 0) { toast("没有道具"); break; }
            bt.itemIdx = 0; set_bphase(BP_ITEM);
        } else if (act == ACT_BACK) end_battle();
        break;
    case BP_MAGIC:
        if (act == ACT_OK) { set_bphase(BP_MENU); magic_cast(); }
        else if (act == ACT_NEXT) { int lo[16]; int nl = activeLoadoutKeys(lo, 16); if (nl == 0) break; bt.magicIdx = (bt.magicIdx + 1) % nl; g.equipped = lo[bt.magicIdx]; g_need_refresh = 1; }
        else if (act == ACT_PREV || act == ACT_BACK) set_bphase(BP_MENU);
        break;
    case BP_ITEM:
        if (act == ACT_OK) { set_bphase(BP_MENU); use_item(); }
        else if (act == ACT_NEXT) { int c = item_opt_count(); if (c == 0) break; bt.itemIdx = (bt.itemIdx + 1) % c; g_need_refresh = 1; }
        else if (act == ACT_PREV || act == ACT_BACK) set_bphase(BP_MENU);
        break;
    case BP_RESULT:
        if (act == ACT_OK) next_wave();
        else if (act == ACT_BACK) end_battle();
        break;
    case BP_EVENT:
        if (act == ACT_OK) apply_event();
        else if (act == ACT_BACK) { bt.eventKind = -1; spawn_and_menu(); }
        break;
    case BP_FAINT:
        if (act == ACT_OK || act == ACT_BACK) {
            g.hp = (int)(g.maxHp * 0.5f + 0.5f); if (g.hp < 1) g.hp = 1;
            g.mp = (int)(g.maxMp * 0.5f + 0.5f);
            if (bt.mode == 1) { for (int i = 0; i < 4; i++) g.dungeonOffers[i] = -1; g.ndoffers = 0; }
            toast("力竭回城休息（无惩罚）");
            end_battle();
        }
        break;
    default: break;
    }
}
void battle_tick(void) {
    if (!bt.active) return;
    uint32_t now = now_ms();
    if (bt.phase == BP_MENU) {
        if (bt.autoBattle && now - bt.tickMs > 600) { bt.tickMs = now; auto_act(); }
    } else if (bt.phase == BP_RESULT) {
        if (bt.autoBattle && now - bt.tickMs > 1400) { bt.tickMs = now; next_wave(); }
    } else if (bt.phase == BP_EVENT) {
        if (bt.autoBattle && now - bt.tickMs > 1000) { bt.tickMs = now; apply_event(); }
    }
}

/* ===================== 入口 ===================== */
void mage_app_start(void) {
    ESP_LOGI(TAG, "魔法帝之路 固件启动 (M2)");
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));
    mage_init();
    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();
        bsp_lvgl_unlock();
    }
    bsp_button_init(mage_on_key, NULL);
    xTaskCreate(game_task, "mage", 12288, NULL, 5, NULL);
}
