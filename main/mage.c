/*
 * mage.c —— 《魔法帝之路》固件入口 + 标题/命名 + 场景派发框架
 *
 * 1:1 复刻路线：本文件先把"启动 → 标题 → 命名 → 进入城镇"这条主链路和
 * 场景派发框架立起来（已用云 CI 验证可编译/可启动）；其余 15 个玩法官屏
 * （战斗 / 伙伴 / 炼金 / 锻造 / 图鉴 …）在后续里程碑里逐个 1:1 填进
 * SCENE 表，不改动已确立的框架与存档结构。
 *
 * 按键（3 键 → ADC 单引脚，BSP 层已去抖）：
 *   UP / DOWN = 移动选择，OK = 确认。长按统一在 game_task 电平轮询里处理（本期仅标题/命名用到 CLICK）。
 */
#include "bsp_display.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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
#define HUD_H  38
#define UI_REFRESH_MS 50

/* ===================== 场景枚举（与 HTML SCENES 对应） ===================== */
enum {
    SC_TITLE = 0, SC_NAME, SC_CITY, SC_STATUS, SC_PARTNERS,
    SC_BOOK, SC_ALCHEMY, SC_BAG, SC_EQUIP, SC_ACAD,
    SC_WILD, SC_GUILD, SC_HOME, SC_FORGE, SC_MARKET,
    SC_DEX, SC_MUPGRADE, SC_BATTLE, SC_MENU, SC_COUNT
};
static const char *SCENE_NAME[SC_COUNT] = {
    "标题", "命名", "城镇", "状态", "伙伴", "魔法书", "炼金", "背包",
    "装备", "魔法学院", "野外试炼", "冒险者公会", "住宅", "锻造房",
    "魔法黑市", "魔物图鉴", "魔法升级", "战斗", "系统"
};

/* ===================== 颜色 ===================== */
#define C_GOLD  lv_color_hex(0xffd93d)
#define C_GRAY  lv_color_hex(0x9a93b5)
#define C_WHITE lv_color_hex(0xf2eefb)
#define C_CYAN  lv_color_hex(0x5ec8f2)
#define C_RED   lv_color_hex(0xff6b6b)
#define C_DIM   lv_color_hex(0x6b6480)
#define C_BG    lv_color_hex(0x0d0b1a)

/* ===================== 全局 ===================== */
static QueueHandle_t s_btn_q = NULL;
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_title_grp = NULL;
static lv_obj_t *g_name_grp  = NULL;
static lv_obj_t *g_info_grp  = NULL;
static lv_obj_t *g_hud       = NULL;

static lv_obj_t *g_title_main = NULL, *g_title_sub = NULL, *g_title_tag = NULL;
static lv_obj_t *g_menu0 = NULL, *g_menu1 = NULL;
static lv_obj_t *g_title_foot = NULL, *g_title_hint = NULL;
static lv_obj_t *g_name_prompt = NULL, *g_name_val = NULL, *g_name_hint = NULL;
static lv_obj_t *g_info_title = NULL, *g_info_desc = NULL;
static lv_obj_t *g_stars[14];

static int  g_scene   = SC_TITLE;
static int  g_titleSel = 0;     /* 0=继续冒险 1=新的旅程 */
static int  g_nameIdx = 0;
static int  g_need_refresh = 1;
static int  g_last_scene = -1;
static uint32_t g_last_ui_ms = 0;
static uint32_t g_last_batt_ms = 0;
static uint32_t g_last_save_ms = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

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

static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();

    g_root = lv_obj_create(scr);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, SCR_W, SCR_H);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, C_BG, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(g_root, LV_SCROLLBAR_MODE_OFF);

    /* 顶栏 HUD（非标题/命名场景显示） */
    g_hud = lv_label_create(g_root);
    use_cjk(g_hud);
    lv_obj_set_style_text_color(g_hud, C_CYAN, 0);
    lv_obj_set_style_text_align(g_hud, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_hud, SCR_W);
    lv_obj_set_pos(g_hud, 0, 4);

    /* ---- 标题组 ---- */
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

    /* ---- 命名组 ---- */
    g_name_grp = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_name_grp);
    lv_obj_set_size(g_name_grp, SCR_W, SCR_H);
    lv_obj_set_pos(g_name_grp, 0, 0);
    lv_obj_set_style_bg_opa(g_name_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_name_grp, LV_SCROLLBAR_MODE_OFF);

    g_name_prompt = mk_label(g_name_grp, "为你的魔法师起名：", 0, 70, C_WHITE);
    g_name_val    = mk_label(g_name_grp, "", 0, 130, C_GOLD);
    g_name_hint   = mk_label(g_name_grp, "↑↓ 换名 · OK 确定", 0, 214, C_DIM);

    /* ---- 占位信息组（后续里程碑各玩法屏填进来后逐屏替换） ---- */
    g_info_grp = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_info_grp);
    lv_obj_set_size(g_info_grp, SCR_W, SCR_H);
    lv_obj_set_pos(g_info_grp, 0, 0);
    lv_obj_set_style_bg_opa(g_info_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_info_grp, LV_SCROLLBAR_MODE_OFF);

    g_info_title = mk_label(g_info_grp, "", 0, 120, C_GOLD);
    g_info_desc  = mk_label(g_info_grp, "（建设中 · 后续里程碑逐屏补全）\nOK 返回城镇", 0, 162, C_GRAY);
    lv_label_set_long_mode(g_info_desc, LV_LABEL_LONG_WRAP);

    /* ---- 星点装饰 ---- */
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

/* ===================== 刷新 ===================== */
static void set_hidden(lv_obj_t *o, bool hide) {
    if (!o) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_ui(void) {
    bool title   = (g_scene == SC_TITLE);
    bool naming  = (g_scene == SC_NAME);
    bool hud_on  = !title && !naming;

    set_hidden(g_title_grp, !title);
    set_hidden(g_name_grp,  !naming);
    set_hidden(g_info_grp,  title || naming);
    set_hidden(g_hud,       !hud_on);

    /* 星点常驻标题背景 */
    for (int i = 0; i < 14; i++) set_hidden(g_stars[i], !title);
    if (title) {
        uint32_t t = now_ms();
        for (int i = 0; i < 14; i++) {
            int on = ((t / 400) + i * 3) % 5;
            lv_obj_set_style_bg_opa(g_stars[i], on < 3 ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }

    if (title) {
        lv_obj_set_style_text_color(g_menu0, g_titleSel == 0 ? C_GOLD : C_GRAY, 0);
        lv_obj_set_style_text_color(g_menu1, g_titleSel == 1 ? C_GOLD : C_GRAY, 0);
        lv_label_set_text(g_menu0, g_titleSel == 0 ? "▶ 继续冒险" : "  继续冒险");
        lv_label_set_text(g_menu1, g_titleSel == 1 ? "▶ 新的旅程" : "  新的旅程");
    } else if (naming) {
        lv_label_set_text(g_name_val, NAMES[g_nameIdx % 7]);
    } else if (hud_on) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  Lv%d   金币 %d\nHP %d/%d   MP %d/%d",
                 RANKS[g.rank], g.level, g.gold, g.hp, g.maxHp, g.mp, g.maxMp);
        lv_label_set_text(g_hud, buf);
    }

    if (!title && !naming) {
        lv_label_set_text(g_info_title, SCENE_NAME[g_scene]);
    }
}

/* ===================== 按键处理 ===================== */
static void mage_set_scene(int s) { g_scene = s; g_need_refresh = 1; }

static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;   /* 本期只用到单击 */

    if (g_scene == SC_TITLE) {
        if (btn == BSP_BTN_UP)        { g_titleSel = 0; g_need_refresh = 1; }
        else if (btn == BSP_BTN_DOWN) { g_titleSel = 1; g_need_refresh = 1; }
        else if (btn == BSP_BTN_OK) {
            if (g_titleSel == 1) {            /* 新的旅程 */
                mage_new_game();
                mage_set_scene(SC_NAME);
            } else {                          /* 继续冒险 */
                if (g_hasSave) mage_set_scene(SC_CITY);
                else { mage_new_game(); mage_set_scene(SC_NAME); }
            }
        }
        return;
    }

    if (g_scene == SC_NAME) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            g_nameIdx = (g_nameIdx + (btn == BSP_BTN_UP ? 6 : 1)) % 7;
            g_need_refresh = 1;
        } else if (btn == BSP_BTN_OK) {
            strncpy(g.name, NAMES[g_nameIdx % 7], sizeof(g.name) - 1);
            g.name[sizeof(g.name) - 1] = '\0';
            mage_persist();
            mage_set_scene(SC_CITY);
        }
        return;
    }

    /* 其余玩法屏（建设中）：OK/任意方向键返回城镇 */
    if (btn == BSP_BTN_OK) { mage_set_scene(SC_CITY); g_need_refresh = 1; }
}

/* ===================== 主循环 ===================== */
static void game_task(void *arg) {
    (void)arg;
    for (;;) {
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);

        uint32_t now = now_ms();

        if (now - g_last_batt_ms > 500) { g_last_batt_ms = now; }
        if (now - g_last_save_ms > 30000) { mage_persist(); g_last_save_ms = now; }

        if (g_need_refresh || g_scene != g_last_scene ||
            now - g_last_ui_ms >= UI_REFRESH_MS) {
            g_last_ui_ms  = now;
            g_last_scene  = g_scene;
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

/* ===================== 入口 ===================== */
void mage_app_start(void) {
    ESP_LOGI(TAG, "魔法帝之路 固件启动");

    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));
    mage_init();

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();
        bsp_lvgl_unlock();
    }

    /* 按键回调（仅入队，由 game_task 消费，绝不在此碰 LVGL） */
    bsp_button_init(mage_on_key, NULL);
    xTaskCreate(game_task, "mage", 8192, NULL, 5, NULL);
}
