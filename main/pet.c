/*
 * pet.c - 电子宠物「口袋兔」应用层（AI Passport / ESP32-C3, LVGL v9）
 *
 * 职责边界（对齐 fishing.c 的分层约定）：
 *   - 照顾规则/衰减/存档/贴纸 全部在 pet_logic.c（纯逻辑，可单测）
 *   - 本文件只负责：LVGL 画面、三键交互、音效、电量、NVS 存档
 *
 * 交互（6-9 岁适龄：一键即反馈、几乎不依赖识字、零挫败）：
 *   列表类（照顾/游戏）：UP/DOWN 移动红色选中胶囊，OK 确认
 *   照顾：喂食/洗澡/陪玩/睡觉 一键即反馈，动作期间播放专属帧动画
 *   小游戏：5 款，统一“上下选 + OK 确认”或“上下移动 + 自动接”的极简玩法
 *   宠物护照：12 枚贴纸收集墙，OK/任意键返回
 *
 * 适龄铁律：
 *   - 不死亡、不惩罚，数值只涨不扣（陪玩仅耗一点精力）
 *   - 无长按、无倒计时焦虑、无失败惩罚
 *   - 睡觉背光调暗护眼省电，睡饱自动醒
 *
 * LVGL 线程安全铁律：所有 lv_obj_* 调用必须在 bsp_lvgl_lock 内执行。
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
#include "fishing_audio.h"
#include "../assets/sprites/pet_sprites.h"
#include "../assets/sprites/pet_room.h"
#include "../assets/sprites/pet_costumes.h"
#include "../assets/sprites/pet_game_sprites.h"

/* ===================== 中文字体（与钓鱼共用同一子集字库） ===================== */
LV_FONT_DECLARE(fishing_cjk_16);

static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ===================== 屏幕 / 调色板 ===================== */
#define SCR_W 240
#define SCR_H 320

#define CREAM   lv_color_make(255, 246, 240)   /* 奶油底 */
#define PINK    lv_color_make(255, 222, 232)   /* 粉面板 */
#define GRASS   lv_color_make(198, 230, 178)   /* 草地黄绿 */
#define RUI_RED lv_color_make(196, 30, 80)     /* 锐胜红点睛 */
#define RED_TXT lv_color_make(153, 0, 58)
#define BROWN   lv_color_make(96, 64, 44)
#define GRAYTX  lv_color_make(120, 100, 90)
#define WHITE   lv_color_make(255, 255, 255)
#define BAR_G   lv_color_make(120, 210, 130)
#define BAR_O   lv_color_make(245, 180, 70)
#define BAR_R   lv_color_make(235, 90, 80)

/* LVGL9 的 lv_color_make() 是内联函数，不是常量表达式，不能直接用于
   文件作用域静态初始化器（编译会报 "initializer element is not constant"）。
   因此这里只存 RGB 分量，运行时再通过 gc() 转换成 lv_color_t。 */
static const uint8_t COLORS_RGB[4][3] = {
    {214, 40,  92},
    {40,  110, 210},
    {240, 200, 50},
    {80,  190, 110},
};
static lv_color_t gc(int i) {
    i = ((i % 4) + 4) % 4;
    return lv_color_make(COLORS_RGB[i][0], COLORS_RGB[i][1], COLORS_RGB[i][2]);
}

/* ===================== 场景模式 ===================== */
enum { SCR_HOME = 0, SCR_MENU, SCR_PASSPORT, SCR_GAME, SCR_DRESS };

/* ===================== 换装（v3） ===================== */
static const lv_image_dsc_t *const COSTUME_IMG[PET_COSTUME_MAX + 1] = {
    NULL, &costume_hat, &costume_crown, &costume_glasses, &costume_scarf, &costume_flower,
};
static const char *const COSTUME_NAMES[PET_COSTUME_MAX + 1] = {
    "不穿", "草帽", "皇冠", "眼镜", "围巾", "花环",
};

/* 颜色配对用的四色蛋（顺序与 COLORS_RGB / gc(i) 严格一致） */
static const lv_image_dsc_t *const EGG_IMG[4] = {
    &spr_egg_red, &spr_egg_blue, &spr_egg_yellow, &spr_egg_green,
};

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

/* ===================== 通用 UI 小工具 ===================== */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, int x, int y, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    use_cjk(l);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, bg, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *make_capsule(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, RUI_RED, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, h / 2, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    /* 不做 move_to_index 沉底：三个调用点都是"先建胶囊、后建文字"，创建顺序
       已保证文字在胶囊之上；若沉到 index 0 会掉到主屏房间背景图下面被盖住。 */
    return c;
}

static bool set_text_cached(lv_obj_t *lbl, const char *txt) {
    if (!lbl || !txt) return false;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0) return false;
    lv_label_set_text(lbl, txt);
    return true;
}

/* ===================== 全局 UI 对象 ===================== */
static lv_obj_t *g_home   = NULL;   /* 照顾主屏容器 */
static lv_obj_t *g_home_bg = NULL;  /* 房间内景背景图（铺底，只露上半段墙区） */
static lv_obj_t *g_layer  = NULL;   /* 菜单/护照/游戏内容容器（纯色底） */

/* 主屏 HUD / 兔子 / 状态条 */
static lv_obj_t *g_lbl_level = NULL;
static lv_obj_t *g_lbl_mood  = NULL;
static lv_obj_t *g_lbl_batt  = NULL;
static lv_obj_t *g_batt_body = NULL;
static lv_obj_t *g_batt_fill = NULL;
static lv_obj_t *g_batt_nub  = NULL;
static lv_obj_t *g_rabbit    = NULL;
static lv_obj_t *g_rab_cos   = NULL;   /* 主屏兔子身上当前装扮叠层 */
static lv_obj_t *g_lbl_zzz   = NULL;
static lv_obj_t *g_lbl_fb    = NULL;   /* 动作反馈文字 */
static lv_obj_t *g_fb_cap    = NULL;   /* 动作反馈白底胶囊（避免与状态条文字重合） */
static lv_obj_t *g_row_fill[4] = {0};
static lv_obj_t *g_home_lbl[6] = {0};
static lv_obj_t *g_home_cap  = NULL;
static lv_obj_t *g_home_hint = NULL;

/* 菜单（小游戏列表） */
static lv_obj_t *g_menu_lbl[7] = {0};
static lv_obj_t *g_menu_cap  = NULL;
static lv_obj_t *g_menu_title = NULL;

/* 换装界面 */
static lv_obj_t *g_dress_rab  = NULL;   /* 2x 大兔子（试衣镜） */
static lv_obj_t *g_dress_cos  = NULL;   /* 装扮预览叠层（同位置同缩放） */
static lv_obj_t *g_dress_cap  = NULL;
static lv_obj_t *g_dress_lbl[6] = {0};
static lv_obj_t *g_dress_msg  = NULL;
static lv_obj_t *g_dress_name = NULL;   /* 镜下当前装扮名 */

/* 护照（贴纸墙） */
static lv_obj_t *g_pass_title = NULL;
static lv_obj_t *g_pass_count = NULL;
static lv_obj_t *g_pass_cell[12] = {0};
static lv_obj_t *g_pass_icon[12] = {0};

/* 游戏层 */
static lv_obj_t *g_g_title = NULL;
static lv_obj_t *g_g_msg   = NULL;
static lv_obj_t *g_g_obj[8] = {0};
static lv_obj_t *g_g_egg[4] = {0};   /* 颜色配对：0..2 答案蛋，3 题目蛋 */
static lv_obj_t *g_g_icon[4] = {0};  /* 记忆翻牌：卡面小图标 */
static lv_obj_t *g_g_mark  = NULL;   /* 躲猫猫：红色选择箭头 */

/* ===================== 运行状态 ===================== */
static int  g_scr = SCR_HOME;
static int  g_rebuild = 0;     /* 需要重建 g_layer 或切换可见性 */
static int  g_home_sel = 0;
static int  g_menu_sel = 0;
static int  g_dress_sel = 0;
static int  g_batt_soc = -1;

/* 兔子动画 / 动作帧 */
static int  g_rab_x = 84;
static int  g_rab_dir = 1;
static int  g_rab_last_ms = 0;
static bool g_rab_bob_up = false;
static int  g_rab_bob_ms = 0;
static const lv_image_dsc_t *g_rab_frame = &rabbit_front;
static int  g_action_until = 0;
static const lv_image_dsc_t *g_action_frame = &rabbit_front;
/* 切屏防抖：进入新画面后 450ms 内忽略 OK/返回类 CLICK。一次按键在部分
   驱动/模拟器上会 PRESS+CLICK 连发，孩子手抖也常连按；刚切屏就误触发
   会“闪进闪出”，看起来像按键失灵（模拟器实测护照页被瞬间弹回）。 */
static int  g_scr_since = 0;

/* 反馈文案有效期 */
static int g_fb_until_ms = 0;

/* 节流缓存 */
static int s_lvl = -1, s_mood = -99, s_batt = -2;
static int s_bar[4] = {-1,-1,-1,-1}, s_sel = -1, s_sleep = false;

static const char *HOME_ITEMS[6] = { "喂食", "洗澡", "陪玩", "睡觉", "小游戏", "护照" };
static const char *MENU_ITEMS[7] = { "接胡萝卜", "颜色配对", "记忆翻牌", "躲猫猫", "节奏蹦蹦", "换装", "返回" };

/* ===================== 简单随机数（无 stdlib rand 依赖） ===================== */
static uint32_t g_rng = 0x12345678u;
static int rnd(int n) {
    if (n <= 0) return 0;
    g_rng = g_rng * 1664525u + 1013904223u;
    return (int)((g_rng >> 8) % (uint32_t)n);
}

/* ===================== 按键队列 ===================== */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q = NULL;

/* ===================== 小游戏状态 ===================== */
enum { G_INTRO = 0, G_PLAY, G_DONE };
typedef struct {
    int id;          /* 0..4 */
    int phase;
    int score;
    int sel;         /* 当前选中项 */
    int target;      /* 正确答案 */
    int correct;     /* 正确答案所在选项下标（颜色配对用） */
    int timer;       /* 计时累加（ms） */
    int sub;         /* 子步骤 / 闪烁序号 */
    int input_idx;   /* 序列复现进度 */
    int seq_len;
    int seq[8];
    int pos;         /* 移动物 x */
    int dir;         /* 移动方向 */
    int obj_x, obj_y;/* 掉落物坐标 */
    int flash_done;  /* 记忆/躲猫猫 展示完毕 */
    int beats;       /* 节奏已按压次数 */
    int granted;     /* 是否已发贴纸+奖励 */
} game_t;
static game_t g_g;

/* ===================== 换装叠层 ===================== */
/* 把当前装扮叠到兔子图上（同尺寸画布，直接同位置覆盖即可对齐头部）。
   0 = 不穿 → 隐藏叠层。主屏与换装页共用。 */
static void costume_overlay_set(lv_obj_t *img, int id, bool scaled2x)
{
    if (!img) return;
    if (id > 0 && id <= PET_COSTUME_MAX) {
        lv_img_set_src(img, COSTUME_IMG[id]);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    (void)scaled2x;
}

/* ===================== 主屏构建 ===================== */
static void build_home(void) {
    lv_obj_t *scr = lv_screen_active();
    g_home = lv_obj_create(scr);
    lv_obj_set_size(g_home, SCR_W, SCR_H);
    lv_obj_set_pos(g_home, 0, 0);
    lv_obj_set_style_bg_color(g_home, CREAM, 0);
    lv_obj_set_style_bg_opa(g_home, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_home, 0, 0);
    lv_obj_set_style_radius(g_home, 0, 0);
    lv_obj_set_style_pad_all(g_home, 0, 0);
    lv_obj_clear_flag(g_home, LV_OBJ_FLAG_SCROLLABLE);

    /* 房间内景背景：天花板带 + 墙纸 + 窗 + 相框 + 落地灯 + 木椅 + 书桌 + 地板条。
       只铺底；下面先垫两块奶油文字面板，保证文字绝不与房间花纹重合（审核驳回点）。 */
    g_home_bg = lv_img_create(g_home);
    lv_img_set_src(g_home_bg, &pet_room);
    lv_obj_set_pos(g_home_bg, 0, 0);

    /* 文字衬底面板（在所有文字/胶囊之前创建，沉在文字下面）：
       状态条区 y116-178 / 照顾列表区 y176-308；房间家具只安排在面板之外的缝隙 */
    make_panel(g_home, 4, 116, 164, 62, CREAM);
    make_panel(g_home, 8, 176, 224, 132, CREAM);

    /* HUD：等级 · 心情 · 电池 */
    g_lbl_level = make_label(g_home, "等级 1", 14, 4, BROWN);
    g_lbl_mood  = make_label(g_home, "开心", 92, 4, RED_TXT);

    g_batt_body = lv_obj_create(g_home);
    lv_obj_set_size(g_batt_body, 12, 8);
    lv_obj_set_pos(g_batt_body, 176, 9);
    lv_obj_set_style_bg_opa(g_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_batt_body, GRAYTX, 0);
    lv_obj_set_style_border_width(g_batt_body, 1, 0);
    lv_obj_set_style_radius(g_batt_body, 1, 0);
    lv_obj_set_style_pad_all(g_batt_body, 0, 0);
    lv_obj_clear_flag(g_batt_body, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_fill = lv_obj_create(g_batt_body);
    lv_obj_set_size(g_batt_fill, 10, 6);
    lv_obj_set_pos(g_batt_fill, 0, 0);
    lv_obj_set_style_bg_color(g_batt_fill, BAR_G, 0);
    lv_obj_set_style_bg_opa(g_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_fill, 0, 0);
    lv_obj_set_style_radius(g_batt_fill, 0, 0);
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_nub = lv_obj_create(g_home);
    lv_obj_set_size(g_batt_nub, 2, 4);
    lv_obj_set_pos(g_batt_nub, 188, 11);
    lv_obj_set_style_bg_color(g_batt_nub, GRAYTX, 0);
    lv_obj_set_style_bg_opa(g_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_nub, 0, 0);
    lv_obj_set_style_radius(g_batt_nub, 0, 0);
    lv_obj_clear_flag(g_batt_nub, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_batt = make_label(g_home, "--", 193, 4, BROWN);

    /* 兔子 */
    g_rabbit = lv_img_create(g_home);
    lv_img_set_src(g_rabbit, &rabbit_front);
    lv_obj_set_pos(g_rabbit, g_rab_x, 26);

    /* 装扮叠层（跟兔子同位置；refresh_home 每帧同步坐标） */
    g_rab_cos = lv_img_create(g_home);
    costume_overlay_set(g_rab_cos, pet_get_costume(), false);
    lv_obj_set_pos(g_rab_cos, g_rab_x, 26);

    g_lbl_zzz = make_label(g_home, "z Z z", g_rab_x + 58, 30, lv_color_make(150, 150, 170));
    lv_obj_add_flag(g_lbl_zzz, LV_OBJ_FLAG_HIDDEN);

    /* 动作反馈：白底圆角胶囊 + 红字，独立一行（兔子底 98 / 状态条顶 118 之间），
       出现时绝不与状态条文字重合（审核驳回点），默认隐藏 */
    g_fb_cap = make_panel(g_home, 16, 100, 208, 18, lv_color_make(255, 250, 242));
    lv_obj_add_flag(g_fb_cap, LV_OBJ_FLAG_HIDDEN);
    g_lbl_fb = make_label(g_fb_cap, "", 0, 0, RED_TXT);
    lv_obj_align(g_lbl_fb, LV_ALIGN_CENTER, 0, 0);

    /* 四条状态条（118 起，与反馈胶囊 100~118 相接不重叠） */
    static const char *ROW[4] = { "饱食", "干净", "玩乐", "精力" };
    int bar_x = 40, bar_w = 120, bar_h = 8, row0 = 118, dyy = 14;
    for (int i = 0; i < 4; ++i) {
        int y = row0 + i * dyy;
        make_label(g_home, ROW[i], 8, y, BROWN);
        lv_obj_t *bg = make_panel(g_home, bar_x, y, bar_w, bar_h, lv_color_make(232, 220, 210));
        (void)bg;
        g_row_fill[i] = make_panel(g_home, bar_x, y, bar_w, bar_h, BAR_G);
    }

    /* 照顾列表（6 项；选中胶囊随 refresh_home 移动，x10 营在列表面板内） */
    g_home_cap = make_capsule(g_home, 10, 176, 208, 22);
    int ly0 = 177, ldy = 21;
    for (int i = 0; i < 6; ++i) {
        g_home_lbl[i] = make_label(g_home, HOME_ITEMS[i], 18, ly0 + i * ldy, BROWN);
    }

    g_home_hint = make_label(g_home, "上下选 · OK 做", 0, 0, GRAYTX);
    lv_obj_align(g_home_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
}

/* ===================== 菜单（小游戏列表） ===================== */
/* 菜单/护照/游戏/换装均为文字密集页：g_layer 用纯奶油底，不铺房间图，
   文字绝不与背景花纹重合（审核驳回点）；房间内景只在主屏露出。 */
static void build_menu(void) {
    lv_obj_clean(g_layer);
    g_menu_title = make_label(g_layer, "小游戏", 0, 0, RED_TXT);
    lv_obj_align(g_menu_title, LV_ALIGN_TOP_MID, 0, 26);
    g_menu_cap = make_capsule(g_layer, 16, 63, 208, 26);
    int ly0 = 66, ldy = 27;
    for (int i = 0; i < 7; ++i) {
        g_menu_lbl[i] = make_label(g_layer, MENU_ITEMS[i], 30, ly0 + i * ldy, BROWN);
    }
}

/* ===================== 护照（贴纸墙） ===================== */
static const char *sticker_name(int id) {
    /* 每个贴纸一个代表汉字（UTF-8 一个汉字 3 字节，不能用 char 数组按字节取）。
       字库由 tools/gen_font.py 扫描本文件自动生成，新增汉字会被自动收录。 */
    static const char *const names[12] = {
        "见", /* PST_FIRST  初次见面   */
        "食", /* PST_FEED5  喂食 5 次  */
        "澡", /* PST_CLEAN5 洗澡 5 次  */
        "玩", /* PST_PLAY5  陪玩 5 次  */
        "睡", /* PST_SLEEP1 睡一次     */
        "二", /* PST_LVL2   升到 2 级  */
        "三", /* PST_LVL3   升到 3 级  */
        "萝", /* PST_GAME_CATCH  接胡萝卜 */
        "色", /* PST_GAME_COLOR  颜色配对 */
        "忆", /* PST_GAME_MEMORY 记忆翻牌 */
        "猫", /* PST_GAME_PEEK   躲猫猫   */
        "跳", /* PST_GAME_RHYTHM 节奏蹦蹦 */
    };
    if (id < 0 || id >= 12) return "?";
    return names[id];
}

static void build_passport(void) {
    lv_obj_clean(g_layer);
    g_pass_title = make_label(g_layer, "宠物护照", 0, 0, RED_TXT);
    lv_obj_align(g_pass_title, LV_ALIGN_TOP_MID, 0, 22);
    g_pass_count = make_label(g_layer, "贴纸 0/12", 0, 0, BROWN);
    lv_obj_align(g_pass_count, LV_ALIGN_TOP_MID, 0, 46);

    int cx[3] = { 18, 92, 166 };
    int cy[4] = { 70, 134, 198, 262 };
    for (int i = 0; i < 12; ++i) {
        int r = i / 3, c = i % 3;
        int x = cx[c], y = cy[r];
        g_pass_cell[i] = make_panel(g_layer, x, y, 56, 56, PINK);
        g_pass_icon[i] = make_label(g_layer, "?", x + 20, y + 18, GRAYTX);
    }
}

/* ===================== 小游戏构建 ===================== */
static void game_init(int id);
static void game_finish(void);
static void build_dress(void);   /* 定义在 passport_input 之后，rebuild_current 要用 */

static void build_game(void) {
    lv_obj_clean(g_layer);
    g_g_title = make_label(g_layer, "", 0, 0, RED_TXT);
    lv_obj_align(g_g_title, LV_ALIGN_TOP_MID, 0, 18);
    g_g_msg = make_label(g_layer, "", 0, 0, BROWN);
    lv_obj_align(g_g_msg, LV_ALIGN_TOP_MID, 0, 286);
    for (int i = 0; i < 8; ++i) g_g_obj[i] = NULL;
    game_init(g_g.id);
}

/* ===================== 重建当前层 ===================== */
static void rebuild_current(void) {
    /* 重置缓存，避免切屏后选中胶囊/睡态停在旧位置 */
    s_sel = -1;
    s_sleep = false;
    if (g_scr == SCR_HOME) {
        lv_obj_clear_flag(g_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_layer, LV_OBJ_FLAG_HIDDEN);
        if (g_scr == SCR_MENU) build_menu();
        else if (g_scr == SCR_PASSPORT) build_passport();
        else if (g_scr == SCR_GAME) build_game();
        else if (g_scr == SCR_DRESS) build_dress();
    }
}

/* ===================== 反馈 ===================== */
static void show_feedback(const char *txt) {
    if (set_text_cached(g_lbl_fb, txt) || g_fb_until_ms == 0) {
        lv_obj_clear_flag(g_fb_cap, LV_OBJ_FLAG_HIDDEN);
    }
    g_fb_until_ms = (int)now_ms() + 1600;
}

/* ===================== 动作执行（主屏照顾） ===================== */
static void do_care(int idx) {
    pet_event_t ev;
    const lv_image_dsc_t *fr = &rabbit_front;
    const char *fb = "";
    sfx_t sfx = SFX_CATCH;
    switch (idx) {
        case 0: ev = pet_feed();   fr = &rabbit_eat;   fb = (ev==PET_EVT_ACTION_FULL)?"已经饱饱的啦":"吃饱啦！"; break;
        case 1: ev = pet_clean();  fr = &rabbit_bath;  fb = (ev==PET_EVT_ACTION_FULL)?"已经很干净啦":"洗得香香的！"; break;
        case 2: ev = pet_play();   fr = &rabbit_play;  fb = (ev==PET_EVT_TOO_TIRED)?"太累啦先睡觉":"玩得好开心！"; break;
        default: ev = pet_toggle_sleep(); fr = &rabbit_sleep;
                 fb = (ev==PET_EVT_WAKE)?"早上好呀！":"晚安，小兔…";
                 if (ev == PET_EVT_WAKE) fr = &rabbit_front;
                 break;
    }
    if (ev == PET_EVT_TOO_TIRED) sfx = SFX_MISS;
    else if (ev == PET_EVT_ACTION_FULL || ev == PET_EVT_WAKE || ev == PET_EVT_SLEEP_START) sfx = SFX_CLICK;
    else sfx = SFX_CATCH;
    fishing_audio_play(sfx);
    g_action_frame = fr;
    g_action_until = (int)now_ms() + (idx == 3 && ev == PET_EVT_SLEEP_START ? 0 : 1100);
    show_feedback(fb);
}

/* ===================== 输入：主屏 ===================== */
static void home_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    pet_status_t st; pet_get_status(&st);
    if (st.sleeping) {
        if (ev != BSP_BTN_CLICK) return;
        pet_toggle_sleep();
        fishing_audio_play(SFX_CLICK);
        show_feedback("早上好呀！");
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP)   { g_home_sel = (g_home_sel + 5) % 6; fishing_audio_play(SFX_CLICK); }
    else if (btn == BSP_BTN_DOWN) { g_home_sel = (g_home_sel + 1) % 6; fishing_audio_play(SFX_CLICK); }
    else if (btn == BSP_BTN_OK) {
        if ((int)now_ms() - g_scr_since < 450) return;  /* 切屏防抖 */
        if (g_home_sel < 4)      { do_care(g_home_sel); }
        else if (g_home_sel == 4){ g_scr = SCR_MENU; g_menu_sel = 0; g_rebuild = 1; g_scr_since = (int)now_ms(); }
        else                     { g_scr = SCR_PASSPORT; g_rebuild = 1; g_scr_since = (int)now_ms(); }
    }
}

/* ===================== 输入：菜单 ===================== */
static void menu_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP)   { g_menu_sel = (g_menu_sel + 6) % 7; fishing_audio_play(SFX_CLICK); }
    else if (btn == BSP_BTN_DOWN) { g_menu_sel = (g_menu_sel + 1) % 7; fishing_audio_play(SFX_CLICK); }
    else if (btn == BSP_BTN_OK) {
        if ((int)now_ms() - g_scr_since < 450) return;  /* 切屏防抖 */
        if (g_menu_sel == 6) { g_scr = SCR_HOME; g_rebuild = 1; g_scr_since = (int)now_ms(); }
        else if (g_menu_sel == 5) { g_dress_sel = pet_get_costume(); g_scr = SCR_DRESS; g_rebuild = 1; g_scr_since = (int)now_ms(); }
        else {
            g_g.id = g_menu_sel; g_scr = SCR_GAME; g_rebuild = 1; g_scr_since = (int)now_ms();
        }
    }
}

/* ===================== 输入：护照 ===================== */
static void passport_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    (void)btn;
    if (ev != BSP_BTN_CLICK) return;
    /* 任意键返回；但刚进入的 450ms 内忽略，防止切屏连击瞬间把护照弹回，
       小朋友会以为护照页“打不开”。 */
    if ((int)now_ms() - g_scr_since < 450) return;
    g_scr = SCR_HOME; g_rebuild = 1; g_scr_since = (int)now_ms();
}

/* ===================== 换装界面 =====================
 * 布局（240x320）：
 *   y6    标题「换装」居中
 *   左侧  试衣镜面板 (8,28,150,230)：2x 兔子居中 + 镜下当前装扮名
 *   右侧  装扮列表 x160..232，6 行每行 24px，与镜子垂直居中
 *   y264  操作提示 / 穿戴反馈（居中，不与任何图形重叠）
 *   底部  长按 OK 返回                                          */
#define DRESS_LIST_X   160
#define DRESS_LIST_Y   71      /* 首行 y：面板 28+230 中心对齐 6 行 x24 */
#define DRESS_ROW_H    24
#define DRESS_RAB_XY   3, 43   /* 兔子在镜面板内相对坐标（144 宽居中于 150） */

static void build_dress(void) {
    lv_obj_clean(g_layer);
    lv_obj_t *title = make_label(g_layer, "换装", 0, 0, RED_TXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* 试衣镜面板：淡青灰底，与奶油层拉开层次 */
    lv_obj_t *mirror = make_panel(g_layer, 8, 28, 150, 230,
                                  lv_color_make(226, 236, 246));
    g_dress_rab = lv_img_create(mirror);
    lv_img_set_src(g_dress_rab, &rabbit_front);
    lv_obj_set_pos(g_dress_rab, DRESS_RAB_XY);
    lv_image_set_scale(g_dress_rab, 512);
    lv_image_set_antialias(g_dress_rab, false);

    g_dress_cos = lv_img_create(mirror);
    lv_obj_set_pos(g_dress_cos, DRESS_RAB_XY);
    lv_image_set_scale(g_dress_cos, 512);
    lv_image_set_antialias(g_dress_cos, false);
    costume_overlay_set(g_dress_cos, g_dress_sel, true);

    /* 镜下装扮名：随选中即时更新 */
    g_dress_name = make_label(mirror, COSTUME_NAMES[g_dress_sel], 0, 0, BROWN);
    lv_obj_align(g_dress_name, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 装扮列表（右侧）：0=不穿，1..5=五件装扮；先建胶囊后建文字 */
    g_dress_cap = make_capsule(g_layer, DRESS_LIST_X, DRESS_LIST_Y, 72, 20);
    for (int i = 0; i < 6; ++i) {
        g_dress_lbl[i] = make_label(g_layer, COSTUME_NAMES[i],
                                    DRESS_LIST_X + 18, DRESS_LIST_Y + 2 + i * DRESS_ROW_H,
                                    BROWN);
    }

    g_dress_msg = make_label(g_layer, "上下选 · OK 穿上", 0, 0, BROWN);
    lv_obj_align(g_dress_msg, LV_ALIGN_TOP_MID, 0, 264);

    lv_obj_t *hint = make_label(g_layer, "长按 OK 返回", 0, 0, GRAYTX);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void dress_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    /* 与小游戏同规则：3 键硬件没有返回键，长按 OK 随时退出 */
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        g_scr = SCR_MENU; g_menu_sel = 5; g_rebuild = 1; g_scr_since = (int)now_ms();
        fishing_audio_play(SFX_CLICK);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP)   { g_dress_sel = (g_dress_sel + 5) % 6; fishing_audio_play(SFX_CLICK); }
    else if (btn == BSP_BTN_DOWN) { g_dress_sel = (g_dress_sel + 1) % 6; fishing_audio_play(SFX_CLICK); }
    else if (btn == BSP_BTN_OK) {
        if ((int)now_ms() - g_scr_since < 450) return;  /* 切屏防抖 */
        pet_set_costume(g_dress_sel);
        nvs_save_now();   /* 低频操作，立即落盘 */
        fishing_audio_play(SFX_CATCH);
        if (g_dress_msg) set_text_cached(g_dress_msg, (g_dress_sel == 0) ? "脱下来啦" : "真好看！");
    }
}

static void refresh_dress(void) {
    if (g_dress_sel != s_sel) {
        s_sel = g_dress_sel;
        lv_obj_set_pos(g_dress_cap, DRESS_LIST_X, DRESS_LIST_Y + g_dress_sel * DRESS_ROW_H);
        for (int i = 0; i < 6; ++i) {
            if (!g_dress_lbl[i]) continue;
            lv_obj_set_style_text_color(g_dress_lbl[i],
                                        (i == g_dress_sel) ? WHITE : BROWN, 0);
        }
        if (g_dress_name) set_text_cached(g_dress_name, COSTUME_NAMES[g_dress_sel]);
        /* 实时预览：选中即试穿（OK 才真正保存） */
        costume_overlay_set(g_dress_cos, g_dress_sel, true);
    }
}

/* 出一道新题：题目色一定出现在三个选项里，正确位置随机。
   init=true 时创建对象，false 时只换色/换蛋（新一题复用已有对象）。*/
static void color_new_round(bool init) {
    static const int OX[3] = { 16, 92, 168 };
    g_g.target  = rnd(4);
    g_g.correct = rnd(3);
    int other[3], n = 0;
    for (int c = 0; c < 4; ++c) if (c != g_g.target) other[n++] = c;
    int oi = 0;
    for (int i = 0; i < 3; ++i) {
        int ci = (i == g_g.correct) ? g_g.target : other[oi++];
        if (init) {
            g_g_obj[1 + i] = make_panel(g_layer, OX[i], 140, 56, 56, gc(ci));
            g_g_egg[i] = lv_img_create(g_layer);
            lv_img_set_src(g_g_egg[i], EGG_IMG[ci]);
            lv_obj_set_pos(g_g_egg[i], OX[i] + 4, 144);
        } else {
            lv_obj_set_style_bg_color(g_g_obj[1 + i], gc(ci), 0);
            lv_img_set_src(g_g_egg[i], EGG_IMG[ci]);
        }
    }
    /* 题目蛋（上方大蛋） */
    if (init) {
        g_g_obj[0] = make_panel(g_layer, 70, 56, 100, 46, gc(g_g.target));
        g_g_egg[3] = lv_img_create(g_layer);
        lv_obj_set_pos(g_g_egg[3], 96, 55);
    }
    lv_obj_set_style_bg_color(g_g_obj[0], gc(g_g.target), 0);
    lv_img_set_src(g_g_egg[3], EGG_IMG[g_g.target]);
}

/* ===================== 小游戏：初始化 ===================== */
static void game_init(int id) {
    g_g.id = id;
    g_g.phase = G_INTRO;
    g_g.score = 0;
    g_g.timer = 0;
    g_g.sub = 0;
    g_g.input_idx = 0;
    g_g.seq_len = 3;
    g_g.flash_done = 0;
    g_g.beats = 0;
    g_g.granted = 0;
    g_g.sel = 0;

    const char *names[5] = { "接胡萝卜", "颜色配对", "记忆翻牌", "躲猫猫", "节奏蹦蹦" };
    if (g_g_title) set_text_cached(g_g_title, names[id]);
    if (g_g_msg) set_text_cached(g_g_msg, "按 OK 开始 · 长按 OK 返回");

    if (id == 0) { /* 接胡萝卜：像素胡萝卜从天上掉 */
        g_g_obj[0] = lv_img_create(g_layer);
        lv_img_set_src(g_g_obj[0], &rabbit_front);
        lv_obj_set_pos(g_g_obj[0], 80, 240);
        g_g.obj_x = 110; g_g.obj_y = 70; g_g.pos = 80;
        g_g_obj[1] = lv_img_create(g_layer);
        lv_img_set_src(g_g_obj[1], &spr_carrot);
        lv_obj_set_pos(g_g_obj[1], g_g.obj_x, g_g.obj_y);
    } else if (id == 1) { /* 颜色配对：四色彩蛋 */
        color_new_round(true);
    } else if (id == 2) { /* 记忆翻牌：卡面小图标 + 卡纸样式 */
        static const lv_image_dsc_t *const ICONS[4] = {
            &spr_paw, &spr_heart, &spr_star, &spr_flower,
        };
        int px[4] = { 40, 140, 40, 140 };
        int py[4] = { 70, 70, 150, 150 };
        for (int i = 0; i < 4; ++i) {
            g_g_obj[i] = make_panel(g_layer, px[i], py[i], 60, 60, lv_color_make(250, 242, 230));
            lv_obj_set_style_border_width(g_g_obj[i], 2, 0);
            lv_obj_set_style_border_color(g_g_obj[i], BROWN, 0);
            lv_obj_set_style_radius(g_g_obj[i], 8, 0);
            g_g_icon[i] = lv_img_create(g_layer);
            lv_img_set_src(g_g_icon[i], ICONS[i]);
            lv_obj_set_pos(g_g_icon[i], px[i] + 20, py[i] + 20);
        }
        for (int i = 0; i < g_g.seq_len; ++i) g_g.seq[i] = rnd(4);
    } else if (id == 3) { /* 躲猫猫：像素灌木丛 + 红箭头选择 */
        int bx[3] = { 24, 96, 168 };
        for (int i = 0; i < 3; ++i) {
            g_g_obj[i] = lv_img_create(g_layer);
            lv_img_set_src(g_g_obj[i], &spr_bush);
            lv_obj_set_pos(g_g_obj[i], bx[i], 120);
        }
        g_g_obj[3] = lv_img_create(g_layer);
        lv_img_set_src(g_g_obj[3], &rabbit_front);
        lv_obj_add_flag(g_g_obj[3], LV_OBJ_FLAG_HIDDEN);
        g_g_mark = lv_img_create(g_layer);
        lv_img_set_src(g_g_mark, &spr_arrow);
        lv_obj_add_flag(g_g_mark, LV_OBJ_FLAG_HIDDEN);
        g_g.target = rnd(3);
    } else if (id == 4) { /* 节奏蹦蹦：小鼓 + 音符 */
        g_g_obj[0] = lv_img_create(g_layer);
        lv_img_set_src(g_g_obj[0], &spr_drum);
        lv_obj_set_pos(g_g_obj[0], 92, 140);
        g_g_obj[1] = lv_img_create(g_layer);
        lv_img_set_src(g_g_obj[1], &spr_note);
        lv_obj_set_pos(g_g_obj[1], 20, 162);
        g_g.pos = 20; g_g.dir = 1;
    }
}

/* ===================== 小游戏：输入 ===================== */
static void game_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    /* 长按 OK 随时退出：硬件只有三个键，游戏里没有专门的“返回”，
       不留退路孩子会被困在小游戏里、打不完出不来（违背“零挫败”适龄铁律）。 */
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        g_scr = SCR_MENU; g_menu_sel = 0; g_rebuild = 1; g_scr_since = (int)now_ms();
        show_feedback("先玩到这儿，回头再来！");
        fishing_audio_play(SFX_CLICK);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK && (int)now_ms() - g_scr_since < 450) return;  /* 进游戏防抖 */

    if (g_g.phase == G_INTRO) {
        if (btn == BSP_BTN_OK) {
            g_g.phase = G_PLAY; g_g.timer = 0; g_g.sub = 0; g_g.flash_done = 0;
            if (g_g_msg) set_text_cached(g_g_msg, "加油！");
            fishing_audio_play(SFX_CLICK);
        }
        return;
    }
    if (g_g.phase == G_DONE) {
        if (btn == BSP_BTN_OK) {
            g_scr = SCR_MENU; g_menu_sel = 0; g_rebuild = 1;
            fishing_audio_play(SFX_CLICK);
        }
        return;
    }

    /* G_PLAY */
    switch (g_g.id) {
        case 0: /* 接胡萝卜：上下移动小兔 */
            if (btn == BSP_BTN_UP)   g_g.pos = (g_g.pos <= 20) ? 20 : g_g.pos - 14;
            else if (btn == BSP_BTN_DOWN) g_g.pos = (g_g.pos >= 158) ? 158 : g_g.pos + 14;
            break;
        case 1: /* 颜色配对 */
        case 4: /* 节奏（移动靠自动，这里 OK 拍点） */
            if (btn == BSP_BTN_UP)   g_g.sel = (g_g.sel + 2) % 3;
            else if (btn == BSP_BTN_DOWN) g_g.sel = (g_g.sel + 1) % 3;
            else if (btn == BSP_BTN_OK && g_g.id == 1) {
                if (g_g.sel == g_g.correct) { g_g.score++; fishing_audio_play(SFX_CATCH); }
                else { fishing_audio_play(SFX_MISS); }
                if (g_g.score >= 5) game_finish();
                else { /* 下一题：重新出题并重刷蛋颜色 */
                       color_new_round(false);
                       g_g.sel = 0; }
            } else if (btn == BSP_BTN_OK && g_g.id == 4) {
                g_g.beats++;
                int hit = (g_g.pos >= 90 && g_g.pos <= 130) ? 1 : 0;
                if (hit) { g_g.score++; fishing_audio_play(SFX_CATCH); }
                else fishing_audio_play(SFX_MISS);
                if (g_g.beats >= 8) game_finish();
            }
            break;
        case 2: /* 记忆翻牌 */
            if (!g_g.flash_done) break;
            if (btn == BSP_BTN_UP)   g_g.sel = (g_g.sel + 3) % 4;
            else if (btn == BSP_BTN_DOWN) g_g.sel = (g_g.sel + 1) % 4;
            else if (btn == BSP_BTN_OK) {
                if (g_g.sel == g_g.seq[g_g.input_idx]) {
                    g_g.input_idx++; fishing_audio_play(SFX_CATCH);
                    if (g_g.input_idx >= g_g.seq_len) {
                        g_g.score++; g_g.input_idx = 0; g_g.sub = 0;
                        g_g.flash_done = 0; g_g.timer = 0;
                        for (int i=0;i<g_g.seq_len;++i) g_g.seq[i] = rnd(4);
                        if (g_g.score >= 3) game_finish();
                    }
                } else { fishing_audio_play(SFX_MISS); game_finish(); }
            }
            break;
        case 3: /* 躲猫猫 */
            if (!g_g.flash_done) break;
            if (btn == BSP_BTN_UP)   g_g.sel = (g_g.sel + 2) % 3;
            else if (btn == BSP_BTN_DOWN) g_g.sel = (g_g.sel + 1) % 3;
            else if (btn == BSP_BTN_OK) {
                if (g_g.sel == g_g.target) { g_g.score++; fishing_audio_play(SFX_CATCH); }
                else fishing_audio_play(SFX_MISS);
                if (g_g.score >= 5) game_finish();
                else { g_g.target = rnd(3); g_g.flash_done = 0; g_g.timer = 0; g_g.sub = 0; g_g.sel = 0; }
            }
            break;
    }
}

/* 游戏结束：发贴纸 + 奖励，进 DONE */
static void game_finish(void) {
    g_g.phase = G_DONE;
    if (!g_g.granted) {
        g_g.granted = 1;
        static const unsigned STK[5] = { PST_GAME_CATCH, PST_GAME_COLOR, PST_GAME_MEMORY, PST_GAME_PEEK, PST_GAME_RHYTHM };
        pet_sticker_grant(STK[g_g.id]);
        pet_add_fun(15);
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "得分 %d！按 OK 返回", g_g.score);
    if (g_g_msg) set_text_cached(g_g_msg, buf);
    fishing_audio_play(SFX_CATCH);
}

/* ===================== 刷新：主屏 ===================== */
static void refresh_batt(void) {
    if (s_batt == g_batt_soc) return;
    s_batt = g_batt_soc;
    if (g_batt_soc < 0) {
        lv_label_set_text(g_lbl_batt, "--");
        lv_obj_add_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(g_batt_body, lv_color_make(170,160,150), 0);
        return;
    }
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(g_batt_body, g_batt_soc<20?BAR_R:GRAYTX, 0);
    lv_obj_set_width(g_batt_fill, 1 + g_batt_soc * 9 / 100);
    lv_obj_set_style_bg_color(g_batt_fill, g_batt_soc<20?BAR_R:BAR_G, 0);
    char b[16]; snprintf(b, sizeof(b), "%d", g_batt_soc);
    lv_label_set_text(g_lbl_batt, b);
}

static void bar_color(lv_obj_t *fill, int v) {
    lv_color_t c = (v>=45)?BAR_G:(v>=20)?BAR_O:BAR_R;
    lv_obj_set_style_bg_color(fill, c, 0);
}

static void refresh_home(int now) {
    pet_status_t st; pet_get_status(&st);
    char buf[32];

    refresh_batt();

    if (st.level != s_lvl) { s_lvl = st.level; snprintf(buf, sizeof(buf), "等级 %d", st.level); set_text_cached(g_lbl_level, buf); }
    int mood_id = (int)(size_t)pet_mood_name(&st);
    if (mood_id != s_mood) { s_mood = mood_id; set_text_cached(g_lbl_mood, pet_mood_name(&st)); }

    const int vals[4] = { st.hunger, st.clean, st.fun, st.energy };
    for (int i = 0; i < 4; ++i) {
        if (vals[i] != s_bar[i]) {
            s_bar[i] = vals[i];
            lv_obj_set_size(g_row_fill[i], vals[i] * 120 / 100, 8);
            bar_color(g_row_fill[i], vals[i]);
        }
    }

    if (g_home_sel != s_sel) {
        s_sel = g_home_sel;
        lv_obj_set_pos(g_home_cap, 10, 176 + g_home_sel * 21);
        /* 选中项文字翻白，否则棕色字压在红色胶囊上看不清（模拟器实测发现） */
        for (int i = 0; i < 6; ++i) {
            if (!g_home_lbl[i]) continue;
            lv_obj_set_style_text_color(g_home_lbl[i],
                                        (i == g_home_sel) ? WHITE : BROWN, 0);
        }
    }

    /* 睡觉态：Zzz + 背对世界 + 背光调暗 */
    if (st.sleeping != s_sleep) {
        s_sleep = st.sleeping;
        if (st.sleeping) {
            lv_obj_clear_flag(g_lbl_zzz, LV_OBJ_FLAG_HIDDEN);
            set_text_cached(g_home_hint, "任意键唤醒");
            bsp_display_backlight(15);
            /* 睡觉要脱帽：躺姿头部位置变了，帽子会悬空穿模 */
            if (g_rab_cos) lv_obj_add_flag(g_rab_cos, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_lbl_zzz, LV_OBJ_FLAG_HIDDEN);
            set_text_cached(g_home_hint, "上下选 · OK 做");
            bsp_display_backlight(80);
            costume_overlay_set(g_rab_cos, pet_get_costume(), false);
        }
    }

    /* 兔子动画：漫步 + 呼吸 */
    int dt = now - g_rab_last_ms; if (dt < 0) dt = 0; g_rab_last_ms = now;
    g_rab_x += g_rab_dir * 22 * dt / 1000;
    if (g_rab_x <= 46) { g_rab_x = 46; g_rab_dir = 1; }
    if (g_rab_x >= 122) { g_rab_x = 122; g_rab_dir = -1; }
    g_rab_bob_ms += dt;
    if (g_rab_bob_ms > 500) { g_rab_bob_ms = 0; g_rab_bob_up = !g_rab_bob_up; }
    int by = 26 + (g_rab_bob_up ? 0 : 2);

    /* 决定当前帧 */
    const lv_image_dsc_t *fr = &rabbit_front;
    if (st.sleeping) fr = &rabbit_sleep;
    else if (now < g_action_until) fr = g_action_frame;
    else if (g_rab_dir > 0) fr = &rabbit_front_walk;
    else fr = &rabbit_front_walk_r;

    if (fr != g_rab_frame) {
        g_rab_frame = fr;
        lv_img_set_src(g_rabbit, fr);
    }
    lv_obj_set_pos(g_rabbit, g_rab_x, by);
    lv_obj_set_pos(g_rab_cos, g_rab_x, by);   /* 装扮跟着兔子走 */
    if (st.sleeping) lv_obj_set_pos(g_lbl_zzz, g_rab_x + 58, by + 4);

    /* 反馈清理 */
    if (g_fb_until_ms != 0 && now >= g_fb_until_ms) {
        g_fb_until_ms = 0;
        set_text_cached(g_lbl_fb, "");
        lv_obj_add_flag(g_fb_cap, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 刷新：菜单 ===================== */
static void refresh_menu(void) {
    if (g_menu_sel != s_sel) {
        s_sel = g_menu_sel;
        lv_obj_set_pos(g_menu_cap, 16, 63 + g_menu_sel * 27);
        /* 同主屏：选中项文字翻白，保证红底上可读 */
        for (int i = 0; i < 7; ++i) {
            if (!g_menu_lbl[i]) continue;
            lv_obj_set_style_text_color(g_menu_lbl[i],
                                        (i == g_menu_sel) ? WHITE : BROWN, 0);
        }
    }
}

/* ===================== 刷新：护照 ===================== */
static void refresh_passport(void) {
    char buf[32]; snprintf(buf, sizeof(buf), "贴纸 %d/12", pet_sticker_count());
    set_text_cached(g_pass_count, buf);
    for (int i = 0; i < 12; ++i) {
        bool got = pet_sticker_has((unsigned)i);
        if (got) {
            lv_obj_set_style_bg_color(g_pass_cell[i], gc(i), 0);
            set_text_cached(g_pass_icon[i], sticker_name(i));
            lv_obj_set_style_text_color(g_pass_icon[i], lv_color_make(255,255,255), 0);
        } else {
            lv_obj_set_style_bg_color(g_pass_cell[i], PINK, 0);
            set_text_cached(g_pass_icon[i], "?");
            lv_obj_set_style_text_color(g_pass_icon[i], GRAYTX, 0);
        }
    }
}

/* ===================== 刷新：游戏 ===================== */
static void refresh_game(int now, int dt) {
    (void)now;
    if (g_g.phase != G_PLAY) return;

    switch (g_g.id) {
        case 0: { /* 接胡萝卜 */
            g_g.obj_y += 55 * dt / 1000;
            int pc = g_g.pos + 36, cc = g_g.obj_x + 12;   /* 胡萝卜 24 宽取中心 */
            if (g_g.obj_y >= 244) {
                if (cc >= pc - 28 && cc <= pc + 28) { g_g.score++; fishing_audio_play(SFX_CATCH); }
                g_g.obj_y = 60; g_g.obj_x = 40 + rnd(140);
                if (g_g.score >= 8) game_finish();
            }
            lv_obj_set_pos(g_g_obj[0], g_g.pos, 240);
            lv_obj_set_pos(g_g_obj[1], g_g.obj_x, g_g.obj_y);
            char b[40]; snprintf(b, sizeof(b), "接到 %d/8", g_g.score);
            set_text_cached(g_g_msg, b);
            break;
        }
        case 1: { /* 颜色配对：高亮选中 */
            for (int i = 0; i < 3; ++i)
                lv_obj_set_style_border_width(g_g_obj[1 + i], (i == g_g.sel) ? 4 : 0, 0);
            char b[40]; snprintf(b, sizeof(b), "对 %d/5", g_g.score);
            set_text_cached(g_g_msg, b);
            break;
        }
        case 2: { /* 记忆翻牌 */
            if (!g_g.flash_done) {
                g_g.timer += dt;
                for (int i = 0; i < 4; ++i)
                    lv_obj_set_style_bg_color(g_g_obj[i], lv_color_make(250, 242, 230), 0);
                int cur = g_g.seq[g_g.sub];
                lv_obj_set_style_bg_color(g_g_obj[cur], gc(cur), 0);
                if (g_g.timer > 500) { g_g.timer = 0; g_g.sub++;
                    if (g_g.sub >= g_g.seq_len) { g_g.flash_done = 1; g_g.sel = 0;
                        for (int i=0;i<4;i++) lv_obj_set_style_bg_color(g_g_obj[i], lv_color_make(250, 242, 230),0);
                        if (g_g_msg) set_text_cached(g_g_msg, "照顺序按出来！"); } }
            } else {
                for (int i = 0; i < 4; ++i)
                    lv_obj_set_style_border_width(g_g_obj[i], (i == g_g.sel) ? 4 : 2, 0);
                char b[40]; snprintf(b, sizeof(b), "第 %d 关 对 %d", g_g.score+1, g_g.input_idx);
                set_text_cached(g_g_msg, b);
            }
            break;
        }
        case 3: { /* 躲猫猫 */
            if (!g_g.flash_done) {
                g_g.timer += dt;
                lv_obj_add_flag(g_g_mark, LV_OBJ_FLAG_HIDDEN);
                if (g_g.timer < 900) {
                    lv_obj_clear_flag(g_g_obj[3], LV_OBJ_FLAG_HIDDEN);
                    int bx[3] = { 24, 96, 168 };
                    lv_obj_set_pos(g_g_obj[3], bx[g_g.target], 120);
                } else {
                    lv_obj_add_flag(g_g_obj[3], LV_OBJ_FLAG_HIDDEN);
                    g_g.flash_done = 1; g_g.sel = 0;
                    if (g_g_msg) set_text_cached(g_g_msg, "它藏哪了？");
                }
            } else {
                /* 图片对象不吃 border 样式，改用红色箭头指示选中灌木 */
                int bx[3] = { 24, 96, 168 };
                lv_obj_set_pos(g_g_mark, bx[g_g.sel] + 23, 106);
                lv_obj_clear_flag(g_g_mark, LV_OBJ_FLAG_HIDDEN);
                char b[40]; snprintf(b, sizeof(b), "对 %d/5", g_g.score);
                set_text_cached(g_g_msg, b);
            }
            break;
        }
        case 4: { /* 节奏蹦蹦：音符左右滑向小鼓 */
            g_g.pos += g_g.dir * 130 * dt / 1000;
            if (g_g.pos <= 20) { g_g.pos = 20; g_g.dir = 1; }
            if (g_g.pos >= 210) { g_g.pos = 210; g_g.dir = -1; }
            lv_obj_set_pos(g_g_obj[1], g_g.pos, 162);
            char b[40]; snprintf(b, sizeof(b), "拍 %d/8 中 %d", g_g.beats, g_g.score);
            set_text_cached(g_g_msg, b);
            break;
        }
    }
}

/* ===================== 按键分发 ===================== */
static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    /* LONG 只给小游戏用（长按 OK 退出）；列表页若也收 LONG，
       一次长按会在 PRESS/LONG/CLICK 里连触发多次动作。 */
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_PRESS && ev != BSP_BTN_LONG) return;
    switch (g_scr) {
        case SCR_HOME:
            if (ev != BSP_BTN_LONG) {
                home_input(btn, ev);
            }
            break;
        case SCR_MENU:
            if (ev != BSP_BTN_LONG) {
                menu_input(btn, ev);
            }
            break;
        case SCR_PASSPORT:
            if (ev != BSP_BTN_LONG) {
                passport_input(btn, ev);
            }
            break;
        case SCR_GAME:
            game_input(btn, ev);
            break;
        case SCR_DRESS:
            dress_input(btn, ev);
            break;
    }
}

void pet_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_btn_q) return;
    const btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ===================== 主任务 ===================== */
#define LOOP_MS       20
#define UI_REFRESH_MS 50
#define BATT_MS       500
#define SAVE_MS       30000

static void pet_task(void *arg) {
    (void)arg;
    int last_ui = 0, last_batt = 0, last_save = 0;
    int prev_last = (int)now_ms();

    for (;;) {
        int now = (int)now_ms();
        int dt = now - prev_last; if (dt < 0) dt = 0; prev_last = now;

        pet_event_t ev = pet_tick(now);
        /* 音频不涉及 LVGL，放在锁外播放，避免播放耗时长时间占用显示锁 */
        if (ev == PET_EVT_SLEEP_FULL || ev == PET_EVT_LEVEL_UP) fishing_audio_play(SFX_CATCH);

        if (now - last_batt > BATT_MS) { g_batt_soc = bsp_battery_soc(); last_batt = now; }
        if (now - last_save > SAVE_MS) { nvs_save_now(); last_save = now; }

        /* 注意：按键分发链路（handle_btn → page_input）以及 show_feedback 都会
           调用 lv_obj_* / lv_label_*，LVGL 不是线程安全的，因此必须整套放进
           bsp_lvgl_lock 内执行，不能在锁外摸界面（历史 Bug：UI 任务被冻结）。 */
        const bool due_ui   = (now - last_ui >= UI_REFRESH_MS);
        const bool has_key  = (uxQueueMessagesWaiting(s_btn_q) > 0);
        const bool has_evt  = (ev != PET_EVT_NONE);
        if ((due_ui || has_key || has_evt) && bsp_lvgl_lock(100)) {
            if (due_ui) last_ui = now;

            btn_ev_t e;
            while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);

            if (ev == PET_EVT_SLEEP_FULL) { show_feedback("睡饱啦，精神满满！"); g_action_frame = &rabbit_front; g_action_until = 0; }
            else if (ev == PET_EVT_LEVEL_UP) { show_feedback("照顾升级啦！"); }

            if (g_rebuild) { rebuild_current(); g_rebuild = 0; }

            if (due_ui) {
                if (g_scr == SCR_HOME) refresh_home(now);
                else if (g_scr == SCR_MENU) refresh_menu();
                else if (g_scr == SCR_PASSPORT) refresh_passport();
                else if (g_scr == SCR_GAME) refresh_game(now, dt);
                else if (g_scr == SCR_DRESS) refresh_dress();
            }
            bsp_lvgl_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

/* ===================== 入口 ===================== */
void pet_app_start(void) {
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    pet_init();
    nvs_load_save();
    fishing_audio_init();
    g_rng = (uint32_t)now_ms() ^ 0x9e3779b9u;

    if (bsp_lvgl_lock(1000)) {
        build_home();
        lv_obj_t *scr = lv_screen_active();
        g_layer = lv_obj_create(scr);
        lv_obj_set_size(g_layer, SCR_W, SCR_H);
        lv_obj_set_pos(g_layer, 0, 0);
        lv_obj_set_style_bg_color(g_layer, CREAM, 0);
        lv_obj_set_style_bg_opa(g_layer, LV_OPA_COVER, 0);  /* 纯色底：文字页不铺房间图 */
        lv_obj_set_style_border_width(g_layer, 0, 0);
        lv_obj_set_style_radius(g_layer, 0, 0);
        lv_obj_set_style_pad_all(g_layer, 0, 0);
        lv_obj_clear_flag(g_layer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_add_flag(g_layer, LV_OBJ_FLAG_HIDDEN);

        g_scr = SCR_HOME;
        g_rebuild = 0;
        g_rab_last_ms = (int)now_ms();
        refresh_home((int)now_ms());
        bsp_lvgl_unlock();
    }
    xTaskCreate(pet_task, "pet", 8192, NULL, 5, NULL);
}
