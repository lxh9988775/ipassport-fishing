/*
 * pet.c - 电子宠物「小白兔」应用层（AI Passport / ESP32-C3, LVGL v9）
 *
 * 职责边界（对齐 fishing.c 的分层约定）：
 *   - 照顾规则/衰减/存档全部在 pet_logic.c（纯逻辑，可单测）
 *   - 本文件只负责：LVGL 画面、三键交互、音效、电量显示、NVS 存档
 *
 * 交互（6-9 岁适龄：一键即反馈、无需识字太多、零挫败）：
 *   UP/DOWN 单击  = 在 喂食/洗澡/陪玩/睡觉 四个动作间选择
 *   OK 单击       = 执行选中动作；睡觉中任意单击 = 唤醒
 *
 * 适龄细节：
 *   - 没有长按、没有双击、没有倒计时、没有失败
 *   - 睡觉时背光调暗（护眼 + 省电），睡饱自动醒
 *   - 状态条颜色分级：绿(>=45) / 橙(20..44) / 红(<20)，一眼看懂
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "pet_logic.h"
#include "fishing_audio.h"          /* 复用同一套 SFX（fishing_audio.c 已在构建里） */
#include "../assets/sprites/pet_sprites.h"

/* ===================== 中文字体（与钓鱼共用同一子集字库） ===================== */
LV_FONT_DECLARE(fishing_cjk_16);

static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ===================== 屏幕布局 ===================== */
#define SCR_W 240
#define SCR_H 320
#define HUD_H 24

/* 精灵位置（72x72） */
#define PET_X 84
#define PET_Y 34

/* 状态条 */
#define BAR_X     68
#define BAR_W     110
#define BAR_H     10
#define ROW_Y0    132
#define ROW_DY    24

/* 动作按钮 */
#define ACT_Y     244
#define ACT_CELL_W 58
#define ACT_COUNT 4

/* ===================== NVS ===================== */
#define NVS_NS  "petgame"
#define NVS_KEY "save"

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void nvs_load_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
        uint8_t *buf = (uint8_t *)malloc(len);
        if (buf) {
            if (nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
                pet_save_apply(buf, (int)len);
            }
            free(buf);
        }
    }
    nvs_close(h);
}

static void nvs_save_now(void) {
    uint8_t buf[64];
    int n = pet_save_serialize(buf, (int)sizeof(buf));
    if (n <= 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, buf, (size_t)n);
    nvs_commit(h);
    nvs_close(h);
}

/* ===================== UI 对象 ===================== */
static lv_obj_t *g_lbl_level   = NULL;
static lv_obj_t *g_lbl_mood    = NULL;
static lv_obj_t *g_lbl_batt    = NULL;
static lv_obj_t *g_batt_body   = NULL;
static lv_obj_t *g_batt_fill   = NULL;
static lv_obj_t *g_batt_nub    = NULL;
static lv_obj_t *g_rabbit      = NULL;
static lv_obj_t *g_lbl_zzz     = NULL;
static lv_obj_t *g_lbl_fb      = NULL;   /* 动作反馈（吃饱啦！等） */
static lv_obj_t *g_row_lbl[4]  = {0};
static lv_obj_t *g_row_fill[4] = {0};
static lv_obj_t *g_act_lbl[ACT_COUNT] = {0};
static lv_obj_t *g_act_sel     = NULL;
static lv_obj_t *g_lbl_hint    = NULL;

static int  g_act_idx = 0;
static int  g_batt_soc = -1;

/* ===================== 按键队列 ===================== */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q = NULL;

/* ===================== 工具 ===================== */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, int x, int y, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    use_cjk(l);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* 只在文本真的变了才写给 LVGL（与 fishing.c 同一招，省中文重排） */
static bool set_text_cached(lv_obj_t *lbl, const char *txt) {
    if (!lbl || !txt) return false;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0) return false;
    lv_label_set_text(lbl, txt);
    return true;
}

static const char *ACTION_NAMES[ACT_COUNT] = { "喂食", "洗澡", "陪玩", "睡觉" };

/* ===================== UI 构造 ===================== */
static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 246, 240), 0);  /* 奶油底，贴宠物氛围 */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ---- 顶栏 HUD：等级 · 心情 · 电池 ---- */
    g_lbl_level = make_label(scr, "等级 1", 17, 4, lv_color_make(90, 60, 40));
    g_lbl_mood  = make_label(scr, "开心", 92, 4, lv_color_make(153, 0, 58));   /* 锐胜红点睛 */

    /* 电池（复用 fishing 的画法：壳+电量条+凸点，不依赖符号字体） */
    g_batt_body = lv_obj_create(scr);
    lv_obj_set_size(g_batt_body, 12, 8);
    lv_obj_set_pos(g_batt_body, 176, 9);
    lv_obj_set_style_bg_opa(g_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_batt_body, lv_color_make(120, 100, 90), 0);
    lv_obj_set_style_border_width(g_batt_body, 1, 0);
    lv_obj_set_style_radius(g_batt_body, 1, 0);
    lv_obj_set_style_pad_all(g_batt_body, 0, 0);
    lv_obj_clear_flag(g_batt_body, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_fill = lv_obj_create(g_batt_body);
    lv_obj_set_size(g_batt_fill, 10, 6);
    lv_obj_set_pos(g_batt_fill, 0, 0);
    lv_obj_set_style_bg_color(g_batt_fill, lv_color_make(90, 220, 120), 0);
    lv_obj_set_style_bg_opa(g_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_fill, 0, 0);
    lv_obj_set_style_radius(g_batt_fill, 0, 0);
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_nub = lv_obj_create(scr);
    lv_obj_set_size(g_batt_nub, 2, 4);
    lv_obj_set_pos(g_batt_nub, 188, 11);
    lv_obj_set_style_bg_color(g_batt_nub, lv_color_make(120, 100, 90), 0);
    lv_obj_set_style_bg_opa(g_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_nub, 0, 0);
    lv_obj_set_style_radius(g_batt_nub, 0, 0);
    lv_obj_clear_flag(g_batt_nub, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_batt = make_label(scr, "--", 193, 4, lv_color_make(90, 60, 40));

    /* ---- 兔子精灵 ---- */
    g_rabbit = lv_img_create(scr);
    lv_img_set_src(g_rabbit, &rabbit_front);
    lv_obj_set_pos(g_rabbit, PET_X, PET_Y);

    g_lbl_zzz = make_label(scr, "z Z z", PET_X + 58, PET_Y + 4, lv_color_make(150, 150, 170));
    lv_obj_add_flag(g_lbl_zzz, LV_OBJ_FLAG_HIDDEN);

    /* ---- 动作反馈 ---- */
    g_lbl_fb = make_label(scr, "", 0, 0, lv_color_make(153, 0, 58));
    lv_obj_align(g_lbl_fb, LV_ALIGN_TOP_MID, 0, 112);

    /* ---- 四条状态 ---- */
    static const char *ROW_NAMES[4] = { "饱食", "干净", "玩乐", "精力" };
    for (int i = 0; i < 4; ++i) {
        int y = ROW_Y0 + i * ROW_DY;
        g_row_lbl[i] = make_label(scr, ROW_NAMES[i], 20, y, lv_color_make(90, 60, 40));
        lv_obj_t *bg = lv_obj_create(scr);
        lv_obj_set_size(bg, BAR_W, BAR_H);
        lv_obj_set_pos(bg, BAR_X, y + 4);
        lv_obj_set_style_bg_color(bg, lv_color_make(235, 222, 210), 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bg, 0, 0);
        lv_obj_set_style_radius(bg, 5, 0);
        lv_obj_set_style_pad_all(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

        g_row_fill[i] = lv_obj_create(scr);
        lv_obj_set_size(g_row_fill[i], BAR_W, BAR_H);
        lv_obj_set_pos(g_row_fill[i], BAR_X, y + 4);
        lv_obj_set_style_bg_color(g_row_fill[i], lv_color_make(90, 220, 120), 0);
        lv_obj_set_style_bg_opa(g_row_fill[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g_row_fill[i], 0, 0);
        lv_obj_set_style_radius(g_row_fill[i], 5, 0);
        lv_obj_clear_flag(g_row_fill[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ---- 动作按钮行 ---- */
    for (int i = 0; i < ACT_COUNT; ++i) {
        g_act_lbl[i] = make_label(scr, ACTION_NAMES[i], 0, 0, lv_color_make(90, 60, 40));
        lv_obj_set_width(g_act_lbl[i], ACT_CELL_W);
        lv_obj_set_style_text_align(g_act_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_act_lbl[i], 6 + i * ACT_CELL_W, ACT_Y);
    }
    /* 选中胶囊：画在标签后面（先创建的先画，所以这里单独建一个再移到最底） */
    g_act_sel = lv_obj_create(scr);
    lv_obj_set_size(g_act_sel, ACT_CELL_W, 24);
    lv_obj_set_pos(g_act_sel, 6, ACT_Y - 3);
    lv_obj_set_style_bg_color(g_act_sel, lv_color_make(153, 0, 58), 0);   /* 锐胜红 */
    lv_obj_set_style_bg_opa(g_act_sel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_act_sel, 0, 0);
    lv_obj_set_style_radius(g_act_sel, 12, 0);
    lv_obj_set_style_pad_all(g_act_sel, 0, 0);
    lv_obj_clear_flag(g_act_sel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_to_index(g_act_sel, 0);   /* 沉底，别盖住文字 */

    /* ---- 底部提示 ---- */
    g_lbl_hint = make_label(scr, "上下选 · OK 做", 0, 0, lv_color_make(120, 100, 90));
    lv_obj_align(g_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/* ===================== 刷新 ===================== */
static int s_lvl_drawn = -1;
static int s_mood_drawn = -99;      /* 用指针值区分，见 refresh */
static int s_bar_drawn[4] = { -1, -1, -1, -1 };
static int s_sel_drawn = -1;
static int s_batt_drawn = -2;
static int s_rabbit_y = -1;
static bool s_sleep_drawn = false;
/* 反馈文案有效期（0 = 无）。必须先于 refresh_ui 声明（C 先声明后使用）。 */
static int g_fb_until_ms = 0;

static void bar_color(lv_obj_t *fill, int v) {
    lv_color_t c = (v >= 45) ? lv_color_make(90, 220, 120)
                 : (v >= 20) ? lv_color_make(240, 180, 70)
                             : lv_color_make(230, 80, 70);
    lv_obj_set_style_bg_color(fill, c, 0);
}

static void refresh_batt(void) {
    if (s_batt_drawn == g_batt_soc) return;
    s_batt_drawn = g_batt_soc;
    if (g_batt_soc < 0) {
        lv_label_set_text(g_lbl_batt, "--");
        lv_obj_add_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(g_batt_body, lv_color_make(170, 160, 150), 0);
        return;
    }
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(g_batt_body,
                                  g_batt_soc < 20 ? lv_color_make(230, 60, 50)
                                                  : lv_color_make(120, 100, 90), 0);
    lv_obj_set_width(g_batt_fill, 1 + g_batt_soc * 9 / 100);
    lv_obj_set_style_bg_color(g_batt_fill,
                              g_batt_soc < 20 ? lv_color_make(230, 60, 50)
                                              : lv_color_make(90, 220, 120), 0);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", g_batt_soc);
    lv_label_set_text(g_lbl_batt, buf);
}

static void refresh_ui(int now) {
    pet_status_t st;
    pet_get_status(&st);
    char buf[64];

    refresh_batt();

    /* 等级 / 心情 */
    if (st.level != s_lvl_drawn) {
        s_lvl_drawn = st.level;
        snprintf(buf, sizeof(buf), "等级 %d", st.level);
        set_text_cached(g_lbl_level, buf);
    }
    if ((int)(size_t)pet_mood_name(&st) != s_mood_drawn) {
        s_mood_drawn = (int)(size_t)pet_mood_name(&st);
        set_text_cached(g_lbl_mood, pet_mood_name(&st));
    }

    /* 四条状态 */
    const int vals[4] = { st.hunger, st.clean, st.fun, st.energy };
    for (int i = 0; i < 4; ++i) {
        if (vals[i] != s_bar_drawn[i]) {
            s_bar_drawn[i] = vals[i];
            int w = vals[i] * BAR_W / 100;
            lv_obj_set_size(g_row_fill[i], w, BAR_H);
            bar_color(g_row_fill[i], vals[i]);
        }
    }

    /* 选中胶囊 */
    if (g_act_idx != s_sel_drawn) {
        s_sel_drawn = g_act_idx;
        lv_obj_set_pos(g_act_sel, 6 + g_act_idx * ACT_CELL_W, ACT_Y - 3);
    }

    /* 睡觉态：Zzz 显隐 + 提示文案 + 兔子换背面（睡觉背对世界更可爱） */
    if (st.sleeping != s_sleep_drawn) {
        s_sleep_drawn = st.sleeping;
        if (st.sleeping) {
            lv_obj_clear_flag(g_lbl_zzz, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(g_rabbit, &rabbit_back);
            set_text_cached(g_lbl_hint, "任意键唤醒");
            bsp_display_backlight(15);
        } else {
            lv_obj_add_flag(g_lbl_zzz, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(g_rabbit, &rabbit_front);
            set_text_cached(g_lbl_hint, "上下选 · OK 做");
            bsp_display_backlight(80);
        }
    }

    /* 反馈文案超时清理 */
    if (g_fb_until_ms != 0 && now >= g_fb_until_ms) {
        g_fb_until_ms = 0;
        set_text_cached(g_lbl_fb, "");
    }
}

/* ===================== 动作执行 ===================== */
static void show_feedback(const char *txt, int now) {
    set_text_cached(g_lbl_fb, txt);
    g_fb_until_ms = now + 1600;
}

static void do_action(int now) {
    pet_event_t ev;
    switch (g_act_idx) {
        case 0: ev = pet_feed();  break;
        case 1: ev = pet_clean(); break;
        case 2: ev = pet_play();  break;
        default: ev = pet_toggle_sleep(); break;
    }
    switch (ev) {
        case PET_EVT_ACTION_OK:
            fishing_audio_play(SFX_CATCH);
            if (g_act_idx == 0)      show_feedback("吃饱啦！", now);
            else if (g_act_idx == 1) show_feedback("洗得香香的！", now);
            else                     show_feedback("玩得好开心！", now);
            break;
        case PET_EVT_ACTION_FULL:
            fishing_audio_play(SFX_CLICK);
            if (g_act_idx == 0)      show_feedback("已经饱饱的啦", now);
            else                     show_feedback("已经很干净啦", now);
            break;
        case PET_EVT_TOO_TIRED:
            fishing_audio_play(SFX_MISS);
            show_feedback("太累了，先睡觉吧", now);
            break;
        case PET_EVT_SLEEP_START:
            fishing_audio_play(SFX_CLICK);
            show_feedback("晚安，小白…", now);
            break;
        case PET_EVT_WAKE:
            fishing_audio_play(SFX_CLICK);
            show_feedback("早上好呀！", now);
            break;
        case PET_EVT_LEVEL_UP:
            fishing_audio_play(SFX_CATCH);
            show_feedback("照顾升级啦！", now);
            break;
        default:
            break;
    }
}

/* ===================== 按键 ===================== */
static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    pet_status_t st;
    pet_get_status(&st);
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_PRESS) return;

    if (st.sleeping) {
        /* 睡觉中：任意单击唤醒；忽略 PRESS（避免唤醒后立刻又触发动作） */
        if (ev != BSP_BTN_CLICK) return;
        pet_toggle_sleep();
        fishing_audio_play(SFX_CLICK);
        return;
    }

    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        g_act_idx = (g_act_idx + ACT_COUNT - 1) % ACT_COUNT;
        fishing_audio_play(SFX_CLICK);
    } else if (btn == BSP_BTN_DOWN) {
        g_act_idx = (g_act_idx + 1) % ACT_COUNT;
        fishing_audio_play(SFX_CLICK);
    } else if (btn == BSP_BTN_OK) {
        do_action((int)now_ms());
    }
}

/* ===================== 主任务 ===================== */
#define LOOP_MS          20
#define UI_REFRESH_MS    100
#define BATT_MS          500
#define SAVE_MS          30000
#define BOB_MS           600

static void pet_task(void *arg) {
    (void)arg;
    int last_ui_ms = 0;
    int last_batt_ms = 0;
    int last_save_ms = 0;
    int last_bob_ms = 0;
    bool bob_up = false;

    for (;;) {
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);

        int now = (int)now_ms();

        pet_event_t ev = pet_tick(now);
        if (ev == PET_EVT_SLEEP_FULL) {
            fishing_audio_play(SFX_CATCH);
            show_feedback("睡饱啦，精神满满！", now);
        } else if (ev == PET_EVT_LEVEL_UP) {
            fishing_audio_play(SFX_CATCH);
            show_feedback("照顾升级啦！", now);
        }

        if (now - last_batt_ms > BATT_MS) {
            g_batt_soc = bsp_battery_soc();
            last_batt_ms = now;
        }
        if (now - last_save_ms > SAVE_MS) {
            nvs_save_now();
            last_save_ms = now;
        }

        /* 兔子呼吸：每 600ms 上下浮 2px */
        if (now - last_bob_ms > BOB_MS) {
            last_bob_ms = now;
            bob_up = !bob_up;
            int y = PET_Y + (bob_up ? 0 : 2);
            if (y != s_rabbit_y) {
                s_rabbit_y = y;
                lv_obj_set_pos(g_rabbit, PET_X, y);
            }
        }

        if (now - last_ui_ms >= UI_REFRESH_MS) {
            last_ui_ms = now;
            if (bsp_lvgl_lock(100)) {
                refresh_ui(now);
                bsp_lvgl_unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

void pet_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_btn_q) return;
    const btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ===================== 入口 ===================== */
void pet_app_start(void) {
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    pet_init();
    nvs_load_save();
    fishing_audio_init();

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui((int)now_ms());
        bsp_lvgl_unlock();
    }
    xTaskCreate(pet_task, "pet", 4096, NULL, 5, NULL);
}
