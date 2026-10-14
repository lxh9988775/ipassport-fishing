/* 魔法帝之路 —— 战斗 HUD（1:1 复刻 HTML index.html 的 drawBattleHUD + drawActionCards）
 *
 * 原版结构（已逐行核对 HTML 2048-2100 行）：
 *   顶部 26px  血条面板：HP 条 + MP 条 + 波次 + AUTO 状态（金框）
 *   中间      纯战斗画面，零文字（敌人血条/Lv 画在画面上）
 *   底部 44px 三张动作卡：A 攻击 / B 魔法 / C 道具，卡上有回合徽标
 *
 * 铁律：所有对象在 build 期一次性创建，之后只改 pos/size/text，
 * 绝不在周期刷新里 lv_obj_clean 重建（C3 单核会卡死）。
 */
#include "mage_fx.h"
#include "mage_state.h"
#include "mage_data.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* 中文子集字库（与 mage.c 共用 assets/fonts/mage_cjk_16.c / mage_cjk_12.c）
 * 字号层级对照 HTML：12px = HUD/行内小字（HTML 8-10px 的等价物），
 * 16px = 标题/正文。26px 顶部面板里两行小字必须用 12px 才放得下（16px 必重叠）。 */
LV_FONT_DECLARE(mage_cjk_16);
LV_FONT_DECLARE(mage_cjk_12);
LV_FONT_DECLARE(lv_font_montserrat_12);

#define SCR_W 240
#define SCR_H 320
#define HUD_H 26        /* HTML: box(0,0,W,26) */
#define CARD_Y 276      /* HTML: y = H-44 = 276 */
#define CARD_W 72
#define CARD_H 40

static const char *TAG = "mage_hud";

typedef struct {
    lv_obj_t *root;
    lv_obj_t *panel;          /* 顶部 26px 底板 */
    lv_obj_t *hpBar, *mpBar;  /* HP/MP 条 */
    lv_obj_t *hpTx, *mpTx;    /* HP/MP 数值 */
    lv_obj_t *waveTx;         /* 右上波次 */
    lv_obj_t *statTx;         /* 右下 AUTO/编/球/卡/药 */
    lv_obj_t *card[3];        /* A攻击 B魔法 C道具 */
    lv_obj_t *cardTx[3];
    lv_obj_t *cardBadge[3];   /* 键位字母圆片 */
    lv_obj_t *turnBox;        /* 回合徽标 */
    lv_obj_t *turnTx;
    int8_t shown;         /* 上一次 show/hide 的值（-1=未初始化），防每帧重复切 flag */
    /* 文本与条宽的记忆值：只有真正变化时才调 LVGL，防每帧重排导致卡死 */
    char hpTxt[24], mpTxt[24], waveTxt[40], statTxt[40], turnTxt[24];
    int  hpW, mpW;
    bool built;
} HudLayer;

static HudLayer H = {0};
static lv_obj_t *g_hud_parent = NULL;

static lv_obj_t *mk_panel(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_bar(lv_obj_t *p, int x, int y, int w, int h, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 1, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_tx(lv_obj_t *p, const char *t, int x, int y, int w, uint32_t col, const lv_font_t *font)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_set_style_text_font(o, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(o, t);
    lv_obj_set_pos(o, x, y);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void mage_hud_attach(lv_obj_t *parent)
{
    g_hud_parent = parent;
    H.built = false;
    H.shown = -1;   /* -1 = 尚未设置过，保证首次 show/hide 一定会执行 */
}

void mage_hud_build(void)
{
    if (H.built || !g_hud_parent) return;
    const int8_t keepShown = H.shown;   /* memset 会清零 shown，必须保留 */
    memset(&H, 0, sizeof(H));
    H.shown = keepShown;

    /* 根容器：整个 HUD 组的显隐开关 */
    H.root = lv_obj_create(g_hud_parent);
    lv_obj_remove_style_all(H.root);
    lv_obj_set_size(H.root, SCR_W, SCR_H);
    lv_obj_set_pos(H.root, 0, 0);
    lv_obj_set_style_bg_opa(H.root, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(H.root, LV_SCROLLBAR_MODE_OFF);

    /* ---- 顶部 26px 面板：HTML box(0,0,W,26,'rgba(16,12,26,.92)',PAL.frame) ---- */
    H.panel = mk_panel(H.root, 0, 0, SCR_W, HUD_H, 0x100c1a, LV_OPA_90);
    lv_obj_set_style_border_width(H.panel, 1, 0);
    lv_obj_set_style_border_color(H.panel, lv_color_hex(0x6a5a3a), 0);

    /* HP 条：HTML bar(20,4,58,7)，槽色 #3a1a1a，填充 PAL.red */
    lv_obj_t *hpSlot = mk_panel(H.panel, 20, 4, 58, 7, 0x3a1a1a, LV_OPA_COVER);
    H.hpBar = mk_panel(hpSlot, 0, 0, 58, 7, 0xe05555, LV_OPA_COVER);
    H.hpTx = mk_tx(H.panel, "80/80", 82, 2, 52, 0xffffff, &lv_font_montserrat_12);
    mk_tx(H.panel, "HP", 7, 3, 14, 0xff8888, &lv_font_montserrat_12);

    /* MP 条：HTML bar(20,15,58,5) */
    lv_obj_t *mpSlot = mk_panel(H.panel, 20, 15, 58, 5, 0x1a2a3a, LV_OPA_COVER);
    H.mpBar = mk_panel(mpSlot, 0, 0, 58, 5, 0x5ec8f2, LV_OPA_COVER);
    H.mpTx = mk_tx(H.panel, "50/50", 82, 13, 52, 0x5ec8f2, &lv_font_montserrat_12);
    mk_tx(H.panel, "MP", 7, 14, 14, 0x7cb8ff, &lv_font_montserrat_12);

    /* 右上两行：HTML txt(wlabel,W-6,11,...,'right') / txt(状态,W-6,22,...,'right')
     * 右缘统一 = W-6 = 234。宽度 108 -> x = 234-108 = 126。
     * 字号 12px：26px 面板两行（y=1 / y=14）才互不重叠（16px 必压线）。 */
    H.waveTx = mk_tx(H.panel, "1/10", SCR_W - 114, 1, 108, 0xffd93d, &mage_cjk_12);
    lv_obj_set_style_text_align(H.waveTx, LV_TEXT_ALIGN_RIGHT, 0);
    H.statTx = mk_tx(H.panel, "AUTO·2·1·1", SCR_W - 114, 14, 108, 0x7fdc8a, &mage_cjk_12);
    lv_obj_set_style_text_align(H.statTx, LV_TEXT_ALIGN_RIGHT, 0);

    /* ---- 底部三张动作卡：HTML y=H-44=276, x=8+i*78, 72x40 ---- */
    {
        static const char *nm[3] = {"攻击", "魔法", "道具"};
        static const char *bd[3] = {"A", "B", "C"};
        static const uint32_t c1[3] = {0xff8a3d, 0x5a8aff, 0x3dbd7a};
        static const uint32_t c0[3] = {0xffb74d, 0x7cd4ff, 0x8fe89a};
        for (int i = 0; i < 3; i++) {
            int x = 8 + i * 78;
            /* 卡片底：HTML 渐变 #332a5e -> #1a1430，用 #241d4a 近似 */
            H.card[i] = mk_panel(H.root, x, CARD_Y, CARD_W, CARD_H, 0x241d4a, LV_OPA_COVER);
            lv_obj_set_style_border_width(H.card[i], 1, 0);
            lv_obj_set_style_border_color(H.card[i], lv_color_hex(0x8a6a2a), 0);
            /* 主色徽带：HTML px(x+2,y+3,2.5,36,c1) */
            mk_panel(H.card[i], 2, 3, 3, 34, c1[i], LV_OPA_COVER);
            mk_panel(H.card[i], 2, 3, 3, 5, c0[i], LV_OPA_COVER);
            /* 键位徽章：HTML arc(x+14,y+11,7) 金框圆片 */
            H.cardBadge[i] = mk_panel(H.card[i], 7, 4, 15, 15, 0x1a1430, LV_OPA_COVER);
            lv_obj_set_style_radius(H.cardBadge[i], 8, 0);
            lv_obj_set_style_border_width(H.cardBadge[i], 1, 0);
            lv_obj_set_style_border_color(H.cardBadge[i], lv_color_hex(0xc9a44a), 0);
            H.cardTx[i] = mk_tx(H.cardBadge[i], bd[i], 0, 1, 15, 0xffd93d, &lv_font_montserrat_12);
            lv_obj_set_style_text_align(H.cardTx[i], LV_TEXT_ALIGN_CENTER, 0);
            /* 卡片名：HTML txt(cards[i][0],x+38,y+30) */
            lv_obj_t *nmLbl = mk_tx(H.card[i], nm[i], 12, 18, 58, 0xffffff, &mage_cjk_12);
            lv_obj_set_style_text_align(nmLbl, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    /* 回合徽标：HTML box(W/2-lw/2, y-7, lw, 14, '#0d0a1a','#c9a44a') */
    H.turnBox = mk_panel(H.root, SCR_W / 2 - 30, CARD_Y - 7, 60, 14, 0x0d0a1a, LV_OPA_COVER);
    lv_obj_set_style_border_width(H.turnBox, 1, 0);
    lv_obj_set_style_border_color(H.turnBox, lv_color_hex(0xc9a44a), 0);
    H.turnTx = mk_tx(H.turnBox, "回合 1", 0, 1, 60, 0xffd93d, &mage_cjk_12);
    lv_obj_set_style_text_align(H.turnTx, LV_TEXT_ALIGN_CENTER, 0);

    H.built = true;
    ESP_LOGI(TAG, "战斗 HUD 就绪（1:1 复刻原版：顶部血条 + 底部三卡）");
}

/* 防卡死铁律：refresh_ui 每 60ms 调一次本函数，而
 * lv_obj_add_flag / remove_flag 每次调用都会触发样式重算与区域 invalidate。
 * 在主界面（非战斗）in_bt 恒为 false，若无条件 add_flag 就是每 60ms
 * 触发一次全屏重绘 -> mage 任务占满 CPU -> IDLE 饿死 -> 5s 看门狗反复
 * 重启，表现为「卡死在主界面进不去」。
 * 修法：记住上一次状态，只在真正变化时切 flag。 */
void mage_hud_show(bool on)
{
    if (!g_hud_parent) return;
    if (on) {
        mage_hud_build();
        if (!H.built) return;
    } else if (!H.built) {
        /* 从未构建过：只需隐藏父容器即可，不能碰还不存在的 H.root */
        if (H.shown != 0) {
            H.shown = 0;
            lv_obj_add_flag(g_hud_parent, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    /* 状态记忆：只有真正翻转时才调 LVGL（否则每 60ms 触发一次全屏 invalidate） */
    if (H.shown == (int8_t)(on ? 1 : 0)) return;
    if (on) {
        /* 关键：父容器 hud_root 在 build_ui 里被加了 HIDDEN，
         * 只切子对象 H.root 的 flag 父级仍隐藏 -> 整个 HUD 永远不显示。
         * 必须父子两个一起切。 */
        lv_obj_remove_flag(g_hud_parent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(H.root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(H.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_hud_parent, LV_OBJ_FLAG_HIDDEN);
    }
    H.shown = (int8_t)(on ? 1 : 0);
}

/* 文本记忆：与 lv_label_set_text 配合，内容相同则完全不碰 LVGL。
 * 必须用「按目标缓冲尺寸安全拷贝」而不是 snprintf(dst,N,"%s",src) ——
 * 工程开 -Werror=format-truncation，当 dst 小于 src 时 snprintf 直接报错：
 *   error: '%s' directive output may be truncated writing up to 63 bytes
 *          into a region of size 24 */
static void hud_keep(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) { n = dstsz - 1; }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 每帧只改文本与条宽，不建对象。
 *
 * 【卡死修复】lv_label_set_text 每次调用都会重新排版 + invalidate；
 * 本函数由 refresh_ui 每 60ms 调一次，一轮 5 次 = 每秒 80 次文本重排，
 * 在全屏贴图场景下足以让 C3 单核被 mage 任务占满、IDLE 饿死触发看门狗。
 * 改为：文本与条宽都先与上次比对，相同则完全不动 LVGL。 */
void mage_hud_update(int hp, int maxHp, int mp, int maxMp,
                     int wave, int waveMax, bool crawl,
                     bool autoMode, bool autoCap, int loadout, int orbs, int seals, int potions,
                     int turn)
{
    if (!H.built) return;
    char want[64];
    int w;

    /* HP 数值：HTML bar(...) + txt(ceil(hp)+'/'+max) */
    w = maxHp > 0 ? (int)((float)hp * 58.0f / (float)maxHp) : 0;
    if (w < 0) { w = 0; }
    if (w > 58) { w = 58; }
    if (w != H.hpW) { H.hpW = w; lv_obj_set_size(H.hpBar, w, 7); }
    snprintf(want, sizeof(want), "%d/%d", hp, maxHp);
    if (strcmp(H.hpTxt, want) != 0) {
        hud_keep(H.hpTxt, sizeof(H.hpTxt), want);
        lv_label_set_text(H.hpTx, want);
    }

    w = maxMp > 0 ? (int)((float)mp * 58.0f / (float)maxMp) : 0;
    if (w < 0) { w = 0; }
    if (w > 58) { w = 58; }
    if (w != H.mpW) { H.mpW = w; lv_obj_set_size(H.mpBar, w, 5); }
    snprintf(want, sizeof(want), "%d/%d", mp, maxMp);
    if (strcmp(H.mpTxt, want) != 0) {
        hud_keep(H.mpTxt, sizeof(H.mpTxt), want);
        lv_label_set_text(H.mpTx, want);
    }

    /* 波次：HTML wlabel */
    if (crawl) snprintf(want, sizeof(want), "试炼 %d/%d", wave, waveMax);
    else       snprintf(want, sizeof(want), "%d/%d%s", wave, waveMax,
                        wave == 10 ? " BOSS" : (wave == 5 ? " 精英" : ""));
    if (strcmp(H.waveTxt, want) != 0) {
        hud_keep(H.waveTxt, sizeof(H.waveTxt), want);
        lv_label_set_text(H.waveTx, want);
    }

    /* 状态行：HTML txt((auto?'AUTO':'手动')+...+' 编'+n+' 球'+..)
     * 放在顶部面板第二行右侧（y=13），字号小一点避免与波次行撞在一起 */
    snprintf(want, sizeof(want), "%s·%d·%d·%d",
             autoMode ? "AUTO" : "手动", orbs, seals, potions);
    (void)autoCap;
    (void)loadout;
    if (strcmp(H.statTxt, want) != 0) {
        hud_keep(H.statTxt, sizeof(H.statTxt), want);
        lv_label_set_text(H.statTx, want);
    }

    /* 回合徽标 */
    snprintf(want, sizeof(want), "\xe5\x9b\x9e\xe5\x90\x88 %d", turn);
    if (strcmp(H.turnTxt, want) != 0) {
        hud_keep(H.turnTxt, sizeof(H.turnTxt), want);
        lv_label_set_text(H.turnTx, want);
    }
}

bool mage_hud_ready(void)
{
    return H.built;
}
