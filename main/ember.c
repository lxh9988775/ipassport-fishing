/* ember.c — 《余烬》主玩法（火灵「小余」每日分柴 / 看火色 / 余烬夜话）
 *
 * 架构（严格对齐 fishing.c / pet.c 已验证范式）：
 *   - 按键回调 ember_on_key 仅入队，由 game_task 消费（绝不在中断/BSP 回调里碰 LVGL）；
 *   - 所有 LVGL 操作都包在 bsp_lvgl_lock()/unlock() 内；
 *   - refresh_ui() 先比对“上次绘制状态”，只在翻转时调 LVGL（LVGL 铁律 2：不进周期刷新）；
 *   - 火色只在【添柴/借柴/过夜】时按色阶引擎算一次，比对变了才重染（设计案第八节）。
 *
 * 资源策略（无 PSRAM 安全）：
 *   - 夜森林背景 = 静态 ARGB8888 常量图（放 Flash，一像素不碰）；
 *   - 火焰精灵 = LVGL 矢量层，4 档明度（暗外焰→白热芯）由色阶引擎实时上色，零额外 RAM。
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
#define N_SEL   (N_LAMPS + 1)          /* 4 盏灯 + 1 个「熄灯」 */
#define DAILY_WOOD 8                   /* 每日木柴段数（设计案：约 8 段） */
#define DEW_BONUS 2                    /* 离线底火给的晨露新柴 */
#define OFFLINE_MS (12LL * 3600 * 1000) /* 离线 >12h 视为攒出底火 */

static const char *DIM_NAME[N_LAMPS] = {"工作", "关系", "热爱", "身体"};

/* 余烬夜话（按主轴维度取一句，对齐设计案灵魂：被理解的感觉） */
static const char *NIGHT_WORDS[N_LAMPS] = {
    "今天你把热留给了手头的事，歇下来时，它替你亮着。",
    "你又为人操心了。小余说，被记挂的人，夜里不冷。",
    "那点热爱你还留着，没被日子磨掉，挺好的。",
    "你终于没忘了身体。它悄悄多烧了一截，谢谢你。",
};
static const char *NIGHT_WORD_BALANCED =
    "四盏都点到一点，不偏不倚。小余说，这样睡得最安稳。";

/* ---------------- 状态 ---------------- */
typedef struct {
    /* 持久化 */
    uint16_t night;
    uint32_t tot[N_LAMPS];     /* 跨天累计段数（主轴判定来源） */
    uint32_t debt;             /* 借明日柴欠下的量（次夜扣减） */
    uint32_t seed;             /* 每只设备唯一底色偏移种子 */
    int64_t  last_ms;          /* 上次活跃单调毫秒，离线底火用 */
    /* 本夜运行 */
    uint8_t  lamp[N_LAMPS];    /* 本夜投入各灯段数 */
    int      wood_left;
    bool     borrowed;         /* 本夜是否借过柴（发灰/黑蓝） */
    bool     dew;              /* 本夜是否带晨露新柴 */
    int      sel;              /* 选中项 0..3=灯, 4=熄灯 */
    bool     show_color;       /* 看火色 overlay */
    bool     ember_card;       /* 余烬卡 */
    /* 火色（recompute_fire 更新） */
    float    fire_h, fire_s, fire_l;
    char     color_name[24];
    char     color_hex[8];
    /* 防焦虑戳戳 */
    int      poke_cnt;
    int64_t  poke_t0;
    int64_t  penalty_until;
} game_t;
static game_t g;

/* ---------------- UI 对象 ---------------- */
static lv_obj_t *g_bg;
static lv_obj_t *g_spirit, *g_halo, *g_body, *g_core, *g_eye_l, *g_eye_r, *g_mouth;
static lv_obj_t *g_lamp[N_LAMPS], *g_lamp_lbl[N_LAMPS];
static lv_obj_t *g_lbl_wood, *g_lbl_night, *g_lbl_hint;
static lv_obj_t *g_color_ovl, *g_color_name, *g_color_hex, *g_color_dim[N_LAMPS];
static lv_obj_t *g_card, *g_card_title, *g_card_line, *g_card_name;

/* 绘制状态快照（LVGL 铁律 2：只在翻转时刷新） */
static struct {
    int sel; int wood_left; int night; bool borrowed; bool show_color; bool ember_card;
    uint8_t lamp[N_LAMPS];
    int fire_h100, fire_s100, fire_l100;  /* 放大 100 倍比对 */
    char color_name[24]; char color_hex[8];
    int poke_state;                        /* 0 正常 / 1 惩罚中 */
} drawn;

/* 外部资源 */
extern const lv_img_dsc_t ember_bg_dsc;
extern lv_font_t ember_cjk_16;

/* ---------------- BSP ---------------- */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q;

void ember_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
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
        uint32_t v = 0; char k[4]; snprintf(k, sizeof(k), "t%d", i);
        nvs_get_u32(s_nvs, k, &v); g.tot[i] = v;
    }
    uint32_t debt = 0; nvs_get_u32(s_nvs, "debt", &debt); g.debt = debt;
    uint32_t seed = 0; nvs_get_u32(s_nvs, "seed", &seed);
    if (seed == 0) {                       /* 首次开机：写唯一底色种子 */
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
        char k[4]; snprintf(k, sizeof(k), "t%d", i); nvs_set_u32(s_nvs, k, g.tot[i]);
    }
    nvs_set_u32(s_nvs, "debt", g.debt);
    nvs_set_i64(s_nvs, "last", (int64_t)(esp_timer_get_time() / 1000));
    nvs_commit(s_nvs);
}

/* ---------------- 火色计算（只在状态变化时调用） ---------------- */
static void recompute_fire(void) {
    uint8_t tot4[EMBER_NDIM];
    for (int i = 0; i < N_LAMPS; i++) tot4[i] = (g.tot[i] > 255) ? 255 : (uint8_t)g.tot[i];
    ember_color_t c;
    ember_colorFor(tot4, g.borrowed, &c);
    /* 唯一底色偏移：每台设备 ±2° 微差（seed 决定，持久稳定） */
    float off = (float)((int32_t)(g.seed & 0x7) - 3);   /* -3..+3 */
    if (off > 2) off = 2;
    if (off < -2) off = -2;
    g.fire_h = c.h + off * 0.6f;
    g.fire_s = c.s;
    g.fire_l = c.l;
    strncpy(g.color_name, ember_colorName(c.h, c.s, c.l), sizeof(g.color_name) - 1);
    g.color_name[sizeof(g.color_name) - 1] = '\0';
    strncpy(g.color_hex, c.hex, sizeof(g.color_hex) - 1);
    g.color_hex[sizeof(g.color_hex) - 1] = '\0';
}

/* ---------------- 火焰分层上色 ---------------- */
static void flame_set(lv_obj_t *o, float h, float s, float l) {
    ember_rgb_t rgb; ember_hslToRgb(h, s, l, &rgb);
    lv_obj_set_style_bg_color(o, lv_color_make((uint8_t)rgb.r, (uint8_t)rgb.g, (uint8_t)rgb.b), 0);
}

static void recolor_spirit(void) {
    float h = g.fire_h, s = g.fire_s;
    /* 四档明度单调递增、色相固定（设计案第七节） */
    flame_set(g_halo, h, s * 0.85f, 0.30f);
    flame_set(g_body, h, s * 0.95f, 0.52f);
    flame_set(g_core, h, s * 0.50f, 0.92f);   /* 白热芯：近白、略带色相 */
    /* 发灰/借柴：整体压暗一档，读起来“凉” */
    if (g.borrowed) {
        lv_obj_set_style_bg_opa(g_halo, 70, 0);
        lv_obj_set_style_bg_opa(g_body, 200, 0);
    } else {
        lv_obj_set_style_bg_opa(g_halo, 110, 0);
        lv_obj_set_style_bg_opa(g_body, LV_OPA_COVER, 0);
    }
}

/* ---------------- 建 UI ---------------- */
static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const char *txt,
                          lv_color_t color, lv_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &ember_cjk_16, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_align(l, align, x, y);
    return l;
}

static void build_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 静态夜森林背景 */
    g_bg = lv_img_create(scr);
    lv_img_set_src(g_bg, &ember_bg_dsc);
    lv_obj_set_pos(g_bg, 0, 0);

    /* 火焰精灵容器（pad_all=0，子元素坐标相对内容区） */
    g_spirit = lv_obj_create(scr);
    lv_obj_set_size(g_spirit, 130, 150);
    lv_obj_set_pos(g_spirit, 55, 148);
    lv_obj_set_style_bg_opa(g_spirit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_spirit, 0, 0);
    lv_obj_set_style_pad_all(g_spirit, 0, 0);
    lv_obj_clear_flag(g_spirit, LV_OBJ_FLAG_SCROLLABLE);

    g_halo = lv_obj_create(g_spirit);
    lv_obj_set_size(g_halo, 116, 116);
    lv_obj_set_style_radius(g_halo, 58, 0);
    lv_obj_set_style_border_width(g_halo, 0, 0);
    lv_obj_set_style_pad_all(g_halo, 0, 0);
    lv_obj_align(g_halo, LV_ALIGN_CENTER, 0, 12);

    g_body = lv_obj_create(g_spirit);
    lv_obj_set_size(g_body, 76, 96);
    lv_obj_set_style_radius(g_body, 38, 0);
    lv_obj_set_style_border_width(g_body, 0, 0);
    lv_obj_set_style_pad_all(g_body, 0, 0);
    lv_obj_align(g_body, LV_ALIGN_CENTER, 0, 20);

    g_core = lv_obj_create(g_spirit);
    lv_obj_set_size(g_core, 44, 54);
    lv_obj_set_style_radius(g_core, 22, 0);
    lv_obj_set_style_border_width(g_core, 0, 0);
    lv_obj_set_style_pad_all(g_core, 0, 0);
    lv_obj_align(g_core, LV_ALIGN_CENTER, 0, 24);

    g_eye_l = lv_obj_create(g_core);
    lv_obj_set_size(g_eye_l, 8, 8);
    lv_obj_set_style_radius(g_eye_l, 4, 0);
    lv_obj_set_style_bg_color(g_eye_l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_eye_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_eye_l, 0, 0);
    lv_obj_set_style_pad_all(g_eye_l, 0, 0);
    lv_obj_align(g_eye_l, LV_ALIGN_CENTER, -9, -6);

    g_eye_r = lv_obj_create(g_core);
    lv_obj_set_size(g_eye_r, 8, 8);
    lv_obj_set_style_radius(g_eye_r, 4, 0);
    lv_obj_set_style_bg_color(g_eye_r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_eye_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_eye_r, 0, 0);
    lv_obj_set_style_pad_all(g_eye_r, 0, 0);
    lv_obj_align(g_eye_r, LV_ALIGN_CENTER, 9, -6);

    static lv_point_precise_t mouth_pts[] = {{-7, 4}, {0, 9}, {7, 4}};
    g_mouth = lv_line_create(g_core);
    lv_line_set_points(g_mouth, mouth_pts, 3);
    lv_obj_set_style_line_color(g_mouth, lv_color_black(), 0);
    lv_obj_set_style_line_width(g_mouth, 2, 0);
    lv_obj_align(g_mouth, LV_ALIGN_CENTER, 0, 8);

    /* 4 盏灯（中部一排） */
    int lamp_x[N_LAMPS] = {30, 88, 146, 204};
    for (int i = 0; i < N_LAMPS; i++) {
        g_lamp[i] = lv_obj_create(scr);
        lv_obj_set_size(g_lamp[i], 22, 22);
        lv_obj_set_style_radius(g_lamp[i], 11, 0);
        lv_obj_set_style_bg_color(g_lamp[i], lv_color_make(40, 40, 52), 0);
        lv_obj_set_style_bg_opa(g_lamp[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(g_lamp[i], lv_color_white(), 0);
        lv_obj_set_style_border_width(g_lamp[i], 0, 0);
        lv_obj_set_style_pad_all(g_lamp[i], 0, 0);
        lv_obj_set_pos(g_lamp[i], lamp_x[i] - 11, 116);
        g_lamp_lbl[i] = mk_label(scr, lamp_x[i], 140, DIM_NAME[i],
                                 lv_color_make(210, 210, 220), LV_ALIGN_CENTER);
    }

    /* HUD：余柴（左上）、第N夜（右上） */
    g_lbl_wood = mk_label(scr, 4, 4, "余柴 8", lv_color_white(), LV_ALIGN_TOP_LEFT);
    g_lbl_night = mk_label(scr, -4, 4, "第1夜", lv_color_white(), LV_ALIGN_TOP_RIGHT);
    g_lbl_hint  = mk_label(scr, 0, 302,
                           "↑↓选灯  OK添柴  长OK看火色   当前：工作",
                           lv_color_make(200, 200, 210), LV_ALIGN_BOTTOM_MID);

    /* 看火色 overlay（默认隐藏） */
    g_color_ovl = lv_obj_create(scr);
    lv_obj_set_size(g_color_ovl, SCR_W, SCR_H);
    lv_obj_set_pos(g_color_ovl, 0, 0);
    lv_obj_set_style_bg_color(g_color_ovl, lv_color_make(8, 10, 22), 0);
    lv_obj_set_style_bg_opa(g_color_ovl, 235, 0);
    lv_obj_set_style_border_width(g_color_ovl, 0, 0);
    lv_obj_set_style_pad_all(g_color_ovl, 0, 0);
    lv_obj_add_flag(g_color_ovl, LV_OBJ_FLAG_HIDDEN);
    mk_label(g_color_ovl, 0, 18, "今夜火色", lv_color_make(255, 230, 180),
             LV_ALIGN_TOP_MID);
    for (int i = 0; i < N_LAMPS; i++) {
        lv_obj_t *bar = lv_obj_create(g_color_ovl);
        lv_obj_set_size(bar, 120, 14);
        lv_obj_set_style_radius(bar, 7, 0);
        ember_rgb_t rgb; ember_dim_rgb(i, &rgb);
        lv_obj_set_style_bg_color(bar, lv_color_make((uint8_t)rgb.r, (uint8_t)rgb.g, (uint8_t)rgb.b), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_pos(bar, 60, 50 + i * 24);
        g_color_dim[i] = mk_label(g_color_ovl, 50, 50 + i * 24, DIM_NAME[i],
                                  lv_color_white(), LV_ALIGN_LEFT_MID);
    }
    g_color_name = mk_label(g_color_ovl, 0, 170, "—", lv_color_make(255, 240, 200),
                            LV_ALIGN_TOP_MID);
    g_color_hex  = mk_label(g_color_ovl, 0, 196, "#000000", lv_color_make(180, 200, 220),
                            LV_ALIGN_TOP_MID);
    mk_label(g_color_ovl, 0, 296, "按任意键返回", lv_color_make(160, 160, 170),
             LV_ALIGN_BOTTOM_MID);

    /* 余烬卡（默认隐藏） */
    g_card = lv_obj_create(scr);
    lv_obj_set_size(g_card, SCR_W, SCR_H);
    lv_obj_set_pos(g_card, 0, 0);
    lv_obj_set_style_bg_color(g_card, lv_color_make(14, 8, 6), 0);
    lv_obj_set_style_bg_opa(g_card, 240, 0);
    lv_obj_set_style_border_width(g_card, 0, 0);
    lv_obj_set_style_pad_all(g_card, 0, 0);
    lv_obj_add_flag(g_card, LV_OBJ_FLAG_HIDDEN);
    g_card_title = mk_label(g_card, 0, 40, "余烬", lv_color_make(255, 200, 150),
                            LV_ALIGN_TOP_MID);
    g_card_line  = mk_label(g_card, 0, 110, "—", lv_color_make(230, 220, 210),
                            LV_ALIGN_TOP_MID);
    g_card_name  = mk_label(g_card, 0, 210, "—", lv_color_make(255, 230, 190),
                            LV_ALIGN_TOP_MID);
    mk_label(g_card, 0, 296, "明早见 — 按任意键", lv_color_make(160, 160, 170),
             LV_ALIGN_BOTTOM_MID);
}

/* ---------------- 熄灯 / 过夜 ---------------- */
static void end_night(void) {
    g.ember_card = true;
    /* 夜话按主轴维度取句（均衡则专用句） */
    ember_color_t c; uint8_t tot4[EMBER_NDIM];
    for (int i = 0; i < N_LAMPS; i++) tot4[i] = (g.tot[i] > 255) ? 255 : (uint8_t)g.tot[i];
    ember_colorFor(tot4, false, &c);
    const char *line = c.balanced ? NIGHT_WORD_BALANCED : NIGHT_WORDS[c.main_i >= 0 ? c.main_i : 0];
    lv_label_set_text(g_card_line, line);
    char buf[40];
    snprintf(buf, sizeof(buf), "今夜：%s  %s", g.color_name, g.color_hex);
    lv_label_set_text(g_card_name, buf);
}

static void next_night(void) {
    g.ember_card = false;
    g.night += 1;
    g.lamp[0] = g.lamp[1] = g.lamp[2] = g.lamp[3] = 0;
    g.borrowed = false;
    /* 借柴欠债次夜扣减（反透支） */
    int w = DAILY_WOOD - (int)g.debt;
    if (w < 0) w = 0;
    g.debt = 0;
    g.wood_left = w;
    recompute_fire();
    nvs_save();
}

/* ---------------- 输入 ---------------- */
static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);

    /* 余烬卡：任意键过夜 */
    if (g.ember_card) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) next_night();
        return;
    }
    /* 看火色 overlay：任意键关闭 */
    if (g.show_color) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) g.show_color = false;
        return;
    }

    if (ev != BSP_BTN_CLICK) return;   /* 只用单击，避免 PRESS/CLICK 双触发 */

    if (btn == BSP_BTN_UP) {
        g.sel = (g.sel + N_SEL - 1) % N_SEL;
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        g.sel = (g.sel + 1) % N_SEL;
        return;
    }
    if (btn == BSP_BTN_OK) {
        if (g.sel == N_LAMPS) {            /* 熄灯 → 翻余烬卡 */
            end_night();
            nvs_save();
            return;
        }
        /* 选中的是一盏灯：添柴 */
        if (g.wood_left > 0) {
            g.lamp[g.sel]++; g.tot[g.sel]++; g.wood_left--;
            recompute_fire(); nvs_save();
            g.poke_cnt = 0;
        } else {
            /* 柴已尽仍按 = 借明天的柴（反透支预警 + 发灰） */
            g.lamp[g.sel]++; g.tot[g.sel]++; g.borrowed = true; g.debt++;
            recompute_fire(); nvs_save();
            /* 防焦虑戳戳：短时间内反复借柴 */
            if (now - g.poke_t0 > 3000) { g.poke_cnt = 0; g.poke_t0 = now; }
            g.poke_cnt++;
            if (g.poke_cnt >= 4) {
                g.penalty_until = now + 1500;
                g.poke_cnt = 0;
            }
        }
        return;
    }
}

/* ---------------- 刷新（只动变化的部分） ---------------- */
static void refresh_ui(void) {
    /* 选择高亮：选中灯描白边，其余无 */
    for (int i = 0; i < N_LAMPS; i++) {
        bool sel = (g.sel == i) && !g.show_color && !g.ember_card;
        lv_obj_set_style_border_width(g_lamp[i], sel ? 2 : 0, 0);
    }
    /* 灯亮暗：投过柴=维度实色，没投=暗灰（不显示数字，只用亮/暗） */
    for (int i = 0; i < N_LAMPS; i++) {
        if (g.lamp[i] > 0) {
            ember_rgb_t rgb; ember_dim_rgb(i, &rgb);
            if (drawn.lamp[i] != g.lamp[i] || drawn.borrowed != g.borrowed) {
                lv_obj_set_style_bg_color(g_lamp[i],
                    lv_color_make((uint8_t)rgb.r, (uint8_t)rgb.g, (uint8_t)rgb.b), 0);
            }
        } else if (drawn.lamp[i] != g.lamp[i]) {
            lv_obj_set_style_bg_color(g_lamp[i], lv_color_make(40, 40, 52), 0);
        }
    }
    /* HUD 文字 */
    if (drawn.wood_left != g.wood_left) {
        char buf[16]; snprintf(buf, sizeof(buf), "余柴 %d", g.wood_left);
        lv_label_set_text(g_lbl_wood, buf);
    }
    if (drawn.night != g.night) {
        char buf[16]; snprintf(buf, sizeof(buf), "第%d夜", g.night);
        lv_label_set_text(g_lbl_night, buf);
    }
    /* 火色重染：比对 h/s/l（放大 100 比对，±0.005 容差） */
    int h100 = (int)(g.fire_h * 100), s100 = (int)(g.fire_s * 100), l100 = (int)(g.fire_l * 100);
    if (h100 != drawn.fire_h100 || s100 != drawn.fire_s100 || l100 != drawn.fire_l100 ||
        drawn.borrowed != g.borrowed) {
        recolor_spirit();
        drawn.fire_h100 = h100; drawn.fire_s100 = s100; drawn.fire_l100 = l100;
        drawn.borrowed = g.borrowed;
    }
    /* 看火色 overlay 内容 */
    if (g.show_color && strcmp(drawn.color_name, g.color_name) != 0) {
        lv_label_set_text(g_color_name, g.color_name);
        lv_label_set_text(g_color_hex, g.color_hex);
    }
    /* 防焦虑惩罚：短暂压暗精灵 + 提示 */
    int poke_state = (esp_timer_get_time() / 1000 < g.penalty_until) ? 1 : 0;
    if (poke_state != drawn.poke_state) {
        if (poke_state) {
            lv_obj_set_style_bg_opa(g_spirit, 120, 0);
            lv_label_set_text(g_lbl_hint, "别老戳我，我去歇会儿。");
        } else {
            lv_obj_set_style_bg_opa(g_spirit, LV_OPA_COVER, 0);
            lv_label_set_text(g_lbl_hint, "↑↓选灯  OK添柴  长OK看火色   当前：工作");
        }
        drawn.poke_state = poke_state;
    }
    /* 选中项名提示（非 overlay / 非卡时） */
    if (!g.show_color && !g.ember_card) {
        const char *nm = (g.sel < N_LAMPS) ? DIM_NAME[g.sel] : "熄灯";
        char buf[48]; snprintf(buf, sizeof(buf),
            "↑↓选灯  OK添柴  长OK看火色   当前：%s", nm);
        if (poke_state == 0) lv_label_set_text(g_lbl_hint, buf);
    }
    /* overlay / 卡 显隐（父级带 HIDDEN，子级自动跟随，LVGL 铁律 4） */
    if (drawn.show_color != g.show_color) {
        if (g.show_color) lv_obj_clear_flag(g_color_ovl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_color_ovl, LV_OBJ_FLAG_HIDDEN);
        drawn.show_color = g.show_color;
    }
    if (drawn.ember_card != g.ember_card) {
        if (g.ember_card) lv_obj_clear_flag(g_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_card, LV_OBJ_FLAG_HIDDEN);
        drawn.ember_card = g.ember_card;
    }

    /* 落盘快照 */
    drawn.sel = g.sel; drawn.wood_left = g.wood_left; drawn.night = g.night;
    drawn.borrowed = g.borrowed;
    memcpy(drawn.lamp, g.lamp, sizeof(g.lamp));
    strncpy(drawn.color_name, g.color_name, sizeof(drawn.color_name) - 1);
    drawn.color_name[sizeof(drawn.color_name) - 1] = '\0';
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
    nvs_load();

    /* 离线底火：距上次 >12h 给晨露新柴 */
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    int dew_bonus = 0;
    if (g.last_ms > 0 && (now - g.last_ms) > OFFLINE_MS) {
        g.dew = true; dew_bonus = DEW_BONUS;
    } else {
        g.dew = false;
    }
    /* 本夜木柴：固定 8 - 旧债 + 晨露 */
    int w = DAILY_WOOD - (int)g.debt + dew_bonus;
    if (w < 0) w = 0;
    g.wood_left = w;

    recompute_fire();

    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();          /* 首帧全量 */
        bsp_lvgl_unlock();
    }
    nvs_save();                /* 写回 last_ms */

    xTaskCreate(game_task, "ember", 6144, NULL, 5, NULL);
}
