/* ember_color.h — 《余烬》火色引擎（单一事实来源，与 tools/ember_color_engine.js 同源）
 *
 * 模型：小余是一团自发光火灵。主色由「占比最高的维度」决定（不是四维平均），
 * 本色（暖橙底火）随累计段数退场，次色只把色相拉几度、只在外焰留冷光，
 * 任何色相旅行都绕开绿带。纯算法、无 LVGL、无副作用，固件与电脑端对照共用同一套规则。
 *
 * 本文件是 ember_color_engine.js 的逐行 C 移植；移植对照见 tools/ember_color_mirror.py。
 */
#ifndef EMBER_COLOR_H
#define EMBER_COLOR_H

#include <stdint.h>
#include <stdbool.h>

typedef struct { float h, s, l; } ember_hsl_t;
typedef struct { int r, g, b; } ember_rgb_t;

#define EMBER_NDIM 4
typedef struct {
    float       h, s, l;     /* 目标色相/饱和/明度 */
    const char *hex;         /* 设计色号（参考） */
    const char *hue;         /* 墨蓝 / 蜜金 / 朱绯 / 苔青 */
    const char *label;       /* 工作 / 关系 / 热爱 / 身体 */
} ember_dim_t;

extern const ember_dim_t EMBER_DIMS[EMBER_NDIM];
extern const ember_hsl_t  EMBER_BASE;       /* 母版火焰实测均值，本色底火 */
extern const ember_hsl_t  EMBER_NEUTRAL;    /* 四盏分匀的雾暖金 */
extern const ember_hsl_t  EMBER_BORROWED;   /* 借明天的柴：黑蓝冷焰 */

typedef struct {
    float       h, s, l;
    ember_rgb_t rgb;
    char        hex[8];        /* #RRGGBB */
    float       progress;      /* 本色退场进度 0..1 */
    int         main_i;        /* 主轴维度下标，-1 表示无/均衡 */
    int         second_i;      /* 次轴维度下标，-1 表示无 */
    bool        balanced;      /* 四盏分匀 */
    char        tipHex[8];     /* 次轴色号（外焰冷光），无则空串 */
    float       tipWeight;     /* 次轴冷光权重 0..0.20 */
    int         W;             /* 累计总段数 */
    float       wMain, wSecond;
    bool        borrowed;      /* 借柴状态色 */
} ember_color_t;

/* ---- 基础工具 ---- */
float ember_norm(float h);
float ember_arc(float a, float b);
int   ember_greenHits(float from, float d);
float ember_travelArc(float from, float to);
float ember_progressFor(int W);

/* ---- 分析 + 主色 ---- */
void  ember_analyze(const uint8_t tot[EMBER_NDIM], int *W, int *main_i, int *second_i,
                    float *wMain, float *wSecond, bool *balanced, float *tipWeight);
void  ember_hslToRgb(float h, float s, float l, ember_rgb_t *out);
void  ember_dim_rgb(int i, ember_rgb_t *out);   /* DIMS[i] 的实色 rgb（外焰冷光用） */
void  ember_rgbToHex(const ember_rgb_t *c, char *out7);
void  ember_colorFor(const uint8_t tot[EMBER_NDIM], bool borrowed, ember_color_t *out);

/* ---- 精灵重染（单像素，与 JS relightPixel 一致）----
 * l0: 骨架 R 通道=色阶位置(0..1)；s0: 骨架 G 通道=笔触密度(0..1)
 * hDeg/sat: 当前火色；tip: 次轴冷光 rgb（可 NULL）；tipWeight: 0..0.20 */
void  ember_relightPixel(float l0, float s0, float hDeg, float sat,
                         const ember_rgb_t *tip, float tipWeight, ember_rgb_t *out);

/* ---- 色名（如「雾蜜金」「玄墨蓝」），返回静态缓冲区指针 ---- */
const char *ember_colorName(float h, float s, float l);

#endif /* EMBER_COLOR_H */
