/* ember.c — 《余烬》主玩法（火灵「小余」· 每日 5 段柴 / 场景即界面）
 *
 * 美术分层（手册 M-03 / X-01 / X-02 / X-07）：
 *   img_bg     静态夜森林 RGB565 常量图，永不 recolor，森林不参与混色；
 *   img_spirit 唯一换色对象：真实母版骨架按同源 relightPixel 逐像素点亮，
 *              构建期预烘 6 档火色 ARGB8888 常量图，运行时仅切 src；
 *   img_card   S4 余烬卡：整屏宝可梦 Full-Art 全图卡（插画满版、文字极少）。
 *   全部资源放 Flash，不占 LV_MEM；无任何程序化几何拼形/程序化夜景。
 *
 * LVGL 铁律（ESP32-C3 单核 / 无 PSRAM）：
 *   1) 按键回调只入队，所有 lv_obj_* 在 bsp_lvgl_lock()/unlock() 内；
 *   2) refresh_ui 先比对「上次绘制状态」，只在翻转时调 LVGL，绝不周期刷新；
 *   3) 面板显式 pad_all=0；
 *   4) 所有 UI 平铺在 scr 上，显隐用「逐对象 + 可见性快照」切换，避免父子
 *      HIDDEN 恢复后子对象不显示的坑；
 *   5) 全屏贴图不做动画、不改样式；换火色只切 img src，且只在色相变化时。
 *
 * 三键矩阵（界面设计规范 V1）：◁左=UP  〇中=OK  ▷右=DOWN
 * 8 屏：S0 首夜 / S1 今夜 / S2 添柴 / S3 火相 / S4 夜话·余烬 /
 *       S5 复燃 / S6 浮层 / S7 借柴确认（借柴必须显式确认）
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "bsp_button.h"

#include "ember_color.h"

/* ---------------- 常量 ---------------- */
#define SCR_W 240
#define SCR_H 320
#define N_LAMPS 4
#define DAILY_WOOD 5                    /* 手册 X-03：每日 5 段柴 */
#define DEW_BONUS 2                     /* 离线底火给的晨露新柴 */
#define OFFLINE_MS (12LL * 3600 * 1000)
#define EMBER_FIRE_FRAMES 6             /* 5 档进程火色 + 1 档借柴黑蓝 */
#define POKE_WINDOW_MS 1500
#define POKE_TRIGGER 5
#define POKE_SHOW_MS 2000
#define UI_MAX 48

/* 外部资源（tools/gen_ember_assets.py 生成；assets/ember/ember_assets.c） */
extern const lv_img_dsc_t ember_bg_dsc;
extern const lv_img_dsc_t ember_spirit_dsc[EMBER_FIRE_FRAMES];
extern const lv_img_dsc_t ember_card_dsc[EMBER_FIRE_FRAMES];
extern const lv_font_t ember_cjk_16;
extern const lv_font_t ember_digit_34;

static const char *DIM_NAME[N_LAMPS] = {"工作", "关系", "热爱", "身体"};

/* 余烬夜话（按主轴维度取一句；面向成年打工族，不卖萌、不打分） */
static const char *NIGHT_WORDS[N_LAMPS] = {
    "今天你把热留给了手头的事，歇下来时，它替你亮着。",
    "你又为人操心了。小余说，被记挂的人，夜里不冷。",
    "那点热爱你还留着，没被日子磨掉，挺好的。",
    "你终于没忘了身体。它悄悄多烧了一截，谢谢你。",
};
static const char *NIGHT_WORD_BALANCED =
    "四盏都点到一点，不偏不倚。小余说，这样睡得最安稳。";

static const char *HINT_TONIGHT = "◁ 观火　〇 添柴　▷ 看火相";
static const char *HINT_ADD     = "◁ 选上一盏　〇 添一把　▷ 选下一盏";
static const char *HINT_EMPTY   = "柴尽了　〇 借明天的柴　▷ 算了";
static const char *HINT_FIRST   = "〇 开始";
static const char *HINT_PHASE   = "任意键返回";
static const char *HINT_BORROW  = "〇 借　▷ 算了";
static const char *HINT_CARD    = "柴尽了·明早见";
static const char *HINT_RELIGHT = "〇 回到今夜";

/* ---------------- 屏幕状态 ---------------- */
typedef enum {
    SC_FIRST = 0,   /* S0 */
    SC_TONIGHT,     /* S1 */
    SC_ADD,         /* S2 */
    SC_PHASE,       /* S3 */
    SC_CARD,        /* S4 */
    SC_RELIGHT,     /* S5 */
    SC_BORROW,      /* S7 */
} screen_t;

/* UI 图层：SCENE 常显（S4 全图卡整屏不透明会盖住它），其余按当前屏切换 */
typedef enum {
    LY_SCENE = 0,  /* 夜森林 + 精灵：所有屏共用底图（S4 全图卡除外） */
    LY_HUD,        /* 大数字 / 四灯 / 键提示：只在 S1、S2 出现 */
    LY_FIRST, LY_PHASE, LY_BORROW, LY_CARD, LY_RELIGHT, LY_POKE,
} layer_t;

/* ---------------- 游戏状态 ---------------- */
typedef struct {
    /* 持久化 */
    uint16_t night;
    uint32_t tot[N_LAMPS];
    uint32_t debt;
    uint32_t seed;
    int64_t  last_ms;
    /* 本夜运行 */
    uint8_t  lamp[N_LAMPS];
    int      wood_left;
    bool     borrowed;
    bool     dew;
    int      sel;
    screen_t screen;
    /* 火色 */
    float    fire_h, fire_s, fire_l;
    int      frame;
    char     color_name[24];
    char     color_hex[8];
    /* 防焦虑浮层（S6） */
    int      poke_cnt;
    int64_t  poke_t0;
    int64_t  poke_until;
} game_t;
static game_t g;

/* ---------------- UI 对象表（平铺，逐对象显隐） ---------------- */
static lv_obj_t *g_ui[UI_MAX];
static uint8_t   g_ui_layer[UI_MAX];
static bool      g_ui_vis[UI_MAX];
static int       g_ui_n;

static lv_obj_t *g_bg, *g_spirit, *g_card;
static lv_obj_t *g_lbl_wood, *g_lbl_unit, *g_lbl_night, *g_lbl_hint;
static lv_obj_t *g_lamp[N_LAMPS];
static lv_obj_t *g_phase_word, *g_card_word, *g_card_hex, *g_card_night;
static lv_obj_t *g_relight_night;

/* 绘制状态快照 */
static struct {
    int wood_left, night, sel, frame;
    bool borrowed;
    screen_t screen;
    bool poke;
    int lamp[N_LAMPS];
    char color_hex[8];
} drawn;

/* ---------------- BSP ---------------- */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q;

void ember_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ---------------- NVS ---------------- */
static nvs_handle_t s_nvs;

static void nvs_load(void) {
    esp_err_t r = nvs_open("ember", NVS_READWRITE, &s_nvs);
    if (r != ESP_OK) { memset(&g, 0, sizeof(g)); return; }
    uint16_t night = 1; nvs_get_u16(s_nvs, "night", &night); g.night = night;
    for (int i = 0; i < N_LAMPS; i++) {
        uint32_t v = 0; char k[8]; snprintf(k, sizeof(k), "t%d", i);
        nvs_get_u32(s_nvs, k, &v); g.tot[i] = v;
    }
    uint32_t debt = 0; nvs_get_u32(s_nvs, "debt", &debt); g.debt = debt;
    uint32_t seed = 0; nvs_get_u32(s_nvs, "seed", &seed);
    if (seed == 0) {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        seed = ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
               ((uint32_t)mac[2] << 8)  | (uint32_t)mac[3];
        if (seed == 0) seed = 0x9E3779B1u;
        nvs_set_u32(s_nvs, "seed", seed); nvs_commit(s_nvs);
    }
    g.seed = seed;
    int64_t last = 0; nvs_get_i64(s_nvs, "last", &last); g.last_ms = last;
}

static void nvs_save(void) {
    if (s_nvs == 0) return;
    nvs_set_u16(s_nvs, "night", g.night);
    for (int i = 0; i < N_LAMPS; i++) {
        char k[8]; snprintf(k, sizeof(k), "t%d", i); nvs_set_u32(s_nvs, k, g.tot[i]);
    }
    nvs_set_u32(s_nvs, "debt", g.debt);
    nvs_set_i64(s_nvs, "last", (int64_t)(esp_timer_get_time() / 1000));
    nvs_commit(s_nvs);
}

/* ---------------- 火色（只在添柴/借柴/过夜时算一次） ---------------- */
static void recompute_fire(void) {
    uint8_t tot4[EMBER_NDIM];
    for (int i = 0; i < N_LAMPS; i++) tot4[i] = (g.tot[i] > 255) ? 255 : (uint8_t)g.tot[i];
    ember_color_t c;
    ember_colorFor(tot4, g.borrowed, &c);
    float off = (float)((int32_t)(g.seed & 0x7) - 3);
    if (off > 2.0f) off = 2.0f;
    if (off < -2.0f) off = -2.0f;
    g.fire_h = c.h + off * 0.6f;
    g.fire_s = c.s;
    g.fire_l = c.l;
    strncpy(g.color_name, ember_colorName(c.h, c.s, c.l), sizeof(g.color_name) - 1);
    g.color_name[sizeof(g.color_name) - 1] = '\0';
    strncpy(g.color_hex, c.hex, sizeof(g.color_hex) - 1);
    g.color_hex[sizeof(g.color_hex) - 1] = '\0';

    /* 火色档位：色号一律来自事实库；借柴沉黑蓝；非暖色相（理论不可达）
       回落到本色档，避免蓝/绿系错切。5 档覆盖「暮橙 → 热爱3·工作2」。 */
    if (g.borrowed) {
        g.frame = EMBER_FIRE_FRAMES - 1;
    } else if (g.fire_h > 90.0f) {
        g.frame = 0;
    } else if (g.fire_h >= 30.5f) {
        g.frame = 0;
    } else if (g.fire_h >= 27.4f) {
        g.frame = 1;
    } else if (g.fire_h >= 24.6f) {
        g.frame = 2;
    } else if (g.fire_h >= 20.8f) {
        g.frame = 3;
    } else {
        g.frame = 4;
    }
}

/* ---------------- UI 构建 ---------------- */
static void reg(lv_obj_t *o, layer_t ly) {
    if (g_ui_n >= UI_MAX) return;
    g_ui[g_ui_n] = o;
    g_ui_layer[g_ui_n] = (uint8_t)ly;
    g_ui_vis[g_ui_n] = true;
    g_ui_n++;
    lv_obj_set_style_pad_all(o, 0, 0);
}

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const char *txt,
                          lv_color_t color, lv_align_t align, layer_t ly,
                          bool big) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, big ? &ember_digit_34 : &ember_cjk_16, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, align, x, y);
    reg(l, ly);
    return l;
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int opa, layer_t ly) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_make(8, 12, 18), 0);
    lv_obj_set_style_bg_opa(p, (lv_opa_t)opa, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    reg(p, ly);
    return p;
}

/* LV_COLOR_DEPTH_16 下 lv_color_t 是 5/6/5 位域，且 lv_color_make 是内联函数
   不能作静态初始化器；用宏在调用处求值，避免颜色被截断。 */
#define C_CREAM lv_color_make(245, 225, 190)
#define C_DIM   lv_color_make(186, 172, 152)
#define C_GOLD  lv_color_make(255, 208, 140)

static void build_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* LY_SCENE：静态夜森林 + 唯一换色精灵（真实母版，非几何拼形） */
    g_bg = lv_img_create(scr);
    lv_img_set_src(g_bg, &ember_bg_dsc);
    lv_obj_set_pos(g_bg, 0, 0);
    reg(g_bg, LY_SCENE);

    g_spirit = lv_img_create(scr);
    lv_img_set_src(g_spirit, &ember_spirit_dsc[0]);
    lv_obj_set_pos(g_spirit, 0, 0);
    reg(g_spirit, LY_SCENE);

    /* HUD：左上 34px 大数字 +「柴」，右上第 N 夜（X-06） */
    g_lbl_wood  = mk_label(scr, 13, 6, "5", C_CREAM, LV_ALIGN_TOP_LEFT, LY_HUD, true);
    g_lbl_unit  = mk_label(scr, 16, 44, "柴", C_DIM, LV_ALIGN_TOP_LEFT, LY_HUD, false);
    g_lbl_night = mk_label(scr, -8, 8, "第 1 夜", C_DIM, LV_ALIGN_TOP_RIGHT, LY_HUD, false);

    /* 四灯：底部 (53,264)(98,274)(144,268)(190,258) φ8，无文字标签（X-06） */
    const int lamp_x[N_LAMPS] = {53, 98, 144, 190};
    const int lamp_y[N_LAMPS] = {264, 274, 268, 258};
    for (int i = 0; i < N_LAMPS; i++) {
        lv_obj_t *o = lv_obj_create(scr);
        lv_obj_set_size(o, 8, 8);
        lv_obj_set_style_radius(o, 4, 0);
        lv_obj_set_style_bg_color(o, lv_color_make(74, 68, 60), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_border_color(o, C_GOLD, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(o, lamp_x[i] - 4, lamp_y[i] - 4);
        reg(o, LY_HUD);
        g_lamp[i] = o;
    }

    g_lbl_hint = mk_label(scr, 0, -8, HINT_TONIGHT, C_DIM,
                          LV_ALIGN_BOTTOM_MID, LY_HUD, false);

    /* S0 首夜 */
    mk_panel(scr, 150, LY_FIRST);
    mk_label(scr, 0, 40, "今晚，火先亮着。", C_CREAM, LV_ALIGN_TOP_MID, LY_FIRST, false);
    mk_label(scr, 0, -12, HINT_FIRST, C_DIM, LV_ALIGN_BOTTOM_MID, LY_FIRST, false);

    /* S3 火相 */
    mk_panel(scr, 170, LY_PHASE);
    g_phase_word = mk_label(scr, 0, 24, "今天分得匀，火很稳。", C_CREAM,
                            LV_ALIGN_TOP_MID, LY_PHASE, false);
    mk_label(scr, 0, -12, HINT_PHASE, C_DIM, LV_ALIGN_BOTTOM_MID, LY_PHASE, false);

    /* S7 借柴确认（必须显式确认，不得柴尽再按即自动借） */
    mk_panel(scr, 190, LY_BORROW);
    mk_label(scr, 0, 30, "要借明天的柴吗？", C_CREAM, LV_ALIGN_TOP_MID, LY_BORROW, false);
    mk_label(scr, 0, 56, "火会亮，也会发灰。", C_CREAM, LV_ALIGN_TOP_MID, LY_BORROW, false);
    mk_label(scr, 0, -12, HINT_BORROW, C_DIM, LV_ALIGN_BOTTOM_MID, LY_BORROW, false);

    /* S5 复燃 */
    mk_panel(scr, 120, LY_RELIGHT);
    mk_label(scr, 0, 30, "你不在的时候，", C_CREAM,
             LV_ALIGN_TOP_MID, LY_RELIGHT, false);
    mk_label(scr, 0, 54, "它自己缓过来了。", C_CREAM,
             LV_ALIGN_TOP_MID, LY_RELIGHT, false);
    g_relight_night = mk_label(scr, 0, 84, "第 2 夜", C_GOLD,
                               LV_ALIGN_TOP_MID, LY_RELIGHT, false);
    mk_label(scr, 0, -12, HINT_RELIGHT, C_DIM, LV_ALIGN_BOTTOM_MID, LY_RELIGHT, false);

    /* S6 浮层：高频连按，不打断、不计数、不换屏 */
    mk_label(scr, 0, -40, "你又在硬撑了。", C_GOLD,
             LV_ALIGN_BOTTOM_MID, LY_POKE, false);

    /* S4 余烬卡：整屏全图大卡（插画满版，文字只留角标） */
    g_card = lv_img_create(scr);
    lv_img_set_src(g_card, &ember_card_dsc[0]);
    lv_obj_set_pos(g_card, 0, 0);
    reg(g_card, LY_CARD);
    g_card_night = mk_label(scr, -14, 36, "第 1 夜", C_DIM,
                            LV_ALIGN_TOP_RIGHT, LY_CARD, false);
    g_card_hex = mk_label(scr, -14, 18, "#DF7C4F", C_GOLD,
                          LV_ALIGN_TOP_RIGHT, LY_CARD, false);
    g_card_word = mk_label(scr, 0, -26, "—", C_CREAM,
                           LV_ALIGN_BOTTOM_MID, LY_CARD, false);
    mk_label(scr, 0, -8, HINT_CARD, C_DIM, LV_ALIGN_BOTTOM_MID, LY_CARD, false);

    /* 初始：全部非 MAIN 层隐藏（逐对象切 flag，避免父子 HIDDEN 坑） */
    for (int i = 0; i < g_ui_n; i++) {
        if (g_ui_layer[i] != LY_SCENE) {
            lv_obj_add_flag(g_ui[i], LV_OBJ_FLAG_HIDDEN);
            g_ui_vis[i] = false;
        }
    }
}

/* ---------------- 屏幕流程 ---------------- */
static layer_t layer_of(screen_t s) {
    switch (s) {
        case SC_FIRST:   return LY_FIRST;
        case SC_PHASE:   return LY_PHASE;
        case SC_BORROW:  return LY_BORROW;
        case SC_CARD:    return LY_CARD;
        case SC_RELIGHT: return LY_RELIGHT;
        default:         return LY_SCENE;   /* S1/S2：无覆盖层，只显示场景 + HUD */
    }
}

/* 只改状态；所有 LVGL 文案更新统一放到 refresh_ui() 的锁内。 */
static void enter_card(void) {
    g.screen = SC_CARD;
}

static void next_night(void) {
    g.night += 1;
    for (int i = 0; i < N_LAMPS; i++) g.lamp[i] = 0;
    g.borrowed = false;
    g.dew = false;
    g.sel = 0;
    int w = DAILY_WOOD - (int)g.debt;
    if (w < 0) w = 0;
    g.debt = 0;
    g.wood_left = w;
    g.screen = SC_TONIGHT;
    recompute_fire();
    nvs_save();
}

static void poke_hit(int64_t now) {
    if (now - g.poke_t0 > POKE_WINDOW_MS) {
        g.poke_cnt = 0;
        g.poke_t0 = now;
    }
    g.poke_cnt++;
    if (g.poke_cnt >= POKE_TRIGGER) {
        g.poke_until = now + POKE_SHOW_MS;
        g.poke_cnt = 0;
    }
}

/* ---------------- 输入 ---------------- */
static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;   /* 只用单击，避免 PRESS/CLICK 双触发 */
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    poke_hit(now);

    switch (g.screen) {
        case SC_FIRST:
            if (btn == BSP_BTN_OK) g.screen = SC_TONIGHT;
            return;

        case SC_RELIGHT:
            if (btn == BSP_BTN_OK) g.screen = SC_TONIGHT;
            return;

        case SC_PHASE:
            g.screen = SC_TONIGHT;      /* 任意键返回 */
            return;

        case SC_BORROW:
            if (btn == BSP_BTN_DOWN) { enter_card(); return; }   /* ▷ 算了 */
            if (btn == BSP_BTN_OK) {                              /* 〇 借 */
                g.lamp[g.sel]++; g.tot[g.sel]++; g.borrowed = true; g.debt++;
                recompute_fire(); nvs_save();
                g.screen = SC_ADD;
            }
            return;

        case SC_CARD:
            next_night();               /* 任意键过夜 */
            return;

        case SC_TONIGHT:
            if (btn == BSP_BTN_OK) { g.screen = SC_ADD; return; }
            if (btn == BSP_BTN_DOWN) { g.screen = SC_PHASE; return; }
            return;                     /* ◁ 观火：不跳屏 */

        case SC_ADD:
        default:
            if (btn == BSP_BTN_UP) {
                g.sel = (g.sel + N_LAMPS - 1) % N_LAMPS;
                return;
            }
            if (btn == BSP_BTN_DOWN) {
                g.sel = (g.sel + 1) % N_LAMPS;
                return;
            }
            if (btn == BSP_BTN_OK) {
                if (g.wood_left <= 0) { g.screen = SC_BORROW; return; }
                g.lamp[g.sel]++; g.tot[g.sel]++; g.wood_left--;
                recompute_fire(); nvs_save();
                /* 柴尽不硬结束：按手册「柴尽了·明早见」由玩家自己收尾，
                   仍可按 〇 进 S7 借柴、▷ 直接翻余烬卡 */
            }
            return;
    }
}

/* ---------------- 刷新（只动翻转的部分） ---------------- */
static void refresh_ui(void) {
    /* 统一旧快照：所有比对都用本帧开始时的值，避免「先更新再比较」漏刷新 */
    const int      old_wood  = drawn.wood_left;
    const int      old_night = drawn.night;
    const int      old_sel   = drawn.sel;
    const int      old_frame = drawn.frame;
    const screen_t old_scr   = drawn.screen;
    const bool     old_poke  = drawn.poke;

    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    bool poke = (now < g.poke_until);
    layer_t want = layer_of(g.screen);

    /* 1) 图层显隐：逐对象，比对快照（避免父子 HIDDEN 恢复不显示）
     *    场景层除 S4 全图卡外常显；HUD 只在 S1/S2 出现，覆盖屏不叠加两套提示。 */
    bool scene_vis = (g.screen != SC_CARD);
    bool hud_vis = (g.screen == SC_TONIGHT || g.screen == SC_ADD);
    for (int i = 0; i < g_ui_n; i++) {
        uint8_t ly = g_ui_layer[i];
        bool vis;
        if (ly == LY_SCENE) {
            vis = scene_vis;
        } else if (ly == LY_HUD) {
            vis = hud_vis;
        } else if (ly == LY_POKE) {
            vis = poke;
        } else {
            vis = (ly == (uint8_t)want);
        }
        if (vis != g_ui_vis[i]) {
            if (vis) {
                lv_obj_clear_flag(g_ui[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_ui[i], LV_OBJ_FLAG_HIDDEN);
            }
            g_ui_vis[i] = vis;
        }
    }

    /* 2) 火色切档（唯一换色对象；只在档位翻转时切，绝不周期刷新） */
    if (old_frame != g.frame) {
        lv_img_set_src(g_spirit, &ember_spirit_dsc[g.frame]);
        lv_img_set_src(g_card, &ember_card_dsc[g.frame]);
    }

    /* 3) HUD：左上大数字 / 第 N 夜 */
    if (old_wood != g.wood_left) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", g.wood_left);
        lv_label_set_text(g_lbl_wood, buf);
    }
    if (old_night != (int)g.night) {
        char buf[80]; snprintf(buf, sizeof(buf), "第 %d 夜", (int)g.night);
        lv_label_set_text(g_lbl_night, buf);
        lv_label_set_text(g_relight_night, buf);
    }

    /* 4) 四灯：投过柴亮维度实色，选中加描边；只在变化时改 */
    bool in_add = (g.screen == SC_ADD || g.screen == SC_TONIGHT);
    for (int i = 0; i < N_LAMPS; i++) {
        if (drawn.lamp[i] != (int)g.lamp[i]) {
            if (g.lamp[i] > 0) {
                ember_rgb_t rgb; ember_dim_rgb(i, &rgb);
                lv_obj_set_style_bg_color(g_lamp[i],
                    lv_color_make((uint8_t)rgb.r, (uint8_t)rgb.g, (uint8_t)rgb.b), 0);
            } else {
                lv_obj_set_style_bg_color(g_lamp[i], lv_color_make(74, 68, 60), 0);
            }
        }
        if (old_sel != g.sel || old_scr != g.screen) {
            lv_obj_set_style_border_width(g_lamp[i], (in_add && g.sel == i) ? 2 : 0, 0);
        }
    }

    /* 5) 底部键提示 */
    const char *hint = HINT_ADD;
    if (g.screen == SC_TONIGHT) hint = HINT_TONIGHT;
    if (g.screen == SC_ADD) hint = (g.wood_left > 0) ? HINT_ADD : HINT_EMPTY;
    if (old_scr != g.screen || old_wood != g.wood_left) {
        lv_label_set_text(g_lbl_hint, hint);
    }

    /* 6) 进屏文案（全部在锁内） */
    if (old_scr != g.screen) {
        if (g.screen == SC_PHASE) {
            ember_color_t c; uint8_t tot4[EMBER_NDIM];
            for (int i = 0; i < N_LAMPS; i++) tot4[i] = (g.tot[i] > 255) ? 255 : (uint8_t)g.tot[i];
            ember_colorFor(tot4, false, &c);
            lv_label_set_text(g_phase_word,
                c.balanced ? "四盏都点到，火很稳。" : "今天你把热，分给了最要紧的那盏。");
        }
        if (g.screen == SC_CARD) {
            ember_color_t c; uint8_t tot4[EMBER_NDIM];
            for (int i = 0; i < N_LAMPS; i++) tot4[i] = (g.tot[i] > 255) ? 255 : (uint8_t)g.tot[i];
            ember_colorFor(tot4, false, &c);
            const char *line = c.balanced ? NIGHT_WORD_BALANCED
                                          : NIGHT_WORDS[c.main_i >= 0 ? c.main_i : 0];
            char buf[80];
            lv_label_set_text(g_card_word, line);
            lv_label_set_text(g_card_hex, g.color_hex);
            snprintf(buf, sizeof(buf), "第 %d 夜", (int)g.night);
            lv_label_set_text(g_card_night, buf);
        }
    }
    (void)old_poke;

    /* 7) 落盘快照 */
    drawn.wood_left = g.wood_left;
    drawn.night = (int)g.night;
    drawn.sel = g.sel;
    drawn.frame = g.frame;
    drawn.screen = g.screen;
    drawn.borrowed = g.borrowed;
    drawn.poke = poke;
    for (int i = 0; i < N_LAMPS; i++) drawn.lamp[i] = (int)g.lamp[i];
    strncpy(drawn.color_hex, g.color_hex, sizeof(drawn.color_hex) - 1);
    drawn.color_hex[sizeof(drawn.color_hex) - 1] = '\0';
}

/* ---------------- 主循环 ---------------- */
static void game_task(void *arg) {
    (void)arg;
    while (1) {
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);
        if (bsp_lvgl_lock(100)) { refresh_ui(); bsp_lvgl_unlock(); }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* ---------------- 应用入口（由 ember_main.c 调用） ---------------- */
void ember_app_start(void) {
    memset(&g, 0, sizeof(g));
    memset(&drawn, 0, sizeof(drawn));
    memset(g_ui, 0, sizeof(g_ui));
    g_ui_n = 0;
    nvs_load();

    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    int dew_bonus = 0;
    bool fresh = (g.last_ms == 0);
    if (!fresh && (now - g.last_ms) > OFFLINE_MS) {
        g.dew = true; dew_bonus = DEW_BONUS;
    }
    int w = DAILY_WOOD - (int)g.debt + dew_bonus;
    if (w < 0) w = 0;
    g.wood_left = w;

    if (fresh) g.screen = SC_FIRST;
    else if (g.dew) g.screen = SC_RELIGHT;
    else g.screen = SC_TONIGHT;

    recompute_fire();
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();          /* 首帧全量 */
        bsp_lvgl_unlock();
    }
    nvs_save();

    xTaskCreate(game_task, "ember", 6144, NULL, 5, NULL);
}
