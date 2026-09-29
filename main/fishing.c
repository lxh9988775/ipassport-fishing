/*
 * fishing.c - 钓鱼 v2「竿影浮标」应用层（AI Passport / ESP32-C3, LVGL v9）
 *
 * 职责边界（遵循官方 BSP/应用分层）：
 *   - 游戏"规则与计分"全部在 fishing_logic.c（纯逻辑，可单测）
 *   - 本文件只负责：LVGL 画面、三键交互、音效触发、电量显示、NVS 存档
 *
 * v2 交互（核心玩法：星露谷式收线）：
 *   IDLE    OK 单击=抛竿 / OK 长按=开菜单
 *   BITE    OK 单击=提竿（错过窗口就跑鱼）
 *   REELING 按住 OK=捕捉区上抬，松开=下落 —— 把鱼稳在区内涨进度
 *   CODEX   上下翻鱼种 / OK 单击=看详情 / OK 长按=返回
 *
 * 按键说明：BSP 不提供"松开"事件，这里用 PRESS(按下) 开始长按、CLICK(按下并抬起)
 * 结束长按，既拿到按下瞬间的低延迟，也能可靠知道手已松开。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "fishing_logic.h"
#include "fishing_audio.h"
#include "../assets/sprites/sprites.h"

/* ===================== 中文字体 ===================== */
/* 屏上文案用中文。LVGL 自带字体不含本游戏用字（实测「钓 鱼 饵 蚯 蚓 竿」等缺字），
 * 故用 lv_font_conv 生成的专用子集字体 assets/fonts/fishing_cjk_16.c。
 * 改文案后同步流程：改文案 -> 跑 tools/gen_font.py -> 跑 tools/check_cjk_coverage.py。 */
LV_FONT_DECLARE(fishing_cjk_16);

static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ===================== 屏幕布局 ===================== */
#define SCR_W 240
#define SCR_H 320
#define HUD_H 22

/* 收线轨道像素区域 */
#define TRK_X 196
#define TRK_W 26
#define TRK_Y 46
#define TRK_H 224
/* 进度条像素区域 */
#define PRG_X 20
#define PRG_W 16
#define PRG_Y 46
#define PRG_H 224

/* ===================== 时钟 / NVS ===================== */
static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#define NVS_NS   "fishing"
#define NVS_KEY  "save"     /* 版本化存档 blob，见 fishing_logic.c 的说明 */

/* 读存档：失败就当新号，不崩 */
static void nvs_load_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
        uint8_t *buf = (uint8_t *)malloc(len);
        if (buf) {
            if (nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
                fishing_save_apply(buf, (int)len);   /* 内含版本校验与迁移 */
            }
            free(buf);
        }
    }
    nvs_close(h);
}

static void nvs_save_now(void) {
    uint8_t buf[512];
    int n = fishing_save_serialize(buf, (int)sizeof(buf));
    if (n <= 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, buf, (size_t)n);
    nvs_commit(h);
    nvs_close(h);
}

/* ===================== sprite 查表 ===================== */
static const lv_image_dsc_t *const FISH_IMG[FISH_SPECIES_COUNT] = {
    &fish_baitiao, &fish_jiyu, &fish_luofei, &fish_lianyu, &fish_bianyu, &fish_caoyu,
    &fish_liyu, &fish_qingyu, &fish_makou, &fish_huangsang, &fish_qiaozui, &fish_luyu,
    &fish_guiyu, &fish_nianyu, &fish_junyu, &fish_ganyu, &fish_xiaohuang, &fish_daiyu,
    &fish_bayu, &fish_bimu, &fish_shiban, &fish_jinqiang, &fish_qiyu, &fish_xiaosha,
};
static const lv_image_dsc_t *const SIL_IMG[FISH_SPECIES_COUNT] = {
    &sil_baitiao, &sil_jiyu, &sil_luofei, &sil_lianyu, &sil_bianyu, &sil_caoyu,
    &sil_liyu, &sil_qingyu, &sil_makou, &sil_huangsang, &sil_qiaozui, &sil_luyu,
    &sil_guiyu, &sil_nianyu, &sil_junyu, &sil_ganyu, &sil_xiaohuang, &sil_daiyu,
    &sil_bayu, &sil_bimu, &sil_shiban, &sil_jinqiang, &sil_qiyu, &sil_xiaosha,
};
static const lv_image_dsc_t *const BG_IMG[FISH_SPOT_COUNT] = { &bg_pond, &bg_river, &bg_sea };

/* ===================== UI 对象 ===================== */
/*
 * 屏幕布局有两条硬约束，改坐标前务必先读：
 *   1) 屏只有 240px 宽，四角还有 30px 圆角遮罩（BSP_LVGL_SCREEN_RADIUS），
 *      被遮区域一律涂黑 —— 顶部 y=4 那一行实际只剩 x∈[16,223] 可见。
 *   2) 中文 16px 一个字，四个中文标签横排就是 128px 起，塞不下。
 * 所以顶栏只放三项："得分 / 最高 / 电池图形+百分比"，钓点名改挂场景面板右上角。
 * 改完必须跑 tools/check_ui_layout.py（出屏/被圆角切/互相压住 都会报出来）。
 */
static lv_obj_t *g_bg          = NULL;   /* 背景（按钓点切换） */
static lv_obj_t *g_lbl_score   = NULL;
static lv_obj_t *g_lbl_high    = NULL;
static lv_obj_t *g_lbl_batt    = NULL;   /* 电量百分比数字（单位由电池图形表达） */
static lv_obj_t *g_batt_body   = NULL;   /* 电池外壳 */
static lv_obj_t *g_batt_fill   = NULL;   /* 电池内部电量条 */
static lv_obj_t *g_batt_nub    = NULL;   /* 电池正极凸点 */
static lv_obj_t *g_lbl_spot    = NULL;   /* 钓点名（挂在场景面板右上角的胶囊里） */
static lv_obj_t *g_spot_cap    = NULL;   /* 钓点名胶囊底 */
static lv_obj_t *g_lbl_hint    = NULL;   /* 底部提示 */

/* 钓鱼场景 */
static lv_obj_t *g_scene       = NULL;
static lv_obj_t *g_floatbob    = NULL;
static lv_obj_t *g_fish_scene  = NULL;

/* 收线界面 */
static lv_obj_t *g_reel        = NULL;
static lv_obj_t *g_track       = NULL;
static lv_obj_t *g_zone        = NULL;
static lv_obj_t *g_zone_fill   = NULL;
static lv_obj_t *g_fishmark    = NULL;
static lv_obj_t *g_prg_fill    = NULL;
static lv_obj_t *g_lbl_reel    = NULL;

/* 上鱼结算 */
static lv_obj_t *g_result      = NULL;
static lv_obj_t *g_res_img     = NULL;
static lv_obj_t *g_res_name    = NULL;
static lv_obj_t *g_res_info    = NULL;

/* 菜单 */
static lv_obj_t *g_menu        = NULL;
static lv_obj_t *g_menu_rows[5];
static lv_obj_t *g_menu_selbar = NULL;
static int g_menu_idx = 0;
static int g_menu_edit = 0;   /* 是否进入编辑某项 */

/* 图鉴 */
static lv_obj_t *g_codex       = NULL;
static lv_obj_t *g_cdx_img     = NULL;
static lv_obj_t *g_cdx_name    = NULL;
static lv_obj_t *g_cdx_meta    = NULL;
static lv_obj_t *g_cdx_desc    = NULL;
static lv_obj_t *g_cdx_prog    = NULL;

/* ===================== 按键队列 ===================== */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q = NULL;

static int g_batt_soc = -1;
static int g_last_batt_ms = 0;
static int g_last_save_ms = 0;

/* ===================== 工具 ===================== */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, int x, int y, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    use_cjk(l);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* ===================== UI 构造 ===================== */
static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 背景图（像素场景，随钓点切换） */
    g_bg = lv_img_create(scr);
    lv_img_set_src(g_bg, BG_IMG[0]);
    lv_obj_set_pos(g_bg, 0, 0);

    /* ---- 顶栏 HUD ----
     * 屏只有 240px 宽、左右还被圆角各吃掉约 16px，所以刻意只放三项：
     *   左「得分」 · 中「最高」 · 右「电池图形 + 百分比」
     * 电量不再写「电量」二字 —— 电池图形本身就是单位，省下的宽度留给分数。
     * 三项最坏（得分/最高各四位、电量 100）合计约 181px，落在可见区 x∈[17,223] 内。 */
    lv_obj_t *hud = lv_obj_create(scr);
    lv_obj_set_size(hud, SCR_W, HUD_H + 2);   /* 文字底边在 y=24，底板盖到 24 */
    lv_obj_set_pos(hud, 0, 0);
    lv_obj_set_style_bg_color(hud, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hud, 150, 0);
    lv_obj_set_style_border_width(hud, 0, 0);
    lv_obj_set_style_radius(hud, 0, 0);
    lv_obj_set_style_pad_all(hud, 0, 0);
    lv_obj_clear_flag(hud, LV_OBJ_FLAG_SCROLLABLE);
    (void)hud;

    g_lbl_score = make_label(scr, "得分 0", 17, 4, lv_color_white());
    g_lbl_high  = make_label(scr, "最高 0", 96, 4, lv_color_white());

    /* 电池：壳 + 内部电量条 + 正极凸点。
     * 不用 LV_SYMBOL_BATTERY_* —— 本应用把文字字体换成了 fishing_cjk_16（.fallback
     * 为 NULL），内置符号字形取不到，画出来是空的。 */
    g_batt_body = lv_obj_create(scr);
    lv_obj_set_size(g_batt_body, 12, 8);
    lv_obj_set_pos(g_batt_body, 176, 9);
    lv_obj_set_style_bg_opa(g_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_batt_body, lv_color_white(), 0);
    lv_obj_set_style_border_width(g_batt_body, 1, 0);
    lv_obj_set_style_radius(g_batt_body, 1, 0);
    lv_obj_set_style_pad_all(g_batt_body, 0, 0);
    lv_obj_clear_flag(g_batt_body, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_fill = lv_obj_create(g_batt_body);
    lv_obj_set_size(g_batt_fill, 10, 6);
    lv_obj_set_pos(g_batt_fill, 0, 0);        /* 相对内容区，正好贴在边框内侧 */
    lv_obj_set_style_bg_color(g_batt_fill, lv_color_make(90, 220, 120), 0);
    lv_obj_set_style_bg_opa(g_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_fill, 0, 0);
    lv_obj_set_style_radius(g_batt_fill, 0, 0);
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_nub = lv_obj_create(scr);
    lv_obj_set_size(g_batt_nub, 2, 4);
    lv_obj_set_pos(g_batt_nub, 188, 11);
    lv_obj_set_style_bg_color(g_batt_nub, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_nub, 0, 0);
    lv_obj_set_style_radius(g_batt_nub, 0, 0);
    lv_obj_clear_flag(g_batt_nub, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_batt = make_label(scr, "--", 193, 4, lv_color_white());

    /* 底部提示：文案长短不一，交给 BOTTOM_MID 自动居中，别手算 x 再撞上圆角 */
    g_lbl_hint = make_label(scr, "", 0, 0, lv_color_white());
    lv_obj_set_style_bg_color(g_lbl_hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_lbl_hint, 170, 0);
    lv_obj_set_style_pad_all(g_lbl_hint, 1, 0);
    lv_obj_align(g_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -5);

    /* ---------- 钓鱼场景 ---------- */
    g_scene = lv_obj_create(scr);
    lv_obj_set_size(g_scene, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_scene, 0, HUD_H);
    lv_obj_set_style_bg_opa(g_scene, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_scene, 0, 0);
    lv_obj_clear_flag(g_scene, LV_OBJ_FLAG_SCROLLABLE);

    /* 钓点名：挂在场景面板右上角的小胶囊里。
     * 放在 g_scene 内部 → 切到菜单/图鉴时随场景一起隐藏，show_only() 不用改。
     * 绝对位置 (172,28)~(229,47)，该高度上右侧圆角只吃到 x=237，不会切到文字。 */
    g_spot_cap = lv_obj_create(g_scene);
    lv_obj_set_size(g_spot_cap, 58, 20);
    lv_obj_set_pos(g_spot_cap, 172, 6);
    lv_obj_set_style_bg_color(g_spot_cap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_spot_cap, 140, 0);
    lv_obj_set_style_border_width(g_spot_cap, 0, 0);
    lv_obj_set_style_radius(g_spot_cap, 10, 0);
    lv_obj_set_style_pad_all(g_spot_cap, 0, 0);
    lv_obj_clear_flag(g_spot_cap, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_spot = make_label(g_spot_cap, "静水塘", 0, 0, lv_color_white());
    lv_obj_center(g_lbl_spot);

    g_floatbob = lv_img_create(g_scene);
    lv_img_set_src(g_floatbob, &prop_float);
    lv_obj_set_pos(g_floatbob, 110, 96);

    g_fish_scene = lv_img_create(g_scene);
    lv_img_set_src(g_fish_scene, FISH_IMG[0]);
    lv_obj_set_pos(g_fish_scene, 20, 170);
    lv_obj_add_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);

    /* ---------- 收线界面 ---------- */
    g_reel = lv_obj_create(scr);
    lv_obj_set_size(g_reel, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_reel, 0, HUD_H);
    lv_obj_set_style_bg_color(g_reel, lv_color_make(10, 30, 60), 0);
    lv_obj_set_style_bg_opa(g_reel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_reel, 0, 0);
    lv_obj_clear_flag(g_reel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_reel, LV_OBJ_FLAG_HIDDEN);

    /* 进度条（左） */
    lv_obj_t *prg_bg = lv_obj_create(g_reel);
    lv_obj_set_size(prg_bg, PRG_W, PRG_H);
    lv_obj_set_pos(prg_bg, PRG_X, PRG_Y);
    lv_obj_set_style_bg_color(prg_bg, lv_color_make(40, 40, 60), 0);
    lv_obj_set_style_bg_opa(prg_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(prg_bg, 0, 0);
    lv_obj_set_style_radius(prg_bg, 0, 0);
    lv_obj_clear_flag(prg_bg, LV_OBJ_FLAG_SCROLLABLE);
    g_prg_fill = lv_obj_create(g_reel);
    lv_obj_set_size(g_prg_fill, PRG_W, 0);
    lv_obj_set_pos(g_prg_fill, PRG_X, PRG_Y + PRG_H);
    lv_obj_set_style_bg_color(g_prg_fill, lv_color_make(80, 220, 120), 0);
    lv_obj_set_style_bg_opa(g_prg_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_prg_fill, 0, 0);
    lv_obj_set_style_radius(g_prg_fill, 0, 0);
    lv_obj_clear_flag(g_prg_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* 收线轨道（右） */
    g_track = lv_obj_create(g_reel);
    lv_obj_set_size(g_track, TRK_W, TRK_H);
    lv_obj_set_pos(g_track, TRK_X, TRK_Y);
    lv_obj_set_style_bg_color(g_track, lv_color_make(30, 55, 90), 0);
    lv_obj_set_style_bg_opa(g_track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_track, lv_color_make(180, 210, 255), 0);
    lv_obj_set_style_border_width(g_track, 2, 0);
    lv_obj_set_style_radius(g_track, 3, 0);
    lv_obj_clear_flag(g_track, LV_OBJ_FLAG_SCROLLABLE);

    g_zone = lv_obj_create(g_reel);
    lv_obj_set_size(g_zone, TRK_W - 4, 40);
    lv_obj_set_pos(g_zone, TRK_X + 2, TRK_Y + 2);
    lv_obj_set_style_bg_color(g_zone, lv_color_make(90, 230, 140), 0);
    lv_obj_set_style_bg_opa(g_zone, LV_OPA_50, 0);
    lv_obj_set_style_border_width(g_zone, 0, 0);
    lv_obj_set_style_radius(g_zone, 0, 0);
    lv_obj_clear_flag(g_zone, LV_OBJ_FLAG_SCROLLABLE);

    g_fishmark = lv_obj_create(g_reel);
    lv_obj_set_size(g_fishmark, TRK_W - 6, 10);
    lv_obj_set_pos(g_fishmark, TRK_X + 3, TRK_Y + 100);
    lv_obj_set_style_bg_color(g_fishmark, lv_color_make(255, 200, 70), 0);
    lv_obj_set_style_bg_opa(g_fishmark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_fishmark, 0, 0);
    lv_obj_clear_flag(g_fishmark, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_reel = make_label(g_reel, "收线中", 40, TRK_Y, lv_color_white());

    /* ---------- 结算 ---------- */
    g_result = lv_obj_create(scr);
    lv_obj_set_size(g_result, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_result, 0, HUD_H);
    lv_obj_set_style_bg_color(g_result, lv_color_make(8, 24, 48), 0);
    lv_obj_set_style_bg_opa(g_result, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_result, 0, 0);
    lv_obj_clear_flag(g_result, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_result, LV_OBJ_FLAG_HIDDEN);

    g_res_img = lv_img_create(g_result);
    lv_obj_set_pos(g_res_img, 84, 40);
    g_res_name = make_label(g_result, "", 8, 84, lv_color_white());
    g_res_info = make_label(g_result, "", 8, 106, lv_color_white());
    lv_obj_set_width(g_res_info, SCR_W - 16);
    lv_obj_set_style_text_line_space(g_res_info, 4, 0);
    lv_label_set_long_mode(g_res_info, LV_LABEL_LONG_WRAP);

    /* ---------- 菜单 ---------- */
    g_menu = lv_obj_create(scr);
    lv_obj_set_size(g_menu, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_menu, 0, HUD_H);
    lv_obj_set_style_bg_color(g_menu, lv_color_make(6, 18, 36), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu, 0, 0);
    lv_obj_clear_flag(g_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);

    g_menu_selbar = lv_obj_create(g_menu);
    lv_obj_set_size(g_menu_selbar, SCR_W - 12, 26);
    lv_obj_set_pos(g_menu_selbar, 6, 70);
    lv_obj_set_style_bg_color(g_menu_selbar, lv_color_make(153, 0, 58), 0); /* 锐胜红 */
    lv_obj_set_style_bg_opa(g_menu_selbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu_selbar, 0, 0);
    lv_obj_set_style_radius(g_menu_selbar, 4, 0);
    lv_obj_clear_flag(g_menu_selbar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; ++i) {
        g_menu_rows[i] = make_label(g_menu, "", 12, 74 + i * 30, lv_color_white());
    }

    /* ---------- 图鉴 ---------- */
    g_codex = lv_obj_create(scr);
    lv_obj_set_size(g_codex, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_codex, 0, HUD_H);
    lv_obj_set_style_bg_color(g_codex, lv_color_make(6, 18, 36), 0);
    lv_obj_set_style_bg_opa(g_codex, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_codex, 0, 0);
    lv_obj_clear_flag(g_codex, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_codex, LV_OBJ_FLAG_HIDDEN);

    g_cdx_img  = lv_img_create(g_codex);
    lv_obj_set_pos(g_cdx_img, 92, 14);
    g_cdx_name = make_label(g_codex, "", 8, 56, lv_color_white());
    /* 稀有度/完美标记上移到名称行，meta 只留体长+钓数：
     * 原来「传说 · 最大 888.8 厘米 · 已钓 8888 条 · 完美✓」要 345px，屏只有 240px。
     * DOT 模式兜底 —— 万一以后文案又变长，末尾显示省略号，绝不出屏、绝不折行。 */
    g_cdx_meta = make_label(g_codex, "", 8, 78, lv_color_make(190, 210, 240));
    lv_obj_set_width(g_cdx_meta, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_meta, LV_LABEL_LONG_DOT);
    g_cdx_desc = make_label(g_codex, "", 8, 104, lv_color_white());
    lv_obj_set_width(g_cdx_desc, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_cdx_desc, 4, 0);
    g_cdx_prog = make_label(g_codex, "", 8, 250, lv_color_make(190, 210, 240));
}

/* 面板互斥显示 */
static void show_only(int which) {
    lv_obj_add_flag(g_scene,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_reel,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_result, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_menu,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_codex,  LV_OBJ_FLAG_HIDDEN);
    if (which == 0) lv_obj_clear_flag(g_scene,  LV_OBJ_FLAG_HIDDEN);
    if (which == 1) lv_obj_clear_flag(g_reel,   LV_OBJ_FLAG_HIDDEN);
    if (which == 2) lv_obj_clear_flag(g_result, LV_OBJ_FLAG_HIDDEN);
    if (which == 3) lv_obj_clear_flag(g_menu,   LV_OBJ_FLAG_HIDDEN);
    if (which == 4) lv_obj_clear_flag(g_codex,  LV_OBJ_FLAG_HIDDEN);
}

/* ===================== 各面板刷新 ===================== */

/* 电量：图形 + 数字。
 * 刻意不写在 refresh_scene 里 —— 它是常驻顶栏的一部分，收线/菜单/图鉴页也得刷新，
 * 否则在那些页面里电量会冻在上一次的值。 */
static void hud_batt_refresh(void) {
    if (g_batt_soc < 0) {                      /* 电量计不应答就优雅降级 */
        lv_label_set_text(g_lbl_batt, "--");
        lv_obj_add_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(g_batt_body, lv_color_make(130, 130, 130), 0);
        return;
    }
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
    /* 低电量连外壳一起变红 —— 只填 2px 红条在 8px 高的电池上根本看不出来 */
    lv_obj_set_style_border_color(g_batt_body,
                                  g_batt_soc < 20 ? lv_color_make(230, 60, 50)
                                                  : lv_color_white(), 0);
    lv_obj_set_width(g_batt_fill, 1 + g_batt_soc * 9 / 100);   /* 1..10 px */
    lv_obj_set_style_bg_color(g_batt_fill,
                              g_batt_soc < 20 ? lv_color_make(230, 60, 50)
                                              : lv_color_make(90, 220, 120), 0);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", g_batt_soc);
    lv_label_set_text(g_lbl_batt, buf);
}

static void refresh_scene(const fishing_status_t *st) {
    lv_img_set_src(g_bg, BG_IMG[st->spot]);
    lv_label_set_text(g_lbl_spot, fishing_spot_name(st->spot));
    lv_obj_center(g_lbl_spot);        /* 「深海」比三字名窄 16px，重新居中 */

    char buf[160];
    snprintf(buf, sizeof(buf), "得分 %d", st->score);
    lv_label_set_text(g_lbl_score, buf);
    snprintf(buf, sizeof(buf), "最高 %d", st->high_score);
    lv_label_set_text(g_lbl_high, buf);

    lv_obj_add_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);
    int bob_y = 96;
    const char *hint = "";
    switch (st->state) {
        case STATE_IDLE:    hint = "OK 抛竿 · 长按菜单"; break;
        case STATE_CASTING: hint = "抛竿中…"; bob_y = 70; break;
        case STATE_WAITING: hint = "等鱼上钩…"; bob_y = 96; break;
        case STATE_BITE:
            hint = "咬钩了！按 OK 提竿";
            bob_y = 116;
            lv_obj_clear_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);
            if (st->cur_species >= 0) lv_img_set_src(g_fish_scene, SIL_IMG[st->cur_species]);
            break;
        default: hint = ""; break;
    }
    lv_obj_set_pos(g_floatbob, 110, bob_y);
    lv_label_set_text(g_lbl_hint, hint);
}

static void refresh_reel(const fishing_status_t *st) {
    char buf[160];
    snprintf(buf, sizeof(buf), "得分 %d", st->score);
    lv_label_set_text(g_lbl_score, buf);

    /* 捕捉区：把 0..1000 定点换算成像素 */
    int zh = st->reel_bar_h * TRK_H / 1000;
    int top = TRK_Y + st->reel_bar_pos * TRK_H / 1000;
    if (zh < 8) zh = 8;
    lv_obj_set_size(g_zone, TRK_W - 4, zh);
    lv_obj_set_pos(g_zone, TRK_X + 2, top);

    int fy = TRK_Y + st->reel_fish_pos * TRK_H / 1000;
    lv_obj_set_pos(g_fishmark, TRK_X + 3, fy - 5);

    int ph = st->reel_progress * PRG_H / 1000;
    lv_obj_set_size(g_prg_fill, PRG_W, ph);
    lv_obj_set_pos(g_prg_fill, PRG_X, PRG_Y + PRG_H - ph);

    const fish_species_t *f = fishing_species_info(st->cur_species);
    snprintf(buf, sizeof(buf), "收线 %d%%\n%s", st->reel_progress / 10,
             f ? f->name : "");
    lv_label_set_text(g_lbl_reel, buf);
    lv_label_set_text(g_lbl_hint, "按住 OK 抬竿 · 松开落下");
}

static void refresh_result(const fishing_status_t *st) {
    const catch_result_t *res = fishing_last_catch();
    char buf[192];
    if (st->state == STATE_CATCH && res && res->species >= 0) {
        const fish_species_t *f = fishing_species_info(res->species);
        lv_img_set_src(g_res_img, FISH_IMG[res->species]);
        snprintf(buf, sizeof(buf), "%s  %s", f->name, fishing_rarity_name((rarity_t)f->rarity));
        lv_label_set_text(g_res_name, buf);
        snprintf(buf, sizeof(buf), "%d.%d 厘米 · %d 克\n本次 +%d 分%s",
                 res->len_mm / 10, res->len_mm % 10, res->wgt_g, res->score,
                 res->perfect ? "\n完美钓获！" : "");
        lv_label_set_text(g_res_info, buf);
        lv_label_set_text(g_lbl_hint, res->first_catch ? "新收录！加入图鉴" : "已放入图鉴");
    } else {
        lv_img_set_src(g_res_img, SIL_IMG[0]);
        lv_label_set_text(g_res_name, "跑鱼了…");
        lv_label_set_text(g_res_info, "再试一次，注意提前跟竿");
        lv_label_set_text(g_lbl_hint, "别灰心");
    }
    snprintf(buf, sizeof(buf), "得分 %d", st->score);
    lv_label_set_text(g_lbl_score, buf);
}

static void refresh_menu(const fishing_status_t *st) {
    char buf[96];
    for (int i = 0; i < 5; ++i) {
        const char *txt = "";
        switch (i) {
            case 0: txt = "开始钓鱼"; break;
            case 1: snprintf(buf, sizeof(buf), "鱼竿   %s", fishing_rod_name(st->rod)); txt = buf; break;
            case 2: snprintf(buf, sizeof(buf), "饵料   %s", fishing_bait_name(st->bait)); txt = buf; break;
            case 3: {
                if (fishing_spot_unlocked(st->spot))
                    snprintf(buf, sizeof(buf), "钓点   %s", fishing_spot_name(st->spot));
                else
                    snprintf(buf, sizeof(buf), "钓点   %s(未解锁)", fishing_spot_name(st->spot));
                txt = buf; break;
            }
            case 4: snprintf(buf, sizeof(buf), "图鉴   %d/%d", st->codex_total, FISH_SPECIES_COUNT); txt = buf; break;
            default: break;
        }
        lv_label_set_text(g_menu_rows[i], txt);
    }
    lv_obj_set_pos(g_menu_selbar, 6, 70 + g_menu_idx * 30);
    lv_label_set_text(g_lbl_hint, g_menu_edit ? "上下改值·OK确认" : "上下选·OK进入·长按返回");
}

static void refresh_codex(const fishing_status_t *st) {
    (void)st;
    int idx = fishing_codex_cursor();
    const fish_species_t *f = fishing_species_info(idx);
    bool seen = fishing_codex_is_seen(idx);
    char buf[192];

    lv_img_set_src(g_cdx_img, seen ? FISH_IMG[idx] : SIL_IMG[idx]);
    if (seen) {
        snprintf(buf, sizeof(buf), "%d. %s · %s%s", idx + 1, f->name,
                 fishing_rarity_name((rarity_t)f->rarity),
                 fishing_codex_is_perfect(idx) ? " ✓" : "");
        lv_label_set_text(g_cdx_name, buf);
        snprintf(buf, sizeof(buf), "最长 %d.%d 厘米 · 钓 %d 条",
                 fishing_codex_best_len(idx) / 10, fishing_codex_best_len(idx) % 10,
                 fishing_codex_count(idx));
        lv_label_set_text(g_cdx_meta, buf);
        lv_label_set_text(g_cdx_desc, f->desc);
    } else {
        snprintf(buf, sizeof(buf), "%d. ???", idx + 1);
        lv_label_set_text(g_cdx_name, buf);
        snprintf(buf, sizeof(buf), "%s · 栖息于%s", fishing_rarity_name((rarity_t)f->rarity),
                 fishing_spot_name((spot_t)f->spot));
        lv_label_set_text(g_cdx_meta, buf);
        lv_label_set_text(g_cdx_desc, "还没有见过它，钓上来就能解锁");
    }
    fishing_status_t s2;
    fishing_get_status(&s2);
    snprintf(buf, sizeof(buf), "收录 %d/%d   累计钓获 %d", s2.codex_total, FISH_SPECIES_COUNT, s2.total_catch);
    lv_label_set_text(g_cdx_prog, buf);
    lv_label_set_text(g_lbl_hint, "上下翻·OK详情·长按返回");
}

static void refresh_ui(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    hud_batt_refresh();                    /* 顶栏电量：所有页面都刷新 */
    switch (st.state) {
        case STATE_MENU:                       show_only(3); refresh_menu(&st);        break;
        case STATE_CODEX:
        case STATE_CODEX_INFO:                 show_only(4); refresh_codex(&st);       break;
        case STATE_REELING:                    show_only(1); refresh_reel(&st);        break;
        case STATE_CATCH:
        case STATE_ESCAPE:                     show_only(2); refresh_result(&st);      break;
        default:                               show_only(0); refresh_scene(&st);       break;
    }
}

/* ===================== 按键处理 ===================== */
static void menu_change_value(int dir) {
    fishing_status_t st;
    fishing_get_status(&st);
    if (g_menu_idx == 1) {
        int v = ((int)st.rod + (dir > 0 ? 1 : FISH_ROD_COUNT - 1)) % FISH_ROD_COUNT;
        fishing_set_rod((rod_t)v);
    } else if (g_menu_idx == 2) {
        int v = ((int)st.bait + (dir > 0 ? 1 : FISH_BAIT_COUNT - 1)) % FISH_BAIT_COUNT;
        fishing_set_bait((bait_t)v);
    } else if (g_menu_idx == 3) {
        /* 只能在已解锁的钓点之间循环，避免玩家选到进不去的关卡 */
        for (int k = 1; k <= FISH_SPOT_COUNT; ++k) {
            int v = ((int)st.spot + (dir > 0 ? k : -k + FISH_SPOT_COUNT * 2)) % FISH_SPOT_COUNT;
            if (fishing_spot_unlocked((spot_t)v)) { fishing_set_spot((spot_t)v); break; }
        }
    }
}

static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    fishing_status_t st;
    fishing_get_status(&st);

    /* ---- 收线中：按下按住 / 抬起松开（用 CLICK 代表"按下并抬起"） ---- */
    if (st.state == STATE_REELING) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_PRESS) fishing_reel_hold(true);
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) fishing_reel_hold(false);
        return;
    }

    /* ---- 图鉴 ---- */
    if (st.state == STATE_CODEX || st.state == STATE_CODEX_INFO) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) { fishing_codex_close(); return; }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)        fishing_codex_move(-1);
        else if (btn == BSP_BTN_DOWN) fishing_codex_move(1);
        else if (btn == BSP_BTN_OK)   fishing_codex_toggle_info();
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 菜单 ---- */
    if (st.state == STATE_MENU) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            fishing_exit_menu(); g_menu_edit = 0; fishing_audio_play(SFX_CLICK);
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)   { if (g_menu_edit) menu_change_value(-1); else g_menu_idx = (g_menu_idx + 4) % 5; }
        else if (btn == BSP_BTN_DOWN) { if (g_menu_edit) menu_change_value(1); else g_menu_idx = (g_menu_idx + 1) % 5; }
        else if (btn == BSP_BTN_OK) {
            if (g_menu_idx == 0) {
                fishing_exit_menu();
                fishing_cast();
            } else if (g_menu_idx == 4) {
                fishing_codex_open();
            } else {
                g_menu_edit = !g_menu_edit;
            }
        }
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 场景：OK 抛竿 / 提竿，长按开菜单 ---- */
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK && st.state == STATE_IDLE) {
        fishing_enter_menu(); fishing_audio_play(SFX_CLICK); return;
    }
    if (ev == BSP_BTN_PRESS && btn == BSP_BTN_OK) {
        if (st.state == STATE_IDLE) { fishing_cast(); fishing_audio_play(SFX_CLICK); }
        else if (st.state == STATE_BITE) {
            fishing_strike();
            fishing_audio_play(SFX_CLICK);
        }
    }
}

/* ===================== 主任务 ===================== */
static void game_task(void *arg) {
    (void)arg;
    for (;;) {
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);

        int now = (int)now_ms();
        fishing_event_t evt = fishing_tick(now);
        if (evt == EVT_BITE_START)        fishing_audio_play(SFX_BITE);
        else if (evt == EVT_BITE_TIMEOUT) fishing_audio_play(SFX_MISS);
        else if (evt == EVT_CATCH)        { fishing_audio_play(SFX_CATCH); nvs_save_now(); }
        else if (evt == EVT_ESCAPE)       fishing_audio_play(SFX_MISS);

        /* 电量节流 */
        if (now - g_last_batt_ms > 500) { g_batt_soc = bsp_battery_soc(); g_last_batt_ms = now; }
        /* 每 30 秒兜底存一次（钓到鱼时也会存），避免频繁擦写 flash */
        if (now - g_last_save_ms > 30000) { nvs_save_now(); g_last_save_ms = now; }

        if (bsp_lvgl_lock(100)) { refresh_ui(); bsp_lvgl_unlock(); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void fishing_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_btn_q) return;
    const btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ===================== 入口 ===================== */
void fishing_app_start(void) {
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    fishing_init(now_ms());
    nvs_load_save();                 /* 内含版本校验/迁移，失败就当新号 */
    fishing_audio_init();

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();
        bsp_lvgl_unlock();
    }
    xTaskCreate(game_task, "fishing", 6144, NULL, 5, NULL);
}
