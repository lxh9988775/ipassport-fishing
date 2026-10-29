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
#include "mage_draw.h"
#include "mage_fx.h"
#include "mage_hud.h"
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

/* ===================== 战斗相位枚举（供全部场景/演出函数引用，必须置于文件首位） ===================== */
#define BP_MENU 0
#define BP_MAGIC 1
#define BP_ITEM  2
#define BP_ENEMY 3
#define BP_RESULT 4
#define BP_EVENT 5
#define BP_FAINT 6
#define BP_LOOT 7
#define BP_INTRO 8   /* 遭遇演出：1:1 复刻 HTML walk+meet，进战斗先亮出敌群再开打 */
#define BP_KILL 9   /* 击杀演出延迟：清场后停留 ~0.9s 让伤害飘字/受击特效可见，再进结算 */
#define BTPX 70

/* ===================== 字体 ===================== */
LV_FONT_DECLARE(mage_cjk_16);
LV_FONT_DECLARE(mage_cjk_12);   /* 12px 子集：frameBox 面板正文/行内小字（HTML 8-10px 的等价物） */
static lv_obj_t *use_font12(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &mage_cjk_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    return o;
}
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

/* 城镇簇场景：魔法小镇(CITY)及其所有子面板都铺 town_art 背景（HTML 用 drawTownBg），
 * 仅标题/命名/战斗 用各自背景。这样进入任意板块/功能面板都不会"没背景图"。 */
static bool is_town_scene(int s) {
    switch (s) {
    case SC_CITY: case SC_STATUS: case SC_PARTNERS: case SC_BOOK:
    case SC_ALCHEMY: case SC_BAG: case SC_EQUIP: case SC_ACAD:
    case SC_WILD: case SC_GUILD: case SC_HOME: case SC_FORGE:
    case SC_MARKET: case SC_DEX: case SC_MUPGRADE: case SC_MENU:
    case SC_PROMOTE: case SC_RESEARCH: case SC_CRAFT:
        return true;
    default:
        return false;
    }
}

/* frameBox 金框面板页：HTML 里这些页面全部包在 frameBox(10,10,W-20,H-50,标题) 里。
 * 住宅/锻造房/魔法黑市/晋升仪式 在 HTML 里无金框，保持原样。 */
static bool is_frame_scene(int s) {
    switch (s) {
    case SC_CITY: case SC_STATUS: case SC_PARTNERS: case SC_BOOK: case SC_ALCHEMY:
    case SC_BAG: case SC_EQUIP: case SC_ACAD: case SC_WILD:
    case SC_GUILD: case SC_DEX: case SC_MUPGRADE: case SC_MENU:
    case SC_RESEARCH: case SC_CRAFT: case SC_HOME: case SC_FORGE:
    case SC_MARKET: case SC_PROMOTE:
        return true;
    default:
        return false;
    }
}

/* ===================== 动作枚举 ===================== */
enum { ACT_OK = 0, ACT_NEXT, ACT_PREV, ACT_BACK, ACT_LUP };

/* ===================== 颜色 ===================== */
#define C_GOLD  lv_color_hex(0xffd93d)
#define C_GRAY  lv_color_hex(0x9a93b5)
#define C_WHITE lv_color_hex(0xf2eefb)
#define C_CYAN  lv_color_hex(0x5ec8f2)
#define C_RED   lv_color_hex(0xff6b6b)
#define C_GREEN lv_color_hex(0x7fdc8a)
#define C_DIM   lv_color_hex(0x6b6480)
#define C_SUB   lv_color_hex(0xc9c2e6)   /* 菜单/状态副标题：C_DIM 太暗看不清，改用亮藕荷色 */
#define C_PANEL lv_color_hex(0x0d0b1a)   /* 战斗底部文字面板底色 */
#define C_BG    lv_color_hex(0x0d0b1a)

/* ===================== 全局 ===================== */
static QueueHandle_t s_btn_q = NULL;
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_title_grp = NULL, *g_name_grp = NULL, *g_scene_grp = NULL, *g_hud = NULL;
/* 美术层容器：城镇主视觉（周期刷新时需防重复叠加，故单独持有并在切页时清理） */
static lv_obj_t *g_scene_art = NULL, *g_city_art = NULL, *g_city_dim = NULL;
static bool g_city_art_shown = false;
static lv_obj_t *g_title_main = NULL, *g_title_sub = NULL, *g_title_tag = NULL;
static lv_obj_t *g_menu0 = NULL, *g_menu1 = NULL, *g_title_foot = NULL, *g_title_hint = NULL;
static lv_obj_t *g_name_prompt = NULL, *g_name_val = NULL, *g_name_hint = NULL;
static lv_obj_t *g_scene_title = NULL, *g_scene_body = NULL, *g_scene_hint = NULL, *g_scene_img = NULL;
/* frameBox 金框面板组（HTML frameBox 1:1，仅面板页显示） */
static lv_obj_t *g_frame = NULL;        /* (10,10,220,270) 深底金框 */
static lv_obj_t *g_frame_bar = NULL;    /* (10,9,220,16) 顶部标题条 */
static lv_obj_t *g_frame_title = NULL;  /* 标题文字（左上，金色） */
static lv_obj_t *g_dex_box = NULL;      /* 图鉴立绘盒 (86,36,68,58) */
static lv_obj_t *g_part_card = NULL;    /* 伙伴立绘卡 (68,30,104,96) 描金双框 */

/* 伙伴 4×2 立绘网格（设计感细化：替代旧纯文字列表，参考社区网格布局）。
 * build 期一次性建 8 格（每格=立绘+名+角标），刷新只改 src/text/pos/style；
 * 立绘用烘焙期缩好的 sp_<type>_thumb（运行期禁止 LVGL 缩放）。 */
#define PART_GRID_N 8
static lv_obj_t *g_part_root = NULL;                 /* 网格容器（仅伙伴页显示） */
static lv_obj_t *g_part_cell[PART_GRID_N];           /* 8 个格子容器 */
static lv_obj_t *g_part_img[PART_GRID_N];            /* 每格立绘 lv_img */
static lv_obj_t *g_part_name[PART_GRID_N];           /* 每格名字 */
static lv_obj_t *g_part_tag[PART_GRID_N];            /* 每格角标（资质/等级） */
static lv_obj_t *g_part_sel_tri = NULL;              /* 选中三角（单对象，刷新时移动） */
static lv_obj_t *g_part_info = NULL;                 /* 选中伙伴情报条 */
static int g_part_sig = -1;                            /* 网格脏标：记录(npartners,sel,active,menu) */
/* 长按 OK 弹出的伙伴操作菜单（必须可见，杜绝误删） */
#define PART_MENU_MAX 6   /* 出战/进化/放生/返回城市/关闭菜单 = 最多 5 项，留 1 余量 */
static lv_obj_t *g_part_menu = NULL;                 /* 操作菜单面板（覆盖在网格之上） */
static lv_obj_t *g_part_menu_title = NULL;           /* 菜单标题 */
static lv_obj_t *g_part_menu_opt[PART_MENU_MAX];     /* 选项文本（最多 4 项） */
static lv_obj_t *g_part_menu_tri = NULL;             /* 菜单内选择器三角 */
static lv_obj_t *g_part_conf = NULL;                 /* 放生二次确认面板 */
static lv_obj_t *g_part_conf_txt = NULL;             /* 确认文字（含伙伴名） */
static lv_obj_t *g_part_conf_hint = NULL;            /* 确认操作提示 */

/* 菜单卡片列表（魔法学院/野外试炼/冒险者公会）：每条 = 图标+名称+副文+右标。
 * 替代原先「单一大 label + 16px 中文无断行点冲出金框」的方案（用户反馈超框+无图标）。
 * 改用 12px 卡片式布局，与 HTML 1:1；LVGL 对象 build 期一次性建好，刷新仅改 text/src。 */
#define MENU_ROWS 8
#define ROW_X 8
#define ROW_W 204
#define ROW_H 33   /* 12px字库行高15：name(1..16)+sub(17..32)；旧29两行叠字且sub超框 */
#define ROW_PITCH 34
#define ROW_Y0 27  /* 金框标题条底 y=25 + 2px；8行列表底=298 < 提示行302 */
#define LIST_BOTTOM 294  /* 动态行距下列表底界（提示行302之上留8px） */
#define PITCH_MAX 88     /* 行数少(>=3)时行拉高铺满框，但别夸张成通天大卡 */
typedef struct { const char *icon; const char *name; const char *sub; const char *rgt; } MenuRow;
static lv_obj_t *g_list = NULL;
static lv_obj_t *g_row_box[MENU_ROWS];
static lv_obj_t *g_row_ico[MENU_ROWS];
static lv_obj_t *g_row_name[MENU_ROWS];
static lv_obj_t *g_row_sub[MENU_ROWS];
static lv_obj_t *g_row_rgt[MENU_ROWS];
static lv_obj_t *g_row_tri[MENU_ROWS];   /* 选中三角（替代 ▶ 文字符号，避免字体缺字形豆腐块） */
static int menu_list_dirty = 1;
static int menu_last_key = -1;
/* 渲染数据源（由三个 scene 的刷新分支填充） */
static struct { MenuRow rows[MENU_ROWS]; int n; int sel; } g_menu;
/* 文本缓冲（指向本缓冲，跨调用有效） */
static char g_menu_sub[MENU_ROWS][56];
static char g_menu_rgt[MENU_ROWS][16];
static int g_list_y0 = ROW_Y0;   /* 详情页(图鉴/伙伴/角色)把卡片列表下移到立绘下方 */
/* 城镇地点 / 功能面板的图标映射（每个板块都有专属设计图标） */
static const char *CITY_LOC_ICON[7] = {"sp_icon_book","sp_icon_map","sp_icon_sword",
                                       "sp_icon_bed","sp_icon_hammer","sp_icon_coin","sp_icon_staff"};
static const char *CITY_FUNC_ICON[8] = {"sp_icon_heart","sp_icon_paw","sp_icon_book",
                                         "sp_icon_flask","sp_icon_bag","sp_icon_shield",
                                         "sp_icon_crystal","sp_icon_gear"};

/* 属性元素图标：与 ELEMS[9] 顺序一致（火/水/雷/风/土/冰/光/暗/虚无）。
 * 法术行按 spellSplit 的 e 选对应图标（用户要求"用属性图标"，不再千篇一律书本）。 */
static const char *ELEM_ICON[9] = {
    "sp_icon_fire", "sp_icon_water", "sp_icon_thunder", "sp_icon_wind", "sp_icon_earth",
    "sp_icon_ice", "sp_icon_light", "sp_icon_dark", "sp_icon_void"
};
static void menu_list_render(void);
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
/* 上一次已渲染到 label 的 toast 文本（比对用，避免每 60ms 重复 set_text 触发重排） */
static char g_toast_t[64];
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
/* 显示/隐藏统一入口。
 *
 * 【卡死根因修复，2026-10-13】原先无条件调 lv_obj_add_flag/remove_flag，
 * 而 refresh_ui 每 60ms 跑一次、每次要调用本函数约 10 次。LVGL 每次
 * add_flag/remove_flag 都会做样式重算并把对象所在区域标脏重绘 ——
 * 于是在 240x320 全屏贴图之上每秒产生 160+ 次 invalidate。
 * ESP32-C3 单核 160MHz 扛不住：mage 任务占满 CPU 不放，IDLE 任务被饿死，
 * 5 秒任务看门狗反复触发重启（串口日志：task_wdt: CPU 0: mage）。
 * 症状就是「卡死在主界面，连游戏都进不去」。
 *
 * 修法：记住每个对象上一次的隐藏状态，只在真正翻转时才调 LVGL API。
 * 对象指针即身份，静态表足够（本文件对象总数 < 40）。
 */
#define HIDDEN_TRACK_MAX 64
/* 用非 const 指针：与 lv_obj_t * 直接比较，避免 -Wpointer-sign（工程开 -Werror） */
static struct { lv_obj_t *o; uint8_t hidden; } s_hidden_track[HIDDEN_TRACK_MAX];
static uint8_t s_hidden_cnt = 0;

static void set_hidden(lv_obj_t *o, bool h) {
    if (!o) return;
    /* 查表：若记录的状态与目标一致，说明无需改动 LVGL */
    for (uint8_t i = 0; i < s_hidden_cnt; i++) {
        if (s_hidden_track[i].o == o) {
            if ((s_hidden_track[i].hidden != 0) == h) return;
            s_hidden_track[i].hidden = h ? 1 : 0;
            break;
        }
    }
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);

    /* 新对象：登记一次（登记本身不改 flag，只记状态） */
    bool found = false;
    for (uint8_t i = 0; i < s_hidden_cnt; i++) {
        if (s_hidden_track[i].o == o) { found = true; break; }
    }
    if (!found && s_hidden_cnt < HIDDEN_TRACK_MAX) {
        s_hidden_track[s_hidden_cnt].o = o;
        s_hidden_track[s_hidden_cnt].hidden = h ? 1 : 0;
        s_hidden_cnt++;
    }
}
static int spellModLen(int slot) { return slot == 0 ? 3 : slot == 1 ? 9 : 5; }

/* ===================== 战斗引擎（M3，1:1 复刻 HTML 战斗逻辑）前向声明 ===================== */
static void battle_render(void);
static void battle_handle(int act);
static void battle_tick(void);
static void battle_force_render(void);

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
    /* 标题页主视觉：与 HTML 一致用 title_art 全屏铺底。
     * 必须建在所有文字之后并移到最底层，否则会盖住标题文字。 */
    lv_obj_t *t_bg = NULL, *t_hero = NULL;
    mage_draw_bg_ret(g_title_grp, "title", 0, 0, SCR_W, SCR_H, &t_bg);
    if (t_bg)   { lv_obj_move_foreground(t_bg);   lv_obj_move_to_index(t_bg, 0);   }
    /* 标题页主角：按当前职级显示分级形象（见习学徒→魔法帝），1:1 免缩放 */
    mage_draw_sprite_ret(g_title_grp, mage_hero_sprite_for_rank(g.rank), SCR_W / 2, 268, 36, 1.0f, 0.0f, &t_hero);
    if (t_hero) { lv_obj_move_foreground(t_hero); lv_obj_move_to_index(t_hero, 0); }

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

    /* 美术层：城镇主视觉的容器，创建后即置于最底（文字压在其上） */
    g_scene_art = lv_obj_create(g_scene_grp);
    lv_obj_remove_style_all(g_scene_art);
    lv_obj_set_size(g_scene_art, SCR_W, SCR_H);
    lv_obj_set_pos(g_scene_art, 0, 0);
    lv_obj_set_style_bg_opa(g_scene_art, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_scene_art, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_to_index(g_scene_art, 0);

    /* ---- frameBox 金框面板组（HTML frameBox 1:1 复刻，1d1636 底 + c9a227 框 +
     *      上下 2px 金沿 + 2a2348 标题条）。必须在 g_scene_img 之前创建，
     *      保证立绘压在框上、文字压在立绘上。 ---- */
    {
        /* 主框：box(10,10,W-20,H-50,'#1d1636',PAL.gold2) */
        g_frame = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_frame);
        lv_obj_set_size(g_frame, 220, 270);
        lv_obj_set_pos(g_frame, 10, 10);
        lv_obj_set_style_bg_color(g_frame, lv_color_hex(0x1d1636), 0);
        lv_obj_set_style_bg_opa(g_frame, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g_frame, 1, 0);
        lv_obj_set_style_border_color(g_frame, lv_color_hex(0xc9a227), 0);
        lv_obj_set_style_radius(g_frame, 0, 0);
        lv_obj_remove_flag(g_frame, LV_OBJ_FLAG_SCROLLABLE);
        /* 上下 2px 金沿：HTML px(x,y,w,2,PAL.gold) / px(x,y+h-2,w,2,PAL.gold) */
        static const int strip_y[2] = {0, 268};
        for (int i = 0; i < 2; i++) {
            lv_obj_t *s = lv_obj_create(g_frame);
            lv_obj_remove_style_all(s);
            lv_obj_set_size(s, 220, 2);
            lv_obj_set_pos(s, 0, strip_y[i]);
            lv_obj_set_style_bg_color(s, lv_color_hex(0xffd93d), 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s, 0, 0);
        }
        lv_obj_add_flag(g_frame, LV_OBJ_FLAG_HIDDEN);

        /* 标题条：box(10,9,W-20,16,'#2a2348',PAL.gold2)，标题字 (x+8,y+11) 金色 */
        g_frame_bar = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_frame_bar);
        lv_obj_set_size(g_frame_bar, 220, 16);
        lv_obj_set_pos(g_frame_bar, 10, 9);
        lv_obj_set_style_bg_color(g_frame_bar, lv_color_hex(0x2a2348), 0);
        lv_obj_set_style_bg_opa(g_frame_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g_frame_bar, 1, 0);
        lv_obj_set_style_border_color(g_frame_bar, lv_color_hex(0xc9a227), 0);
        lv_obj_set_style_radius(g_frame_bar, 0, 0);
        lv_obj_remove_flag(g_frame_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_frame_bar, LV_OBJ_FLAG_HIDDEN);

        g_frame_title = lv_label_create(g_scene_grp);
        use_font12(g_frame_title);
        lv_obj_set_style_text_color(g_frame_title, C_GOLD, 0);
        lv_obj_set_width(g_frame_title, 200);
        lv_label_set_text(g_frame_title, "");
        lv_obj_set_pos(g_frame_title, 18, 12);
        lv_obj_add_flag(g_frame_title, LV_OBJ_FLAG_HIDDEN);

        /* 图鉴立绘盒：HTML box(W/2-34,36,68,58,'rgba(20,15,40,.9)',PAL.gold2) */
        g_dex_box = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_dex_box);
        lv_obj_set_size(g_dex_box, 68, 58);
        lv_obj_set_pos(g_dex_box, 86, 36);
        lv_obj_set_style_bg_color(g_dex_box, lv_color_hex(0x140f28), 0);
        lv_obj_set_style_bg_opa(g_dex_box, LV_OPA_90, 0);
        lv_obj_set_style_border_width(g_dex_box, 1, 0);
        lv_obj_set_style_border_color(g_dex_box, lv_color_hex(0xc9a227), 0);
        lv_obj_set_style_radius(g_dex_box, 0, 0);
        lv_obj_remove_flag(g_dex_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_dex_box, LV_OBJ_FLAG_HIDDEN);

        /* 伙伴立绘卡：HTML 双框 box(68,30,104,96,'rgba(30,22,58,.9)',稀有度色)
         *              + 内框 box(71,33,98,90,'rgba(16,11,36,.6)') */
        g_part_card = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_part_card);
        lv_obj_set_size(g_part_card, 104, 96);
        lv_obj_set_pos(g_part_card, 68, 30);
        lv_obj_set_style_bg_color(g_part_card, lv_color_hex(0x1e163a), 0);
        lv_obj_set_style_bg_opa(g_part_card, LV_OPA_90, 0);
        lv_obj_set_style_border_width(g_part_card, 1, 0);
        lv_obj_set_style_border_color(g_part_card, lv_color_hex(0xc9a227), 0);
        lv_obj_set_style_radius(g_part_card, 0, 0);
        lv_obj_remove_flag(g_part_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *pin = lv_obj_create(g_part_card);
        lv_obj_remove_style_all(pin);
        lv_obj_set_size(pin, 98, 90);
        lv_obj_set_pos(pin, 3, 3);
        lv_obj_set_style_bg_color(pin, lv_color_hex(0x100b24), 0);
        lv_obj_set_style_bg_opa(pin, LV_OPA_60, 0);
        lv_obj_set_style_radius(pin, 0, 0);
        lv_obj_add_flag(g_part_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 伙伴 4×2 立绘网格（仅在 SC_PARTNERS 显示，替代旧纯文字列表）----
     * 全对象 build 期一次性创建；刷新只改 src/text/pos/style（防每帧重绘卡死）。 */
    {
        g_part_root = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_part_root);
        lv_obj_set_size(g_part_root, 220, 250);
        lv_obj_set_pos(g_part_root, 10, 26);
        lv_obj_set_style_bg_opa(g_part_root, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(g_part_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_part_root, LV_OBJ_FLAG_HIDDEN);

        const int cw = 54, ch = 100;
        for (int i = 0; i < PART_GRID_N; i++) {
            int col = i % 4, rowi = i / 4;
            lv_obj_t *c = lv_obj_create(g_part_root);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, cw - 4, ch - 4);
            lv_obj_set_pos(c, 2 + col * cw, 2 + rowi * ch);
            lv_obj_set_style_bg_color(c, lv_color_hex(0x231b42), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_set_style_border_color(c, C_GRAY, 0);
            lv_obj_set_style_radius(c, 0, 0);
            lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
            g_part_cell[i] = c;
            /* 立绘（脚底中心锚点，刷新时按实际宽高重定位居中） */
            lv_obj_t *img = lv_img_create(c);
            lv_obj_remove_style_all(img);
            /* 双保险：显式清零背景/边框/内边距，避免透明像素露默认主题白底 */
            lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(img, 0, 0);
            lv_obj_set_style_pad_all(img, 0, 0);
            lv_obj_set_pos(img, 8, 20);
            g_part_img[i] = img;
            lv_obj_t *nm = lv_label_create(c);
            use_cjk(nm);
            lv_obj_set_style_text_color(nm, C_WHITE, 0);
            lv_obj_set_style_text_font(nm, &mage_cjk_12, 0);
            lv_obj_set_width(nm, cw - 4);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(nm, 2, 66);
            g_part_name[i] = nm;
            lv_obj_t *tg = lv_label_create(c);
            use_cjk(tg);
            lv_obj_set_style_text_color(tg, C_GOLD, 0);
            lv_obj_set_style_text_font(tg, &mage_cjk_12, 0);
            lv_obj_set_width(tg, cw - 4);
            lv_label_set_long_mode(tg, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(tg, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(tg, 2, 82);
            g_part_tag[i] = tg;
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
        /* 选中三角（替代 ▶ 豆腐块）：单对象，刷新时移动到选中格 */
        g_part_sel_tri = lv_img_create(g_part_root);
        lv_obj_remove_style_all(g_part_sel_tri);
        lv_obj_set_style_bg_opa(g_part_sel_tri, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_part_sel_tri, 0, 0);
        lv_obj_set_style_pad_all(g_part_sel_tri, 0, 0);
        const lv_image_dsc_t *td = mage_img("sp_tri");
        if (td) lv_img_set_src(g_part_sel_tri, td);
        lv_obj_set_size(g_part_sel_tri, 10, 12);
        lv_obj_add_flag(g_part_sel_tri, LV_OBJ_FLAG_HIDDEN);
        /* 选中伙伴情报条（保留关键可读信息，替代旧密集 IV 列表） */
        g_part_info = lv_label_create(g_scene_grp);
        use_cjk(g_part_info);
        lv_obj_set_style_text_color(g_part_info, C_DIM, 0);
        lv_obj_set_style_text_font(g_part_info, &mage_cjk_12, 0);
        lv_obj_set_style_text_align(g_part_info, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(g_part_info, SCR_W - 16);
        lv_obj_set_pos(g_part_info, 8, 280);
        lv_obj_add_flag(g_part_info, LV_OBJ_FLAG_HIDDEN);
    }
    /* ---- 伙伴操作菜单（短按 OK 弹出，必须可见，杜绝误删）----
     * 以前伙伴页长按 OK 既想开菜单又想退出，造成"退出不了"——长按OK只弹出菜单，
     * 必须再翻到"返回城市"短按OK才能离开，连按只会开关菜单。
     * 现改为：网格态【短按OK=打开此菜单】【长按OK=直接退出到城市】(与其他界面一致)；
     * 菜单首项为"出战"(安全默认，短按OK即设出战，落点再不会误删)；"放生"需二次确认面板才生效。 */
    {
        g_part_menu = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_part_menu);
        lv_obj_set_size(g_part_menu, 200, 160);
        lv_obj_set_pos(g_part_menu, 20, 78);
        lv_obj_set_style_bg_color(g_part_menu, lv_color_hex(0x140d28), 0);
        lv_obj_set_style_bg_opa(g_part_menu, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(g_part_menu, C_GOLD, 0);
        lv_obj_set_style_border_width(g_part_menu, 2, 0);
        lv_obj_remove_flag(g_part_menu, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_part_menu, LV_OBJ_FLAG_HIDDEN);

        g_part_menu_title = lv_label_create(g_part_menu);
        use_cjk(g_part_menu_title);
        lv_obj_set_style_text_color(g_part_menu_title, C_GOLD, 0);
        lv_obj_set_style_text_font(g_part_menu_title, &mage_cjk_12, 0);
        lv_obj_set_width(g_part_menu_title, 200);
        lv_obj_set_style_text_align(g_part_menu_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_part_menu_title, 0, 8);

        for (int i = 0; i < PART_MENU_MAX; i++) {
            g_part_menu_opt[i] = lv_label_create(g_part_menu);
            use_cjk(g_part_menu_opt[i]);
            lv_obj_set_style_text_color(g_part_menu_opt[i], C_WHITE, 0);
            lv_obj_set_style_text_font(g_part_menu_opt[i], &mage_cjk_12, 0);
            lv_obj_set_width(g_part_menu_opt[i], 200);
            lv_obj_set_style_text_align(g_part_menu_opt[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(g_part_menu_opt[i], 0, 32 + i * 26);
            lv_obj_add_flag(g_part_menu_opt[i], LV_OBJ_FLAG_HIDDEN);
        }
        g_part_menu_tri = lv_img_create(g_part_menu);
        lv_obj_remove_style_all(g_part_menu_tri);
        lv_obj_set_style_bg_opa(g_part_menu_tri, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_part_menu_tri, 0, 0);
        lv_obj_set_style_pad_all(g_part_menu_tri, 0, 0);
        const lv_image_dsc_t *mtd = mage_img("sp_tri");
        if (mtd) lv_img_set_src(g_part_menu_tri, mtd);
        lv_obj_set_size(g_part_menu_tri, 10, 12);
        lv_obj_add_flag(g_part_menu_tri, LV_OBJ_FLAG_HIDDEN);

        /* 放生二次确认面板（红框警示，必须显式 OK 才生效） */
        g_part_conf = lv_obj_create(g_scene_grp);
        lv_obj_remove_style_all(g_part_conf);
        lv_obj_set_size(g_part_conf, 200, 92);
        lv_obj_set_pos(g_part_conf, 20, 108);
        lv_obj_set_style_bg_color(g_part_conf, lv_color_hex(0x2a0d18), 0);
        lv_obj_set_style_bg_opa(g_part_conf, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(g_part_conf, lv_color_hex(0xff5555), 0);
        lv_obj_set_style_border_width(g_part_conf, 2, 0);
        lv_obj_remove_flag(g_part_conf, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_part_conf, LV_OBJ_FLAG_HIDDEN);

        g_part_conf_txt = lv_label_create(g_part_conf);
        use_cjk(g_part_conf_txt);
        lv_obj_set_style_text_color(g_part_conf_txt, C_WHITE, 0);
        lv_obj_set_style_text_font(g_part_conf_txt, &mage_cjk_12, 0);
        lv_obj_set_width(g_part_conf_txt, 200);
        lv_obj_set_style_text_align(g_part_conf_txt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_part_conf_txt, 0, 16);

        g_part_conf_hint = lv_label_create(g_part_conf);
        use_cjk(g_part_conf_hint);
        lv_obj_set_style_text_color(g_part_conf_hint, C_SUB, 0);
        lv_obj_set_style_text_font(g_part_conf_hint, &mage_cjk_12, 0);
        lv_obj_set_width(g_part_conf_hint, 200);
        lv_obj_set_style_text_align(g_part_conf_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_part_conf_hint, 0, 58);
    }
    /* 图鉴/伙伴/角色立绘层：按 sp_<type> 原生尺寸展示（位于金框之上、文字之下），
     * 周期刷新用状态记忆（场景+物种键比对）防每帧重设 src 触发重绘。 */
    g_scene_img = lv_image_create(g_scene_grp);
    lv_obj_remove_style_all(g_scene_img);
    /* 双保险：显式清零背景/边框/内边距，避免透明像素露默认主题白底 */
    lv_obj_set_style_bg_opa(g_scene_img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_scene_img, 0, 0);
    lv_obj_set_style_pad_all(g_scene_img, 0, 0);
    lv_obj_set_pos(g_scene_img, 150, 52);
    lv_obj_add_flag(g_scene_img, LV_OBJ_FLAG_HIDDEN);

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

    /* 菜单卡片列表（魔法学院/野外试炼/冒险者公会） */
    g_list = lv_obj_create(g_scene_grp);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_size(g_list, 220, 270);
    lv_obj_set_pos(g_list, 10, 10);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_remove_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MENU_ROWS; i++) {
        lv_obj_t *b = lv_obj_create(g_list);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, ROW_W, ROW_H);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x18122c), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, C_GRAY, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        g_row_box[i] = b;
        /* 选中三角（替代 ▶ 文字符号；置于行首，与预览一致） */
        lv_obj_t *tri = lv_img_create(b);
        lv_obj_remove_style_all(tri);
        lv_obj_set_style_bg_opa(tri, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tri, 0, 0);
        lv_obj_set_style_pad_all(tri, 0, 0);
        const lv_image_dsc_t *td = mage_img("sp_tri");
        if (td) lv_img_set_src(tri, td);
        lv_obj_set_size(tri, 10, 12);
        lv_obj_align(tri, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_flag(tri, LV_OBJ_FLAG_HIDDEN);
        g_row_tri[i] = tri;
        lv_obj_t *ic = lv_img_create(b);
        /* 顺序铁律：remove_style_all 必须在 set_size 之前。
         * lv_obj_set_size 把尺寸写进对象本地样式，remove_style_all 会清空整份
         * 本地样式（含尺寸）——若先 set_size 再 remove，尺寸约束被剥掉，图标按
         * 源图原生像素显示而放大、压住文字（57174b1 引入的回归，已修正）。 */
        lv_obj_remove_style_all(ic);
        lv_obj_set_style_bg_opa(ic, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ic, 0, 0);
        lv_obj_set_style_pad_all(ic, 0, 0);
        lv_obj_set_size(ic, 22, 22);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);
        g_row_ico[i] = ic;
        lv_obj_t *nm = lv_label_create(b);
        use_cjk(nm);
        lv_obj_set_style_text_color(nm, C_WHITE, 0);
        lv_obj_set_style_text_font(nm, &mage_cjk_12, 0);
        lv_obj_set_width(nm, 140);
        lv_obj_set_pos(nm, 46, 1);
        g_row_name[i] = nm;
        lv_obj_t *sb = lv_label_create(b);
        use_cjk(sb);
        lv_obj_set_style_text_color(sb, C_SUB, 0);
        lv_obj_set_style_text_font(sb, &mage_cjk_12, 0);
        lv_obj_set_width(sb, 150);
        lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);   /* 超宽自动省略号，绝不压到右列 */
        lv_obj_set_pos(sb, 46, 17);
        g_row_sub[i] = sb;
        lv_obj_t *rg = lv_label_create(b);
        use_cjk(rg);
        lv_obj_set_style_text_color(rg, C_GOLD, 0);
        lv_obj_set_style_text_font(rg, &mage_cjk_12, 0);
        lv_obj_set_width(rg, 76);   /* 56 会把「见习魔法师」(60px)裁成「凡习魔法师」 */
        lv_label_set_long_mode(rg, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(rg, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(rg, ROW_W - 80, 9);   /* y 由 menu_list_render 按行高居中 */
        g_row_rgt[i] = rg;
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }

    g_scene_hint = lv_label_create(g_scene_grp);
    use_font12(g_scene_hint);   /* HTML 底部操作提示为 8-9px -> 12px 子集 */
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

    /* 战斗演出层：挂在 g_root 下、沉到最底（index 0），保证文字压在其上。
     * 对象池在这里一次性构建完成，之后每帧只改属性。 */
    lv_obj_t *fx_root = lv_obj_create(g_root);
    lv_obj_remove_style_all(fx_root);
    lv_obj_set_size(fx_root, SCR_W, SCR_H);
    lv_obj_set_pos(fx_root, 0, 0);
    lv_obj_set_style_bg_opa(fx_root, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(fx_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_to_index(fx_root, 0);
    mage_fx_attach(fx_root);
    lv_obj_add_flag(fx_root, LV_OBJ_FLAG_HIDDEN);   /* 非战斗时隐藏 */

    /* 战斗 HUD（顶部血条 + 底部动作卡）：1:1 复刻 HTML drawBattleHUD/drawActionCards */
    lv_obj_t *hud_root = lv_obj_create(g_root);
    lv_obj_remove_style_all(hud_root);
    lv_obj_set_size(hud_root, SCR_W, SCR_H);
    lv_obj_set_pos(hud_root, 0, 0);
    lv_obj_set_style_bg_opa(hud_root, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(hud_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_to_index(hud_root, 2);   /* 在 fx 层之上、普通文字之下 */
    mage_hud_attach(hud_root);
    lv_obj_add_flag(hud_root, LV_OBJ_FLAG_HIDDEN);

    /* 把 build_ui 里各处直接 add_flag 设定的初始隐藏状态登记进跟踪表，
     * 否则首次 set_hidden(...,false) 会被误判为「状态没变」而跳过。 */
    /* 注意：这里**不能**用 `static lv_obj_t *arr[] = {g_title_grp, ...}`。
     * g_title_grp 等是运行期赋值的全局变量，C 语言要求静态存储期数组的
     * 初始化器必须是常量表达式，否则报：
     *   error: initializer element is not constant
     * 用局部（自动存储期）数组即可。 */
    s_hidden_cnt = 0;
    lv_obj_t *init_hidden[] = { g_title_grp, g_name_grp, g_hud, g_toast,
                               g_part_root, g_part_info, g_part_menu, g_part_conf };
    for (size_t k = 0; k < sizeof(init_hidden)/sizeof(init_hidden[0]); k++) {
        if (s_hidden_cnt >= HIDDEN_TRACK_MAX) { break; }
        /* 【必须先真的设 flag 再登记】这些对象创建时并没有加 HIDDEN
         * （lv_obj 默认可见），若只登记 hidden=1 而不设 flag，
         * 首次 set_hidden(o,true) 会因「表里已记为隐藏」早退，
         * 对象永远藏不掉 —— 起名界面的文字就这样叠在了战斗画面上。 */
        lv_obj_add_flag(init_hidden[k], LV_OBJ_FLAG_HIDDEN);
        s_hidden_track[s_hidden_cnt].o = init_hidden[k];
        s_hidden_track[s_hidden_cnt].hidden = 1;
        s_hidden_cnt++;
    }
    for (int k = 0; k < 14; k++) {
        if (s_hidden_cnt >= HIDDEN_TRACK_MAX) { break; }
        lv_obj_add_flag(g_stars[k], LV_OBJ_FLAG_HIDDEN);
        s_hidden_track[s_hidden_cnt].o = g_stars[k];
        s_hidden_track[s_hidden_cnt].hidden = 1;
        s_hidden_cnt++;
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

/* 菜单卡片列表渲染：仅当 (场景,选择) 或数据脏标变化时才触碰 LVGL，避免周期重绘卡死。
 * 所有框架页（含详情页）统一走这里；详情页通过 g_list_y0 把列表下移到立绘下方。 */
static void menu_list_render(void) {
    int sel = g_menu.sel;
    int key = ((int)g_scene << 8) | (sel & 0xff);
    if (key == menu_last_key && !menu_list_dirty) return;
    menu_last_key = key;
    menu_list_dirty = 0;

    /* 【动态行距铺满】行数少的页面(学院3行/住宅3行等)把行拉高填满框体，
     * 不再留大片空白（用户铁律：列表必须铺满界面）。
     * pitch = (LIST_BOTTOM - y0) / n，下限 ROW_H(两行行最小 33，字库行高铁律)，
     * 上限 PITCH_MAX；框体高度随内容自适应。
     * frame_h = 末行底边 + 6px 余量：旧写法 fb-10 让金框比末行矮 6px，末行文字压在
     * 框下边被裁（用户问题③），现改为框体包住全部行并有安全余量。 */
    int n = g_menu.n;
    int pitch = ROW_PITCH;
    int rowH = ROW_H;
    int frame_h = 270;
    if (n > 0) {
        pitch = (LIST_BOTTOM - g_list_y0) / n;
        if (pitch > PITCH_MAX) pitch = PITCH_MAX;
        if (pitch < ROW_H) pitch = ROW_H;
        rowH = pitch - 1;
        int last_bottom = g_list_y0 + (n - 1) * pitch + rowH;  /* 末行底边(相对 g_list) */
        int fb = last_bottom + 6;                              /* +6px 安全余量，金框下边不压字 */
        if (fb > 300) fb = 300;
        frame_h = fb;
    }
    /* 脏标守卫内执行，仅场景/选中变化时才碰；同值重复 set 无害 */
    lv_obj_set_height(g_frame, frame_h);
    lv_obj_set_height(g_list, frame_h);

    for (int i = 0; i < MENU_ROWS; i++) {
        if (i < g_menu.n) {
            MenuRow *r = &g_menu.rows[i];
            lv_obj_clear_flag(g_row_box[i], LV_OBJ_FLAG_HIDDEN);
            const lv_image_dsc_t *dsc = mage_img(r->icon);
            if (dsc) { lv_img_set_src(g_row_ico[i], dsc); set_hidden(g_row_ico[i], false); }
            else { set_hidden(g_row_ico[i], true); }   /* 图标缺失时收起，避免出现破图方块 */
            lv_label_set_text(g_row_name[i], r->name);
            lv_label_set_text(g_row_sub[i], r->sub ? r->sub : "");
            lv_label_set_text(g_row_rgt[i], r->rgt ? r->rgt : "");
            /* 副标题宽度约束：有右列内容时收紧到 72px（46→118，给右列留位），
             * 无右列时放宽到整行；超出一律省略号，永不压到右列价格/动作。 */
            bool has_rgt = (r->rgt && r->rgt[0]);
            lv_obj_set_width(g_row_sub[i], has_rgt ? ((ROW_W - 80) - 46 - 6) : (ROW_W - 46 - 6));
            lv_label_set_long_mode(g_row_sub[i], LV_LABEL_LONG_DOT);
            int y = g_list_y0 + i * pitch;
            lv_obj_set_size(g_row_box[i], ROW_W, rowH);
            lv_obj_set_pos(g_row_box[i], ROW_X, y);
            /* 行内垂直居中：两行块(name 15px + 间 1 + sub 15px)=31；单行块=15 */
            const char *sub = r->sub ? r->sub : "";
            int top;
            if (sub[0]) {
                top = (rowH - 31) / 2;
                if (top < 1) top = 1;
                lv_obj_set_pos(g_row_name[i], 46, top);
                lv_obj_set_pos(g_row_sub[i], 46, top + 16);
            } else {
                top = (rowH - 15) / 2;
                if (top < 0) top = 0;
                lv_obj_set_pos(g_row_name[i], 46, top);
                lv_obj_set_pos(g_row_sub[i], 46, top + 16);
            }
            lv_obj_set_pos(g_row_rgt[i], ROW_W - 80, (rowH - 15) / 2);
            /* 图标/三角按新行高重新垂直居中（对齐是一次性设位，行高变了要重排） */
            lv_obj_align(g_row_ico[i], LV_ALIGN_LEFT_MID, 18, 0);
            lv_obj_align(g_row_tri[i], LV_ALIGN_LEFT_MID, 4, 0);
            bool isSel = (i == g_menu.sel);
            lv_obj_set_style_bg_color(g_row_box[i],
                isSel ? lv_color_hex(0x3a2c5c) : lv_color_hex(0x18122c), 0);
            lv_obj_set_style_border_color(g_row_box[i], isSel ? C_GOLD : C_GRAY, 0);
            lv_obj_set_style_border_width(g_row_box[i], isSel ? 2 : 1, 0);
            lv_obj_set_style_text_color(g_row_name[i], C_WHITE, 0);
            lv_obj_set_style_text_color(g_row_sub[i], C_SUB, 0);
            lv_obj_set_style_text_color(g_row_rgt[i], isSel ? C_GOLD : C_DIM, 0);
            set_hidden(g_row_tri[i], !isSel);   /* 选中三角替代 ▶ 豆腐块 */
        } else {
            lv_obj_add_flag(g_row_box[i], LV_OBJ_FLAG_HIDDEN);
            set_hidden(g_row_tri[i], true);
        }
    }
}

/* 计算某伙伴操作菜单选项数：可进化 + 放生 + 返回城市 + 关闭菜单。
 * 与 scene_handle 共用，保证渲染与按键逻辑一致。 */
static int partner_menu_nopts(Partner *p) {
    MobDef *bd = &ENEMY[p->typeIdx];
    int n = 0;
    n += 1;  /* 出战（菜单首项，安全默认，短按OK即设出战） */
    if (bd->hasEvo && !p->evolved && p->level >= bd->evo.lvl) n++;  /* 进化 */
    n += 3;  /* 放生 / 返回城市 / 关闭菜单 */
    return n;
}

/* ===================== 伙伴 4×2 网格渲染 ===================== */
/* 全部对象在 build_ui 一次性建好；本函数只改 src/text/pos/style。
 * 脏标守卫：sig 含 (active,sel,np,菜单开关,菜单选择,确认) 变化才碰 LVGL，
 * 防每 60ms 重绘卡死。菜单/确认面板为独立可见层，杜绝"看不见的菜单误删"。 */
static void partners_grid_render(void) {
    int np = (g.npartners > PART_GRID_N) ? PART_GRID_N : g.npartners;
    int sel = (np > 0) ? clampi(partIdx, 0, np - 1) : 0;
    int sig = (g.activePartner & 0xff)
            | ((sel & 0xf) << 8)
            | ((np & 0xf) << 12)
            | (partMenuOpen ? (1 << 16) : 0)
            | ((partMenu & 0x3) << 17)
            | (partRelConf ? (1 << 19) : 0);
    if (sig == g_part_sig) return;
    g_part_sig = sig;

    /* ---- 放生二次确认面板（红框警示，必须显式 OK 才生效）---- */
    if (partRelConf) {
        set_hidden(g_part_root, true);
        set_hidden(g_part_info, true);
        set_hidden(g_part_menu, true);
        set_hidden(g_part_conf, false);
        lv_obj_move_foreground(g_part_conf);
        char b[48];
        snprintf(b, sizeof(b), "放生 %s？", g.partners[sel].name);
        lv_label_set_text(g_part_conf_txt, b);
        lv_label_set_text(g_part_conf_hint, "OK 确认放生 · 返回 取消");
        return;
    }
    /* ---- 伙伴操作菜单（可见面板）---- */
    if (partMenuOpen) {
        set_hidden(g_part_root, true);
        set_hidden(g_part_info, true);
        set_hidden(g_part_conf, true);
        set_hidden(g_part_menu, false);
        lv_obj_move_foreground(g_part_menu);
        lv_label_set_text(g_part_menu_title, "伙伴菜单");
        Partner *pp = &g.partners[sel];
        MobDef *bb = &ENEMY[pp->typeIdx];
        char opts[PART_MENU_MAX][20];
        int nopts = 0;
        snprintf(opts[nopts], sizeof(opts[nopts]), "出战"); nopts++;   /* 菜单首项：设当前伙伴出战 */
        if (bb->hasEvo && !pp->evolved && pp->level >= bb->evo.lvl) {
            snprintf(opts[nopts], sizeof(opts[nopts]), "进化"); nopts++;
        }
        snprintf(opts[nopts], sizeof(opts[nopts]), "放生");    nopts++;
        snprintf(opts[nopts], sizeof(opts[nopts]), "返回城市"); nopts++;
        snprintf(opts[nopts], sizeof(opts[nopts]), "关闭菜单"); nopts++;
        for (int i = 0; i < PART_MENU_MAX; i++) {
            if (i < nopts) {
                lv_label_set_text(g_part_menu_opt[i], opts[i]);
                lv_obj_set_style_text_color(g_part_menu_opt[i],
                    (i == partMenu) ? C_GOLD : C_WHITE, 0);
                lv_obj_clear_flag(g_part_menu_opt[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_part_menu_opt[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_set_pos(g_part_menu_tri, 26, 32 + partMenu * 26 + 2);
        lv_obj_clear_flag(g_part_menu_tri, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* ---- 正常：4×2 网格 ---- */
    set_hidden(g_part_menu, true);
    set_hidden(g_part_conf, true);
    set_hidden(g_part_root, !(np > 0));
    set_hidden(g_part_info, false);

    if (np == 0) {
        for (int i = 0; i < PART_GRID_N; i++) {
            lv_obj_add_flag(g_part_cell[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_part_img[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(g_part_sel_tri, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_part_info, "还没有伙伴 · 去野外打到虚弱再收服");
        return;
    }

    for (int i = 0; i < PART_GRID_N; i++) {
        bool owned = (i < np);
        if (owned) {
            char pn[40];
            snprintf(pn, sizeof(pn), "sp_%s_thumb", ENEMY[g.partners[i].typeIdx].type);
            const lv_image_dsc_t *d = mage_img(pn);
            if (d) {
                lv_img_set_src(g_part_img[i], d);
                int w = (int)d->header.w, h = (int)d->header.h;
                lv_obj_set_pos(g_part_img[i], (54 - 4 - w) / 2, 60 - h);
                lv_obj_clear_flag(g_part_img[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_part_img[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(g_part_name[i], g.partners[i].name);
            int ivs0 = g.partners[i].ivs[0] < 0 ? 0 : g.partners[i].ivs[0];
            int ivs1 = g.partners[i].ivs[1] < 0 ? 0 : g.partners[i].ivs[1];
            int ivs2 = g.partners[i].ivs[2] < 0 ? 0 : g.partners[i].ivs[2];
            int total = ivs0 + ivs1 + ivs2;
            const char *grade = total >= 40 ? "S" : total >= 33 ? "A" : total >= 26 ? "B" : "C";
            char tg[24];
            snprintf(tg, sizeof(tg), "%s Lv%d", grade, g.partners[i].level);
            lv_label_set_text(g_part_tag[i], tg);
            lv_obj_clear_flag(g_part_cell[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_part_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_part_name[i], "?");
            lv_label_set_text(g_part_tag[i], "未解锁");
            lv_obj_clear_flag(g_part_cell[i], LV_OBJ_FLAG_HIDDEN);
        }
        bool isSel = (i == sel);
        lv_obj_set_style_border_color(g_part_cell[i], isSel ? C_GOLD : C_GRAY, 0);
        lv_obj_set_style_border_width(g_part_cell[i], isSel ? 2 : 1, 0);
        lv_obj_set_style_bg_color(g_part_cell[i],
            isSel ? lv_color_hex(0x3a2c5c) : lv_color_hex(0x231b42), 0);
    }

    /* 选中三角移动到选中格左上 */
    {
        int col = sel % 4, rowi = sel / 4;
        lv_obj_set_pos(g_part_sel_tri, 2 + col * 54 + 4, 2 + rowi * 100 + 5);
        lv_obj_clear_flag(g_part_sel_tri, LV_OBJ_FLAG_HIDDEN);
    }

    /* 选中伙伴情报条（保留关键可读信息，替代旧密集 IV 列表） */
    {
        Partner *p = &g.partners[sel];
        int ivs0 = p->ivs[0] < 0 ? 0 : p->ivs[0];
        int ivs1 = p->ivs[1] < 0 ? 0 : p->ivs[1];
        int ivs2 = p->ivs[2] < 0 ? 0 : p->ivs[2];
        int total = ivs0 + ivs1 + ivs2;
        const char *grade = total >= 40 ? "S" : total >= 33 ? "A" : total >= 26 ? "B" : "C";
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s 资质%s(%d/45) · Lv%d%s",
            p->name, g.activePartner == sel ? " ★" : "",
            grade, total, p->level, g.activePartner == sel ? " · 出战" : "");
        lv_label_set_text(g_part_info, buf);
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

    /* 离开城镇页时销毁主视觉容器，下次进入重建。
     * 注意：必须把 g_scene_art 本身置 NULL（它已被 lv_obj_clean 掏空，
     * 不置 NULL 会让下一行 g_scene_grp 上残留一个游离容器，后续往里塞的
     * 城镇贴图会变成不受控的游离对象，永久盖在战斗画面上 —— 这就是
     * 「进战斗显示的却是城镇背景」的根因）。 */
    if (!is_town_scene(g_scene) && g_city_art_shown) {
        if (g_scene_art) {
            lv_obj_delete(g_scene_art);
            g_scene_art = NULL;
        }
        g_city_art = NULL;
        g_city_dim = NULL;
        g_city_art_shown = false;
    }

    if (title) {
        /* 星星闪烁动画已停用：在全屏贴图之上做透明度动画会每帧触发全屏重绘，
         * ESP32-C3 单核扛不住 -> 整屏闪烁。星点保留为静态装饰（HTML 的呼吸感
         * 在 240x320 + 单核上代价太大，视觉收益不值这个闪屏代价）。 */
        static bool star_init = false;
        if (!star_init) {
            star_init = true;
            for (int i = 0; i < 14; i++) {
                lv_obj_set_style_bg_opa(g_stars[i], (i % 3 == 0) ? LV_OPA_60 : LV_OPA_20, 0);
            }
        }
        lv_obj_set_style_text_color(g_menu0, g_titleSel == 0 ? C_GOLD : C_GRAY, 0);
        lv_obj_set_style_text_color(g_menu1, g_titleSel == 1 ? C_GOLD : C_GRAY, 0);
        lv_label_set_text(g_menu0, g_titleSel == 0 ? "继续冒险" : "  继续冒险");
        lv_label_set_text(g_menu1, g_titleSel == 1 ? "新的旅程" : "    新的旅程");
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

    /* 城镇簇：小镇背景常驻（HTML 所有子面板都 drawTownBg）。
     * 只在离开城镇簇（进战斗/标题）时才销毁；切到任意子面板都保留并只更新暗度。 */
    if (is_town_scene(g_scene)) {
        if (!g_city_art) {
            /* g_scene_art 可能已在离开城镇时被删除（可能是 NULL）。
             * 直接对它 lv_obj_clean 会空指针崩溃 -> 必须先判空并重建容器。 */
            if (!g_scene_art) {
                g_scene_art = lv_obj_create(g_scene_grp);
                lv_obj_remove_style_all(g_scene_art);
                lv_obj_set_size(g_scene_art, SCR_W, SCR_H);
                lv_obj_set_pos(g_scene_art, 0, 0);
                lv_obj_set_style_bg_opa(g_scene_art, LV_OPA_TRANSP, 0);
                lv_obj_set_scrollbar_mode(g_scene_art, LV_SCROLLBAR_MODE_OFF);
            } else {
                lv_obj_clean(g_scene_art);
            }
            /* 重建后必须沉到最底：g_scene_art 是新创建的子对象，会浮到
             * g_scene_grp 最上层，盖住 g_scene_title/body/hint 文字 ->
             * 「回城文字消失」（问题④）。沉底后文字压在城镇主视觉之上。 */
            lv_obj_move_to_index(g_scene_art, 0);
            g_city_art = lv_obj_create(g_scene_art);
            lv_obj_remove_style_all(g_city_art);
            lv_obj_set_size(g_city_art, SCR_W, SCR_H);
            lv_obj_set_pos(g_city_art, 0, 0);
            lv_obj_set_style_bg_opa(g_city_art, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(g_city_art, 0, 0);
            lv_obj_remove_flag(g_city_art, LV_OBJ_FLAG_SCROLLABLE);
            mage_draw_bg(g_city_art, "town", 0, 0, SCR_W, SCR_H);
            /* 压暗层：图标/文字压在其上，保证可读性（HTML rgba(8,6,16,.6~.72)）。
             * 放在图片之上、文字标签之下（同属 g_scene_art 容器，文字在父容器 g_scene_grp）。 */
            g_city_dim = lv_obj_create(g_scene_art);
            lv_obj_remove_style_all(g_city_dim);
            lv_obj_set_size(g_city_dim, SCR_W, SCR_H);
            lv_obj_set_pos(g_city_dim, 0, 0);
            lv_obj_set_style_bg_color(g_city_dim, C_BG, 0);
            lv_obj_set_style_border_width(g_city_dim, 0, 0);
            lv_obj_remove_flag(g_city_dim, LV_OBJ_FLAG_SCROLLABLE);
            g_city_art_shown = true;
        }
        /* 暗度按场景记忆：CITY 地图/功能栏稍亮(.60)，子面板更暗(.70) */
        static lv_opa_t s_dim = 0;
        lv_opa_t want = (g_scene == SC_CITY) ? LV_OPA_60 : LV_OPA_70;
        if (s_dim != want) { s_dim = want; lv_obj_set_style_bg_opa(g_city_dim, want, 0); }
    }

    switch (g_scene) {
    case SC_CITY: {
        s_ttl[0] = '\0';
        if (g.cityMode == 0) {
            for (int i = 0; i < 7; i++)
                g_menu.rows[i] = (MenuRow){ CITY_LOC_ICON[i], LOCATIONS[i].n, LOCATIONS[i].d,
                                           i == locSel ? "" : "" };
            g_menu.n = 7; g_menu.sel = locSel;
            lv_label_set_text(g_scene_hint, "↑↓ 选地点 · OK 进入 · 长按 功能栏");
        } else {
            for (int i = 0; i < 8; i++)
                g_menu.rows[i] = (MenuRow){ CITY_FUNC_ICON[i], FUNCS[i].n, "",
                                           i == funcSel ? "" : "" };
            g_menu.n = 8; g_menu.sel = funcSel;
            lv_label_set_text(g_scene_hint, "↑↓ 选功能 · OK 打开 · 长按 回地图");
        }
        menu_list_dirty = 1;
        break;
    }
    case SC_STATUS: {
        int atk = 7 + g.level * 2 + g.rank * 3 + g.atkBonus + eqAtk();
        g_menu.n = 0;
        snprintf(g_menu_sub[0], sizeof(g_menu_sub[0]), "%d / %d", g.hp, g.maxHp);
        g_menu.rows[0] = (MenuRow){"sp_icon_heart", "生命", g_menu_sub[0], ""};
        snprintf(g_menu_sub[1], sizeof(g_menu_sub[1]), "%d / %d", g.mp, g.maxMp);
        g_menu.rows[1] = (MenuRow){"sp_icon_bolt", "魔力", g_menu_sub[1], ""};
        /* 职级·Lv 合并进等级行 sub（HTML 原版同款：职级是独立信息行，不占右列，
         * 否则长 sub「Lv7 · 经验210/320」会和右列「见习魔法师」互相压字） */
        snprintf(g_menu_sub[2], sizeof(g_menu_sub[2]), "%s · Lv%d", RANKS[g.rank], g.level);
        g_menu.rows[2] = (MenuRow){"sp_icon_star", "等级", g_menu_sub[2], ""};
        snprintf(g_menu_sub[3], sizeof(g_menu_sub[3]), "%d / %d", g.exp, expNeed());
        g_menu.rows[3] = (MenuRow){"sp_icon_scroll", "经验", g_menu_sub[3], ""};
        snprintf(g_menu_sub[4], sizeof(g_menu_sub[4]), "%d（装+%d）", atk, eqAtk());
        g_menu.rows[4] = (MenuRow){"sp_icon_sword", "攻击", g_menu_sub[4], ""};
        snprintf(g_menu_sub[5], sizeof(g_menu_sub[5]), "减伤 %d", eqDef());
        g_menu.rows[5] = (MenuRow){"sp_icon_shield", "护甲", g_menu_sub[5], ""};
        snprintf(g_menu_sub[6], sizeof(g_menu_sub[6]), "%d · 击杀%d", g.gold, g.kills);
        g_menu.rows[6] = (MenuRow){"sp_icon_coin", "金币", g_menu_sub[6], ""};
        g_menu.n = 7; g_menu.sel = 0;
        lv_label_set_text_fmt(g_scene_hint, "伙伴 %d 只 · OK/长按 回城 · ↓ 管理装备", g.cats);
        menu_list_dirty = 1;
        break;
    }
    case SC_BAG: {
        int nP = 0, nB = 0;
        for (int id = 0; id < 20; id++) if (g.pbag[id] > 0) nP++;
        for (int id = 0; id < 40; id++) if (g.mbag[id] > 0) nB++;
        g_menu.n = 0;
        #define BAGROW(ic,nm,val) do{ if(g_menu.n<MENU_ROWS){ \
            snprintf(g_menu_rgt[g_menu.n],sizeof(g_menu_rgt[g_menu.n]),"×%d",val); \
            g_menu.rows[g_menu.n]=(MenuRow){ic,nm,"",g_menu_rgt[g_menu.n]}; g_menu.n++; } }while(0)
        BAGROW("sp_icon_potion","体力药水", g.potions);
        BAGROW("sp_icon_herb","月光草", g.herbs);
        BAGROW("sp_icon_bone","兽骨", g.mats);
        BAGROW("sp_icon_crystal","魔晶", g.crystals);
        BAGROW("sp_icon_orb","捕捉球", g.orbs);
        BAGROW("sp_icon_seal","封印卡", g.seals);
        int stones = 0; for (int i = 1; i <= 4; i++) stones += stoneN(i);
        BAGROW("sp_icon_star","魔石", stones);
        #undef BAGROW
        g_menu.sel = 0;
        snprintf(g_menu_sub[0], sizeof(g_menu_sub[0]), "已收集 药%d/20 材%d/40", nP, nB);
        lv_label_set_text(g_scene_hint, g_menu_sub[0]);
        menu_list_dirty = 1;
        break;
    }
    case SC_DEX: {
        int seen = 0, caught = 0;
        for (int i = 0; i < DEXORDER_COUNT; i++) { if (g.dexSeen[i]) seen++; if (g.dexCaught[i]) caught++; }
        dexIdx = clampi(dexIdx, 0, DEXORDER_COUNT - 1);
        g_menu.n = 0;
        snprintf(g_menu_sub[0], sizeof(g_menu_sub[0]), "已收录 %d/%d · 收服%d", seen, DEXORDER_COUNT, caught);
        g_menu.rows[0] = (MenuRow){"sp_icon_book", "收录进度", g_menu_sub[0], ""};
        int t = enemyIndex(DEXORDER[dexIdx]);
        MobDef *bd = &ENEMY[t];
        int seenF = g.dexSeen[dexIdx];
        snprintf(g_menu_sub[1], sizeof(g_menu_sub[1]), "%s", seenF ? bd->n : "？？？（未发现）");
        g_menu.rows[1] = (MenuRow){"sp_icon_staff", "当前", g_menu_sub[1],
                                    seenF && g.dexCaught[dexIdx] ? "已收服" : ""};
        if (seenF) {
            snprintf(g_menu_sub[2], sizeof(g_menu_sub[2]), "%s", speciesZones(dexIdx));
            g_menu.rows[2] = (MenuRow){"sp_icon_map", "出没", g_menu_sub[2], ""};
            char sk[40]; sk[0] = 0;
            for (int i = 0; i < bd->nSkills; i++) {
                int l = (int)strlen(sk); if (l) sk[l++] = ' ';
                strncat(sk, SKILLS[skillIndex(bd->skills[i])].n, sizeof(sk) - l - 1);
            }
            snprintf(g_menu_sub[3], sizeof(g_menu_sub[3]), "%s", sk);
            g_menu.rows[3] = (MenuRow){"sp_icon_star", "天赋", g_menu_sub[3], ""};
            char cap[40]; cap[0] = 0;
            if (bd->capturable) {
                const char *tool = bd->cap.tool && strcmp(bd->cap.tool, "seal") == 0 ? "封印卡" : "捕捉球";
                if (bd->hasCap && bd->cap.hp > 0) snprintf(cap, sizeof(cap), "用%s(血<%d%%)", tool, (int)(bd->cap.hp * 100));
                else snprintf(cap, sizeof(cap), "用%s", tool);
            } else snprintf(cap, sizeof(cap), "不可捕捉");
            g_menu.rows[4] = (MenuRow){"sp_icon_paw", "捕捉", cap, ""};
        }
        g_menu.n = seenF ? 5 : 2;
        g_menu.sel = 0;
        lv_label_set_text(g_scene_hint, "OK 捕捉条件 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_BOOK: {
        int n = discoveredCount();
        int total = n + 1;  /* +1 = 研究新魔法 */
        bookIdx = clampi(bookIdx, 0, total - 1);
        g_menu.n = 0;
        int win = 6, start = clampi(bookIdx - 2, 0, total - win);
        if (start < 0) start = 0;
        for (int i = start; i < start + win && i < total && g_menu.n < MENU_ROWS; i++) {
            if (i < n) {
                int k = discoveredKeyAt(i);
                int a, e, m; spell_split(k, &a, &e, &m);
                int inlo = 0; for (int j = 0; j < g.nload; j++) if (g.loadout[j] == k) inlo = 1;
                snprintf(g_menu_sub[g_menu.n], sizeof(g_menu_sub[g_menu.n]), "Lv%d 威力%d", slv(k), spellPower(k));
                g_menu.rows[g_menu.n] = (MenuRow){ELEM_ICON[e], spellName(k), g_menu_sub[g_menu.n], inlo ? "★出战" : ""};
            } else {
                g_menu.rows[g_menu.n] = (MenuRow){"sp_icon_staff", "研究新魔法", "三槽自由拼装独门魔法", ""};
            }
            g_menu.n++;
        }
        g_menu.sel = clampi(bookIdx - start, 0, g_menu.n - 1);
        char hb[64];
        if (bookIdx < n) {
            int k = discoveredKeyAt(bookIdx);
            int a, e, m; spell_split(k, &a, &e, &m);
            int inlo = 0; for (int j = 0; j < g.nload; j++) if (g.loadout[j] == k) inlo = 1;
            snprintf(hb, sizeof(hb), "%s · %s魔法 Lv%d 威力%d 耗%d%s",
                     spellName(k), TIER_NAMES[m], slv(k), spellPower(k), spellCost(k), inlo ? " ★出战" : "");
        } else {
            snprintf(hb, sizeof(hb), "三槽自由拼装 · OK 加入/移出 · ↓ 切换");
        }
        lv_label_set_text(g_scene_hint, hb);
        menu_list_dirty = 1;
        break;
    }
    case SC_PARTNERS: {
        if (g.npartners == 0) {
            g_menu.n = 0;
            lv_label_set_text(g_scene_hint, "长按 返回");
            break;
        }
        partIdx = clampi(partIdx, 0, (g.npartners > PART_GRID_N ? PART_GRID_N - 1 : g.npartners - 1));
        g_menu.n = 0;   /* 伙伴页改用 4×2 立绘网格（g_part_root），不再用文字列表 */
        lv_label_set_text(g_scene_hint,
            partRelConf ? "OK 确认放生 · 返回 取消"
            : partMenuOpen ? "↑↓ 选择 · OK 确定 · 长按OK 关闭"
            : "OK 出战 · ↓ 切换 · 长按 菜单");
        break;
    }
    case SC_MENU: {
        menuIdx = clampi(menuIdx, 0, 6);
        const char *micon[7] = {"sp_icon_bolt","sp_icon_paw","sp_icon_star",
                                "sp_icon_crown","sp_icon_scroll","sp_icon_scroll","sp_icon_gear"};
        const char *mname[7] = {"自动战斗","自动捕捉","音效","难度","导出存档","导入存档","重新开始"};
        g_menu.n = 7; g_menu.sel = menuIdx;
        for (int i = 0; i < 7; i++) {
            const char *val = "";
            if (i == 0) val = g.autoMode ? "开" : "关";
            else if (i == 1) val = g.autoCapture ? "开" : "关";
            else if (i == 2) val = g.sound ? "开" : "关";
            else if (i == 3) val = diffC()->n;
            g_menu.rows[i] = (MenuRow){micon[i], mname[i], val, i == menuIdx ? "" : ""};
        }
        lv_label_set_text(g_scene_hint, "OK 执行 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_HOME: {
        homeSel = clampi(homeSel, 0, 2);
        g_menu.n = 3; g_menu.sel = homeSel;
        g_menu.rows[0] = (MenuRow){"sp_icon_bed",   "休息",     "恢复全部状态",       homeSel==0?"":""};
        g_menu.rows[1] = (MenuRow){"sp_icon_flask", "炼金台",   "炼制药剂与捕捉球",   homeSel==1?"":""};
        g_menu.rows[2] = (MenuRow){"sp_icon_book",  "魔法研究", "消耗材料升魔法等级", homeSel==2?"":""};
        lv_label_set_text(g_scene_hint, "OK 选择 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_RESEARCH: {
        int n = discoveredCount();
        g_menu.n = 0;
        if (n == 0) {
            g_menu.rows[0] = (MenuRow){"sp_icon_book", "魔法书是空的", "去战斗或炼金习得魔法", ""};
            g_menu.n = 1; g_menu.sel = 0;
        } else {
            resIdx = clampi(resIdx, 0, n - 1);
            int win = 6, start = clampi(resIdx - 2, 0, n - win);
            if (start < 0) start = 0;
            for (int i = start; i < start + win && i < n && g_menu.n < MENU_ROWS; i++) {
                int k = discoveredKeyAt(i);
                int a, e, m; spell_split(k, &a, &e, &m);
                int lv = slv(k);
                if (i == resIdx) {
                    int rate = researchRateFor(k);
                    snprintf(g_menu_sub[g_menu.n], sizeof(g_menu_sub[g_menu.n]),
                             "%s Lv%d→%d 成%d%%", TIER_NAMES[m], lv, lv + 1, rate);
                    g_menu.rows[g_menu.n] = (MenuRow){ELEM_ICON[e], spellName(k),
                                                     g_menu_sub[g_menu.n], "OK研究"};
                } else {
                    snprintf(g_menu_sub[g_menu.n], sizeof(g_menu_sub[g_menu.n]),
                             "%s Lv%d", TIER_NAMES[m], lv);
                    g_menu.rows[g_menu.n] = (MenuRow){ELEM_ICON[e], spellName(k),
                                                     g_menu_sub[g_menu.n], ""};
                }
                g_menu.n++;
            }
            g_menu.sel = clampi(resIdx - start, 0, g_menu.n - 1);
        }
        lv_label_set_text(g_scene_hint, "OK 研究 · ↓ 切换 · 长按 返回住宅");
        menu_list_dirty = 1;
        break;
    }
    case SC_FORGE: {
        forgeSel = clampi(forgeSel, 0, 1);
        g_menu.n = 2; g_menu.sel = forgeSel;
        int lw = g.weaponLv, la = g.armorLv;
        snprintf(g_menu_sub[0], sizeof(g_menu_sub[0]), "%d级 · %d金+兽骨x%d", lw + 1, 30 * (lw + 1), 2 * (lw + 1));
        snprintf(g_menu_sub[1], sizeof(g_menu_sub[1]), "%d级 · %d金+兽骨x%d", la + 1, 30 * (la + 1), 2 * (la + 1));
        g_menu.rows[0] = (MenuRow){"sp_icon_sword", "锻造武器", g_menu_sub[0], "攻击+2"};
        g_menu.rows[1] = (MenuRow){"sp_icon_robe",  "锻造法袍", g_menu_sub[1], "生命+8"};
        lv_label_set_text(g_scene_hint, "OK 锻造 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_MARKET: {
        marketIdx = clampi(marketIdx, 0, 4);
        const char *mkicon[5] = {"sp_icon_herb","sp_icon_bone","sp_icon_crystal",
                                 "sp_icon_seal","sp_icon_staff"};
        g_menu.n = 5; g_menu.sel = marketIdx;
        for (int i = 0; i < 5; i++) {
            int hold = 0;
            switch (MARKET[i].holdField) {
                case 0: hold = g.herbs; break;
                case 1: hold = g.mats; break;
                case 2: hold = g.crystals; break;
                case 3: hold = g.seals; break;
                default: hold = -1; break;
            }
            if (hold >= 0)
                snprintf(g_menu_sub[i], sizeof(g_menu_sub[i]), "持有 %d · %d金", hold, MARKET[i].price);
            else
                snprintf(g_menu_sub[i], sizeof(g_menu_sub[i]), "随机魔法 · %d金", MARKET[i].price);
            g_menu.rows[i] = (MenuRow){mkicon[i], MARKET[i].n, g_menu_sub[i],
                                       i == marketIdx ? "" : ""};
        }
        lv_label_set_text(g_scene_hint, "OK 购买 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_ALCHEMY: {
        alcIdx = clampi(alcIdx, 0, 6);
        g_menu.n = 7; g_menu.sel = alcIdx;
        for (int i = 0; i < 7; i++) {
            RecipeDef *r = &RECIPES[i];
            const char *ic = "sp_icon_flask";
            if (r->makeStone > 0) ic = "sp_icon_crystal";
            else if (!strcmp(r->outName, "potions")) ic = "sp_icon_potion";
            else if (!strcmp(r->outName, "orbs")) ic = "sp_icon_orb";
            else if (!strcmp(r->outName, "seals")) ic = "sp_icon_seal";
            char cost[40]; cost[0] = 0;
            if (r->cH) { int l = (int)strlen(cost); snprintf(cost + l, sizeof(cost) - l, "草%d ", r->cH); }
            if (r->cM) { int l = (int)strlen(cost); snprintf(cost + l, sizeof(cost) - l, "骨%d ", r->cM); }
            if (r->cC) { int l = (int)strlen(cost); snprintf(cost + l, sizeof(cost) - l, "晶%d", r->cC); }
            if (r->makeStone) { int l = (int)strlen(cost); snprintf(cost + l, sizeof(cost) - l, "→%s", STONE_NAMES[r->makeStone]); }
            snprintf(g_menu_sub[i], sizeof(g_menu_sub[i]), "%s", cost);
            g_menu.rows[i] = (MenuRow){ic, r->n, g_menu_sub[i], "炼制"};
        }
        lv_label_set_text(g_scene_hint, "OK 炼制 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_ACAD: {
        acadIdx = clampi(acadIdx, 0, 2);
        int tc = 20 + g.studyCnt * 15;
        /* 价格按 HTML 原版并入名字（'魔力修行 · 20金'），不另占右列——
         * 否则右列价格会与下方副标题互相压字（设备实测重叠）。 */
        char nm0[32], nm1[32];
        snprintf(nm0, sizeof(nm0), "魔力修行 · %d金", tc);
        snprintf(nm1, sizeof(nm1), "学习属性魔法 · 45金");
        snprintf(g_menu_sub[0], sizeof(g_menu_sub[0]), "魔力上限 +5（当前 %d）", g.maxMp);
        snprintf(g_menu_sub[1], sizeof(g_menu_sub[1]), "已学 %d/135 种魔法", discoveredCount());
        snprintf(g_menu_sub[2], sizeof(g_menu_sub[2]), "%s",
                 promoteReady() ? "已达考核标准" : "需击杀/委托达标");
        g_menu.n = 3; g_menu.sel = acadIdx;
        g_menu.rows[0] = (MenuRow){"sp_icon_orb", nm0, g_menu_sub[0], ""};
        g_menu.rows[1] = (MenuRow){"sp_icon_book", nm1, g_menu_sub[1], ""};
        g_menu.rows[2] = (MenuRow){"sp_icon_crown", "职业晋升考核", g_menu_sub[2], ""};
        lv_label_set_text(g_scene_hint, "OK 确定 · ↓ 切换 · 长按 返回");
        s_b[0] = '\0';
        menu_list_dirty = 1;
        break;
    }
    case SC_PROMOTE: {
        g_menu.n = 2; g_menu.sel = 0;
        g_menu.rows[0] = (MenuRow){"sp_icon_crown", "当前职级", RANKS[g.rank], ""};
        g_menu.rows[1] = (MenuRow){"sp_icon_star",  "晋升为",
            g.rank < 6 ? RANKS[g.rank + 1] : "魔法帝！", "OK接受"};
        lv_label_set_text(g_scene_hint, "OK 接受晋升 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_CRAFT: {
        craftSlot = clampi(craftSlot, 0, 2);
        int key = spell_key(g.spell[0], g.spell[1], g.spell[2]);
        const char *cic[3] = {"sp_icon_staff","sp_icon_bolt","sp_icon_crystal"};
        g_menu.n = 4;
        g_menu.rows[0] = (MenuRow){cic[0], "形态", FORMS[g.spell[0]].n,  "OK切"};
        g_menu.rows[1] = (MenuRow){cic[1], "元素", ELEMS[g.spell[1]].n,  "OK切"};
        g_menu.rows[2] = (MenuRow){cic[2], "修饰", MODS[g.spell[2]].n,   "OK切"};
        snprintf(g_menu_sub[3], sizeof(g_menu_sub[3]), "威力%d 耗%d", spellPower(key), spellCost(key));
        { int a, e, m; spell_split(key, &a, &e, &m);
        g_menu.rows[3] = (MenuRow){ELEM_ICON[e], spellName(key), g_menu_sub[3], "预览"}; }
        g_menu.sel = craftSlot;
        lv_label_set_text(g_scene_hint, "OK 切槽 · ↓ 换项 · 长按 装配");
        menu_list_dirty = 1;
        break;
    }
    case SC_EQUIP: {
        g_menu.n = 0;
        /* 已装备（展示用，不可选中） */
        for (int s = 0; s < 3 && g_menu.n < MENU_ROWS; s++) {
            EquipItem *it = &g.equip[s];
            if (it->rar < 0) continue;
            const char *ic = (s == 0) ? "sp_icon_sword" : (s == 1) ? "sp_icon_robe" : "sp_icon_staff";
            g_menu.rows[g_menu.n] = (MenuRow){ic, it->name, "已装备", ""};
            g_menu.n++;
        }
        int eqBase = g_menu.n;
        if (g.nebag == 0 && eqBase == 0) {
            g_menu.rows[0] = (MenuRow){"sp_icon_gear", "装备背包是空的", "去锻造/战斗获取", ""};
            g_menu.n = 1; g_menu.sel = 0;
        } else {
            eqSel = clampi(eqSel, 0, g.nebag - 1);
            int maxBag = MENU_ROWS - eqBase;
            int win = (maxBag > 6) ? 6 : (maxBag < 1 ? 1 : maxBag);
            int start = clampi(eqSel - 2, 0, g.nebag - win);
            if (start < 0) start = 0;
            for (int i = start; i < start + win && i < g.nebag && g_menu.n < MENU_ROWS; i++) {
                EbagItem *it = &g.ebag[i];
                char stat[20];
                if (it->atk) snprintf(stat, sizeof(stat), "攻击+%d", it->atk);
                else if (it->def) snprintf(stat, sizeof(stat), "减伤%d", it->def);
                else if (it->pow > 0) snprintf(stat, sizeof(stat), "法强+%d%%", (int)((it->pow - 1) * 100));
                else stat[0] = 0;
                const char *ic = (it->slot == 'w') ? "sp_icon_sword" : (it->slot == 'a') ? "sp_icon_robe" : "sp_icon_staff";
                snprintf(g_menu_sub[g_menu.n], sizeof(g_menu_sub[g_menu.n]), "%s", stat);
                g_menu.rows[g_menu.n] = (MenuRow){ic, it->name, g_menu_sub[g_menu.n], i == eqSel ? "" : ""};
                g_menu.n++;
            }
            g_menu.sel = eqBase + clampi(eqSel - start, 0, g.nebag - start - 1);
            if (g_menu.sel >= g_menu.n) g_menu.sel = g_menu.n - 1;
        }
        lv_label_set_text(g_scene_hint, "OK 装备/卸下 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    case SC_GUILD: {
        guildIdx = clampi(guildIdx, 0, 3);
        g_menu.n = 4; g_menu.sel = guildIdx;
        for (int i = 0; i < 4; i++) {
            QuestDef *q = &QUESTS[i];
            int lock = g.arank < q->rank;
            int acc = g.accepted == i;
            int prog = questProg(q);
            snprintf(g_menu_sub[i], sizeof(g_menu_sub[i]), "%s · %d金", q->sub, q->gold);
            if (lock) snprintf(g_menu_rgt[i], sizeof(g_menu_rgt[i]), "未解锁");
            else if (acc) snprintf(g_menu_rgt[i], sizeof(g_menu_rgt[i]), "进度%d/%d", prog, q->need);
            else snprintf(g_menu_rgt[i], sizeof(g_menu_rgt[i]), "A 接取");
            g_menu.rows[i] = (MenuRow){
                (q->type[0] == 'h') ? "sp_icon_herb" : "sp_icon_sword",
                q->n, g_menu_sub[i], g_menu_rgt[i]};
        }
        char hbuf[40];
        snprintf(hbuf, sizeof(hbuf), "冒险者 %s级 · 完成%d单", ARANKS[g.arank], g.questDone);
        lv_label_set_text(g_scene_hint, hbuf);
        s_b[0] = '\0';
        menu_list_dirty = 1;
        break;
    }
    case SC_WILD: {
        ensureDungeonOffers();
        int n = 5 + 1 + g.ndoffers;
        wildIdx = clampi(wildIdx, 0, n - 1);
        g_menu.n = n; g_menu.sel = wildIdx;
        for (int i = 0; i < 5; i++) {
            int lock = (i > 0 && !g.bossKilled[i - 1]) || g.level < WILD_REQ[i];
            snprintf(g_menu_sub[i], sizeof(g_menu_sub[i]), "%s",
                     lock ? "先击败上一章BOSS" : "野外区域 · 可挑战");
            snprintf(g_menu_rgt[i], sizeof(g_menu_rgt[i]), "%s", lock ? "锁" : "A 出发");
            g_menu.rows[i] = (MenuRow){"sp_icon_map", CHAPTERS[i], g_menu_sub[i], g_menu_rgt[i]};
        }
        int base = 5;
        snprintf(g_menu_sub[base], sizeof(g_menu_sub[base]), "%s",
                 g.rank < 1 ? "晋升初级魔法师后开放" : "精英连战 3 层");
        snprintf(g_menu_rgt[base], sizeof(g_menu_rgt[base]), "%s", g.rank < 1 ? "锁" : "A 进入");
        g_menu.rows[base] = (MenuRow){"sp_icon_gate", "遗迹秘境", g_menu_sub[base], g_menu_rgt[base]};
        for (int i = 0; i < g.ndoffers; i++) {
            int id = g.dungeonOffers[i];
            int r = base + 1 + i;
            snprintf(g_menu_sub[r], sizeof(g_menu_sub[r]), "%s", DUNGEONS[id].kind);
            snprintf(g_menu_rgt[r], sizeof(g_menu_rgt[r]), "A 进入");
            g_menu.rows[r] = (MenuRow){"sp_icon_gate", DUNGEONS[id].name, g_menu_sub[r], g_menu_rgt[r]};
        }
        lv_label_set_text(g_scene_hint, "OK 出发/进入 · ↓ 切换 · 长按 返回");
        s_b[0] = '\0';
        menu_list_dirty = 1;
        break;
    }
    case SC_BATTLE: {
        battle_render();
        break;
    }
    case SC_MUPGRADE: {
        int n = discoveredCount();
        g_menu.n = 0;
        if (n == 0) {
            g_menu.rows[0] = (MenuRow){"sp_icon_star", "魔法书是空的", "去战斗或炼金习得魔法", ""};
            g_menu.n = 1; g_menu.sel = 0;
        } else {
            muIdx = clampi(muIdx, 0, n - 1);
            int win = 6, start = clampi(muIdx - 2, 0, n - win);
            if (start < 0) start = 0;
            for (int i = start; i < start + win && i < n && g_menu.n < MENU_ROWS; i++) {
                int k = discoveredKeyAt(i);
                int a, e, m; spell_split(k, &a, &e, &m);
                int lv = slv(k);
                if (i == muIdx) {
                    int gold = 50 * (lv + 1);
                    snprintf(g_menu_sub[g_menu.n], sizeof(g_menu_sub[g_menu.n]),
                             "%s Lv%d→%d · %d金%s", TIER_NAMES[m], lv, lv + 1, gold,
                             m > 0 ? "+魔石" : "");
                    g_menu.rows[g_menu.n] = (MenuRow){ELEM_ICON[e], spellName(k),
                                                     g_menu_sub[g_menu.n], "OK升"};
                } else {
                    snprintf(g_menu_sub[g_menu.n], sizeof(g_menu_sub[g_menu.n]),
                             "%s Lv%d", TIER_NAMES[m], lv);
                    g_menu.rows[g_menu.n] = (MenuRow){ELEM_ICON[e], spellName(k),
                                                     g_menu_sub[g_menu.n], ""};
                }
                g_menu.n++;
            }
            g_menu.sel = clampi(muIdx - start, 0, g_menu.n - 1);
        }
        lv_label_set_text(g_scene_hint, "OK 升级 · ↓ 切换 · 长按 返回");
        menu_list_dirty = 1;
        break;
    }
    default:
        break;
    }

    if (s_ttl[0]) lv_label_set_text(g_scene_title, s_ttl);
    else lv_label_set_text(g_scene_title, SCENE_NAME[g_scene]);
    lv_label_set_text(g_scene_body, s_b);

    /* ---- frameBox 面板版式：只在切页时动一次（防周期重绘/重排导致卡死） ----
     * 详情页（图鉴/伙伴/角色）正文从立绘下方开始并居中（对照 HTML 各行 y 坐标）；
     * 列表页正文从框内顶部开始左对齐；非面板页恢复原版式。 */
    {
        static int last_frame_scene = -999;
        bool fon = is_frame_scene(g_scene);
        if (last_frame_scene != g_scene) {
            last_frame_scene = g_scene;
            bool pg = (g_scene == SC_PARTNERS);
            set_hidden(g_frame, !fon);
            set_hidden(g_frame_bar, !fon);
            set_hidden(g_frame_title, !fon);
            set_hidden(g_scene_title, fon);          /* 金框标题条替代旧居中标题 */
            set_hidden(g_dex_box, !fon || g_scene != SC_DEX);
            set_hidden(g_part_card, true);           /* 旧单卡被 4×2 网格取代 */
            /* 所有框架页统一用带图标的卡片列表（g_list），隐藏旧纯文字 body；
             * 伙伴页改用 4×2 立绘网格（g_part_root），隐藏列表 */
            set_hidden(g_scene_body, fon);
            set_hidden(g_list, !(fon && !pg));
            set_hidden(g_part_root, !(fon && pg && g.npartners > 0));
            set_hidden(g_part_info, !(fon && pg));
            /* 详情页：卡片列表下移到立绘下方，立绘盒居中；并把立绘置顶，
             * 彻底根治「文字/卡片压住立绘」的遮挡问题（用户问题④）。 */
            if (g_scene == SC_DEX)            g_list_y0 = 96;
            else if (g_scene == SC_PARTNERS)  g_list_y0 = ROW_Y0;
            else                              g_list_y0 = ROW_Y0;   /* 状态页 7 行铺满：与普通列表同 y0 */
            if (fon) {
                lv_label_set_text(g_frame_title, s_ttl[0] ? s_ttl : SCENE_NAME[g_scene]);
                if (g_scene == SC_DEX)           { lv_obj_move_foreground(g_dex_box);   lv_obj_move_foreground(g_scene_img); }
                else if (g_scene == SC_PARTNERS) { lv_obj_move_foreground(g_part_root); }
            } else {
                lv_obj_set_pos(g_scene_body, 10, 38);
                lv_obj_set_width(g_scene_body, SCR_W - 16);
                lv_obj_set_style_text_font(g_scene_body, &mage_cjk_16, 0);
                lv_obj_set_style_text_align(g_scene_body, 0, LV_TEXT_ALIGN_LEFT);
                set_hidden(g_list, true);            /* 离帧场景（含战斗）确保列表隐藏 */
                set_hidden(g_part_root, true);
                set_hidden(g_part_info, true);
            }
        }
    }

    /* 菜单卡片列表渲染 / 伙伴网格渲染（仅场景/选择/数据变化时触碰 LVGL，避免周期重绘卡死） */
    if (is_frame_scene(g_scene)) {
        if (g_scene == SC_PARTNERS) partners_grid_render();
        else menu_list_render();
    }

    /* 图鉴/伙伴/角色立绘：按 sp_<type> 原生尺寸展示，脚底中心锚点对齐 HTML 布局
     * （图鉴 foot=120,90；伙伴卡 foot=120,118；角色页 sp_hero foot=120,88）。
     * 状态记忆键 = 场景:物种（同物种跨页也要重定位），只在键变化时碰 LVGL。 */
    {
        static char lastKey[32] = "";
        char key[32] = "";
        const char *typeName = NULL;
        int footY = 90;
        if (g_scene == SC_DEX) {
            int di = clampi(dexIdx, 0, DEXORDER_COUNT - 1);
            if (g.dexSeen[di]) {                 /* HTML：未发现只画 '?' 不画立绘 */
                int t = enemyIndex(DEXORDER[di]);
                if (t >= 0) { typeName = ENEMY[t].type; footY = 90; }
            }
        }
        /* 状态页改为 7 行属性列表（职级·Lv/经验各占一行），不再画立绘：
         * 立绘会占掉 88px 顶部空间，7 行两行块放不下且行距过挤。 */
        if (typeName && typeName[0]) {
            snprintf(key, sizeof(key), "%d:%s", (int)g_scene, typeName);
            if (strcmp(lastKey, key) != 0) {
                char nm[32]; snprintf(nm, sizeof(nm), "sp_%s", typeName);
                const lv_image_dsc_t *dsc = mage_img(nm);
                if (dsc) {
                    int w = (int)dsc->header.w, h = (int)dsc->header.h;
                    lv_image_set_src(g_scene_img, dsc);
                    lv_obj_set_pos(g_scene_img, 120 - w / 2, footY - h);
                    strncpy(lastKey, key, sizeof(lastKey) - 1);
                    lastKey[sizeof(lastKey) - 1] = '\0';
                }
            }
            if (lv_obj_has_flag(g_scene_img, LV_OBJ_FLAG_HIDDEN))
                lv_obj_clear_flag(g_scene_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (!lv_obj_has_flag(g_scene_img, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(g_scene_img, LV_OBJ_FLAG_HIDDEN);
            lastKey[0] = '\0';
        }
    }

    /* 战斗页：隐藏通用 HUD/正文/提示，改用 1:1 原版 HUD；其余页面反过来隐藏原版 HUD。
     * 防卡死：set_hidden 内部调 lv_obj_add_flag/remove_flag，每 60ms 调一次
     * 会持续触发样式重算 -> mage 任务占满 CPU -> IDLE 饿死 -> 看门狗重启。
     * 只有状态真正翻转时才动手。 */
    {
        const bool in_bt = (g_scene == SC_BATTLE) && bt.active;
        ESP_LOGI(TAG, "FX g_scene=%d bt.active=%d in_bt=%d", g_scene, bt.active ? 1 : 0, in_bt ? 1 : 0);
        /* 结算/战利品/晕厥面板（BP_RESULT / BP_LOOT / BP_FAINT / BP_INTRO）需完整露出，
         * 隐藏顶部血条/状态栏，否则会压住结算面板顶部的文字（用户实测：战斗结束字体被遮挡）。 */
        mage_hud_show(in_bt && bt.phase != BP_LOOT && bt.phase != BP_INTRO
                            && bt.phase != BP_RESULT && bt.phase != BP_FAINT);
        /* 底部三卡只在可交互阶段（主菜单/魔法/道具选择）显示；
         * 结算面板底边 286、远征总结算底边 290 都压在卡区(y=276..316)上，
         * 卡不藏就会盖住 footer（用户实测照片：结算栏被操作栏挡住）。 */
        mage_hud_cards_show(in_bt && (bt.phase == BP_MENU || bt.phase == BP_MAGIC ||
                                      bt.phase == BP_ITEM));
        /* 战斗演出层（含 battle_bg 背景图）必须随战斗状态显隐：
         * 否则战斗结束后 battle_art_update 不再被调用，mage_fx_show(false)
         * 永远不触发，battle_bg 会残留在透明背景的场景（如状态/背包页）之后
         * 持续显示 —— 这就是「其他界面直接用了 BOSS 战背景」的根因。 */
        mage_fx_show(in_bt);
        set_hidden(g_hud, in_bt);
        set_hidden(g_scene_body, in_bt || is_frame_scene(g_scene));
        set_hidden(g_scene_hint, in_bt);
        set_hidden(g_scene_title, in_bt || is_frame_scene(g_scene));
        /* 战斗时把底部 toast 抬到卡片之上（y=232），避免压住动作卡与回合徽标
         * （卡片从 276 起、徽标 269~283）；非战斗回落原位 264。记忆上次值防每帧重排。 */
        static int8_t s_toastY = -1;
        int ty = in_bt ? 232 : 264;
        if (s_toastY != (int8_t)ty) { s_toastY = (int8_t)ty; lv_obj_set_pos(g_toast, 8, ty); }
    }


    if (g_scene != SC_BATTLE) {
        lv_obj_set_pos(g_scene_body, 10, 38);
        lv_obj_set_style_bg_opa(g_scene_body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_scene_body, 0, 0);
        lv_obj_set_style_pad_all(g_scene_body, 0, 0);
    }

    /* toast 显隐（显隐已由 set_hidden 做状态记忆，这里只处理文本）。
     * 战斗页的 toast 由 battle_render 自行掌管（BP_INTRO 横幅 / BP_FAINT 提示），
     * 这里必须跳过，否则会把刚亮出的"遭遇战"横幅立刻藏掉。 */
    if (g_scene != SC_BATTLE && now_ms() < g_toast_until) {
        set_hidden(g_toast, false);
        /* lv_label_set_text 每次都会重排文本并 invalidate。
         * refresh_ui 每 60ms 跑一次，必须先比对内容，相同则跳过。 */
        if (strcmp(g_toast_t, g_toast_buf) != 0) {
            /* 用 memcpy + 显式截断，不用 snprintf(dst,N,"%s",src)
             * （工程开 -Werror=format-truncation 会直接报错） */
            size_t n = strlen(g_toast_buf);
            if (n >= sizeof(g_toast_t)) { n = sizeof(g_toast_t) - 1; }
            memcpy(g_toast_t, g_toast_buf, n);
            g_toast_t[n] = '\0';
            lv_label_set_text(g_toast, g_toast_buf);
        }
    } else {
        set_hidden(g_toast, true);
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
            /* 功能栏长按OK(返回键) = 真正回地图（SC_WILD），兑现提示"长按 回地图"。
             * 旧逻辑只在 cityMode 0↔1 间互切，导致进城后困在菜单迷宫出不来（用户反馈"菜单关不掉"）。 */
            else if (act == ACT_BACK) { mage_set_scene(SC_WILD); g.cityMode = 0; g_need_refresh = 1; }
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
        int nopts = partner_menu_nopts(p);
        int setOpt  = 0;                                                 /* 出战（菜单首项，安全默认） */
        int evoOpt  = (bd->hasEvo && !p->evolved && p->level >= bd->evo.lvl) ? 1 : -1;
        int relOpt  = (evoOpt >= 0) ? evoOpt + 1 : 1;                     /* 放生 */
        int cityOpt = relOpt + 1;                                         /* 返回城市 */
        int closeOpt = nopts - 1;                                         /* 关闭菜单（末项） */

        if (partRelConf) {
            if (act == ACT_OK) {
                char nm[40]; snprintf(nm, sizeof(nm), "%s", p->name);  /* 先记名，再移位 */
                /* 收敛出战位 */
                if (g.activePartner >= g.npartners) g.activePartner = 0;
                /* 删除伙伴 */
                for (int i = partIdx; i < g.npartners - 1; i++) g.partners[i] = g.partners[i + 1];
                g.npartners--; g.cats = g.npartners;
                if (g.activePartner >= g.npartners) g.activePartner = 0;
                if (partIdx >= g.npartners) partIdx = 0;
                partRelConf = 0; partMenuOpen = 0; mage_persist();
                toast("已放生 %s", nm);
            } else if (act == ACT_BACK) { partRelConf = 0; }
            g_need_refresh = 1; break;
        }
        if (partMenuOpen) {
            if (act == ACT_NEXT) partMenu = (partMenu + 1) % nopts;
            else if (act == ACT_PREV) partMenu = (partMenu + nopts - 1) % nopts;
            else if (act == ACT_OK) {
                if (partMenu == setOpt) { g.activePartner = partIdx; mage_persist(); toast("出战：%s", p->name); partMenuOpen = 0; }
                else if (partMenu == evoOpt) { doEvolve(p); partMenuOpen = 0; }
                else if (partMenu == relOpt) { partRelConf = 1; }   /* 进入二次确认，绝不秒删 */
                else if (partMenu == cityOpt) { partMenuOpen = 0; mage_set_scene(SC_CITY); g.cityMode = 1; }
                else { partMenuOpen = 0; }   /* 关闭菜单 / 越界 */
            } else if (act == ACT_BACK) { partMenuOpen = 0; }   /* 菜单内长按OK=关菜单回网格 */
            g_need_refresh = 1; break;
        }
        /* 网格态：短按OK=打开伙伴菜单（出战/进化/放生/返回城市/关闭）；长按OK(BACK)=直接退出到城市 */
        if (act == ACT_OK) { partMenuOpen = 1; partMenu = setOpt; }
        else if (act == ACT_NEXT) { partIdx = (partIdx + 1) % g.npartners; }
        else if (act == ACT_PREV) { partIdx = (partIdx + g.npartners - 1) % g.npartners; }
        else if (act == ACT_BACK) { mage_set_scene(SC_CITY); g.cityMode = 1; }  /* 修复：伙伴界面长按OK直接退出，不再陷入菜单 */
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
        menu_list_dirty = 1;
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
        menu_list_dirty = 1;
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
        menu_list_dirty = 1;
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
        else if (btn == BSP_BTN_UP) act = ACT_LUP;   /* 长按↑ = 切回自动战斗 */
    }
    if (act < 0) return;
    scene_handle(act);
}

static void game_task(void *arg) {
    (void)arg;
    for (;;) {
        btn_ev_t e;
        if (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) {
            handle_btn(e.btn, e.ev);
        }

        uint32_t now = now_ms();
        if (bt.active) battle_tick();
        if (now - g_last_save_ms > 30000) { mage_persist(); g_last_save_ms = now; }

        if (g_need_refresh || g_scene != g_last_scene || now - g_last_ui_ms >= UI_REFRESH_MS) {
            g_last_ui_ms = now;
            g_last_scene = g_scene;
            g_need_refresh = 0;
            if (bsp_lvgl_lock(100)) {
                refresh_ui();
                bsp_lvgl_unlock();
            }
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
    if (p < 0.02f) p = 0.02f;
    if (p > 0.98f) p = 0.98f;
    return (int)(p * 100);
}

/* ---------- 前向声明（战斗函数互相引用，集中声明避免隐式声明报错） ---------- */
static void kill_award(BattleEnemy *e);
static void reap_dead(void);
static void after_player_action(void);
static void win_battle(void);
static void kill_clear(void);
static void spawn_boss(void);
static void finish_crawl(void);
static void finish_expedition(void);
static void throw_orb(int seal);
static void end_battle(void);

/* ---------- 伤害/击杀 ---------- */
static void deal_to(BattleEnemy *e, int dmg) {
    if (e->brokenTurns > 0) dmg = (int)(dmg * 1.4f + 0.5f);   /* 破防易伤 ×1.4 */
    e->hp -= dmg; if (e->hp < 0) e->hp = 0;
    /* 演出：受击后仰 + 闪红 + 粒子迸溅（HTML e.hurtT / fx） */
    e->hurtT  = 0.22f;
    e->flashT = 0.15f;
    mage_fx_spawn(FX_K_PARTICLE, (float)(e->x) - (float)(int)bt.cam,
                   250.0f - (float)e->typeIdx * 20.0f, 0xff9ad5, 5.0f, 0.45f);
    /* 1:1 复刻 HTML：bt.nums.push({x:e.x, y:enemyFeetY()-34, v:''+dmg, c:'#fff', t:0.8}) */
    mage_fx_num(e->x, 250 - 34, dmg, 0xffffff);
    if (e->hp <= 0) bt.shake = 0.30f;   /* 仅击杀瞬间震屏，普通命中靠受击后仰/闪红反馈，避免每击狂震 */
    if (e->boss && !e->enraged && e->hp > 0 && e->hp <= (int)(e->maxHp * 0.65f)) {
        e->enraged = 1; e->atk = (int)(e->atk * 1.6f + 0.5f);
        e->bossCD = e->bossCD < 2 ? e->bossCD : 2;
        toast("%s 狂暴了！攻击大幅提升", ENEMY[e->typeIdx].n);   /* 不用 emoji：子集字库无 emoji 字形，屏上只会出豆腐块 */
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
        else kill_clear();
        return;
    }
    int dmg = (int)((9 + g.level * 3 + g.rank * 3 + g.atkBonus + eqAtk()) * mage_rf(0.9f, 1.1f) + 0.5f);
    if (dmg < 1) dmg = 1;
    bt.lungeT = 0.5f;   /* 演出：主角突进（延长到 0.5s，跨多帧可见"冲出去又退回"） */
    deal_to(e, dmg);
    /* 演出：在敌人身上炸开命中火花（用户问题⑧：让玩家看清攻击打在敌人身上，而非自己） */
    mage_fx_spawn(FX_K_RING, (float)(e->x) - (float)(int)bt.cam, 212.0f, 0xffe27a, 16.0f, 0.30f);
    mage_fx_spawn(FX_K_PARTICLE, (float)(e->x) - (float)(int)bt.cam, 216.0f, 0xfff0c0, 7.0f, 0.40f);
    reap_dead();
    battle_force_render();
    after_player_action();
}
static void partner_strike(void) {
    if (g.npartners <= 0 || g.activePartner < 0 || g.activePartner >= g.npartners) return;
    Partner *p = &g.partners[g.activePartner];
    BattleEnemy *target = nearest_enemy(175);
    if (!target) return;
    /* 演出：伙伴猫突击（HTML bt.catLunge 冲向目标再退回） */
    bt.catLungeT = 0.40f; bt.catLungeDur = 0.40f;
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

/* "#rrggbb" 字符串 -> 0xRRGGBB（魔法元素色用于演出着色）。定义见 battle_art_update 区。 */
static uint32_t hex2col(const char *s);

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
    /* 演出：英雄突进（HTML bt.lungeT）+ 施法聚能（按元素着色，让用户一眼区分魔法属性） */
    bt.lungeT = 0.5f;
    uint32_t eleCol = hex2col(ELEMS[e2].c);
    /* 英雄脚下魔法阵（元素色） */
    mage_fx_spawn(FX_K_CIRCLE, (float)(BTPX) - (float)(int)bt.cam, 252.0f, eleCol, 22.0f, 0.5f);
    /* 施法聚能光球（元素色，替换原先固定紫色 0xb98cff） */
    mage_fx_spawn(FX_K_CHARGE, (float)(hits[0]->x) - (float)(int)bt.cam,
                   205.0f, eleCol, 12.0f, 0.35f);
    for (int i = 0; i < take; i++) {
        BattleEnemy *en = hits[i];
        int wk = 0; for (int j = 0; j < ENEMY[en->typeIdx].nWeak; j++) if (ENEMY[en->typeIdx].weakOf[j] == e2) wk = 1;
        int resisted = (en->resist[0] >= 0 && (en->resist[0] == e2 || en->resist[1] == e2)) ? 1 : 0;
        int dmg = (int)(pow * mage_rf(0.9f, 1.15f) + 0.5f); if (dmg < 1) dmg = 1;
        if (resisted) { dmg = (int)(dmg * 0.45f + 0.5f); en->resistKnown = 1; toast("抗性!"); }
        /* 命中环（元素色）：每次命中都在敌人身上炸开一圈对应属性的光环 */
        mage_fx_spawn(FX_K_RING, (float)(en->x) - (float)(int)bt.cam, 212.0f, eleCol, 14.0f, 0.35f);
        if (wk) {
            dmg = (int)(dmg * 1.5f + 0.5f);
            bt.weakKnown = 1;   /* 弱点揭示（HTML e.weakKnown 命中后才显示图标） */
            /* 演出：地面魔法阵 + 扩散环（元素色强化） */
            mage_fx_spawn(FX_K_CIRCLE, (float)(en->x) - (float)(int)bt.cam,
                           252.0f, 0x7fd4ff, 20.0f, 0.5f);
            mage_fx_spawn(FX_K_RING, (float)(en->x) - (float)(int)bt.cam,
                          210.0f, eleCol, 18.0f, 0.45f);
            bt.shake = 0.8f;   /* 弱点暴击强震屏 */
            if (en->shield > 0 && en->brokenTurns <= 0) {
                en->shield--;
                en->brokenT = 3.0f;   /* 破防后盾槽闪烁 */
                if (en->shield <= 0) { en->brokenTurns = 3; toast("破防！%s 硬直，受伤×1.4", ENEMY[en->typeIdx].n); }  /* HTML 原文 brokenT=4.5s（实时引擎）；回合制下等价取 3 个敌方回合 */
            }
        }
        deal_to(en, dmg);
    }
    reap_dead();
    battle_force_render();
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
        if (bt.enemyCount == 0) { kill_clear(); return; }
    } else {
        int heal = (int)(e->maxHp * 0.08f + 0.5f);
        e->hp += heal; if (e->hp > e->maxHp) e->hp = e->maxHp;
        toast("%s 挣脱了！(概率%d%%)", ENEMY[e->typeIdx].n, p);
    }
}

/* ---------- 敌方回合 ---------- */
static void apply_enemy_hit(BattleEnemy *e, int big) {
    int d = (int)(e->atk + mage_ri(-1, 2)); if (d < 1) d = 1;
    if (big) { d = (int)(d * 2.6f + 0.5f); toast("%s 大招命中！", ENEMY[e->typeIdx].n); }
    d = (int)(d * diffC()->dmgTaken + 0.5f); if (d < 1) d = 1;
    d -= eqDef(); if (d < 1) d = 1;
    Partner *pr = NULL;
    if (g.npartners > 0 && g.activePartner >= 0 && g.activePartner < g.npartners) pr = &g.partners[g.activePartner];
    if (pr && pr->traitIdx == traitIndex("tough")) d = (int)(d * 0.9f + 0.5f);
    if (bt.guardBlock > 0) { int cut = (int)(d * bt.guardBlock + 0.5f); d -= cut; if (d < 1) d = 1; bt.guardBlock -= 0.15f; if (bt.guardBlock < 0) bt.guardBlock = 0; }
    g.hp -= d; if (g.hp < 0) g.hp = 0;
    /* 1:1 复刻 HTML：bt.nums.push({x:bt.px, y:200, v:'-'+d, c:PAL.red, t:0.8})
     * 原版是**红色飘字**（向上飘 22px/s，0.8s 淡出），不是 toast 横幅。
     * 之前用 toast("-%d HP") 在底部拉了一条蓝底白字横条，与原版完全不同。 */
    mage_fx_num(BTPX, 200, d, 0xff6b6b);
    /* 演出：敌方冲锋前倾 + BOSS 大招蓄力警告 + 英雄受击后仰闪红 */
    e->lungeT = 0.35f; e->lungeDur = 0.35f;
    if (big) e->lungeBig = 1;
    bt.hurtT = 0.25f;
    bt.shake = big ? 1.0f : 0.5f;
    mage_fx_spawn(FX_K_RING, (float)(BTPX - (int)bt.cam), 210.0f,
                  big ? 0xff6b6b : 0xffa04d, 18.0f, 0.4f);
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
    if (bt.enemyCount == 0) { kill_clear(); return; }
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
    if (bt.enemyCount == 0) { kill_clear(); return; }
    bt.turn++;
    set_bphase(BP_MENU);
}
static void after_player_action(void) {
    /* BOSS 蓄力警告只在出招那一回合闪（HTML e.lunge.big 的瞬时标记） */
    for (int i = 0; i < bt.enemyCount; i++) bt.enemies[i].lungeBig = 0;
    partner_strike();
    if (bt.enemyCount == 0) { kill_clear(); return; }
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
        e->bob = mage_rf(0.0f, 6.28f);   /* 跳跃相位错开，避免几只怪同步弹跳（HTML e.bob） */
        if (e->elite) { e->hp = (int)(e->hp * 2.2f + 0.5f); e->maxHp = e->hp; e->atk = (int)(e->atk * 1.5f + 0.5f); }
    }
    /* 多怪站位修复（用户问题⑦）：原来 e->x = BTPX+78+i*16 只隔 16px，敌人立绘宽 30+ 必重叠成一团。
     * 改为在本波内沿 [BTPX+72, BTPX+146] 均匀分布（英雄在 BTPX=70，敌人整体偏右），
     * 单怪居中、多怪互不遮挡。 */
    {
        int n = bt.enemyCount;
        int x0 = BTPX + 72, x1 = BTPX + 146;
        for (int i = 0; i < n; i++) {
            int x = (n <= 1) ? ((x0 + x1) / 2) : (x0 + (x1 - x0) * i / (n - 1));
            bt.enemies[i].x = x;
        }
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
    e->bob = mage_rf(0.0f, 6.28f);
    int pl[9]; int np = 0;
    for (int i = 0; i < 9; i++) { int wk = 0; for (int j = 0; j < bd->nWeak; j++) if (bd->weakOf[j] == i) wk = 1; if (!wk) pl[np++] = i; }
    e->resist[0] = -1; e->resist[1] = -1;
    for (int k = 0; k < 2 && np > 0; k++) {
        int pi = mage_ri(0, np - 1); e->resist[k] = pl[pi];
        for (int j = pi; j < np - 1; j++) pl[j] = pl[j + 1];
        np--;
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
    /* 首波给一次操作提示（用户问题①⑨：OK攻击 / ↓魔法 / ↑道具 / 长按OK回城） */
    if (bt.wave == 1) toast("OK攻击 · ↓魔法 · ↑道具 · 长按↑自动/手动 · 长按OK回城");
}

/* ---------- 自动战斗 ---------- */
static void auto_act(void) {
    if (bt.enemyCount == 0) { kill_clear(); return; }
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
        if (bt.enemyCount == 0) { kill_clear(); return; }
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
static void kill_clear(void) {
    /* 清场后不直接进结算：停留 ~0.9s 让伤害飘字/受击特效/伙伴演出可见，
     * 解决"出手后直接跳结算、看不到攻击过程"的问题。 */
    if (bt.phase == BP_RESULT || bt.phase == BP_KILL) return;
    set_bphase(BP_KILL);
}
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
    for (int i = 0; i < 4; i++) g.dungeonOffers[i] = -1;
    g.ndoffers = 0;
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
        int pid[6], mid[6], pc = 0, mc = 0;
        int n = mage_ri(1, 3);
        for (int i = 0; i < n; i++) {
            if (mage_rf(0, 1) < 0.45f) { int id = mage_ri(0, 19); g.pbag[id]++; if (pc < 6) pid[pc++] = id; }
            else { int id = mage_ri(0, 39); g.mbag[id]++; if (mc < 6) mid[mc++] = id; }
        }
        int stN = 0, stId = 0, gotStone = 0, gotOrb = 0, gotCry = 0;
        if (mage_rf(0, 1) < 0.30f) { stId = 1 + mage_ri(0, 3); if (stId > 4) stId = 4; stN = mage_ri(1, 2); g.stones[stId] += stN; gotStone = 1; }
        if (mage_rf(0, 1) < 0.20f) { g.orbs += 2; gotOrb = 1; }
        if (mage_rf(0, 1) < 0.12f) { g.crystals++; gotCry = 1; }
        /* 战利品面板（1:1 复刻 HTML lootShow「宝箱战利品」：金币 + 具体物品） */
        bt.lootN = 0;
        snprintf(bt.lootTitle, sizeof(bt.lootTitle), "★ 宝箱战利品 ★");
        if (bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "金币 +%d", gd);
        for (int i = 0; i < pc && bt.lootN < 8; i++) snprintf(bt.lootLines[bt.lootN++], 32, "药品·%s", POTIONS[pid[i]].n);
        for (int i = 0; i < mc && bt.lootN < 8; i++) snprintf(bt.lootLines[bt.lootN++], 32, "素材·%s", MATERIALS[mid[i]].n);
        if (gotStone && bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "%s ×%d", STONE_NAMES[stId], stN);
        if (gotOrb && bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "捕捉球 ×2");
        if (gotCry && bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "魔晶 ×1");
        toast("宝箱开启！+%d金", gd);
        mage_persist(); bt.eventKind = -1; set_bphase(BP_LOOT); g_need_refresh = 1; return;
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
        int gotCry = 0; if (mage_rf(0, 1) < 0.35f) { g.crystals++; gotCry = 1; }
        int mid = mage_ri(0, 39); g.mbag[mid]++;
        bt.lootN = 0;
        snprintf(bt.lootTitle, sizeof(bt.lootTitle), "★ 采集收获 ★");
        if (bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "月光草 ×%d", h);
        if (gotCry && bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "魔晶 ×1");
        if (bt.lootN < 8) snprintf(bt.lootLines[bt.lootN++], 32, "素材·%s", MATERIALS[mid].n);
        toast("采集到月光草 x%d", h);
        mage_persist(); bt.eventKind = -1; set_bphase(BP_LOOT); g_need_refresh = 1; return;
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
    spawn_group(); set_bphase(BP_INTRO);
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
    spawn_group(); set_bphase(BP_INTRO);
    mage_set_scene(SC_BATTLE); g_need_refresh = 1;
}
void battle_start_boss(int chapter) {
    memset(&bt, 0, sizeof(bt));
    g.mapChapter = chapter;
    bt.active = 1; bt.mode = 0; bt.wave = 10;      /* 直接打 BOSS 波 */
    bt.autoBattle = g.autoMode; bt.autoCapture = g.autoCapture;
    bt.lastSpell = -1; bt.eventKind = -1; bt.itemIdx = 0; bt.magicIdx = 0;
    bt.tickMs = now_ms();
    spawn_group(); set_bphase(BP_INTRO);
    mage_set_scene(SC_BATTLE); g_need_refresh = 1;
}

/* ---------- 渲染 / 输入 / 自动推进 ---------- */

/* 战斗画面美术层已抽到 main/mage_fx.c（战斗演出层）。
 *
 * 那里实现了 HTML 原版的实时演出：背景横向滚动、敌我跳跃与落地压扁、
 * 受击后仰、冲锋前倾、暴击震屏、伤害飘字、粒子/魔法阵、BOSS 巨型化与蓄力警告、
 * 精英标记、破防盾槽、前景草丛视差 1.3。
 *
 * 防闪屏铁律（ESP32-C3 单核）：对象池一次构建，之后每帧只改属性，
 * 绝不 lv_obj_clean 重建；离开战斗必须隐藏，否则会盖住后续页面持续重绘。 */
/* 战斗演出层推进。dt 固定 0.15s（不是真实 20ms）：
 * ESP32-C3 单核每帧重绘一屏贴图必然过载，进战斗就卡死。
 * 250ms 约 4fps，动画偏慢但绝不卡死（宁可慢，不能卡）。
 * 宁可动画慢，绝不能卡死或烧板。 */
static float g_fx_acc = 0.0f;
static uint32_t g_fx_last = 0;
static bool g_fx_force = false;
#define FX_INTERVAL 0.18f   /* 真实时间节流：约 5.5fps。原 0.25s/调用在 60ms 刷新下
                               * 实测只会 ~1.3fps（主角突进 0.22s 一帧就结束→看起来完全不动）。
                               * 改真实时间累加后稳定 5.5fps，单核全屏重绘仍安全（背景静态只 blit）。 */

/* "#rrggbb" 字符串 -> 0xRRGGBB（魔法元素色用于演出着色） */
static uint32_t hex2col(const char *s) {
    if (!s || s[0] != '#') return 0xffffff;
    unsigned int v = 0;
    sscanf(s + 1, "%x", &v);
    return (uint32_t)v;
}

static void battle_art_update(void) {
    const bool in_battle = (g_scene == SC_BATTLE) && bt.active;
    mage_fx_show(in_battle);
    if (!in_battle) {
        g_fx_acc = 0.0f; g_fx_last = 0; g_fx_force = false;
        return;
    }
    /* 真实时间累加：无论 refresh 调用频率如何，都按固定 ~180ms 渲染一帧，
     * 主角突进(0.5s)可跨 2~3 帧显示，肉眼可见"冲出去又退回"。 */
    uint32_t now = now_ms();
    if (g_fx_last == 0) g_fx_last = now;
    g_fx_acc += (float)(now - g_fx_last) / 1000.0f;
    g_fx_last = now;
    if (!g_fx_force && g_fx_acc < FX_INTERVAL) return;
    g_fx_force = false;
    g_fx_acc = 0.0f;
    mage_fx_tick(FX_INTERVAL);
    mage_fx_render();
}

/* 玩家行动瞬间（攻击/施法）强制下一帧立即渲染，让演出第一帧立刻可见 */
static void battle_force_render(void) { g_fx_force = true; }

/* 战斗文字：按 HTML 原版 drawBattle 收敛 —— 敌人血条/等级画在画面上（演出层负责），
 * 这里只保留必要信息，且必须能塞进底部 196..320 这 124px 面板内，
 * 否则会把画面顶出去导致「纯文字看不到战斗」。 */
static void battle_render(void) {
    ESP_LOGI(TAG, "BATTLE_RENDER enter g_scene=%d bt.active=%d enemyCount=%d", g_scene, bt.active ? 1 : 0, bt.enemyCount);
    /* 1:1 复刻 HTML drawBattle()：画面占满，文字只在顶部血条与底部动作卡上，
     * **战斗画面上不出现任何大段文字**（此前用一个大 label 把文字铺满屏幕，
     * 是「看起来是纯文字游戏 + 看不到战斗画面」的根因）。 */
    mage_hud_build();
    const bool crawl = (bt.mode == 1);
    int wmax = crawl ? bt.dungeonRounds : 10;
    mage_hud_update(g.hp, g.maxHp, g.mp, g.maxMp,
                    bt.wave, wmax, crawl,
                    bt.autoBattle, g.autoCapture,
                    activeLoadoutKeys(NULL, 0), g.orbs, g.seals, g.potions, bt.turn);

    battle_art_update();

    /* 底部动作卡之上不再放任何文字；选中态与选项列表用 toast 提示，
     * 与原版「卡片高亮 + 弹层」语义一致，且绝不遮挡画面。 */
    s_init();
    static char cbuf[16][48];
    const char *clines[16]; int cn = 0; const char *ctitle = ""; const char *cfoot = ""; int showChoices = 0;
    switch (bt.phase) {
    case BP_MAGIC: {
        int lo[16]; int nl = activeLoadoutKeys(lo, 16);
        for (int i = 0; i < nl && cn < 16; i++) {
            int k = lo[i];
            int idx = cn++;
            snprintf(cbuf[idx], sizeof(cbuf[idx]), "%s%s  威力%d 耗%d",
                     (i == bt.magicIdx) ? "> " : "  ",
                     spellName(k), spellPower(k), spellCost(k));
            clines[idx] = cbuf[idx];
        }
        ctitle = "选择魔法"; cfoot = "OK 施放 · ↓↑切换 · 长按返回";
        showChoices = 1;
        break;
    }
    case BP_ITEM: {
        const char *kn[3] = {"药水", "捕捉球", "封印卡"};
        int cnt[3] = { g.potions, g.orbs, g.seals };
        int n = item_opt_count();
        for (int i = 0; i < n && cn < 16; i++) {
            int kind = item_opt_kind(i);
            int idx = cn++;
            snprintf(cbuf[idx], sizeof(cbuf[idx]), "%s%s  ×%d",
                     (i == bt.itemIdx) ? "> " : "  ",
                     kn[kind], cnt[kind]);
            clines[idx] = cbuf[idx];
        }
        ctitle = "选择道具"; cfoot = "OK 使用 · ↓↑切换 · 长按返回";
        showChoices = 1;
        break;
    }
    case BP_RESULT: {
        /* 1:1 复刻 HTML drawResult / drawExpoSummary：金币/经验 + 月光草/兽骨/药水 + BOSS 装备 */
        char b0[48], b1[40], b2[40], b3[40], b4[40], b5[40], b7[40], b8[64];
        char rowsR[96], sub[28];
        const char *lp[12]; int nl = 0;
        if (bt.done) {
            /* HTML drawExpoSummary：固定 6 行「名目左对齐 / 数值右对齐」 */
            const char *rowsL = "总击杀\n金币\n经验\n月光草\n兽骨\n药水";
            snprintf(rowsR, sizeof(rowsR), "%d 只\n+%d\n+%d\n+%d\n+%d\n+%d",
                     bt.killCnt, bt.goldGain, bt.expGain, bt.herbGain, bt.matGain, bt.potGain);
            const char *xp[4]; int nx = 0;
            xp[nx++] = "★ BOSS 战利品";
            if (bt.bossEquip[0]) { snprintf(b7, sizeof(b7), "%s", bt.bossEquip); xp[nx++] = b7; }
            snprintf(b8, sizeof(b8), "魔晶+%d 捕捉球+%d 封印卡+%d", bt.bossCrystal, bt.bossOrb, bt.bossSeal);
            xp[nx++] = b8;
            if (promoteReady()) xp[nx++] = "★ 已达晋升标准！";
            snprintf(sub, sizeof(sub), "%s 已肃清", CHAPTERS[clampi(g.mapChapter, 0, 4)]);
            mage_fx_expo(1, "★ 远征胜利 ★", sub, rowsL, rowsR, 6, xp, nx, "A继续·B回城·长按↑自动");
        } else {
            snprintf(b0, sizeof(b0), "第 %d 波 清场（累计收益）", bt.wave);
            snprintf(b1, sizeof(b1), "金币 +%d", bt.goldGain); lp[nl++] = b1;
            snprintf(b2, sizeof(b2), "经验 +%d", bt.expGain); lp[nl++] = b2;
            if (bt.herbGain) { snprintf(b3, sizeof(b3), "月光草 +%d", bt.herbGain); lp[nl++] = b3; }
            if (bt.matGain)  { snprintf(b4, sizeof(b4), "兽骨 +%d", bt.matGain); lp[nl++] = b4; }
            if (bt.potGain)  { snprintf(b5, sizeof(b5), "药水 +%d", bt.potGain); lp[nl++] = b5; }
            if (promoteReady()) lp[nl++] = "★ 已达晋升标准！";
            mage_fx_result(1, b0, "A继续·B回城·长按↑自动", lp, nl);
        }
        break;
    }
    case BP_LOOT: {
        /* 宝箱/采集战利品面板（1:1 复刻 HTML lootShow：顶部小面板、无底部提示） */
        const char *lp[8]; int nl = 0;
        for (int i = 0; i < bt.lootN && i < 8; i++) lp[nl++] = bt.lootLines[i];
        mage_fx_loot(1, bt.lootTitle, NULL, lp, nl);
        break;
    }
    case BP_EVENT: {
        const char *ev[4] = {"宝箱", "陷阱", "泉水", "稀有药草"};
        s_add("遭遇 %s", bt.eventKind >= 0 ? ev[bt.eventKind] : "?");
        break;
    }
    case BP_FAINT: {
        /* 战败结算面板（对等 BP_RESULT）：展示本场收益 + 力竭回城叙事（无惩罚，回城恢复 50% 状态） */
        char b0[48], b1[40], b2[40], b3[40], b4[40], b5[40];
        const char *lp[8]; int nl = 0;
        snprintf(b0, sizeof(b0), "魔力耗尽，力竭撤退…");
        if (bt.goldGain) { snprintf(b1, sizeof(b1), "金币 +%d", bt.goldGain); lp[nl++] = b1; }
        if (bt.expGain)  { snprintf(b2, sizeof(b2), "经验 +%d", bt.expGain); lp[nl++] = b2; }
        if (bt.herbGain) { snprintf(b3, sizeof(b3), "月光草 +%d", bt.herbGain); lp[nl++] = b3; }
        if (bt.matGain)  { snprintf(b4, sizeof(b4), "兽骨 +%d", bt.matGain); lp[nl++] = b4; }
        if (bt.potGain)  { snprintf(b5, sizeof(b5), "药水 +%d", bt.potGain); lp[nl++] = b5; }
        if (nl == 0) lp[nl++] = "未获得战利品（下次留意魔力）";
        mage_fx_result(0, b0, "OK 回城休整 · 长按返回", lp, nl);
        break;
    }
    default:
        break;
    }

    /* 战利品/结算面板（BP_RESULT / BP_LOOT / BP_FAINT）各自显示完整面板，
     * 其余阶段用 g_toast 提示（仅 BP_INTRO 显示"遭遇战"横幅，避免遮挡战斗画面）。
     * 注：BP_FAINT 现由上方 case 调用 mage_fx_result 接管结算面板，不再走顶部提示。 */
    if (bt.phase == BP_INTRO) {
        const char *msg = (bt.phase == BP_FAINT) ? s_b : "★ 遭遇战开始！";
        if (strcmp(g_toast_t, msg) != 0) {
            size_t n = strlen(msg); if (n >= sizeof(g_toast_t)) n = sizeof(g_toast_t) - 1;
            memcpy(g_toast_t, msg, n); g_toast_t[n] = '\0';
            lv_label_set_text(g_toast, msg);
        }
        set_hidden(g_toast, false);
        g_toast_until = 0;
    } else {
        set_hidden(g_toast, true);
    }
    lv_label_set_text(g_scene_hint, "");

    /* 事件面板（宝箱/陷阱/泉水/药草）与结算/战利品面板共用 evtRoot：
     * BP_EVENT 只归事件面板管；BP_RESULT/BP_LOOT 由上面 switch 里的
     * mage_fx_result/expo/loot 接管（绝不能再调 mage_fx_event(-1) 把共用根隐藏）；
     * 其余阶段把两者都收掉。 */
    if (bt.phase == BP_EVENT) {
        mage_fx_event(bt.eventKind);
    } else if (bt.phase != BP_RESULT && bt.phase != BP_LOOT) {
        mage_fx_event(-1);
        mage_fx_loot(0, NULL, NULL, NULL, 0);
    }
    /* 魔法/道具选择列表面板：进入子菜单显示，离开（含回主菜单/结算）立即收起，
     * 不抢 RESULT/LOOT/EVENT 面板。 */
    if (showChoices) mage_fx_choices(1, ctitle, clines, cn, cfoot);
    else if (bt.phase != BP_RESULT && bt.phase != BP_LOOT && bt.phase != BP_EVENT)
        mage_fx_choices(0, NULL, NULL, 0, NULL);
}

static void battle_handle(int act) {
    switch (bt.phase) {
    case BP_INTRO:
        /* 遭遇演出中：OK 跳过进入选招；BACK 直接撤退回城（不再被 INTRO 吞掉） */
        if (act == ACT_OK) { set_bphase(BP_MENU); bt.tickMs = now_ms(); }
        else if (act == ACT_BACK) end_battle();
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); }
        break;
    case BP_MENU:
        /* 关键修复（用户问题①②）：自动战斗时，按 下/上/返回 不应只被「切回手动」吞掉，
         * 否则「只能攻击、选不了魔法道具」。改为：任意指令都会先接管手动，再执行对应动作；
         * 返回键始终直接回城（只按一次即可，不再需要按两遍）。 */
        if (act == ACT_OK) {
            if (bt.autoBattle) { bt.autoBattle = 0; toast("手动模式"); }
            hero_strike();
        }
        else if (act == ACT_NEXT) {
            if (bt.autoBattle) bt.autoBattle = 0;
            int lo[16]; int nl = activeLoadoutKeys(lo, 16); if (nl == 0) { toast("没有魔法"); break; }
            bt.magicIdx = 0; g.equipped = lo[0]; set_bphase(BP_MAGIC);
        }
        else if (act == ACT_PREV) {
            if (bt.autoBattle) bt.autoBattle = 0;
            if (item_opt_count() == 0) { toast("没有道具"); break; }
            bt.itemIdx = 0; set_bphase(BP_ITEM);
        }
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); }
        else if (act == ACT_BACK) end_battle();
        break;
    case BP_MAGIC:
        if (act == ACT_OK) { set_bphase(BP_MENU); magic_cast(); }
        else if (act == ACT_NEXT) { int lo[16]; int nl = activeLoadoutKeys(lo, 16); if (nl == 0) break; bt.magicIdx = (bt.magicIdx + 1) % nl; g.equipped = lo[bt.magicIdx]; g_need_refresh = 1; }
        else if (act == ACT_PREV || act == ACT_BACK) set_bphase(BP_MENU);
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); set_bphase(BP_MENU); }
        break;
    case BP_ITEM:
        if (act == ACT_OK) { set_bphase(BP_MENU); use_item(); }
        else if (act == ACT_NEXT) { int c = item_opt_count(); if (c == 0) break; bt.itemIdx = (bt.itemIdx + 1) % c; g_need_refresh = 1; }
        else if (act == ACT_PREV || act == ACT_BACK) set_bphase(BP_MENU);
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); set_bphase(BP_MENU); }
        break;
    case BP_RESULT:
        if (act == ACT_OK) next_wave();
        else if (act == ACT_BACK) end_battle();
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); }
        break;
    case BP_EVENT:
        if (act == ACT_OK) apply_event();
        else if (act == ACT_BACK) { bt.eventKind = -1; spawn_and_menu(); }
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); }
        break;
    case BP_LOOT:
        if (act == ACT_OK || act == ACT_BACK) spawn_and_menu();
        else if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); }
        break;
    case BP_FAINT:
        if (act == ACT_LUP) { bt.autoBattle = 1; toast("自动模式"); }
        else if (act == ACT_OK || act == ACT_BACK) {
            g.hp = (int)(g.maxHp * 0.5f + 0.5f); if (g.hp < 1) g.hp = 1;
            g.mp = (int)(g.maxMp * 0.5f + 0.5f);
            if (bt.mode == 1) {
                for (int i = 0; i < 4; i++) g.dungeonOffers[i] = -1;
                g.ndoffers = 0;
            }
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
    if (bt.phase == BP_INTRO) {
        /* 1:1 复刻 HTML walk+meet：进战斗先亮出敌群约 2.5s（固件无行走动画的折中），
         * 让"战斗开始"清晰可见，而不是一进战斗就直接结算。 */
        if (now - bt.tickMs > 2500) { bt.tickMs = now; set_bphase(BP_MENU); }
    } else if (bt.phase == BP_MENU) {
        /* HTML autoT=1.4：每 1.4s 出手一次，刷新时能看到招式/飘字，而非一闪而过 */
        if (bt.autoBattle && now - bt.tickMs > 1400) { bt.tickMs = now; auto_act(); }
    } else if (bt.phase == BP_RESULT) {
        /* HTML autoT2=2.0：每波结算停留 2s，避免"每波只闪 1 帧" */
        if (bt.autoBattle && now - bt.tickMs > 2000) { bt.tickMs = now; next_wave(); }
    } else if (bt.phase == BP_EVENT) {
        if (bt.autoBattle && now - bt.tickMs > 1200) { bt.tickMs = now; apply_event(); }
    } else if (bt.phase == BP_LOOT) {
        /* HTML lootShow.t=3 秒后自动消失继续前进；C 端同口径 3 秒自动继续，
         * 按键（OK/BACK）可随时跳过；自动战斗更快（1.5s）。 */
        if (now - bt.tickMs > 3000 || (bt.autoBattle && now - bt.tickMs > 1500)) {
            bt.tickMs = now; spawn_and_menu();
        }
    } else if (bt.phase == BP_KILL) {
        /* 击杀演出窗口：清场后停留 ~0.9s 再进结算，让伤害飘字/受击特效可见 */
        if (now - bt.tickMs > 900) { bt.tickMs = now; win_battle(); }
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
