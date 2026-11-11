/* ember.c — 《余烬》主玩法（火灵「小余」· 每日 5 段柴 / 场景即界面）
 *
 * 2026-11-11 真机返工（用户反馈「一点文字看不见 / 右边还有白线条」）：
 *   1) 白线：源图 ember_bg_clean.png 右侧 x=236..239 四列 320 行全白，
 *      生成器已按邻接列镜像回填，静态背景不再带白边。
 *   2) 文字：真机验证只有 lv_img 能稳定绘制，lv_label / lv_obj 经
 *      LV_OBJ_FLAG_HIDDEN 初始隐藏再恢复后全部不显示（连四灯圆点都没有）。
 *      故把 HUD 大数字、四灯、键提示、覆盖屏文案全部预烘成 ARGB8888
 *      透明贴图；运行时只用 lv_img_set_src 切图，不再使用任何 HIDDEN、
 *      label、panel。「隐藏」= 切到 4x4 全透明图 ember_none_dsc。
 *
 * 美术分层（手册 M-03 / X-01 / X-02 / X-07）：
 *   img_bg     静态夜森林 RGB565 常量图，永不 recolor；
 *   img_spirit 唯一换色对象，真实母版骨架按同源 relightPixel 逐像素点亮，
 *              构建期预烘 6 档火色 ARGB8888，运行时仅切 src；
 *   img_card   S4 余烬卡：整屏 Full-Art 全图卡，卡名/色号/夜话烘在图里；
 *   img_ovl    S0/S3/S5/S7 覆盖屏整屏贴图；
 *   img_wood / img_lamps / img_sel / img_hint  常显 HUD 贴图。
 *   全部放 Flash，不占 LV_MEM；无任何程序化几何拼形。
 *
 * 2026-11-11 二次真机返工（用户反馈「板子现在很烫」）：
 *   上一版 8 层全是 ARGB8888 半透明贴图，含整屏 240x320 遮罩，且每层被
 *   lv_obj_set_size 强制固定 240x320 —— 隐藏成 4x4 透明图后对象仍占满全屏，
 *   LVGL 每帧逐像素 alpha 混合几十万像素，C3 单核持续满载。
 *   现改：UI 全为不透明 RGB565（直接 memcpy）、覆盖屏缩成 240x80 文字条、
 *   隐藏图 1x1、去掉所有固定尺寸、主循环改事件驱动（200ms 而非 40ms 轮询）。
 *   运行时唯一大面积混合层只剩精灵；diag 日志量化锁等待与切图次数。
 *
 * LVGL 铁律（ESP32-C3 单核 / 无 PSRAM）：
 *   1) 按键回调只入队，所有 lv_obj_* 在 bsp_lvgl_lock()/unlock() 内；
 *   2) refresh_ui 用「上次绘制索引」比对，只在索引翻转时切图，绝不周期重绘；
 *   3) 无 label / 无样式动画 / 无 HIDDEN 恢复；
 *   4) 不做整屏半透明层，不固定图片对象尺寸。
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
#include "esp_log.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "bsp_button.h"

#include "ember_color.h"

/* ---------------- 常量 ---------------- */
#define SCR_W 240
#define SCR_H 320
#define N_LAMPS 4
#define DAILY_WOOD 5                    /* 手册 X-03：每日 5 段柴 */
#define DEW_BONUS 2
#define OFFLINE_MS (12LL * 3600 * 1000)
#define EMBER_FIRE_FRAMES 6
#define POKE_WINDOW_MS 1500
#define POKE_TRIGGER 5
#define POKE_SHOW_MS 2000

/* UI 贴图索引（与 tools/gen_ember_assets.py 的 HINTS / OVERLAYS 一一对应） */
#define HINT_FIRST    0
#define HINT_TONIGHT  1
#define HINT_ADD      2
#define HINT_PHASE    3
#define HINT_CARD     4
#define HINT_RELIGHT  5
#define HINT_POKE     6
#define HINT_BORROW   7
#define HINT_EMPTY    8
#define OVL_FIRST     0
#define OVL_PHASE     1
#define OVL_RELIGHT   2
#define OVL_BORROW    3

static const char *TAG = "ember";

/* 外部资源：全部是 lv_img_dsc_t，固件不再引用任何 LVGL 字体 */
extern const lv_img_dsc_t ember_bg_dsc;
extern const lv_img_dsc_t ember_spirit_dsc[EMBER_FIRE_FRAMES];
extern const lv_img_dsc_t ember_card_dsc[EMBER_FIRE_FRAMES];
extern const lv_img_dsc_t ember_none_dsc[1];
extern const lv_img_dsc_t ember_wood_dsc[6];
extern const lv_img_dsc_t ember_hint_dsc[9];
extern const lv_img_dsc_t ember_lamp_dsc[16];
extern const lv_img_dsc_t ember_sel_dsc[4];
extern const lv_img_dsc_t ember_ovl_dsc[4];

/* ---------------- 屏幕状态 ---------------- */
typedef enum {
    SC_FIRST = 0, SC_TONIGHT, SC_ADD, SC_PHASE,
    SC_CARD, SC_RELIGHT, SC_BORROW,
} screen_t;

/* ---------------- 游戏状态 ---------------- */
typedef struct {
    uint16_t night;
    uint32_t tot[N_LAMPS];
    uint32_t debt;
    uint32_t seed;
    int64_t  last_ms;
    uint8_t  lamp[N_LAMPS];
    int      wood_left;
    bool     borrowed;
    bool     dew;
    int      sel;
    screen_t screen;
    float    fire_h, fire_s, fire_l;
    int      frame;
    int      poke_cnt;
    int64_t  poke_t0;
    int64_t  poke_until;
} game_t;
static game_t g;

/* ---------------- UI 对象（全部 lv_img，常显，靠切 src 显隐） ---------------- */
static lv_obj_t *g_bg, *g_spirit, *g_ovl, *g_card;
static lv_obj_t *g_wood, *g_lamps, *g_sel, *g_hint;

/* 绘制索引快照：-1 表示该层当前切在透明图 */
static struct {
    int frame, card_frame, ovl, wood, lamp_bits, sel, hint;
    screen_t screen;
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
static lv_obj_t *mk_img(lv_obj_t *parent, const lv_img_dsc_t *d, int x, int y) {
    lv_obj_t *o = lv_img_create(parent);
    lv_img_set_src(o, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_pad_all(o, 0, 0);
    /* 不固定尺寸：隐藏时切到 1x1 图，对象随之缩到 1x1。
       上一版给每层强制 240x320，即使隐藏仍常驻整屏透明区，
       LVGL 每帧要遍历 8 层全屏做混合 —— 这是板子发烫的主因。
       位置是绝对坐标，切图造成的尺寸变化不会挪动左上角。 */
    return o;
}

static void build_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* 层级：底图 → 精灵(唯一大面积 ARGB 层) → 覆盖文字条 → 卡片 → HUD → 键提示。
       除底图/精灵外首帧一律 1x1 隐藏图；所有 UI 图为不透明 RGB565，
       LVGL 直接拷贝，不做逐像素 alpha 混合。 */
    g_bg     = mk_img(scr, &ember_bg_dsc, 0, 0);
    g_spirit = mk_img(scr, &ember_spirit_dsc[0], 0, 0);
    g_ovl    = mk_img(scr, &ember_none_dsc[0], 0, 100);
    g_card   = mk_img(scr, &ember_none_dsc[0], 0, 0);
    g_wood   = mk_img(scr, &ember_none_dsc[0], 8, 5);
    g_lamps  = mk_img(scr, &ember_none_dsc[0], 40, 247);
    g_sel    = mk_img(scr, &ember_none_dsc[0], 40, 247);
    g_hint   = mk_img(scr, &ember_none_dsc[0], 0, 288);

    ESP_LOGI(TAG, "ui ready: RGB565 UI / 1x1 hidden / spirit only ARGB layer");
}

/* ---------------- 屏幕流程 ---------------- */
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
static int64_t s_last_key_ms = 0;

static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    /* 同时认按下与单击：按下即时响应，350ms 内的同一次单击不再重复触发 */
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_PRESS) return;
    if (now - s_last_key_ms < 350) return;
    s_last_key_ms = now;
    ESP_LOGI(TAG, "key btn=%d ev=%d screen=%d", (int)btn, (int)ev, (int)g.screen);
    poke_hit(now);

    switch (g.screen) {
        case SC_FIRST:
            if (btn == BSP_BTN_OK) g.screen = SC_TONIGHT;
            return;

        case SC_RELIGHT:
            if (btn == BSP_BTN_OK) g.screen = SC_TONIGHT;
            return;

        case SC_PHASE:
            g.screen = SC_TONIGHT;
            return;

        case SC_BORROW:
            if (btn == BSP_BTN_DOWN) { g.screen = SC_CARD; return; }
            if (btn == BSP_BTN_OK) {
                g.lamp[g.sel]++; g.tot[g.sel]++; g.borrowed = true; g.debt++;
                recompute_fire(); nvs_save();
                g.screen = SC_ADD;
            }
            return;

        case SC_CARD:
            next_night();
            return;

        case SC_TONIGHT:
            if (btn == BSP_BTN_OK) { g.screen = SC_ADD; return; }
            if (btn == BSP_BTN_DOWN) { g.screen = SC_PHASE; return; }
            return;

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
            }
            return;
    }
}

/* ---------------- 刷新（只切变化的图，绝不周期重绘） ---------------- */
static uint32_t s_switch_cnt;       /* 诊断：本统计周期内实际切图次数 */
static uint32_t s_lock_wait_sum;    /* 诊断：等 LVGL 锁累计毫秒 */
static uint32_t s_lock_wait_max;
static uint32_t s_loop_cnt;

static void set_ui_src(lv_obj_t *o, const lv_img_dsc_t *d) {
    lv_img_set_src(o, d);
    s_switch_cnt++;
}

static void refresh_ui(void) {
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    bool poke = (now < g.poke_until);
    bool main_scr = (g.screen == SC_TONIGHT || g.screen == SC_ADD);
    bool card_scr = (g.screen == SC_CARD);

    /* 覆盖屏索引 */
    int ovl = -1;
    switch (g.screen) {
        case SC_FIRST:   ovl = OVL_FIRST;   break;
        case SC_PHASE:   ovl = OVL_PHASE;   break;
        case SC_RELIGHT: ovl = OVL_RELIGHT; break;
        case SC_BORROW:  ovl = OVL_BORROW;  break;
        default: break;
    }

    /* 键提示：覆盖屏不再用整屏半透明贴图，底部提示条统一复用 hint 图；
       卡片屏自带「柴尽了·明早见」故不叠；高频连按浮层优先级最高 */
    int hint = -1;
    if (main_scr) {
        if (g.screen == SC_ADD && g.wood_left <= 0) hint = HINT_EMPTY;
        else hint = (g.screen == SC_ADD) ? HINT_ADD : HINT_TONIGHT;
    } else {
        switch (g.screen) {
            case SC_FIRST:   hint = HINT_FIRST;   break;
            case SC_PHASE:   hint = HINT_PHASE;   break;
            case SC_RELIGHT: hint = HINT_RELIGHT; break;
            case SC_BORROW:  hint = HINT_BORROW;  break;
            default: break;
        }
    }
    if (poke) hint = HINT_POKE;

    /* 火色档（精灵唯一换色对象；卡片跟着同档） */
    if (drawn.frame != g.frame) {
        set_ui_src(g_spirit, &ember_spirit_dsc[g.frame]);
        drawn.frame = g.frame;
    }

    int card_frame = card_scr ? g.frame : -1;
    if (drawn.card_frame != card_frame) {
        set_ui_src(g_card, (card_frame >= 0) ? &ember_card_dsc[card_frame]
                                            : &ember_none_dsc[0]);
        drawn.card_frame = card_frame;
    }

    if (drawn.ovl != ovl) {
        set_ui_src(g_ovl, (ovl >= 0) ? &ember_ovl_dsc[ovl] : &ember_none_dsc[0]);
        drawn.ovl = ovl;
    }

    /* HUD：只在主屏出现，其余切透明图 */
    int wood = g.wood_left;
    if (wood < 0) wood = 0;
    if (wood > 5) wood = 5;
    int wood_idx = main_scr ? wood : -1;
    if (drawn.wood != wood_idx) {
        set_ui_src(g_wood, (wood_idx >= 0) ? &ember_wood_dsc[wood_idx]
                                          : &ember_none_dsc[0]);
        drawn.wood = wood_idx;
    }

    int bits = 0;
    for (int i = 0; i < N_LAMPS; i++) {
        if (g.lamp[i] > 0) bits |= (1 << i);
    }
    int lamp_idx = main_scr ? bits : -1;
    if (drawn.lamp_bits != lamp_idx) {
        set_ui_src(g_lamps, (lamp_idx >= 0) ? &ember_lamp_dsc[lamp_idx]
                                           : &ember_none_dsc[0]);
        drawn.lamp_bits = lamp_idx;
    }

    int sel_idx = main_scr ? g.sel : -1;
    if (drawn.sel != sel_idx) {
        set_ui_src(g_sel, (sel_idx >= 0) ? &ember_sel_dsc[sel_idx]
                                        : &ember_none_dsc[0]);
        drawn.sel = sel_idx;
    }

    if (drawn.hint != hint) {
        set_ui_src(g_hint, (hint >= 0) ? &ember_hint_dsc[hint]
                                      : &ember_none_dsc[0]);
        drawn.hint = hint;
    }

    if (drawn.screen != g.screen) {
        ESP_LOGI(TAG, "screen=%d wood=%d frame=%d ovl=%d hint=%d",
                 (int)g.screen, g.wood_left, g.frame, ovl, hint);
        drawn.screen = g.screen;
    }
}

/* ---------------- 主循环（事件驱动 + 渲染负载诊断） ---------------- */
static void game_task(void *arg) {
    (void)arg;
    int64_t diag_t0 = (int64_t)(esp_timer_get_time() / 1000);
    while (1) {
        btn_ev_t e;
        /* 阻塞等按键；200ms 超时只用于让高频连按浮层自然消失。
           静止时 5Hz 醒一次做稳定比对，不再 25Hz 空转轮询。 */
        if (xQueueReceive(s_btn_q, &e, pdMS_TO_TICKS(200)) == pdTRUE) {
            do { handle_btn(e.btn, e.ev); }
            while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE);
        }

        int64_t t0 = esp_timer_get_time();
        if (bsp_lvgl_lock(100)) {
            uint32_t wait_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            s_lock_wait_sum += wait_ms;
            if (wait_ms > s_lock_wait_max) s_lock_wait_max = wait_ms;
            refresh_ui();
            bsp_lvgl_unlock();
        }
        s_loop_cnt++;

        int64_t now = (int64_t)(esp_timer_get_time() / 1000);
        if (now - diag_t0 >= 5000) {
            uint32_t n = s_loop_cnt ? s_loop_cnt : 1;
            ESP_LOGI(TAG, "diag lock_wait avg=%lu max=%lu ms loops=%lu swaps=%lu",
                     (unsigned long)(s_lock_wait_sum / n),
                     (unsigned long)s_lock_wait_max,
                     (unsigned long)s_loop_cnt,
                     (unsigned long)s_switch_cnt);
            s_lock_wait_sum = 0;
            s_lock_wait_max = 0;
            s_loop_cnt = 0;
            s_switch_cnt = 0;
            diag_t0 = now;
        }
    }
}

/* ---------------- 应用入口 ---------------- */
void ember_app_start(void) {
    memset(&g, 0, sizeof(g));
    memset(&drawn, 0xFF, sizeof(drawn));   /* 全部 -2，首帧强制全量切图 */
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
        refresh_ui();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock timeout at boot");
    }
    ESP_LOGI(TAG, "boot screen=%d wood=%d night=%d frame=%d",
             (int)g.screen, g.wood_left, (int)g.night, g.frame);
    nvs_save();

    xTaskCreate(game_task, "ember", 6144, NULL, 5, NULL);
}
