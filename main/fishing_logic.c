/*
 * fishing_logic.c - 钓鱼逻辑层实现
 *
 * 所有参数都用"整数 + 定点"表达，避免浮点（ESP32-C3 无 FPU 习惯省着用）。
 * 计分倍率用"乘以 100 再除 100"的整数形式。
 */

#include "fishing_logic.h"

/* ---------- 确定性 RNG（xorshift32） ---------- */
static uint32_t g_rng_state = 0x9E3779B9u;

static uint32_t rng_next(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/* 返回 [0, n) 的整数 */
static int rng_range(int n) {
    if (n <= 0) return 0;
    return (int)(rng_next() % (uint32_t)n);
}

/* ---------- 各钓点的参数表（整数，毫秒 / 倍率*100） ---------- */
typedef struct {
    int wait_min_ms;
    int wait_max_ms;
    int bite_window_ms;   /* 咬钩窗口时长，越难越短 */
    int score_mult100;    /* 分值倍率 * 100 */
} spot_cfg_t;

static const spot_cfg_t SPOT_CFG[SPOT_COUNT] = {
    /* POND  */ { 1500, 4000, 1400, 100 },  /* 1.0x */
    /* RIVER */ { 2500, 6000, 1100, 150 },  /* 1.5x */
    /* SEA   */ { 4000, 9000,  800, 250 },  /* 2.5x */
};

/* 鱼大小档的"权重"：索引 0=小鱼 1=中鱼 2=大鱼。受饵料影响。 */
static const int BAIT_WEIGHTS[BAIT_COUNT][3] = {
    /* WORM  */ { 3, 1, 0 },  /* 小鱼多 */
    /* BREAD */ { 2, 2, 1 },  /* 中小均衡 */
    /* LURE  */ { 1, 2, 3 },  /* 大鱼多 */
};

static const int SIZE_BASE_SCORE[3] = { 10, 25, 60 }; /* 小/中/大 基础分 */

/* ---------- 内部状态 ---------- */
static game_state_t g_state = STATE_IDLE;
static int          g_score = 0;
static int          g_high_score = 0;
static bait_t       g_bait = BAIT_WORM;
static spot_t       g_spot = SPOT_POND;

static int  g_state_enter_ms = 0;   /* 进入当前状态的时刻 */
static int  g_last_tick_ms   = 0;
static int  g_wait_target_ms = 0;   /* WAITING 需要等待的总时长 */
static int  g_bite_window_ms = 0;   /* 当前咬钩窗口时长 */
static catch_result_t g_last_catch; /* strike 成功时的结果缓存 */

/* ---------- 实现 ---------- */
void fishing_init(uint32_t seed) {
    g_rng_state = (seed == 0) ? 0x9E3779B9u : seed;
    g_state = STATE_IDLE;
    g_score = 0;
    g_state_enter_ms = 0;
    g_last_tick_ms = 0;
    g_wait_target_ms = 0;
    g_bite_window_ms = 0;
}

void fishing_set_bait(bait_t b) {
    if (b >= 0 && b < BAIT_COUNT) g_bait = b;
}
void fishing_set_spot(spot_t s) {
    if (s >= 0 && s < SPOT_COUNT) g_spot = s;
}

void fishing_enter_menu(void) {
    /* 仅在非进行中状态允许进菜单，避免打断一局 */
    if (g_state == STATE_IDLE) g_state = STATE_MENU;
}
void fishing_exit_menu(void) {
    if (g_state == STATE_MENU) g_state = STATE_IDLE;
}

int fishing_cast(void) {
    if (g_state != STATE_IDLE && g_state != STATE_MENU) {
        return 0; /* 只有待机/菜单可抛竿 */
    }
    if (g_state == STATE_MENU) g_state = STATE_IDLE;

    const spot_cfg_t* cfg = &SPOT_CFG[g_spot];
    g_wait_target_ms = cfg->wait_min_ms + rng_range(cfg->wait_max_ms - cfg->wait_min_ms + 1);
    g_state = STATE_WAITING;
    g_state_enter_ms = g_last_tick_ms;
    return g_wait_target_ms;
}

fishing_event_t fishing_tick(int now_ms) {
    g_last_tick_ms = now_ms;
    fishing_event_t evt = EVT_NONE;

    switch (g_state) {
        case STATE_WAITING: {
            int elapsed = now_ms - g_state_enter_ms;
            if (elapsed >= g_wait_target_ms) {
                /* 鱼咬钩！进入限时窗口 */
                g_bite_window_ms = SPOT_CFG[g_spot].bite_window_ms;
                g_state = STATE_BITE;
                g_state_enter_ms = now_ms;
                evt = EVT_BITE_START;
            }
            break;
        }
        case STATE_BITE: {
            int elapsed = now_ms - g_state_enter_ms;
            if (elapsed >= g_bite_window_ms) {
                /* 没及时提竿 -> 跑鱼 */
                g_state = STATE_MISS;
                g_state_enter_ms = now_ms;
                evt = EVT_BITE_TIMEOUT;
            }
            break;
        }
        case STATE_CATCH:
        case STATE_MISS: {
            /* 结算态停留 1500ms 后自动回到待机 */
            int elapsed = now_ms - g_state_enter_ms;
            if (elapsed >= 1500) {
                g_state = STATE_IDLE;
                g_state_enter_ms = now_ms;
                evt = EVT_CAST_READY;
            }
            break;
        }
        default:
            break; /* IDLE / MENU：无时间驱动事件 */
    }
    return evt;
}

const catch_result_t* fishing_strike(void) {
    if (g_state != STATE_BITE) {
        return NULL; /* 不在咬钩窗口，提竿无效 */
    }
    int elapsed = g_last_tick_ms - g_state_enter_ms;
    if (elapsed < 0 || elapsed > g_bite_window_ms) {
        return NULL; /* 理论上不应发生，保险 */
    }

    /* 按饵料权重抽鱼大小 */
    const int* w = BAIT_WEIGHTS[g_bait];
    int total = w[0] + w[1] + w[2];
    int pick = rng_range(total);
    int size;
    if (pick < w[0])       size = 1;
    else if (pick < w[0] + w[1]) size = 2;
    else                   size = 3;

    int base = SIZE_BASE_SCORE[size - 1];
    /* 分值 = round(base * mult) */
    int mult100 = SPOT_CFG[g_spot].score_mult100;
    int score = (base * mult100 + 50) / 100;

    g_last_catch.fish_size   = size;
    g_last_catch.base_score  = base;
    g_last_catch.score       = score;

    g_score += score;
    if (g_score > g_high_score) g_high_score = g_score;

    g_state = STATE_CATCH;
    g_state_enter_ms = g_last_tick_ms;
    return &g_last_catch;
}

void fishing_get_status(fishing_status_t* out) {
    if (!out) return;
    out->state = g_state;
    out->score = g_score;
    out->high_score = g_high_score;
    out->time_in_state_ms = g_last_tick_ms - g_state_enter_ms;
    out->bite_window_ms = (g_state == STATE_BITE) ? g_bite_window_ms : 0;
    out->bait = g_bait;
    out->spot = g_spot;
}

int fishing_get_high_score(void) { return g_high_score; }
void fishing_set_high_score(int hs) { if (hs > g_high_score) g_high_score = hs; }
