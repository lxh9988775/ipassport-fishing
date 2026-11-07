/* ember_color.c — 《余烬》火色引擎 C 实现（ember_color_engine.js 逐行移植）
 * 对照验证：tools/ember_color_mirror.py 调用本文件同款算法与 JS 引擎对拍。
 */
#include "ember_color.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ---------------- 常量（与 JS 同源） ---------------- */
const ember_dim_t EMBER_DIMS[EMBER_NDIM] = {
    { 208.0f, 0.52f, 0.33f, "#28577F", "墨蓝", "工作" },
    {  44.0f, 0.80f, 0.64f, "#ECC559", "蜜金", "关系" },
    {   6.0f, 0.82f, 0.52f, "#E83420", "朱绯", "热爱" },
    { 158.0f, 0.42f, 0.50f, "#49B58D", "苔青", "身体" },
};
const ember_hsl_t EMBER_BASE     = { 32.7f, 0.61f, 0.64f };
const ember_hsl_t EMBER_NEUTRAL  = { 36.0f, 0.26f, 0.62f };
const ember_hsl_t EMBER_BORROWED = { 220.0f, 0.62f, 0.24f };

static const float STOPS[][2] = { {0,0}, {5,0.40f}, {10,0.70f}, {15,1.0f} };
#define GREEN_LO 75.0f
#define GREEN_HI 155.0f
#define COLD_LO  185.0f
#define COLD_HI  330.0f

static const float RAMP_X[7] = { 0.00f, 0.15f, 0.32f, 0.50f, 0.70f, 0.88f, 1.00f };
static const float RAMP_L[7] = { 0.05f, 0.14f, 0.31f, 0.50f, 0.68f, 0.80f, 0.86f };
static const float RAMP_S[7] = { 1.00f, 1.00f, 0.96f, 0.90f, 0.80f, 0.62f, 0.48f };

/* 色名分带（与 JS BANDS 同源） */
static const float BAND_HI[15] = { 0,10,24,34,52,70,100,140,182,205,238,268,310,342,361 };
static const char *BAND_NAME[15] = {
    "朱绯","绯橙","暮橙","暖金","蜜金","秋禾","苔金","苔青","烟青","墨蓝",
    "夜蓝","暮紫","绯紫","绛红","朱绯"
};

/* ---------------- 基础工具 ---------------- */
float ember_norm(float h) {
    float r = fmodf(h, 360.0f);
    if (r < 0) r += 360.0f;
    return r;
}
float ember_arc(float a, float b) {
    return fmodf((b - a + 540.0f), 360.0f) - 180.0f;
}
int ember_greenHits(float from, float d) {
    int c = 0;
    for (int k = 1; k <= 12; k++) {
        float hh = ember_norm(from + d * (float)k / 12.0f);
        if (hh >= GREEN_LO && hh <= GREEN_HI) c++;
    }
    return c;
}
float ember_travelArc(float from, float to) {
    float d1 = ember_arc(from, to);
    float t = ember_norm(to);
    if (!(t >= COLD_LO && t <= COLD_HI)) return d1;
    float d2 = (d1 >= 0) ? (d1 - 360.0f) : (d1 + 360.0f);
    return ember_greenHits(from, d1) <= ember_greenHits(from, d2) ? d1 : d2;
}
float ember_progressFor(int W) {
    if (W <= 0) return 0.0f;
    if (W >= (int)STOPS[3][0]) return STOPS[3][1];
    for (int i = 0; i < 3; i++) {
        float a = STOPS[i][0], b = STOPS[i + 1][0];
        if (W <= (int)b) {
            float pa = STOPS[i][1], pb = STOPS[i + 1][1];
            return pa + (pb - pa) * (float)(W - (int)a) / (b - a);
        }
    }
    return 1.0f;
}

/* ---------------- 分析 ---------------- */
void ember_analyze(const uint8_t tot[EMBER_NDIM], int *W, int *main_i, int *second_i,
                   float *wMain, float *wSecond, bool *balanced, float *tipWeight) {
    int w = 0, active = 0;
    int order[EMBER_NDIM];
    for (int i = 0; i < EMBER_NDIM; i++) order[i] = i;
    for (int i = 0; i < EMBER_NDIM; i++) { w += tot[i]; if (tot[i] > 0) active++; }
    /* 稳定排序：v 降序，v 相等时 i 升序 */
    for (int i = 0; i < EMBER_NDIM - 1; i++) {
        for (int j = i + 1; j < EMBER_NDIM; j++) {
            int vi = tot[order[i]], vj = tot[order[j]];
            if (vj > vi || (vj == vi && order[j] < order[i])) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    int m  = (w > 0) ? order[0] : -1;
    int sc = (w > 0 && tot[order[1]] > 0) ? order[1] : -1;
    float wm = (m >= 0) ? (float)tot[m] / (float)w : 0.0f;
    float ws = (sc >= 0) ? (float)tot[sc] / (float)w : 0.0f;
    bool bal = (active >= 3 && wm < 0.38f);
    *W = w; *main_i = bal ? -1 : m; *second_i = sc;
    *wMain = wm; *wSecond = ws; *balanced = bal;
    *tipWeight = (!bal && sc >= 0) ? (0.30f * ws / wm > 0.20f ? 0.20f : 0.30f * ws / wm) : 0.0f;
}

/* ---------------- HSL -> RGB ---------------- */
void ember_hslToRgb(float h, float s, float l, ember_rgb_t *out) {
    h = ember_norm(h) / 360.0f;
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (l < 0) l = 0;
    if (l > 1) l = 1;
    if (s == 0.0f) {
        int v = (int)roundf(l * 255.0f);
        out->r = out->g = out->b = v;
        return;
    }
    float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    float p = 2.0f * l - q;
    float fpt[3];
    fpt[0] = h + 1.0f / 3.0f;
    fpt[1] = h;
    fpt[2] = h - 1.0f / 3.0f;
    int ch[3];
    for (int i = 0; i < 3; i++) {
        float t = fpt[i];
        t = t - floorf(t);
        float v;
        if (t < 1.0f / 6.0f)      v = p + (q - p) * 6.0f * t;
        else if (t < 1.0f / 2.0f) v = q;
        else if (t < 2.0f / 3.0f) v = p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        else                      v = p;
        ch[i] = (int)roundf(v * 255.0f);
    }
    out->r = ch[0]; out->g = ch[1]; out->b = ch[2];
}

void ember_dim_rgb(int i, ember_rgb_t *out) {
    ember_hslToRgb(EMBER_DIMS[i].h, EMBER_DIMS[i].s, EMBER_DIMS[i].l, out);
}

void ember_rgbToHex(const ember_rgb_t *c, char *out7) {
    snprintf(out7, 8, "#%02X%02X%02X",
             (unsigned)(c->r < 0 ? 0 : c->r > 255 ? 255 : c->r),
             (unsigned)(c->g < 0 ? 0 : c->g > 255 ? 255 : c->g),
             (unsigned)(c->b < 0 ? 0 : c->b > 255 ? 255 : c->b));
}

/* ---------------- 主色 ---------------- */
void ember_colorFor(const uint8_t tot[EMBER_NDIM], bool borrowed, ember_color_t *out) {
    int W, m, sc; float wm, ws; bool bal; float tipW;
    ember_analyze(tot, &W, &m, &sc, &wm, &ws, &bal, &tipW);
    float p = ember_progressFor(W);

    out->W = W; out->main_i = m; out->second_i = sc;
    out->wMain = wm; out->wSecond = ws; out->balanced = bal;
    out->progress = p; out->tipWeight = tipW; out->borrowed = borrowed;
    out->tipHex[0] = '\0';

    if (borrowed) {
        ember_hslToRgb(EMBER_BORROWED.h, EMBER_BORROWED.s, EMBER_BORROWED.l, &out->rgb);
        ember_rgbToHex(&out->rgb, out->hex);
        out->h = EMBER_BORROWED.h; out->s = EMBER_BORROWED.s; out->l = EMBER_BORROWED.l;
        return;
    }

    ember_hsl_t target;
    if (W == 0) {
        target = EMBER_BASE;
    } else if (bal) {
        target = EMBER_NEUTRAL;
    } else {
        ember_hsl_t D = { EMBER_DIMS[m].h, EMBER_DIMS[m].s, EMBER_DIMS[m].l };
        float h = D.h;
        if (sc >= 0) {
            float pull = 0.08f * ws / wm;
            h = ember_norm(D.h + ember_travelArc(D.h, EMBER_DIMS[sc].h) * pull);
        }
        target.h = h; target.s = D.s; target.l = D.l;
    }

    float dh = ember_travelArc(EMBER_BASE.h, target.h);
    bool risky = (fabsf(dh) > 90.0f) || (ember_greenHits(EMBER_BASE.h, dh) > 0);
    float pHue = risky ? (p * 2.2f < 1.0f ? p * 2.2f : 1.0f) : p;
    float hh = ember_norm(EMBER_BASE.h + dh * pHue);
    float dip = risky ? (0.34f * sinf(3.14159265f * (pHue < 1.0f ? pHue : 1.0f))) : 0.0f;
    float ss = EMBER_BASE.s + (target.s - EMBER_BASE.s) * p - dip;
    if (ss < 0.05f) ss = 0.05f;
    float ll = EMBER_BASE.l + (target.l - EMBER_BASE.l) * p;

    ember_hslToRgb(hh, ss, ll, &out->rgb);
    ember_rgbToHex(&out->rgb, out->hex);
    out->h = hh; out->s = ss; out->l = ll;
    if (sc >= 0) {
        strcpy(out->tipHex, EMBER_DIMS[sc].hex);
    }
}

/* ---------------- 精灵重染（单像素） ---------------- */
static float ramp_val(const float *arr, float x) {
    for (int i = 0; i < 6; i++) {
        if (x <= RAMP_X[i + 1]) {
            float t = (x - RAMP_X[i]) / (RAMP_X[i + 1] - RAMP_X[i]);
            return arr[i] + (arr[i + 1] - arr[i]) * t;
        }
    }
    return arr[6];
}

void ember_relightPixel(float l0, float s0, float hDeg, float sat,
                        const ember_rgb_t *tip, float tipWeight, ember_rgb_t *out) {
    float L = ramp_val(RAMP_L, l0) * 0.86f + l0 * 0.14f;
    if (L < 0) L = 0;
    if (L > 1) L = 1;
    float S = ramp_val(RAMP_S, l0) * sat * (0.82f + 0.28f * s0);
    if (S < 0) S = 0;
    if (S > 0.95f) S = 0.95f;
    ember_hslToRgb(hDeg, S, L, out);
    if (tip && tipWeight > 0.0f && l0 < 0.38f) {
        float w = tipWeight * 0.5f * (1.0f - l0 / 0.38f);
        out->r = (int)roundf((float)out->r * (1.0f - w) + (float)tip->r * w);
        out->g = (int)roundf((float)out->g * (1.0f - w) + (float)tip->g * w);
        out->b = (int)roundf((float)out->b * (1.0f - w) + (float)tip->b * w);
    }
}

/* ---------------- 色名 ---------------- */
const char *ember_colorName(float h, float s, float l) {
    static char buf[24];
    h = ember_norm(h);
    const char *base = "朱绯";
    for (int i = 0; i < 14; i++) {
        if (h >= BAND_HI[i] && h < BAND_HI[i + 1]) { base = BAND_NAME[i]; break; }
    }
    const char *pre = "";
    if (l < 0.34f)      pre = "玄";
    else if (s < 0.28f) pre = "烟";
    else if (s < 0.42f) pre = "雾";
    else if (l < 0.50f) pre = "夜";
    snprintf(buf, sizeof(buf), "%s%s", pre, base);
    return buf;
}
