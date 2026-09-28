/*
 * fishing_logic_test.c - 钓鱼逻辑层单元测试（纯 C，无需硬件）
 *
 * 编译运行（需 gcc）：
 *   gcc tests/fishing_logic_test.c main/fishing_logic.c -o /tmp/fishtest
 *   ./fishtest
 *
 * 覆盖：确定性、抛竿->咬钩、窗口内提竿成功、窗口外无效、
 *       计分倍率、最高分缓存、饵料分布趋势。
 */

#include <stdio.h>
#include <assert.h>
#include "fishing_logic.h"

/* 从当前时钟(*now)延续推进，直到进入咬钩窗口；返回是否成功 */
static int advance_to_bite(int* now, int step) {
    fishing_event_t e;
    while (*now < 200000) {
        *now += step;
        e = fishing_tick(*now);
        if (e == EVT_BITE_START) return 1;
    }
    return 0;
}

int main(void) {
    int now = 0;

    /* 1) 确定性：同种子两次 cast 的等待时长一致 */
    fishing_init(42);
    fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_POND);
    int w1 = fishing_cast();
    fishing_init(42);
    fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_POND);
    int w2 = fishing_cast();
    assert(w1 == w2);
    printf("[OK] 确定性: 同种子等待时长一致 (%d ms)\n", w1);

    /* 2) 抛竿后进入 WAITING，最终咬钩 */
    fishing_init(7);
    fishing_set_spot(SPOT_POND);
    fishing_cast();
    now = 0;
    int got = advance_to_bite(&now, 10);
    assert(got == 1);
    fishing_status_t st; fishing_get_status(&st);
    assert(st.state == STATE_BITE);
    printf("[OK] 等待后进入咬钩窗口 (窗口=%d ms)\n", st.bite_window_ms);

    /* 3) 窗口内提竿成功 */
    now += 50;
    const catch_result_t* r = fishing_strike();
    assert(r != NULL);
    assert(r->score > 0);
    printf("[OK] 窗口内提竿成功: 大小=%d 基础分=%d 实得分=%d\n",
           r->fish_size, r->base_score, r->score);

    /* 4) 窗口外提竿无效（重新抛竿并超时）*/
    fishing_init(7);
    fishing_set_spot(SPOT_POND);
    fishing_cast();
    now = 0;
    advance_to_bite(&now, 10);
    fishing_status_t s2; fishing_get_status(&s2);
    now += s2.bite_window_ms + 100;
    fishing_tick(now); /* 触发超时 -> MISS */
    const catch_result_t* r2 = fishing_strike();
    assert(r2 == NULL);
    printf("[OK] 窗口外提竿无效 (跑鱼)\n");

    /* 5) 计分：SEA 倍率应不低于 POND（同饵料同大小档时不变量）*/
    fishing_init(1);
    fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_POND);
    fishing_cast(); now=0; advance_to_bite(&now,10); now+=50;
    const catch_result_t* rp = fishing_strike();
    int pond_score = rp ? rp->score : -1;
    fishing_init(1);
    fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_SEA);
    fishing_cast(); now=0; advance_to_bite(&now,10); now+=50;
    const catch_result_t* rs = fishing_strike();
    int sea_score = rs ? rs->score : -1;
    assert(pond_score > 0 && sea_score > 0);
    assert(sea_score >= pond_score);
    printf("[OK] 计分倍率: POND=%d SEA=%d (SEA>=POND)\n", pond_score, sea_score);

    /* 6) 最高分缓存（连续多轮，验证 CATCH->IDLE 复位）*/
    fishing_init(3);
    fishing_set_spot(SPOT_SEA);
    now = 0;
    for (int i = 0; i < 5; i++) {
        assert(fishing_cast() > 0);                 /* 需处于 IDLE */
        advance_to_bite(&now, 10);                  /* 延续当前时钟 */
        now += 50; fishing_strike();
        now += 2000; fishing_tick(now);             /* CATCH 结算后回 IDLE */
    }
    int hs = fishing_get_high_score();
    assert(hs > 0);
    fishing_set_high_score(hs);
    assert(fishing_get_high_score() == hs);
    printf("[OK] 最高分缓存=%d\n", hs);

    /* 7) 饵料分布趋势：LURE 大鱼比例应高于 WORM（蒙特卡洛）*/
    int worm_big = 0, lure_big = 0; const int N = 4000;
    fishing_init(99); fishing_set_bait(BAIT_WORM); fishing_set_spot(SPOT_SEA);
    for (int i = 0; i < N; i++) {
        fishing_cast(); now=0; advance_to_bite(&now,10); now+=50;
        const catch_result_t* r3 = fishing_strike();
        if (r3 && r3->fish_size == 3) worm_big++;
    }
    fishing_init(99); fishing_set_bait(BAIT_LURE); fishing_set_spot(SPOT_SEA);
    for (int i = 0; i < N; i++) {
        fishing_cast(); now=0; advance_to_bite(&now,10); now+=50;
        const catch_result_t* r3 = fishing_strike();
        if (r3 && r3->fish_size == 3) lure_big++;
    }
    double wp = (double)worm_big / N, lp = (double)lure_big / N;
    printf("[OK] 大鱼比例 WORM=%.2f LURE=%.2f\n", wp, lp);
    assert(lp > wp);

    printf("\nALL TESTS PASSED\n");
    return 0;
}
