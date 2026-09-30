/*
 * test_fishing_reel_hold.c - 收线「按住 / 松手」去抖单元测试（纯 C，无需硬件）
 *
 * 为什么专门给这几行加单测：
 *   模拟器和实机上都出过「捕捉区上不去下不来、按 OK 有延迟」。根因是收线
 *   用的是按键【事件】，而 BSP 只注册了 4 个回调（PRESS_DOWN / SINGLE_CLICK /
 *   DOUBLE_CLICK / LONG_PRESS_START），按住满 1500ms 后松手走的是
 *   PRESS_LONG_PRESS_UP_CHECK 分支，应用层一个事件都收不到 →
 *   g_holding 永远卡在 true，捕捉区贴顶再也下不来。
 *   现在改成【每帧喂电平】fishing_reel_hold_sample()，去抖逻辑一旦被改坏，
 *   那个「下不来」的 bug 会原样复活 —— 所以必须钉住。
 *
 * 编译运行（需 cc）：
 *   cc -std=c11 -Wall -Wextra -Werror \
 *      tests/test_fishing_reel_hold.c main/fishing_logic.c -o /tmp/reel_hold
 *   /tmp/reel_hold
 */

#include <stdio.h>
#include <assert.h>
#include "fishing_logic.h"

#define FRAME_MS 20   /* 主循环一帧 20ms，与 fishing.c 的 game_task 保持一致 */

static int g_now = 0;

/* 喂一帧：先给电平，再推进 20ms（顺序与 game_task 里一致） */
static void frame(bool ok_down) {
    fishing_reel_hold_sample(ok_down);
    g_now += FRAME_MS;
    fishing_tick(g_now);
}

/* 重开一局并推进到 STATE_REELING */
static void enter_reeling(void) {
    fishing_status_t st;

    g_now = 0;
    fishing_init(20260918u);
    fishing_set_spot(SPOT_POND);
    fishing_set_rod(ROD_HAND);
    fishing_set_bait(BAIT_WORM);
    fishing_cast();
    while (g_now < 200000) {
        g_now += FRAME_MS;
        if (fishing_tick(g_now) == EVT_BITE_START) break;
    }
    fishing_get_status(&st);
    assert(st.state == STATE_BITE);
    g_now += 50;
    fishing_strike();
    fishing_get_status(&st);
    assert(st.state == STATE_REELING);
}

static bool holding(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    return st.reel_holding;
}

static int bar_pos(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    return st.reel_bar_pos;
}

int main(void) {
    fishing_status_t st;

    /* 1) 刚进收线必须是「没按住」—— 否则捕捉区一进场就往上飘 */
    enter_reeling();
    assert(holding() == false);
    printf("[OK] 进入收线默认未按住\n");

    /* 2) 按下同帧生效：抬手那一下不能有延迟（手感最敏感的一环） */
    fishing_reel_hold_sample(true);
    assert(holding() == true);
    printf("[OK] 按下同帧生效（0 帧去抖）\n");

    /* 3) 单帧假松开不算：ADC 采到一次野值不能把竿子掉下去 */
    fishing_reel_hold_sample(false);
    assert(holding() == true);

    /* 4) 连续第二帧松开才真的松手 */
    fishing_reel_hold_sample(false);
    assert(holding() == false);
    printf("[OK] 松手需连续 2 帧（单帧野值被滤掉，<=40ms）\n");

    /* 5) 按住过程中插一帧假松开，不能掉链 */
    fishing_reel_hold_sample(true);
    fishing_reel_hold_sample(false);
    fishing_reel_hold_sample(true);
    assert(holding() == true);
    printf("[OK] 按住中插一帧假松开仍保持按住\n");

    /* 6) 双击序列（按下-松开-按下-松开）结束必须是松开 ——
     *    事件驱动时代这一串会被 DOUBLE_CLICK 吞掉，捕捉区贴顶下不来 */
    fishing_reel_hold_sample(false);
    fishing_reel_hold_sample(true);
    fishing_reel_hold_sample(false);
    fishing_reel_hold_sample(false);
    assert(holding() == false);
    printf("[OK] 双击序列结束是松开\n");

    /* 7) 长按 1000ms 后松手，捕捉区真的往下走 —— 这是原来那个 bug 的主路径。
     *    1000ms 已经逼近 button 组件的 1500ms 长按阈值，是玩家的常态操作。 */
    enter_reeling();
    const int pos0 = bar_pos();
    for (int i = 0; i < 50; i++) frame(true);      /* 按住 1000ms 往上抬 */
    const int top = bar_pos();
    assert(top > pos0);
    frame(false);
    frame(false);                                   /* 松手（2 帧去抖 = 40ms） */
    assert(holding() == false);
    for (int i = 0; i < 10; i++) frame(false);      /* 再 200ms */
    fishing_get_status(&st);
    assert(st.state == STATE_REELING);
    assert(bar_pos() < top);
    printf("[OK] 长按 1000ms 后松手能落下: %d -> %d -> %d\n", pos0, top, bar_pos());

    /* 8) 一直按住不会倒退：捕捉区单调上升，顶到轨道上限就停在那，不许掉头 */
    enter_reeling();
    int prev = bar_pos();
    for (int i = 0; i < 30; i++) {
        frame(true);
        const int cur = bar_pos();
        assert(cur >= prev);
        prev = cur;
    }
    fishing_get_status(&st);
    assert(st.state == STATE_REELING);
    printf("[OK] 持续按住 600ms 捕捉区单调上升至 %d\n", prev);

    /* 9) 跨局：上一局停在「按住」状态，新一局进来必须是没按住，
     *    且第一帧按下立即生效（去抖计数不能跨局残留） */
    enter_reeling();
    for (int i = 0; i < 20; i++) frame(true);
    assert(holding() == true);
    enter_reeling();
    assert(holding() == false);
    fishing_reel_hold_sample(true);
    assert(holding() == true);
    printf("[OK] 跨局复位：新一局未按住且按下立即生效\n");

    printf("\nALL TESTS PASSED\n");
    return 0;
}
