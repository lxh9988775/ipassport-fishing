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

static lv_obj_t *mk_tx(lv_obj_t *p, const char *t, int x, int y, int w, uint32_t col, int fontSize)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(o, t);
    lv_obj_set_pos(o, x, y);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    (void)fontSize;
    return o;
}

void mage_hud_attach(lv_obj_t *parent)
{
    g_hud_parent = parent;
    H.built = false;
}

void mage_hud_build(void)
{
    if (H.built || !g_hud_parent) return;
    memset(&H, 0, sizeof(H));

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
    H.hpTx = mk_tx(H.panel, "80/80", 82, 2, 46, 0xffffff, 8);
    mk_tx(H.panel, "HP", 7, 2, 13, 0xff8888, 7);

    /* MP 条：HTML bar(20,15,58,5) */
    lv_obj_t *mpSlot = mk_panel(H.panel, 20, 15, 58, 5, 0x1a2a3a, LV_OPA_COVER);
    H.mpBar = mk_panel(mpSlot, 0, 0, 58, 5, 0x5ec8f2, LV_OPA_COVER);
    H.mpTx = mk_tx(H.panel, "50/50", 82, 13, 46, 0x5ec8f2, 8);
    mk_tx(H.panel, "MP", 7, 13, 13, 0x7cb8ff, 7);

    H.waveTx = mk_tx(H.panel, "1/10", SCR_W - 70, 1, 68, 0xffd93d, 10);
    lv_obj_set_style_text_align(H.waveTx, LV_TEXT_ALIGN_RIGHT, 0);
    H.statTx = mk_tx(H.panel, "AUTO 3 球 卡 药", SCR_W - 104, 13, 102, 0x7fdc8a, 8);
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
            H.cardTx[i] = mk_tx(H.cardBadge[i], bd[i], 0, 1, 15, 0xffd93d, 9);
            lv_obj_set_style_text_align(H.cardTx[i], LV_TEXT_ALIGN_CENTER, 0);
            /* 卡片名：HTML txt(cards[i][0],x+38,y+30) */
            lv_obj_t *nmLbl = mk_tx(H.card[i], nm[i], 12, 18, 58, 0xffffff, 12);
            lv_obj_set_style_text_align(nmLbl, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    /* 回合徽标：HTML box(W/2-lw/2, y-7, lw, 14, '#0d0a1a','#c9a44a') */
    H.turnBox = mk_panel(H.root, SCR_W / 2 - 30, CARD_Y - 7, 60, 14, 0x0d0a1a, LV_OPA_COVER);
    lv_obj_set_style_border_width(H.turnBox, 1, 0);
    lv_obj_set_style_border_color(H.turnBox, lv_color_hex(0xc9a44a), 0);
    H.turnTx = mk_tx(H.turnBox, "回合 1", 0, 2, 60, 0xffd93d, 8);
    lv_obj_set_style_text_align(H.turnTx, LV_TEXT_ALIGN_CENTER, 0);

    H.built = true;
    ESP_LOGI(TAG, "战斗 HUD 就绪（1:1 复刻原版：顶部血条 + 底部三卡）");
}

void mage_hud_show(bool on)
{
    if (!g_hud_parent) return;
    if (on) {
        mage_hud_build();
        if (!H.built) return;
        /* 关键：父容器 hud_root 在 build_ui 里被加了 HIDDEN，
         * 只切子对象 H.root 的 flag 父级仍隐藏 -> 整个 HUD 永远不显示。
         * 必须父子两个一起切。 */
        lv_obj_remove_flag(g_hud_parent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(H.root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(H.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_hud_parent, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 每帧只改文本与条宽，不建对象 */
void mage_hud_update(int hp, int maxHp, int mp, int maxMp,
                     int wave, int waveMax, bool crawl,
                     bool autoMode, bool autoCap, int loadout, int orbs, int seals, int potions,
                     int turn)
{
    if (!H.built) return;
    char buf[48];
    int w;

    /* HP/MP 条与数值：HTML bar(...) + txt(ceil(hp)+'/'+max) */
    w = maxHp > 0 ? (int)((float)hp * 58.0f / (float)maxHp) : 0;
    if (w < 0) { w = 0; }
    if (w > 58) { w = 58; }
    lv_obj_set_size(H.hpBar, w, 7);
    lv_obj_set_size(H.hpTx, 46, 16);
    snprintf(buf, sizeof(buf), "%d/%d", hp, maxHp);
    lv_label_set_text(H.hpTx, buf);

    w = maxMp > 0 ? (int)((float)mp * 58.0f / (float)maxMp) : 0;
    if (w < 0) { w = 0; }
    if (w > 58) { w = 58; }
    lv_obj_set_size(H.mpBar, w, 5);
    snprintf(buf, sizeof(buf), "%d/%d", mp, maxMp);
    lv_label_set_text(H.mpTx, buf);

    /* 波次：HTML wlabel */
    if (crawl) snprintf(buf, sizeof(buf), "试炼 %d/%d", wave, waveMax);
    else       snprintf(buf, sizeof(buf), "%d/%d%s", wave, waveMax,
                        wave == 10 ? " \xe2\x98\xa0BOSS" : (wave == 5 ? " \xe2\x9a\x94\xe7\xb2\xbe\xe8\x8b\xb1" : ""));
    lv_label_set_text(H.waveTx, buf);

    /* 状态行：HTML txt((auto?'AUTO':'手动')+...+' 编'+n+' 球'+..)
     * 放在顶部面板第二行右侧（y=13），字号小一点避免与波次行撞在一起 */
    snprintf(buf, sizeof(buf), "%s \xe7\xbc\x96%d \xe7\x90\x83%d \xe5\x8d\xa1%d \xe8\x8d\xaf%d",
             autoMode ? "AUTO" : "\xe6\x89\x8b\xe5\x8a\xa8", loadout, orbs, seals, potions);
    (void)autoCap;
    lv_label_set_text(H.statTx, buf);

    /* 回合徽标 */
    snprintf(buf, sizeof(buf), "\xe5\x9b\x9e\xe5\x90\x88 %d", turn);
    lv_label_set_text(H.turnTx, buf);
}

bool mage_hud_ready(void)
{
    return H.built;
}
