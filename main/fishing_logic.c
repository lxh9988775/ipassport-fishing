/*
 * fishing_logic.c - 钓鱼 v2 逻辑层实现
 *
 * 全部用整数 / 定点表达，无浮点、无动态分配（ESP32-C3 无 PSRAM、无 FPU）。
 * 收线小游戏统一使用 0..1000 的定点坐标系，UI 层按比例换算成像素。
 *
 * 本文件与 tools/verify_logic.py 是一一对应的参考实现，改这里请同步改那边。
 */

#include "fishing_logic.h"
#include <string.h>

/* ===================== 收线小游戏平衡参数（定点 0..1000） ===================== */
#define REEL_TRACK        1000   /* 轨道总长 */
#define REEL_PROGRESS_MAX 1000
#define REEL_PROGRESS_0   340    /* 起始进度（原 300：多留一截容错，别一失误就归零） */
#define REEL_GAIN         210    /* 鱼在区内时每秒涨的进度 */
#define REEL_DECAY_BASE   120    /* 区外时每秒掉的基础进度 */
#define REEL_DECAY_POWER  180    /* 掉速 = BASE + power * POWER / 100 */
#define REEL_THINK_MIN    300    /* 换目标的最短间隔 ms（原 260：换太勤看着像乱窜） */
#define REEL_THINK_VAR    380
#define REEL_TIMEOUT_MS   20000  /* 拉锯上限，避免无限僵持 */
#define REEL_PERFECT_MULT 150    /* 完美钓获 1.5x */

/* 松手去抖：主循环喂一帧电平，连续这么多帧都读到"松开"才认定松手（1 帧 = 10ms）。
 * 按下不走去抖（0 帧生效）——手感最敏感的是抬手那一下，不能延迟。
 *
 * 只用 1 帧就够：OK 窗口是 447..1900mV，松开态约 3300mV，中间隔着 1400mV，
 * 而 bsp_button_read_mv() 单次采样噪声只有几十 mV，跨不过窗口边界。
 * 原来 2 帧 × 20ms 主循环 = 40ms 松手延迟，是"抬手发钝"的主观来源之一。 */
#define REEL_RELEASE_FRAMES 1

/*
 * 难度模型的三个关键设计（数值由 tools/tune_balance.py 扫参得出，改之前先跑一遍）：
 *
 * 1) 每条鱼派生一个「挣扎强度」diff_rating(0..100)，游速 = SPD_BASE + rating*PER_DIFF。
 *    上限刻意压在鱼竿最大speed附近 —— 保证再猛的鱼也"追得上"，
 *    难度来自容错余量而不是"追不上"，这样难度曲线天然平滑，不会出现
 *    「常见鱼随便钓 / 传说鱼完全不可能」的断崖。
 * 2) 捕捉区带惯性（加速度有限），会过冲 —— 必须预判，这是操作手感的来源，
 *    也是"手快/手慢"能拉开差距的原因。
 * 3) 鱼到达目标点会歇一小会儿，稀有度越高越不肯歇 —— 给玩家的喘息节奏。
 */
#define REEL_FISH_SPD_BASE      310     /* 原 320；配合 PER_DIFF 下调，峰值鱼速 620→510/s */
#define REEL_FISH_SPD_PER_DIFF  2       /* 原 3 */
#define REEL_DIFF_BASE          20
#define REEL_DIFF_PER_RARITY    20
#define REEL_DIFF_DART          25      /* /100 * dart */
#define REEL_BAR_ACCEL          10000   /* 捕捉区加减速（单位/秒^2） */
#define REEL_DART_CHANCE_BASE   12      /* 冲刺概率 % + rarity*10 */
#define REEL_BURST_MULT_BASE    135     /* 原 155 */
#define REEL_BURST_MULT_STEP    12      /* 原 22 —— 传说鱼原本冲到 1370/s，捕捉区最快 680/s，纯追不上 */
#define REEL_BURST_MS_BASE      170     /* 冲刺时长 ms + rarity*STEP */
#define REEL_BURST_MS_STEP      70      /* 原 90 */
#define REST_CHANCE_BASE        48      /* 歇息概率 % - rarity*STEP */
#define REST_CHANCE_STEP        11

#define CATCH_SHOW_MS     2200   /* 结算展示时长 */
#define CAST_ANIM_MS      600    /* 抛竿动画时长 */

/* ===================== 数据表 ===================== */

/* 24 种鱼：体长/体重取常见真实区间，用于图鉴展示与计分 */
static const fish_species_t SPECIES[FISH_SPECIES_COUNT] = {
    /* --- 静水塘 SPOT_POND --- */
    {"白条",   "银白细长、成群游动的小鱼，出水最勤",   SPOT_POND,  RARITY_COMMON,   0x01,
     100, 200,    20,   80,  10,  30, 15},
    {"鲫鱼",   "圆扁身带银灰光泽，池塘里最好客的鱼",   SPOT_POND,  RARITY_COMMON,   0x03,
     120, 250,   100,  500,  15,  35, 20},
    {"罗非鱼", "体侧有暗色竖纹，适应力强、全年可钓",   SPOT_POND,  RARITY_COMMON,   0x03,
     150, 300,   200,  800,  18,  40, 25},
    {"鲢鱼",   "头大身白，爱在水面滤食，拉力绵长",     SPOT_POND,  RARITY_UNCOMMON, 0x02,
     300, 600,  1000, 3000,  25,  55, 45},
    {"鳊鱼",   "侧扁如刀、银光闪亮，翻身时有爆发力",   SPOT_POND,  RARITY_UNCOMMON, 0x02,
     250, 400,   500, 1500,  28,  60, 40},
    {"草鱼",   "青灰修长、力气极大，吃素却长得很壮",   SPOT_POND,  RARITY_UNCOMMON, 0x02,
     400, 800,  2000, 6000,  35,  65, 60},
    {"鲤鱼",   "金红鳞厚带短须，聪明谨慎的池中之王",   SPOT_POND,  RARITY_RARE,     0x03,
     350, 700,  1000, 5000,  45,  70, 70},
    {"青鱼",   "近黑的青灰巨物，专吃螺蚌，耐力惊人",   SPOT_POND,  RARITY_RARE,     0x03,
     500,1000,  3000,10000,  55,  60, 85},

    /* --- 急流河 SPOT_RIVER --- */
    {"马口鱼", "体侧一道红纹，抢食凶猛的小型猎手",     SPOT_RIVER, RARITY_COMMON,   0x05,
     100, 180,    30,  100,  16,  55, 25},
    {"黄颡鱼", "黄褐带斑、四根胡须，扎一手是它的本事", SPOT_RIVER, RARITY_COMMON,   0x01,
     120, 200,    50,  150,  20,  40, 30},
    {"翘嘴鱼", "嘴部上翘、红唇醒目，追击小鱼极快",     SPOT_RIVER, RARITY_UNCOMMON, 0x04,
     300, 600,   500, 2500,  38,  75, 55},
    {"鲈鱼",   "青绿身带竖斑，爆发力强、横冲直撞",     SPOT_RIVER, RARITY_UNCOMMON, 0x04,
     250, 500,   400, 2000,  42,  70, 60},
    {"鳜鱼",   "褐绿斑纹善于伪装，伏击型底栖猛鱼",     SPOT_RIVER, RARITY_RARE,     0x04,
     250, 450,   500, 2000,  52,  65, 65},
    {"鲶鱼",   "扁头阔口无鳞，夜里最活跃的大力士",     SPOT_RIVER, RARITY_RARE,     0x01,
     350, 700,  1000, 4000,  50,  45, 80},
    {"军鱼",   "鳞片粗硬泛光，急流里的银色坦克",       SPOT_RIVER, RARITY_RARE,     0x06,
     300, 600,  1000, 3000,  58,  60, 75},
    {"鳡鱼",   "尖头流线、水中猛兽，出手即绝杀",       SPOT_RIVER, RARITY_LEGEND,   0x04,
     600,1200, 5000,20000,  80,  85, 95},

    /* --- 深海 SPOT_SEA --- */
    {"小黄鱼", "通体金黄、肉质细嫩，近海最常见",       SPOT_SEA,   RARITY_COMMON,   0x01,
     150, 250,   100,  300,  24,  35, 30},
    {"带鱼",   "银亮如带、无尾鳍，游起来像把长刀",     SPOT_SEA,   RARITY_COMMON,   0x06,
     500,1000,   300, 1000,  30,  50, 45},
    {"鲅鱼",   "流线蓝背、性情凶猛，海上的追击者",     SPOT_SEA,   RARITY_UNCOMMON, 0x04,
     400, 800,   800, 3000,  45,  75, 65},
    {"比目鱼", "双眼同侧、贴沙而卧，擅长伪装伏击",     SPOT_SEA,   RARITY_UNCOMMON, 0x01,
     250, 500,   500, 2000,  40,  30, 50},
    {"石斑鱼", "厚颌带斑、守礁而生，力量与狡猾并存",   SPOT_SEA,   RARITY_RARE,     0x04,
     300, 600,  1000, 4000,  62,  70, 80},
    {"金枪鱼", "蓝背银腹、终日不停洄游的大洋猎手",     SPOT_SEA,   RARITY_LEGEND,   0x04,
     800,2000,10000,60000, 100,  80, 100},
    {"旗鱼",   "长吻如枪、背帆高耸，公认最快的鱼",     SPOT_SEA,   RARITY_LEGEND,   0x04,
    1000,2500,20000,80000, 120,  95, 100},
    {"小鲨鱼", "尖吻灰背、背鳍醒目，海里的顶级掠食者", SPOT_SEA,   RARITY_LEGEND,   0x04,
     600,1500,  5000,30000, 130,  90, 100},
};

/*
 * 三根竿必须有真实取舍，而不是"越贵越强"：
 *   手竿   —— 均衡，新手起步
 *   路亚竿 —— 捕捉区最小、最难，但起落最快、得分系数最高（高手赚钱用）
 *   海竿   —— 捕捉区最大、最稳（钓传说首选），但竿沉起落慢，得分系数一般
 * 数值由 tools/tune_balance.py 按「不同反应延迟 × 不同稀有度」胜率矩阵标定。
 */
/* bar_h 是捕捉区高度（轨道 0..1000 的占比）。
 * 比首版整体放大一档（+20）：区太薄时"看得见鱼却罩不住"，玩家会把
 * "手不够快"误读成"这游戏钓不上来"。放大后仍保留三根竿的取舍关系。 */
static const rod_cfg_t RODS[FISH_ROD_COUNT] = {
    {"手竿",   "轻便均衡，新手最好上手",    280, 680, 600, 100},
    {"路亚竿", "区小难控，但起落最快分最高", 235, 840, 740, 150},
    {"海竿",   "区大容错高，竿沉但最稳",    320, 480, 420, 115},
};

/* bite_window_ms 是提竿窗口。深海原为 800ms，反应慢一点就错过，
 * 对着小屏单手玩太苛刻，统一放宽一档（最短仍有 1.0s）。 */
static const spot_cfg_t SPOTS[FISH_SPOT_COUNT] = {
    {"静水塘", 1200, 3500, 1300, 100,   0},
    {"急流河", 1800, 5000, 1150, 150,   8},
    {"深海",   2500, 7000, 1000, 220,  20},
};

static const char *const RARITY_NAMES[FISH_RARITY_COUNT] = {
    "常见", "少见", "稀有", "传说"
};

/* 各钓点的稀有度权重（越大越容易出） */
static const uint16_t SPOT_RARITY_W[FISH_SPOT_COUNT][FISH_RARITY_COUNT] = {
    /* POND  */ {60, 28, 10,  2},
    /* RIVER */ {45, 32, 18,  5},
    /* SEA   */ {35, 30, 25, 10},
};

#define PREFERRED_BAIT_MULT 3   /* 用到偏好饵料时的权重倍数 */

/* ===================== 确定性 RNG（xorshift32） ===================== */
static uint32_t g_rng_state = 0x9E3779B9u;

static uint32_t rng_next(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

static int rng_range(int n) {
    if (n <= 0) return 0;
    return (int)(rng_next() % (uint32_t)n);
}

/* ===================== 内部状态 ===================== */
static game_state_t g_state = STATE_IDLE;
static int          g_score = 0;
static int          g_high_score = 0;
static bait_t       g_bait = BAIT_WORM;
static rod_t        g_rod  = ROD_HAND;
static spot_t       g_spot = SPOT_POND;

static int  g_state_enter_ms = 0;
static int  g_last_tick_ms   = 0;
static int  g_wait_target_ms = 0;
static int  g_bite_window_ms = 0;

/* 收线小游戏内部量 */
static int  g_fish_pos   = 500;
static int  g_fish_target= 500;
static int  g_fish_dir   = 1;
static int  g_bar_pos    = 330;
static int  g_bar_vel    = 0;    /* 捕捉区当前速度（带惯性） */
static int  g_bar_h      = 340;
static int  g_progress   = REEL_PROGRESS_0;
static bool g_holding    = false;
static int  g_rel_cnt    = 0;       /* 连续读到"松开"的帧数（去抖计数器） */
static bool g_contacted  = false;   /* 是否全程贴住（完美判定） */
static bool g_ever_out   = false;
static int  g_think_ms   = 0;       /* 下次换目标的时刻（绝对毫秒） */
static int  g_rest_until = 0;       /* 鱼歇息到何时（给玩家的喘息） */
static int  g_burst_until= 0;       /* 冲刺到何时 */
static int  g_burst_mult = 100;     /* 冲刺倍率(%) */

static int  g_cur_species = -1;
static int  g_cur_len_mm  = 0;
static int  g_cur_wgt_g   = 0;
static bool g_cur_perfect = false;
static catch_result_t g_last_catch;

/* 图鉴记录（容量按 CODEX_CAP 预留，v3 追加鱼种不搬家） */
static uint8_t  g_codex_cnt[FISHING_CODEX_CAP];
static uint8_t  g_codex_flags[FISHING_CODEX_CAP];   /* bit0 = 完美钓获达成 */
static uint16_t g_codex_best[FISHING_CODEX_CAP];    /* 最大体长 mm */
static int      g_total_catch = 0;
static int      g_codex_cursor = 0;
static fishing_mode_t g_mode = MODE_SINGLE;

/* ===================== 工具 ===================== */
static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* 体长偏向小值：两次随机相乘形成 skew */
static int roll_length(const fish_species_t *f) {
    int span = (int)f->len_max_mm - (int)f->len_min_mm;
    int r1 = rng_range(101);
    int r2 = rng_range(101);
    int frac = (r1 * r2) / 100;      /* 0..100，偏向小 */
    return (int)f->len_min_mm + span * frac / 100;
}

/* 按体重与体长线性对应 */
static int weight_from_len(const fish_species_t *f, int len_mm) {
    int span_l = (int)f->len_max_mm - (int)f->len_min_mm;
    if (span_l <= 0) return (int)f->wgt_min_g;
    int span_w = (int)f->wgt_max_g - (int)f->wgt_min_g;
    int rel = (len_mm - (int)f->len_min_mm) * 1000 / span_l;   /* 0..1000 */
    return (int)f->wgt_min_g + span_w * rel / 1000;
}

/* 按 钓点稀有度权重 + 饵料偏好 抽鱼种 */
static int roll_species(void) {
    const uint16_t *rw = SPOT_RARITY_W[g_spot];
    uint8_t bait_bit = (uint8_t)(1u << g_bait);
    long total = 0;
    int w[FISH_SPECIES_COUNT];
    for (int i = 0; i < FISH_SPECIES_COUNT; ++i) {
        const fish_species_t *f = &SPECIES[i];
        if (f->spot != (uint8_t)g_spot) { w[i] = 0; continue; }
        long v = rw[f->rarity];
        if (f->bait_mask & bait_bit) v *= PREFERRED_BAIT_MULT;
        w[i] = (int)v;
        total += v;
    }
    if (total <= 0) {  /* 兜底：抽不到就给本钓点第一条 */
        for (int i = 0; i < FISH_SPECIES_COUNT; ++i)
            if (SPECIES[i].spot == (uint8_t)g_spot) return i;
        return 0;
    }
    long pick = (long)rng_range((int)total);
    long acc = 0;
    for (int i = 0; i < FISH_SPECIES_COUNT; ++i) {
        if (w[i] <= 0) continue;
        acc += w[i];
        if (pick < acc) return i;
    }
    for (int i = FISH_SPECIES_COUNT - 1; i >= 0; --i)
        if (w[i] > 0) return i;
    return 0;
}

/* ===================== 生命周期 ===================== */
void fishing_init(uint32_t seed) {
    g_rng_state = (seed == 0) ? 0x9E3779B9u : seed;
    g_state = STATE_IDLE;
    g_score = 0;
    g_state_enter_ms = 0;
    g_last_tick_ms = 0;
    g_wait_target_ms = 0;
    g_bite_window_ms = 0;
    g_cur_species = -1;
    g_mode = MODE_SINGLE;
}

void fishing_set_bait(bait_t b) { if (b >= 0 && b < BAIT_COUNT) g_bait = b; }
void fishing_set_rod(rod_t r)   { if (r >= 0 && r < ROD_COUNT)  g_rod  = r; }
void fishing_set_spot(spot_t s) { if (s >= 0 && s < SPOT_COUNT && fishing_spot_unlocked(s)) g_spot = s; }

/* 菜单里"上下键换一项"的动作策略。放在逻辑层而不是 UI 层，是为了能用宿主单测
 * 钉住 —— 钓点这一项以前写在 fishing.c 里，静默跳过没解锁的钓点，玩家按住上下键
 * 画面纹丝不动，被当成"换不了钓点"（社区反馈）。 */
bool fishing_cycle_rod(int dir) {
    g_rod = (rod_t)(((int)g_rod + (dir >= 0 ? 1 : ROD_COUNT - 1)) % ROD_COUNT);
    return true;
}
bool fishing_cycle_bait(int dir) {
    g_bait = (bait_t)(((int)g_bait + (dir >= 0 ? 1 : BAIT_COUNT - 1)) % BAIT_COUNT);
    return true;
}
/* 只在已解锁的钓点之间跳；一个都跳不动时原样返回 false（UI 据此给"还差几条"的提示） */
bool fishing_cycle_spot(int dir) {
    int step = (dir >= 0) ? 1 : -1;
    for (int k = 1; k <= SPOT_COUNT; ++k) {
        int v = ((int)g_spot + step * k + SPOT_COUNT * k) % SPOT_COUNT;
        if (v != (int)g_spot && fishing_spot_unlocked((spot_t)v)) {
            g_spot = (spot_t)v;
            return true;
        }
    }
    return false;
}

void fishing_enter_menu(void) {
    /* 除了 IDLE，CASTING/WAITING/BITE 也放进来 —— 这三态是"长按开始抛竿、
     * 还没上鱼"的中途状态：玩家想开菜单时，按下 OK 的那一瞬间就已经抛竿了
     * （抛竿是按下即生效），等长按判定到点时状态早就不是 IDLE。
     * 撤回这三态不亏任何东西（还没进收线，图鉴和成绩都不受影响）。
     * 收线 / 结算中不给进：那是正在进行的对局，中途开菜单等于白送一条鱼。 */
    switch (g_state) {
        case STATE_IDLE:
        case STATE_CASTING:
        case STATE_WAITING:
        case STATE_BITE:
            g_state = STATE_MENU;
            break;
        default:
            break;
    }
}
void fishing_exit_menu(void) {
    if (g_state == STATE_MENU) g_state = STATE_IDLE;
}

int fishing_cast(void) {
    if (g_state != STATE_IDLE && g_state != STATE_MENU) return 0;
    const spot_cfg_t *cfg = &SPOTS[g_spot];
    g_state = STATE_CASTING;
    g_state_enter_ms = g_last_tick_ms;
    g_wait_target_ms = cfg->wait_min_ms + rng_range(cfg->wait_max_ms - cfg->wait_min_ms + 1);
    return g_wait_target_ms;
}

/*
 * 收线"按住"状态：由【每帧电平】驱动，而不是按键事件。
 *
 * 为什么必须换掉事件驱动（三条都在实机上必然触发）：
 *   1) 长按后松手收不到事件 —— BSP 只注册了 PRESS_DOWN / SINGLE_CLICK /
 *      DOUBLE_CLICK / LONG_PRESS_START（components/bsp/src/bsp_button.c:118-124）。
 *      button 组件在按住满 1500ms 后松手走的是 PRESS_LONG_PRESS_UP_CHECK 分支，
 *      只发 PRESS_UP / LONG_PRESS_UP / PRESS_END，应用层一个都收不到 →
 *      g_holding 永远停在 true，捕捉区贴顶再也下不来。
 *      而单根竿扫完整条轨道要 1.0~1.6 秒，长按 1.5 秒是常态玩法。
 *   2) 松手延迟 —— SINGLE_CLICK 要松手后再等 short_press_time(默认 180ms)
 *      加释放去抖 ~10ms 才发出来，手感上是"松开半拍"。
 *   3) 双击吞事件 —— 间隔 <180ms 的两次按只发 DOUBLE_CLICK，同样拿不到松手。
 *   4) 提竿那一下的 PRESS 被 fishing_strike() 吃掉（UI 层按当前状态分发），
 *      所以提竿后按住不放时这一下 never 会调用到 hold(true) → 捕捉区根本不抬。
 *
 * 改成电平驱动后四条一起消失：UI 层每帧直接把"OK 现在是否被按住"喂进来，
 * 长按多久都不影响松手判定（<=REEL_RELEASE_FRAMES 帧），也不依赖任何事件。
 *
 * 去抖为什么不对称：
 *   按下立即生效（抬手/落手是手感最敏感的一环）；松开要连续 2 帧，
 *   用来滤掉 bsp_button_read_mv() 单次采样的野值。
 */
void fishing_reel_hold_sample(bool down) {
    if (down) {
        g_holding = true;
        g_rel_cnt = 0;
        return;
    }
    if (g_rel_cnt < REEL_RELEASE_FRAMES) g_rel_cnt++;
    if (g_rel_cnt >= REEL_RELEASE_FRAMES) g_holding = false;
}

/* 直接落值（不去抖）：ADC 不可用时的兜底路径与单元测试走这里。 */
void fishing_reel_hold(bool down) {
    g_holding = down;
    g_rel_cnt = down ? 0 : REEL_RELEASE_FRAMES;
}

/* ===================== 长按 OK（电平驱动，全状态共用） =====================
 *
 * 阈值：明显长于一次正常点按（几百毫秒以内，而且长按计时会随按下沿重新开始），
 * 又远短于 button 组件默认的 long_press_time（bsp_button.c 用默认值，
 * 按满约 1.5 秒才发 LONG_PRESS_START）—— 玩家按下去到菜单出来不该干等一秒半。 */
#define HOLD_LONG_MS 700

/*
 * 原来"长按开菜单"是等 BSP 的 BSP_BTN_LONG 事件，这条路在 IDLE 下必然走不到：
 *   - OK 按下那一瞬间 BSP_BTN_PRESS 先到，fishing_cast() 已经把状态推成 CASTING；
 *   - 1.5 秒后 LONG_PRESS_START 才来，此时状态早已不是 IDLE，而调用点的守卫
 *     正是 st.state == STATE_IDLE → 直接 return；fishing_enter_menu() 原先也
 *     只认 STATE_IDLE。⇒ 长按只会抛竿，菜单永远打不开（社区审核据此打回）。
 *
 * 现在改成和收线同源的【每帧电平】判定，好处有三：
 *   1) 不依赖组件发什么事件，也不受 long_press_time 影响，阈值自己说了算；
 *   2) 不占用 fishing_cast()，抛竿仍然按下即生效，手感一点没变；
 *   3) 报一次就锁住（g_hold_fired），手指不松不会再报 —— 所以不会出现
 *      "菜单刚开出来又被同一次长按关掉"的闪一下。
 * UI 层负责按页面分发（钓场开菜单 / 菜单返回 / 图鉴返回），逻辑层只管计时。
 */
static bool g_hold_down;
static bool g_hold_fired;
static int  g_hold_start_ms;

void fishing_hold_reset(void) {
    g_hold_down     = false;
    g_hold_fired    = false;
    g_hold_start_ms = 0;
}

bool fishing_hold_sample(bool down, int now_ms) {
    if (!down) {
        /* 只清"正在计时"。【不清 g_hold_fired】—— 松手之后 iot_button 还会
         * 补发一个 SINGLE_CLICK，UI 层要靠这个标记把它认出来吞掉，
         * 详见 fishing_hold_fired() 的注释。 */
        g_hold_down = false;
        return false;
    }
    if (!g_hold_down) {                /* 这一帧是本次按住的起点，重新计时 */
        g_hold_down     = true;
        g_hold_fired    = false;
        g_hold_start_ms = now_ms;
        return false;
    }
    if (g_hold_fired) return false;    /* 已经报过，松手前不再报 */
    if (now_ms - g_hold_start_ms < HOLD_LONG_MS) return false;
    g_hold_fired = true;
    return true;
}

/* 取用"这次单击是不是长按补发的"。
 *
 * 为什么要留到松手之后：
 *   iot_button 只在按满 long_press_time(1.5s) 时才发 LONG_PRESS_START；
 *   而我们的阈值是 0.7s —— 玩家按 0.7~1.5 秒松手时，组件眼里这只是一次
 *   "普通点击"，于是松手后又补发一个 SINGLE_CLICK。菜单刚被长按开出来，
 *   这个迟到的单击就会被当成「OK 确认」，正好落在第 0 项「开始钓鱼」上：
 *   菜单一闪即关，还顺手抛了一竿。模拟器实测复现，真机同理。
 *
 * 返回 true 表示"这次单击要丢掉"（并且顺手复位，只吞一次）；
 * 返回 false 表示是玩家真实的一次点按，正常处理。 */
bool fishing_hold_take_stale_click(void) {
    if (!g_hold_fired) return false;
    g_hold_fired = false;
    return true;
}

/* ===================== 收线小游戏 ===================== */
static void reel_enter_impl(void) {
    const fish_species_t *f = &SPECIES[g_cur_species];
    const rod_cfg_t *r = &RODS[g_rod];

    g_bar_h    = (int)r->bar_h;
    g_fish_pos = 300 + rng_range(400);          /* 300..700 起始位置 */
    g_fish_target = g_fish_pos;
    g_bar_pos  = clampi(g_fish_pos - g_bar_h / 2, 0, REEL_TRACK - g_bar_h); /* 起始就贴住 */
    g_bar_vel  = 0;
    g_progress = REEL_PROGRESS_0;
    g_holding  = false;
    g_rel_cnt  = 0;                   /* ★ 不复位会把上一局的去抖计数带进来 */
    g_ever_out = false;
    g_contacted = true;
    g_burst_until = 0;
    g_burst_mult  = 100;
    g_rest_until  = 0;
    g_think_ms = g_last_tick_ms + REEL_THINK_MIN + rng_range(REEL_THINK_VAR);
    (void)f;
}

/*
 * 收线推进：dt_ms 毫秒。
 * 返回 1=上鱼 0=继续 -1=跑鱼
 */
static int reel_update(int dt_ms) {
    const fish_species_t *f = &SPECIES[g_cur_species];
    const rod_cfg_t *r = &RODS[g_rod];
    const int rar = (int)f->rarity;

    /* --- 1) 目标点更新：到达即换点，鱼几乎不停歇 --- */
    if (g_fish_pos == g_fish_target) {
        g_fish_target = 30 + rng_range(941);
    }
    if (g_last_tick_ms >= g_think_ms) {
        if (rng_range(100) < 10) {
            g_fish_target = g_fish_pos;
        } else {
            g_fish_target = 30 + rng_range(941);
        }
        /* 突进：偶尔冲刺一小段，逼玩家提前跟竿 */
        if (rng_range(100) < REEL_DART_CHANCE_BASE + rar * 10) {
            g_burst_mult  = REEL_BURST_MULT_BASE + rar * REEL_BURST_MULT_STEP;
            g_burst_until = g_last_tick_ms + REEL_BURST_MS_BASE + rar * REEL_BURST_MS_STEP;
        }
        int think_min = REEL_THINK_MIN - rar * 40;
        if (think_min < 140) think_min = 140;
        g_think_ms = g_last_tick_ms + think_min + rng_range(REEL_THINK_VAR);
    }

    /* --- 2) 游速：由"挣扎强度"决定，上限压在鱼竿能力附近，保证追得上 --- */
    int rating = REEL_DIFF_BASE + rar * REEL_DIFF_PER_RARITY + (int)f->dart * REEL_DIFF_DART / 100;
    if (rating > 100) rating = 100;
    int fspd = REEL_FISH_SPD_BASE + rating * REEL_FISH_SPD_PER_DIFF;
    if (g_last_tick_ms < g_burst_until) {
        fspd = fspd * g_burst_mult / 100;
    }

    /* --- 3) 移动（休息时不挪窝） --- */
    if (g_last_tick_ms < g_rest_until) {
        /* 歇脚中 */
    } else {
        if (g_fish_pos == g_fish_target) {
            int rest_chance = REST_CHANCE_BASE - rar * REST_CHANCE_STEP;
            if (rng_range(100) < rest_chance) {
                g_rest_until = g_last_tick_ms + 120 + rng_range(200);
            } else {
                g_fish_target = 30 + rng_range(941);
            }
        }
        int diff = g_fish_target - g_fish_pos;
        if (diff != 0) {
            int step = fspd * dt_ms / 1000;
            if (step < 1) step = 1;
            if (diff > 0) {
                g_fish_pos += (diff > step ? step : diff);
                g_fish_dir = 1;
            } else {
                g_fish_pos -= ((-diff) > step ? step : (-diff));
                g_fish_dir = -1;
            }
            g_fish_pos = clampi(g_fish_pos, 0, REEL_TRACK);
        }
    }

    /* --- 4) 捕捉区移动：带惯性，会过冲，所以必须预判 ---
     * 坐标约定（别改）：bar_pos 越大 = 越靠屏幕下方。
     *   fishing.c: top = TRK_Y + bar_pos * TRK_H / 1000
     * 所以「按住 = 抬竿 = 往屏幕上方走」必须是 bar_pos 减小。
     * 原先写的是 +rise_spd / -fall_spd，方向正好反了：按下去捕捉区往下钻、
     * 松手反而往上飘，跟屏幕提示「按住 OK 抬竿 · 松开落下」和设计稿里的
     * 「按住 OK = 捕捉区上升」全相反 —— 玩起来就是「按了没反应、上不上下不下」。
     * tests/test_fishing_reel_hold.c 里钉死了屏幕方向，改坏会立刻红。 */
    int tgt_v = g_holding ? -(int)r->rise_spd : (int)r->fall_spd;
    int dv = REEL_BAR_ACCEL * dt_ms / 1000;
    if (dv < 1) dv = 1;
    if (g_bar_vel < tgt_v) g_bar_vel += (tgt_v - g_bar_vel > dv ? dv : tgt_v - g_bar_vel);
    else                   g_bar_vel -= (g_bar_vel - tgt_v > dv ? dv : g_bar_vel - tgt_v);
    g_bar_pos += g_bar_vel * dt_ms / 1000;
    int clamped = clampi(g_bar_pos, 0, REEL_TRACK - g_bar_h);
    if (clamped != g_bar_pos) g_bar_vel = 0;   /* 撞到轨道端点就卸力 */
    g_bar_pos = clamped;

    /* --- 接触判定 --- */
    bool inside = (g_fish_pos >= g_bar_pos) && (g_fish_pos <= g_bar_pos + g_bar_h);
    if (inside) {
        g_progress += REEL_GAIN * dt_ms / 1000;
        g_contacted = true;
    } else {
        int decay = REEL_DECAY_BASE + (int)f->power * REEL_DECAY_POWER / 100;
        g_progress -= decay * dt_ms / 1000;
        if (g_contacted) g_ever_out = true;
    }
    g_progress = clampi(g_progress, 0, REEL_PROGRESS_MAX);

    if (g_progress >= REEL_PROGRESS_MAX) return 1;
    if (g_progress <= 0) return -1;
    return 0;
}

static void commit_catch(void) {
    fish_species_t *f = (fish_species_t *)&SPECIES[g_cur_species];
    const rod_cfg_t *r = &RODS[g_rod];
    const spot_cfg_t *s = &SPOTS[g_spot];

    int span_l = (int)f->len_max_mm - (int)f->len_min_mm;
    int size_f = (span_l <= 0) ? 100
               : 80 + 40 * ((g_cur_len_mm - (int)f->len_min_mm) * 100 / span_l) / 100;

    int sc = (int)f->base_score;
    sc = sc * (int)r->mult100 / 100;
    sc = sc * (int)s->mult100 / 100;
    sc = sc * size_f / 100;
    if (g_cur_perfect) sc = sc * REEL_PERFECT_MULT / 100;
    if (sc < 1) sc = 1;

    bool first = (g_codex_cnt[g_cur_species] == 0);
    bool record = (g_cur_len_mm > (int)g_codex_best[g_cur_species]);

    /* 更新图鉴 */
    if (g_codex_cnt[g_cur_species] < 255) g_codex_cnt[g_cur_species]++;
    if (record) g_codex_best[g_cur_species] = (uint16_t)g_cur_len_mm;
    if (g_cur_perfect) g_codex_flags[g_cur_species] |= 0x01u;
    g_total_catch++;

    g_score += sc;
    if (g_score > g_high_score) g_high_score = g_score;

    g_last_catch.species    = g_cur_species;
    g_last_catch.len_mm     = g_cur_len_mm;
    g_last_catch.wgt_g      = g_cur_wgt_g;
    g_last_catch.score      = sc;
    g_last_catch.perfect    = g_cur_perfect;
    g_last_catch.new_record = record;
    g_last_catch.first_catch= first;
}

/* ===================== tick 主循环 ===================== */
fishing_event_t fishing_tick(int now_ms) {
    int dt = now_ms - g_last_tick_ms;
    if (dt < 0) dt = 0;
    g_last_tick_ms = now_ms;
    fishing_event_t evt = EVT_NONE;

    switch (g_state) {
        case STATE_CASTING: {
            if (now_ms - g_state_enter_ms >= CAST_ANIM_MS) {
                g_state = STATE_WAITING;
                g_state_enter_ms = now_ms;
            }
            break;
        }
        case STATE_WAITING: {
            if (now_ms - g_state_enter_ms >= g_wait_target_ms) {
                g_bite_window_ms = SPOTS[g_spot].bite_window_ms;
                g_state = STATE_BITE;
                g_state_enter_ms = now_ms;
                evt = EVT_BITE_START;
            }
            break;
        }
        case STATE_BITE: {
            if (now_ms - g_state_enter_ms >= g_bite_window_ms) {
                g_cur_species = -1;
                g_state = STATE_ESCAPE;
                g_state_enter_ms = now_ms;
                evt = EVT_BITE_TIMEOUT;
            }
            break;
        }
        case STATE_REELING: {
            int r = reel_update(dt);
            if (r > 0) {
                g_cur_perfect = !g_ever_out;
                commit_catch();
                g_state = STATE_CATCH;
                g_state_enter_ms = now_ms;
                evt = EVT_CATCH;
            } else if (r < 0 || (now_ms - g_state_enter_ms) > REEL_TIMEOUT_MS) {
                g_state = STATE_ESCAPE;
                g_state_enter_ms = now_ms;
                evt = EVT_ESCAPE;
            }
            break;
        }
        case STATE_CATCH:
        case STATE_ESCAPE: {
            if (now_ms - g_state_enter_ms >= CATCH_SHOW_MS) {
                g_state = STATE_IDLE;
                g_state_enter_ms = now_ms;
                evt = EVT_BACK_IDLE;
            }
            break;
        }
        default: break;   /* IDLE / MENU / CODEX：无时间事件 */
    }
    return evt;
}

const catch_result_t *fishing_strike(void) {
    if (g_state != STATE_BITE) return NULL;
    int elapsed = g_last_tick_ms - g_state_enter_ms;
    if (elapsed < 0 || elapsed > g_bite_window_ms) return NULL;

    g_cur_species = roll_species();
    const fish_species_t *f = &SPECIES[g_cur_species];
    g_cur_len_mm = roll_length(f);
    g_cur_wgt_g  = weight_from_len(f, g_cur_len_mm);
    g_cur_perfect = false;

    reel_enter_impl();
    g_state = STATE_REELING;
    g_state_enter_ms = g_last_tick_ms;
    return NULL;   /* 真正的结果在收线结束后由 fishing_get_status / 内部缓存提供 */
}

/* ===================== 查询 ===================== */
void fishing_get_status(fishing_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = g_state;
    out->score = g_score;
    out->high_score = g_high_score;
    out->time_in_state_ms = g_last_tick_ms - g_state_enter_ms;
    out->bite_window_ms = (g_state == STATE_BITE) ? g_bite_window_ms : 0;
    out->bait = g_bait;
    out->rod  = g_rod;
    out->spot = g_spot;
    out->reel_fish_pos = g_fish_pos;
    out->reel_bar_pos  = g_bar_pos;
    out->reel_bar_h    = g_bar_h;
    out->reel_progress = g_progress;
    out->reel_holding  = g_holding;
    out->cur_species   = g_cur_species;
    out->cur_len_mm    = g_cur_len_mm;
    out->cur_wgt_g     = g_cur_wgt_g;
    out->total_catch   = g_total_catch;
    int seen = 0;
    for (int i = 0; i < FISH_SPECIES_COUNT; ++i) if (g_codex_cnt[i] > 0) seen++;
    out->codex_total = seen;
}

int  fishing_get_high_score(void) { return g_high_score; }
void fishing_set_high_score(int hs) { if (hs > g_high_score) g_high_score = hs; }

/* ===================== 图鉴 ===================== */
const fish_species_t *fishing_species_info(int species) {
    if (species < 0 || species >= FISH_SPECIES_COUNT) return NULL;
    return &SPECIES[species];
}
bool fishing_codex_is_seen(int species) {
    return species >= 0 && species < FISH_SPECIES_COUNT && g_codex_cnt[species] > 0;
}
int fishing_codex_count(int species) {
    if (species < 0 || species >= FISH_SPECIES_COUNT) return 0;
    return (int)g_codex_cnt[species];
}
bool fishing_codex_is_perfect(int species) {
    if (species < 0 || species >= FISH_SPECIES_COUNT) return false;
    return (g_codex_flags[species] & 0x01u) != 0;
}
int fishing_codex_best_len(int species) {
    if (species < 0 || species >= FISH_SPECIES_COUNT) return 0;
    return (int)g_codex_best[species];
}
const char *fishing_bait_name(bait_t b) {
    static const char *const N[FISH_BAIT_COUNT] = {"蚯蚓", "面团", "亮片"};
    return (b >= 0 && b < BAIT_COUNT) ? N[b] : "";
}
const char *fishing_rod_name(rod_t r) {
    return (r >= 0 && r < ROD_COUNT) ? RODS[r].name : "";
}
const char *fishing_spot_name(spot_t s) {
    return (s >= 0 && s < SPOT_COUNT) ? SPOTS[s].name : "";
}
const char *fishing_rarity_name(rarity_t r) {
    return (r >= 0 && r < RARITY_COUNT) ? RARITY_NAMES[r] : "";
}
bool fishing_spot_unlocked(spot_t s) {
    if (s < 0 || s >= SPOT_COUNT) return false;
    return g_total_catch >= (int)SPOTS[s].unlock_catch;
}
int fishing_spot_unlock_left(spot_t s) {
    if (s < 0 || s >= SPOT_COUNT) return 0;
    int left = (int)SPOTS[s].unlock_catch - g_total_catch;
    return left > 0 ? left : 0;
}
int fishing_next_locked_spot(void) {
    for (int i = 0; i < SPOT_COUNT; ++i) {
        if (!fishing_spot_unlocked((spot_t)i)) return i;
    }
    return -1;
}

void fishing_codex_open(void) {
    if (g_state == STATE_MENU || g_state == STATE_IDLE || g_state == STATE_CODEX_INFO) {
        g_state = STATE_CODEX;
        if (g_codex_cursor < 0 || g_codex_cursor >= FISH_SPECIES_COUNT) g_codex_cursor = 0;
    }
}
void fishing_codex_close(void) {
    if (g_state == STATE_CODEX || g_state == STATE_CODEX_INFO) g_state = STATE_MENU;
}
void fishing_codex_move(int delta) {
    g_codex_cursor = (g_codex_cursor + delta + FISH_SPECIES_COUNT) % FISH_SPECIES_COUNT;
}
void fishing_codex_toggle_info(void) {
    if (g_state == STATE_CODEX)      g_state = STATE_CODEX_INFO;
    else if (g_state == STATE_CODEX_INFO) g_state = STATE_CODEX;
}
int fishing_codex_cursor(void) { return g_codex_cursor; }

/* 供 UI 读取本次上鱼结果 */
const catch_result_t *fishing_last_catch(void) { return &g_last_catch; }

/* ===================== 版本化存档 ===================== */
/*
 * 布局（小端由目标机决定，这里只约定字段顺序）：
 *   u32 magic | u16 schema | u16 content | u32 features |
 *   i32 high  | u32 total  | u8 cnt[CAP] | u8 flag[CAP] | u16 best[CAP]
 * 旧版本只需把新字段按默认值补齐即可完成迁移，不会丢老玩家进度。
 */
#define SAVE_HDR (4 + 2 + 2 + 4 + 4 + 4)

int fishing_save_size(void) {
    return SAVE_HDR + FISHING_CODEX_CAP * (1 + 1 + 2);
}

int fishing_save_serialize(uint8_t *buf, int cap) {
    int need = fishing_save_size();
    if (!buf || cap < need) return 0;
    int p = 0;
    buf[p++] = (uint8_t)(FISHING_SAVE_MAGIC & 0xFF);
    buf[p++] = (uint8_t)((FISHING_SAVE_MAGIC >> 8) & 0xFF);
    buf[p++] = (uint8_t)((FISHING_SAVE_MAGIC >> 16) & 0xFF);
    buf[p++] = (uint8_t)((FISHING_SAVE_MAGIC >> 24) & 0xFF);
    buf[p++] = (uint8_t)(FISHING_SCHEMA_VER & 0xFF);
    buf[p++] = (uint8_t)((FISHING_SCHEMA_VER >> 8) & 0xFF);
    buf[p++] = (uint8_t)(FISHING_CONTENT_VER & 0xFF);
    buf[p++] = (uint8_t)((FISHING_CONTENT_VER >> 8) & 0xFF);
    uint32_t feat = FISHING_SUPPORTED_FEATURES;
    buf[p++] = (uint8_t)(feat & 0xFF);
    buf[p++] = (uint8_t)((feat >> 8) & 0xFF);
    buf[p++] = (uint8_t)((feat >> 16) & 0xFF);
    buf[p++] = (uint8_t)((feat >> 24) & 0xFF);
    int32_t hs = (int32_t)g_high_score;
    buf[p++] = (uint8_t)(hs & 0xFF);
    buf[p++] = (uint8_t)((hs >> 8) & 0xFF);
    buf[p++] = (uint8_t)((hs >> 16) & 0xFF);
    buf[p++] = (uint8_t)((hs >> 24) & 0xFF);
    uint32_t tc = (uint32_t)g_total_catch;
    buf[p++] = (uint8_t)(tc & 0xFF);
    buf[p++] = (uint8_t)((tc >> 8) & 0xFF);
    buf[p++] = (uint8_t)((tc >> 16) & 0xFF);
    buf[p++] = (uint8_t)((tc >> 24) & 0xFF);
    for (int i = 0; i < FISHING_CODEX_CAP; ++i) buf[p++] = g_codex_cnt[i];
    for (int i = 0; i < FISHING_CODEX_CAP; ++i) buf[p++] = g_codex_flags[i];
    for (int i = 0; i < FISHING_CODEX_CAP; ++i) {
        uint16_t v = g_codex_best[i];
        buf[p++] = (uint8_t)(v & 0xFF);
        buf[p++] = (uint8_t)((v >> 8) & 0xFF);
    }
    return p;
}

int fishing_save_apply(const uint8_t *buf, int len) {
    if (!buf) return -2;
    if (len < SAVE_HDR) return -2;
    int p = 0;
    uint32_t magic = 0;
    for (int i = 0; i < 4; ++i) magic |= ((uint32_t)buf[p++]) << (8 * i);
    if (magic != FISHING_SAVE_MAGIC) return -1;
    uint16_t schema = (uint16_t)(buf[p] | (buf[p + 1] << 8)); p += 2;
    /* content_ver 目前只用于展示/统计，不触发迁移 */
    p += 2;
    p += 4;   /* features 预留给未来按能力位裁剪 */
    if (schema > FISHING_SCHEMA_VER) return -1;   /* 来自更新的版本，不敢瞎读 */

    int32_t hs = 0;
    for (int i = 0; i < 4; ++i) hs |= ((int32_t)buf[p++]) << (8 * i);
    uint32_t tc = 0;
    for (int i = 0; i < 4; ++i) tc |= ((uint32_t)buf[p++]) << (8 * i);

    g_high_score = (int)hs;
    g_total_catch = (int)tc;
    for (int i = 0; i < FISHING_CODEX_CAP; ++i) g_codex_cnt[i]   = (p < len) ? buf[p++] : 0;
    for (int i = 0; i < FISHING_CODEX_CAP; ++i) g_codex_flags[i] = (p < len) ? buf[p++] : 0;
    for (int i = 0; i < FISHING_CODEX_CAP; ++i) {
        uint16_t v = 0;
        if (p + 1 < len) { v = (uint16_t)(buf[p] | (buf[p + 1] << 8)); p += 2; }
        g_codex_best[i] = v;
    }
    /* 迁移：本版只有 schema 1 一种结构，后续版本在此分支补齐新字段即可 */
    return (schema < FISHING_SCHEMA_VER) ? 1 : 0;
}
