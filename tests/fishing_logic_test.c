/*
 * fishing_logic_test.c - 钓鱼逻辑层单元测试（纯 C，无需硬件）
 *
 * v2 重写说明：v1 时代 strike() 直接返回结果，v2 起提竿只是【进入收线小游戏】，
 * 真正的战果要等收线结束（EVT_CATCH）后用 fishing_last_catch() 读取。
 * 所以这里多了一个 play_reel()「模拟玩家」—— 没有它，v2 的任何得分/图鉴断言
 * 都跑不起来（旧文件就是因为没跟上这个变化而编译不过、被 validate.sh 漏掉）。
 *
 * 编译运行（需 cc）：
 *   cc -std=c11 -Wall -Wextra -Werror \
 *      tests/fishing_logic_test.c main/fishing_logic.c -o /tmp/fishtest
 *   /tmp/fishtest
 *
 * 覆盖：确定性、抛竿->咬钩->提竿->收线、窗口外提竿无效、上鱼结果合理性、
 *       图鉴记录、存档往返、关卡解锁、钓点倍率、饵料对鱼种分布的影响。
 */

#include <stdio.h>
#include <assert.h>
#include "fishing_logic.h"

#define FRAME_MS 20   /* 与 fishing.c 主循环一致 */

static int g_now = 0;

static fishing_status_t snap(void) {
    fishing_status_t s;
    fishing_get_status(&s);
    return s;
}

/* 推进到咬钩；成功返回 1 */
static int advance_to_bite(void) {
    while (g_now < 200000) {
        g_now += FRAME_MS;
        if (fishing_tick(g_now) == EVT_BITE_START) return 1;
    }
    return 0;
}

/*
 * 模拟一个手不错的玩家：把捕捉区中线带到鱼身上。
 * 方向必须是【按住 = 抬竿 = 捕捉区往屏幕上方走】：reel_bar_pos 越大越靠屏幕
 * 下方，所以鱼在捕捉区中线以上（pos 更小）就按住，以下就松手。
 * 中间留死区避免来回抽搐。返回 1=上鱼 0=跑鱼/超时。
 */
static int play_reel(int max_frames) {
    int holding_now = 0;
    for (int i = 0; i < max_frames; i++) {
        fishing_status_t s = snap();
        if (s.state != STATE_REELING) {
            return (s.state == STATE_CATCH) ? 1 : 0;
        }
        const int center = s.reel_bar_pos + s.reel_bar_h / 2;
        const int dead   = s.reel_bar_h / 5;   /* 死区：避免来回抽搐 */
        if (s.reel_fish_pos < center - dead)      holding_now = 1;  /* 鱼在上方 → 抬竿 */
        else if (s.reel_fish_pos > center + dead) holding_now = 0;  /* 鱼在下方 → 落竿 */
        fishing_reel_hold(holding_now);
        g_now += FRAME_MS;
        fishing_tick(g_now);
    }
    return 0;
}

/* 打完整一局：init -> 抛竿 -> 咬钩 -> 提竿 -> 收线。返回 1=钓上 */
static int run_round(uint32_t seed, spot_t spot, bait_t bait, rod_t rod) {
    g_now = 0;
    fishing_init(seed);
    fishing_set_spot(spot);      /* 未解锁的钓点会被静默忽略，所以先解锁 */
    fishing_set_bait(bait);
    fishing_set_rod(rod);
    if (fishing_cast() <= 0) return 0;
    if (!advance_to_bite()) return 0;
    g_now += 50;
    fishing_strike();
    if (snap().state != STATE_REELING) return 0;
    return play_reel(1500);      /* 最多 30 秒 */
}

/* 累计捕获数跨 init 保留，所以连打几局就能解锁深海 */
static void unlock_all_spots(void) {
    for (int i = 0; i < 60 && snap().total_catch < 20; i++) {
        run_round(1000u + (uint32_t)i, SPOT_POND, BAIT_WORM, ROD_HAND);
    }
}

int main(void) {
    /* 1) 确定性：同种子两次抛竿的等待时长必须一致（存档回放/复现 bug 的前提） */
    fishing_init(42);
    fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_POND);
    const int w1 = fishing_cast();
    fishing_init(42);
    fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_POND);
    const int w2 = fishing_cast();
    assert(w1 == w2 && w1 > 0);
    printf("[OK] 确定性: 同种子等待时长一致 (%d ms)\n", w1);

    /* 2) 抛竿 -> 咬钩 -> 窗口内提竿 -> 进入收线 */
    fishing_init(7);
    fishing_set_spot(SPOT_POND);
    fishing_cast();
    g_now = 0;
    assert(advance_to_bite() == 1);
    fishing_status_t s = snap();
    assert(s.state == STATE_BITE && s.bite_window_ms > 0);
    g_now += 50;
    fishing_strike();                       /* v2 起这里恒返回 NULL */
    assert(snap().state == STATE_REELING);
    printf("[OK] 咬钩窗口内提竿进入收线 (窗口=%d ms)\n", s.bite_window_ms);

    /* 3) 窗口外提竿无效：超时直接跑鱼，strike 拿不到任何东西 */
    fishing_init(7);
    fishing_set_spot(SPOT_POND);
    fishing_cast();
    g_now = 0;
    assert(advance_to_bite() == 1);
    g_now += snap().bite_window_ms + 100;
    fishing_tick(g_now);
    assert(snap().state == STATE_ESCAPE);
    assert(fishing_strike() == NULL);
    printf("[OK] 窗口外提竿无效（超时跑鱼）\n");

    /* 4) 数据表自检：全鱼种的体长/体重区间必须自洽。
     *    旗鱼的 80000g 曾经撑爆 uint16_t 被截断成 14464g（比下限 20000g 还小），
     *    图鉴里出现"鱼越大体重越轻" —— 这里扫全表，防止有人把字段改回 16 位。 */
    for (int i = 0; i < FISH_SPECIES_COUNT; i++) {
        const fish_species_t *fs = fishing_species_info(i);
        assert(fs != NULL);
        assert(fs->len_max_mm >= fs->len_min_mm);
        assert(fs->wgt_max_g  >= fs->wgt_min_g);
    }
    printf("[OK] 鱼种表自检: %d 种，体长/体重区间自洽\n", FISH_SPECIES_COUNT);

    /* 5) 上鱼结果合理性：鱼种合法、体长落在鱼种区间内、得分为正 */
    fishing_init(2024);
    assert(run_round(2024, SPOT_POND, BAIT_WORM, ROD_HAND) == 1);
    const catch_result_t *r = fishing_last_catch();
    assert(r != NULL);
    assert(r->species >= 0 && r->species < FISH_SPECIES_COUNT);
    const fish_species_t *f = fishing_species_info(r->species);
    assert(f != NULL);
    assert(r->len_mm >= (int)f->len_min_mm && r->len_mm <= (int)f->len_max_mm);
    /* 体重必须落在区间内 —— 旗鱼 80000g 曾经撑爆 uint16_t 被截断成 14464g，
     * 导致"鱼越大体重越轻"，这条就是那个 bug 的回归测试 */
    assert(r->wgt_g >= (int)f->wgt_min_g && r->wgt_g <= (int)f->wgt_max_g);
    assert(r->score > 0);
    printf("[OK] 上鱼结果: %s %dmm %dg %d分\n", f->name, r->len_mm, r->wgt_g, r->score);

    /* 6) 图鉴：捕获后必须能查到，且最大体长记录被刷新 */
    assert(fishing_codex_is_seen(r->species));
    assert(fishing_codex_count(r->species) >= 1);
    assert(fishing_codex_best_len(r->species) >= r->len_mm);
    printf("[OK] 图鉴记录: %s 已收录 %d 条，最大 %dmm\n",
           f->name, fishing_codex_count(r->species),
           fishing_codex_best_len(r->species));

    /* 7) 存档往返：序列化再灌回去，进度必须一模一样 */
    uint8_t buf[512];
    const int n = fishing_save_serialize(buf, (int)sizeof(buf));
    assert(n > 0 && n == fishing_save_size());
    const int tc_before = snap().total_catch;
    const int hs_before = fishing_get_high_score();
    fishing_init(999);                                  /* 换个种子打乱内存状态 */
    assert(fishing_save_apply(buf, n) == 0);            /* 0=成功，无迁移 */
    assert(snap().total_catch == tc_before);
    assert(fishing_get_high_score() == hs_before);
    assert(fishing_codex_is_seen(r->species));
    printf("[OK] 存档往返: %d 字节, 累计捕获=%d 最高分=%d\n", n, tc_before, hs_before);

    /* 8) 关卡解锁：累计捕获够了，深海就该开 */
    assert(tc_before >= 1);
    unlock_all_spots();
    assert(snap().total_catch >= 20);
    assert(fishing_spot_unlocked(SPOT_SEA));
    printf("[OK] 关卡解锁: 累计捕获=%d，深海已开\n", snap().total_catch);

    /* 9) 钓点倍率：深海的单位产出应显著高于静水塘（蒙特卡洛取平均） */
    const int N = 300;
    long pond_sum = 0, sea_sum = 0;
    int pond_n = 0, sea_n = 0;
    for (int i = 0; i < N; i++) {
        if (run_round(5000u + (uint32_t)i, SPOT_POND, BAIT_WORM, ROD_HAND)) {
            pond_sum += fishing_last_catch()->score; pond_n++;
        }
        if (run_round(5000u + (uint32_t)i, SPOT_SEA, BAIT_WORM, ROD_HAND)) {
            sea_sum += fishing_last_catch()->score; sea_n++;
        }
    }
    assert(pond_n > 0 && sea_n > 0);
    const double pond_avg = (double)pond_sum / pond_n;
    const double sea_avg  = (double)sea_sum  / sea_n;
    assert(sea_avg > pond_avg);
    printf("[OK] 钓点倍率: POND 均分=%.1f(%d/%d) SEA 均分=%.1f(%d/%d)\n",
           pond_avg, pond_n, N, sea_avg, sea_n, N);

    /* 10) 饵料影响鱼种分布：亮片在深海的稀有鱼占比应高于蚯蚓 */
    int worm_rare = 0, lure_rare = 0, worm_n = 0, lure_n = 0;
    for (int i = 0; i < N; i++) {
        if (run_round(7000u + (uint32_t)i, SPOT_SEA, BAIT_WORM, ROD_HAND)) {
            const fish_species_t *fw = fishing_species_info(fishing_last_catch()->species);
            if (fw->rarity >= RARITY_RARE) worm_rare++;
            worm_n++;
        }
        if (run_round(7000u + (uint32_t)i, SPOT_SEA, BAIT_LURE, ROD_HAND)) {
            const fish_species_t *fl = fishing_species_info(fishing_last_catch()->species);
            if (fl->rarity >= RARITY_RARE) lure_rare++;
            lure_n++;
        }
    }
    assert(worm_n > 0 && lure_n > 0);
    const double wr = (double)worm_rare / worm_n;
    const double lr = (double)lure_rare / lure_n;
    assert(lr > wr);
    printf("[OK] 饵料分布: 稀有鱼占比 WORM=%.2f LURE=%.2f\n", wr, lr);

    printf("\nALL TESTS PASSED\n");
    return 0;
}
