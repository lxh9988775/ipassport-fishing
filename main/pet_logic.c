/*
 * pet_logic.c - 电子宠物「口袋兔」纯逻辑层实现（v4）
 *
 * 设计要点（对齐 fishing_logic.c 的工程约定）：
 *   - 零动态分配、零浮点：全部整数毫秒/点数
 *   - 衰减用"每点耗时"累加器实现，跨长 dt 也不会一次性跳变
 *   - 存档带 magic + schema_ver，字段只追加不搬家（v1/v2/v3 旧档自动迁移到 v4）
 *   - 贴纸簿 16 枚用 uint16 位图持久化（NVS 复用同一份存档 blob）
 *
 * v4（2026-10-29 微迭代）：
 *   - 多宠物：S_slots[PET_SLOT_MAX] + 宏 #define S S_slots[g_slot_active]，
 *     所有动作/衰减函数体零改动（pet_tick 衰减常量与逻辑 v2 起未动，本版依旧不动）
 *   - 每只宠物独立状态 + 名字（pet_name[16]）
 *   - 里程碑解锁第 2、3 槽（贴纸满 6 枚 / 到达 3 级）
 *   - 装扮扩容到 8 款、贴纸扩容到 16 枚
 *   - 首次启动轻引导标志 intro_done
 */

#include <stddef.h>
#include <string.h>
#include "pet_logic.h"

/* ===================== 衰减节奏（策划方案定死的适龄参数，v2 起从未改） =====================
 * 满状态离机后见到第一个"不开心"的时间（约）：
 *   饱食 100 点 × 40min/点 ≈ 2.8 天
 *   清洁 100 点 × 60min/点 ≈ 4.2 天
 *   玩乐 100 点 × 30min/点 ≈ 2.1 天
 *   精力 100 点 × 45min/点 ≈ 3.1 天
 * 刻意比真实电子宠物慢一个量级：孩子两天不玩，回来宠物只是"有点想你"。 */
#define HUNGER_MS_PER_POINT  (40u  * 60u * 1000u)
#define CLEAN_MS_PER_POINT   (60u  * 60u * 1000u)
#define FUN_MS_PER_POINT     (30u  * 60u * 1000u)
#define ENERGY_MS_PER_POINT  (45u  * 60u * 1000u)
/* 睡觉时：精力 3 分钟回 1 点（0→100 睡 5 小时），饱食衰减放缓一倍，清洁/玩乐冻结 */
#define SLEEP_ENERGY_MS_PER_POINT (3u * 60u * 1000u)

/* 动作收益 */
#define FEED_GAIN    30
#define CLEAN_GAIN   40
#define PLAY_GAIN    25
#define PLAY_COST_E  10
#define PLAY_COST_H  5
#define PLAY_MIN_E   15

#define XP_PER_CARE  5
#define XP_PER_PLAY  8
#define XP_PER_LEVEL 60

/* 贴纸里程碑阈值 */
#define FEED5_N  5
#define CLEAN5_N 5
#define PLAY5_N  5

/* ===================== 内部状态 ===================== */
typedef struct {
    int hunger, clean, fun, energy;
    bool sleeping;
    int xp, care_count;
    int feed_count, clean_count, play_count, sleep_count;  /* v2 分动作计数 */
    uint16_t sticker_mask;                                 /* v2 12→16 枚贴纸位图 */
    int costume;                                           /* v3 当前装扮 0..PET_COSTUME_MAX */
    char name[16];                                        /* v4 每只宠物名字（UTF-8，含结尾最多 16 字节） */
    int last_ms;
    bool inited;
    /* 衰减累加器（毫秒） */
    uint32_t acc_hunger, acc_clean, acc_fun, acc_energy;
} pet_state_t;

static pet_state_t S_slots[PET_SLOT_MAX];

/* v4：活动槽。所有沿用旧写法的函数体里的 S 都指向当前活动宠物，
   因此函数体零改动（含 pet_tick，衰减逻辑丝毫未动）。 */
static int g_slot_active = 0;
static bool g_intro_done = false;          /* 首次启动引导是否已看过（持久化） */
static int  g_last_slot_count = 1;         /* 上一次已解锁槽数，用于解锁提示 */
static uint16_t g_cos_worn[PET_SLOT_MAX];  /* 每只宠物穿过的装扮位图（PST_COS_ALL 用） */
static uint8_t g_season_seen = 0;          /* 经历过的季节位图 bit0..3（PST_SEASON4 用） */

/* 宏必须放在 S_slots / g_slot_active 声明之后；pet.c 不包含本宏（只调函数）。 */
#define S S_slots[g_slot_active]

static void acc_reset_all(void) {
    S.acc_hunger = S.acc_clean = S.acc_fun = S.acc_energy = 0;
}

static void slot_reset(int i) {
    pet_state_t *s = &S_slots[i];
    memset(s, 0, sizeof(*s));
    s->hunger = 80; s->clean = 80; s->fun = 70; s->energy = 90;
    s->sleeping = false;
    s->last_ms = 0;
    s->inited = true;
    /* 默认名字"团子"（UTF-8：e5 9b 9b e5 ad 90） */
    s->name[0] = (char)0xE5; s->name[1] = (char)0x9B; s->name[2] = (char)0x9B;
    s->name[3] = (char)0xE5; s->name[4] = (char)0xAD; s->name[5] = (char)0x90;
    s->name[6] = 0;
}

void pet_init(void) {
    for (int i = 0; i < PET_SLOT_MAX; ++i) slot_reset(i);
    g_slot_active = 0;
    /* 初次见面贴纸（仅当尚未记录过，避免每局重开后反复发） */
    pet_sticker_grant(PST_FIRST);
}

static int clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

static void grant_xp(int amount, pet_event_t *ev) {
    int old_level = S.xp / XP_PER_LEVEL;
    S.xp += amount;
    if (ev && *ev == PET_EVT_NONE && S.xp / XP_PER_LEVEL > old_level) {
        *ev = PET_EVT_LEVEL_UP;
    }
}

/* 单维度慢衰减：acc 累加 dt，每攒满 rate_ms 扣 1 点，扣到 0 为止 */
static void decay(int *val, uint32_t *acc, uint32_t dt, uint32_t rate_ms) {
    if (*val <= 0) { *acc = 0; return; }
    *acc += dt;
    while (*acc >= rate_ms && *val > 0) {
        *acc -= rate_ms;
        (*val)--;
    }
    if (*val <= 0) *acc = 0;
}

/* ===================== 贴纸 ===================== */
void pet_sticker_grant(unsigned id) {
    if (id >= PET_STICKER_MAX) return;
    S.sticker_mask |= (uint16_t)(1u << id);
}

bool pet_sticker_has(unsigned id) {
    if (id >= PET_STICKER_MAX) return false;
    return (S.sticker_mask & (uint16_t)(1u << id)) != 0;
}

int pet_sticker_count(void) {
    int c = 0;
    for (int i = 0; i < PET_STICKER_MAX; ++i)
        if (S.sticker_mask & (uint16_t)(1u << i)) c++;
    return c;
}

/* 照顾类里程碑：在动作计数变化后调用 */
static void check_care_stickers(void) {
    if (S.feed_count  >= FEED5_N)  pet_sticker_grant(PST_FEED5);
    if (S.clean_count >= CLEAN5_N) pet_sticker_grant(PST_CLEAN5);
    if (S.play_count  >= PLAY5_N)  pet_sticker_grant(PST_PLAY5);
    int lvl = S.xp / XP_PER_LEVEL + 1;
    if (lvl >= 2) pet_sticker_grant(PST_LVL2);
    if (lvl >= 3) pet_sticker_grant(PST_LVL3);
    /* v4 新增里程碑贴纸 */
    if (pet_sticker_count() >= 6) pet_sticker_grant(PST_STK6);
    if (lvl >= 5) pet_sticker_grant(PST_LV5);
}

pet_event_t pet_tick(int now_ms) {
    pet_event_t ev = PET_EVT_NONE;
    if (!S.inited) pet_init();

    int dt = now_ms - S.last_ms;
    if (dt < 0) dt = 0;                 /* 时钟回绕/首次：当 0 处理 */
    S.last_ms = now_ms;
    if (dt == 0) return PET_EVT_NONE;

    if (S.sleeping) {
        /* 睡觉：精力回复；饱食衰减放缓一倍；清洁/玩乐冻结 */
        uint32_t udt = (uint32_t)dt;
        S.acc_energy += udt;
        while (S.acc_energy >= SLEEP_ENERGY_MS_PER_POINT && S.energy < 100) {
            S.acc_energy -= SLEEP_ENERGY_MS_PER_POINT;
            S.energy++;
        }
        if (S.energy >= 100) {
            S.sleeping = false;
            acc_reset_all();
            S.sleep_count++;
            pet_sticker_grant(PST_SLEEP1);
            ev = PET_EVT_SLEEP_FULL;    /* 睡饱自动醒 */
        }
        decay(&S.hunger, &S.acc_hunger, udt, HUNGER_MS_PER_POINT * 2u);
    } else {
        uint32_t udt = (uint32_t)dt;
        decay(&S.hunger, &S.acc_hunger, udt, HUNGER_MS_PER_POINT);
        decay(&S.clean,  &S.acc_clean,  udt, CLEAN_MS_PER_POINT);
        decay(&S.fun,    &S.acc_fun,    udt, FUN_MS_PER_POINT);
        decay(&S.energy, &S.acc_energy, udt, ENERGY_MS_PER_POINT);
    }
    return ev;
}

/* ===================== 动作 ===================== */
pet_event_t pet_feed(void) {
    if (!S.inited) pet_init();
    if (S.hunger >= 98) return PET_EVT_ACTION_FULL;
    S.hunger = clamp100(S.hunger + FEED_GAIN);
    S.fun = clamp100(S.fun + 2);
    S.care_count++;
    S.feed_count++;
    pet_event_t ev = PET_EVT_ACTION_OK;
    grant_xp(XP_PER_CARE, &ev);
    check_care_stickers();
    return ev;
}

pet_event_t pet_clean(void) {
    if (!S.inited) pet_init();
    if (S.clean >= 98) return PET_EVT_ACTION_FULL;
    S.clean = clamp100(S.clean + CLEAN_GAIN);
    S.care_count++;
    S.clean_count++;
    pet_event_t ev = PET_EVT_ACTION_OK;
    grant_xp(XP_PER_CARE, &ev);
    check_care_stickers();
    return ev;
}

pet_event_t pet_play(void) {
    if (!S.inited) pet_init();
    if (S.energy < PLAY_MIN_E) return PET_EVT_TOO_TIRED;
    S.fun = clamp100(S.fun + PLAY_GAIN);
    S.energy = clamp100(S.energy - PLAY_COST_E);
    S.hunger = clamp100(S.hunger - PLAY_COST_H);
    S.care_count++;
    S.play_count++;
    pet_event_t ev = PET_EVT_ACTION_OK;
    grant_xp(XP_PER_PLAY, &ev);
    check_care_stickers();
    return ev;
}

pet_event_t pet_toggle_sleep(void) {
    if (!S.inited) pet_init();
    if (S.sleeping) {
        S.sleeping = false;
        acc_reset_all();
        return PET_EVT_WAKE;
    }
    S.sleeping = true;
    acc_reset_all();
    return PET_EVT_SLEEP_START;
}

/* ===================== 小游戏奖励（只涨不扣） ===================== */
void pet_add_fun(int delta) {
    if (!S.inited) pet_init();
    S.fun = clamp100(S.fun + delta);
}

/* ===================== 换装（v3 起，v4 扩到 8 款） ===================== */
void pet_set_costume(int id) {
    if (!S.inited) pet_init();
    if (id < 0 || id > PET_COSTUME_MAX) id = 0;
    S.costume = id;
    if (id > 0) {
        g_cos_worn[g_slot_active] |= (uint16_t)(1u << id);
        /* 穿遍全部装扮（1..PET_COSTUME_MAX）→ 发里程碑贴纸 */
        if ((g_cos_worn[g_slot_active] & 0x1FEu) == 0x1FEu)
            pet_sticker_grant(PST_COS_ALL);
    }
}

int pet_get_costume(void) {
    if (!S.inited) pet_init();
    return S.costume;
}

/* ===================== 多宠物（v4） ===================== */
int pet_get_slot(void) { return g_slot_active; }

bool pet_slot_unlocked(int slot) {
    if (slot <= 0) return true;
    /* 解锁里程碑以"主宠物（槽 0）"的进度为权威，避免切到新槽后判定抖动 */
    int stk = 0;
    for (int i = 0; i < PET_STICKER_MAX; ++i)
        if (S_slots[0].sticker_mask & (uint16_t)(1u << i)) stk++;
    int lvl0 = S_slots[0].xp / XP_PER_LEVEL + 1;
    if (slot == 1) return stk >= 6;       /* 贴纸满 6 枚 → 解锁第 2 只 */
    if (slot == 2) return lvl0 >= 3;       /* 到达 3 级 → 解锁第 3 只 */
    return false;
}

int pet_slot_count(void) {
    int c = 1;
    for (int i = 1; i < PET_SLOT_MAX; ++i) if (pet_slot_unlocked(i)) c++;
    return c;
}

void pet_set_slot(int slot) {
    if (slot >= 0 && slot < PET_SLOT_MAX && pet_slot_unlocked(slot))
        g_slot_active = slot;
}

int pet_unlock_check(void) {
    int now = pet_slot_count();
    int newly = now - g_last_slot_count;
    if (newly < 0) newly = 0;
    g_last_slot_count = now;
    /* 新解锁的槽：补发"初次见面"贴纸，并解锁对应里程碑贴纸 */
    if (newly > 0) {
        for (int i = 1; i < PET_SLOT_MAX; ++i) {
            if (pet_slot_unlocked(i) && !(S_slots[i].sticker_mask & (1u << PST_FIRST)))
                pet_sticker_grant(PST_FIRST);
        }
        pet_sticker_grant(PST_STK6);   /* 解锁第 2 只的同一里程碑 */
    }
    return newly;
}

/* ===================== 命名（v4） ===================== */
void pet_set_name(const char *name) {
    if (!S.inited) pet_init();
    if (!name || !name[0]) name = "团子";
    int i = 0;
    for (; i < 15 && name[i]; ++i) S.name[i] = name[i];
    for (; i < 16; ++i) S.name[i] = 0;
}

const char *pet_get_name(void) {
    if (!S.inited) pet_init();
    if (S.name[0] == 0) return "团子";
    return S.name;
}

/* ===================== 首次启动轻引导（v4） ===================== */
bool pet_needs_intro(void) { return !g_intro_done; }
void pet_mark_intro_done(void) { g_intro_done = true; }

/* ===================== 季节联动（v4，供 C 视觉层调用） ===================== */
void pet_season_seen(int season) {
    if (season < 0 || season > 3) return;
    g_season_seen |= (uint8_t)(1u << season);
    if (g_season_seen == 0x0Fu) pet_sticker_grant(PST_SEASON4);
}

/* ===================== 查询 ===================== */
void pet_get_status(pet_status_t *out) {
    if (!out) return;
    if (!S.inited) pet_init();
    out->hunger = S.hunger;
    out->clean = S.clean;
    out->fun = S.fun;
    out->energy = S.energy;
    out->sleeping = S.sleeping;
    out->xp = S.xp;
    out->level = S.xp / XP_PER_LEVEL + 1;
    out->care_count = S.care_count;
    out->feed_count = S.feed_count;
    out->clean_count = S.clean_count;
    out->play_count = S.play_count;
    out->sleep_count = S.sleep_count;
}

const char *pet_mood_name(const pet_status_t *st) {
    if (!st) return "";
    if (st->sleeping) return "睡得香";
    int worst = st->hunger;
    if (st->clean < worst) worst = st->clean;
    if (st->fun < worst) worst = st->fun;
    if (st->energy < worst) worst = st->energy;
    if (worst < 20) return "不开心";
    if (worst < 45) return "还行";
    return "开心";
}

/* ===================== 存档（v4，含 v1/v2/v3 迁移） =====================
 * 二进制布局（小端，全手动读写，避免结构体对齐陷阱）：
 *   [0..3]   magic            u32
 *   [4]      schema_ver       u8  (=4)
 *   [5]      slot_active      u8
 *   [6]      intro_done       u8
 *   [7]      reserved         u8
 *   每槽 i (i=0..2) base = 8 + i*36：
 *     [0]    hunger   u8
 *     [1]    clean    u8
 *     [2]    fun      u8
 *     [3]    energy   u8
 *     [4]    sleeping u8
 *     [5..6]  xp       u16
 *     [7..8]  care     u16
 *     [9..10] feed     u16
 *     [11..12] clean   u16
 *     [13..14] play    u16
 *     [15..16] sleep   u16
 *     [17..18] sticker  u16
 *     [19]   costume  u8
 *     [20..35] name[16]  (UTF-8，未强制结尾，读取时补 0)
 *   总长 = 8 + 3*36 = 116 字节。
 */
#define SLOT_BYTES 36
#define SAVE_HDR   8
#define SAVE_SIZE  (SAVE_HDR + PET_SLOT_MAX * SLOT_BYTES)

static void put_u8(uint8_t *b, int off, uint8_t v) { b[off] = v; }
static void put_u16(uint8_t *b, int off, uint16_t v) {
    b[off] = (uint8_t)(v & 0xFF); b[off + 1] = (uint8_t)(v >> 8);
}
static void put_name(uint8_t *b, int off, const char *n) {
    for (int i = 0; i < 16; ++i) b[off + i] = (n && n[i]) ? (uint8_t)n[i] : 0;
}
static uint8_t  get_u8(const uint8_t *b, int off) { return b[off]; }
static uint16_t get_u16(const uint8_t *b, int off) {
    return (uint16_t)(b[off] | ((uint16_t)b[off + 1] << 8));
}

int pet_save_size(void) { return SAVE_SIZE; }

int pet_save_serialize(uint8_t *buf, int cap) {
    if (!buf || cap < SAVE_SIZE) return 0;
    memset(buf, 0, SAVE_SIZE);
    /* magic（小端） */
    buf[0] = (uint8_t)(PET_SAVE_MAGIC & 0xFF);
    buf[1] = (uint8_t)((PET_SAVE_MAGIC >> 8) & 0xFF);
    buf[2] = (uint8_t)((PET_SAVE_MAGIC >> 16) & 0xFF);
    buf[3] = (uint8_t)((PET_SAVE_MAGIC >> 24) & 0xFF);
    put_u8(buf, 4, PET_SCHEMA_VER);
    put_u8(buf, 5, (uint8_t)g_slot_active);
    put_u8(buf, 6, (uint8_t)(g_intro_done ? 1 : 0));
    put_u8(buf, 7, 0);
    for (int i = 0; i < PET_SLOT_MAX; ++i) {
        int base = SAVE_HDR + i * SLOT_BYTES;
        pet_state_t *s = &S_slots[i];
        put_u8(buf, base + 0, (uint8_t)s->hunger);
        put_u8(buf, base + 1, (uint8_t)s->clean);
        put_u8(buf, base + 2, (uint8_t)s->fun);
        put_u8(buf, base + 3, (uint8_t)s->energy);
        put_u8(buf, base + 4, s->sleeping ? 1 : 0);
        put_u16(buf, base + 5,  (uint16_t)s->xp);
        put_u16(buf, base + 7,  (uint16_t)s->care_count);
        put_u16(buf, base + 9,  (uint16_t)s->feed_count);
        put_u16(buf, base + 11, (uint16_t)s->clean_count);
        put_u16(buf, base + 13, (uint16_t)s->play_count);
        put_u16(buf, base + 15, (uint16_t)s->sleep_count);
        put_u16(buf, base + 17, s->sticker_mask);
        put_u8(buf, base + 19, (uint8_t)s->costume);
        put_name(buf, base + 20, s->name);
    }
    return SAVE_SIZE;
}

int pet_save_apply(const uint8_t *buf, int len) {
    /* 注意：不能直接用 len < SAVE_SIZE 拒绝。
       v1/v2/v3 旧档比现在短，若按当前长度卡，旧档会被判非法而静默丢弃（玩家进度清零）。
       存档字段历来只追加、从不搬家，前段布局完全一致，所以允许"短读"迁移。 */
    if (!buf || len <= 0) return -2;
    if (len < 5) return -2;   /* 至少需要 magic(4)+schema_ver(1) */

    uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                   | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint8_t ver = buf[4];
    if (magic != PET_SAVE_MAGIC) return -1;

    pet_init();   /* 全部槽先置默认（含槽 0 的初次见面贴纸），下面再被真实数据覆盖 */

    if (ver == PET_SCHEMA_VER) {
        /* 同版本：全量恢复（含名字/槽/引导标志） */
        if (len < SAVE_SIZE) return -2;   /* 声称 v4 却比 v4 还短，视为损坏 */
        for (int i = 0; i < PET_SLOT_MAX; ++i) {
            int base = SAVE_HDR + i * SLOT_BYTES;
            pet_state_t *s = &S_slots[i];
            s->hunger = clamp100(get_u8(buf, base + 0));
            s->clean  = clamp100(get_u8(buf, base + 1));
            s->fun    = clamp100(get_u8(buf, base + 2));
            s->energy = clamp100(get_u8(buf, base + 3));
            s->sleeping = get_u8(buf, base + 4) != 0;
            s->xp = get_u16(buf, base + 5);
            s->care_count = get_u16(buf, base + 7);
            s->feed_count = get_u16(buf, base + 9);
            s->clean_count = get_u16(buf, base + 11);
            s->play_count = get_u16(buf, base + 13);
            s->sleep_count = get_u16(buf, base + 15);
            s->sticker_mask = get_u16(buf, base + 17);
            s->costume = (get_u8(buf, base + 19) > PET_COSTUME_MAX) ? 0 : get_u8(buf, base + 19);
            for (int k = 0; k < 16; ++k) s->name[k] = (char)buf[base + 20 + k];
            s->name[15] = 0;   /* 确保结尾 */
            s->inited = true;
        }
        g_slot_active = get_u8(buf, 5);
        if (g_slot_active >= PET_SLOT_MAX) g_slot_active = 0;
        g_intro_done = get_u8(buf, 6) != 0;
        g_last_slot_count = pet_slot_count();
        return 0;
    }

    if (ver == 1 || ver == 2 || ver == 3) {
        /* v1/v2/v3 → v4 迁移：旧档只有单只宠物（槽 0），槽 1/2 保持默认。
           旧单宠布局（小端）：
             [5]hunger [6]clean [7]fun [8]energy [9]sleeping
             [10..11]xp [12..13]care [14..15]feed [16..17]clean
             [18..19]play [20..21]sleep [22..23]sticker [24]costume
           v1 没有分动作计数（feed/clean/play/sleep 在旧结构体里不存在或占 0），
           短读补 0 后自然是 0，无需特判版本。 */
        uint8_t ob[28];
        memset(ob, 0, sizeof(ob));
        size_t n = sizeof(ob);
        if ((size_t)len < n) n = (size_t)len;
        memcpy(ob, buf, n);

        pet_state_t *s = &S_slots[0];
        s->hunger = clamp100(get_u8(ob, 5));
        s->clean  = clamp100(get_u8(ob, 6));
        s->fun    = clamp100(get_u8(ob, 7));
        s->energy = clamp100(get_u8(ob, 8));
        s->sleeping = get_u8(ob, 9) != 0;
        s->xp = get_u16(ob, 10);
        s->care_count = get_u16(ob, 12);
        s->feed_count = get_u16(ob, 14);
        s->clean_count = get_u16(ob, 16);
        s->play_count = get_u16(ob, 18);
        s->sleep_count = get_u16(ob, 20);
        s->sticker_mask = get_u16(ob, 22);
        s->costume = (get_u8(ob, 24) > PET_COSTUME_MAX) ? 0 : get_u8(ob, 24);
        s->sticker_mask |= (uint16_t)(1u << PST_FIRST);   /* 确保初次见面贴纸 */
        s->inited = true;
        /* 槽 1、2 保持 pet_init 给的默认（名字"团子"，无贴纸） */
        g_intro_done = true;     /* 老玩家：不再播首次引导 */
        g_slot_active = 0;
        g_last_slot_count = pet_slot_count();
        return 1;   /* 迁移成功 */
    }

    /* 比当前更新的版本：结构未知，当新号处理（不崩溃） */
    g_last_slot_count = pet_slot_count();
    return 1;
}

void pet_test_set_stats(int hunger, int clean, int fun, int energy) {
    if (!S.inited) pet_init();
    S.hunger = clamp100(hunger);
    S.clean = clamp100(clean);
    S.fun = clamp100(fun);
    S.energy = clamp100(energy);
}
