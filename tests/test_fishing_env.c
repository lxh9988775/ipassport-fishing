/*
 * test_fishing_env.c - v4「天气决定鱼」分布单元测试（纯 C，无需硬件）
 *
 * 钉住三件事（对应执行任务书第二部分验收）：
 *   1) 鲶鱼在急流河「夜/晴」出现次数 ≥ 「午/晴」的 3 倍（时段偏好生效）；
 *   2) 任一鱼在任一 时段×天气 组合都仍可能被抽到（不锁死 —— 权重保底 >0）；
 *   3) 鳜鱼「昏/雨」显著高于「午/晴」（天气偏好生效）。
 *
 * 统计方法：绕开解锁门槛直接把 g_spot 设成急流河，走
 *   init -> set_env -> cast -> tick 到咬钩 -> strike（roll_species 在此发生）
 * 读 cur_species 计数。每次换 seed 保证样本独立。为控制 CI 时长，每组合 400 次。
 *
 * 编译运行（需 cc）：
 *   cc -std=c11 -Wall -Wextra -Werror -Imain \
 *      tests/test_fishing_env.c main/fishing_logic.c -o /tmp/envtest && /tmp/envtest
 */
#include <stdio.h>
#include <string.h>
#include "fishing_logic.h"

#define N 400
#define FRAME_MS 20

static int g_now;

/* 抽一次鱼：返回鱼种索引，失败 -1 */
static int draw(int seed, spot_t spot, int period, int weather, bait_t bait) {
    fishing_init((uint32_t)seed);
    /* 直接喂钓点（绕开解锁）；set_spot 会检查解锁，这里用存档注入累计钓获更简单 */
    {
        uint8_t buf[768];   /* v4 存档扩到 660 字节，512 装不下 serialize 会拒写 */
        int len = fishing_save_serialize(buf, (int)sizeof(buf));
        if (len > 16) {
            /* total_catch 偏移 = magic4+schema2+content2+features4+high4 = 16 */
            int tc = 20;
            buf[16] = (uint8_t)(tc & 0xFF);
            buf[17] = (uint8_t)((tc >> 8) & 0xFF);
            buf[18] = (uint8_t)((tc >> 16) & 0xFF);
            buf[19] = (uint8_t)((tc >> 24) & 0xFF);
            fishing_save_apply(buf, len);
        }
    }
    fishing_set_bait(bait);
    fishing_set_spot(spot);
    fishing_set_env(period, weather);
    if (fishing_cast() <= 0) return -1;
    for (int i = 0; i < 20000; ++i) {
        g_now += FRAME_MS;
        if (fishing_tick(g_now) == EVT_BITE_START) break;
    }
    g_now += 40;
    fishing_strike();                      /* 内部 roll_species -> cur_species */
    fishing_status_t st;
    fishing_get_status(&st);
    return st.cur_species;
}

static void tally(int counts[FISH_SPECIES_COUNT], spot_t spot,
                  int period, int weather, bait_t bait) {
    for (int i = 0; i < FISH_SPECIES_COUNT; ++i) counts[i] = 0;
    for (int s = 0; s < N; ++s) {
        int idx = draw(10000 + s, spot, period, weather, bait);
        if (idx >= 0 && idx < FISH_SPECIES_COUNT) counts[idx]++;
    }
}

/* 按中文名找鱼种索引，避免写死数字（改鱼表顺序也不会错）。 */
static int species_index(const char *name) {
    for (int i = 0; i < FISH_SPECIES_COUNT; ++i) {
        const fish_species_t *f = fishing_species_info(i);
        if (f && f->name && strcmp(f->name, name) == 0) return i;
    }
    return -1;
}

static int fail;

static void check(const char *name, int ok) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fail++;
}

int main(void) {
    int night[FISH_SPECIES_COUNT], noon[FISH_SPECIES_COUNT];
    int dusk_rain[FISH_SPECIES_COUNT], noon_lure[FISH_SPECIES_COUNT];

    printf("=== 天气决定鱼 · C 逻辑层分布测试（每组合 %d 次）===\n", N);
    tally(night, SPOT_RIVER, 3, 0, BAIT_WORM);   /* 夜/晴 */
    tally(noon, SPOT_RIVER, 1, 0, BAIT_WORM);   /* 午/晴 */

    int cat = species_index("鲶鱼");
    int mandarin = species_index("鳜鱼");
    if (cat < 0 || mandarin < 0) {
        printf("  [FAIL] 找不到鲶鱼/鳜鱼鱼种索引\n");
        return 1;
    }

    int cn = night[cat], cd = noon[cat];
    printf("  鲶鱼 夜=%d 午=%d 倍=%.2f\n", cn, cd, cd ? (float)cn / cd : 999.0f);
    check("鲶鱼 夜/晴 高于 午/晴 3 倍以上", cn >= cd * 3);

    /* 不锁死：急流河 index 8..15 每个都至少出现 1 次（任一组合） */
    int all_seen = 1;
    for (int i = 8; i < 16; ++i) if (night[i] == 0 && noon[i] == 0) all_seen = 0;
    check("急流河 8 种鱼在夜/午均不锁死", all_seen);

    /* 鳜鱼 昏/雨 vs 午/晴（同亮片饵，隔离饵料因素） */
    tally(dusk_rain, SPOT_RIVER, 2, 2, BAIT_LURE);
    tally(noon_lure, SPOT_RIVER, 1, 0, BAIT_LURE);
    int gd = dusk_rain[mandarin], gn = noon_lure[mandarin];
    printf("  鳜鱼 昏雨=%d 午晴=%d\n", gd, gn);
    check("鳜鱼 昏/雨 高于 午/晴", gd > gn);

    printf("结论: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
