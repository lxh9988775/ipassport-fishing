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
 * 另一条被钉死的东西是【方向】：按住 = 抬竿 = 捕捉区往屏幕上方走。
 *   reel_update() 里那对符号（+rise_spd / -fall_spd）在 v2 里是反的，
 *   按下去捕捉区往下钻、松手反而往上飘，与屏幕提示和设计稿全相反。
 *   用例 6/7 用 bar_top_y() 比【屏幕 y】，而不是比 bar_pos —— 符号再反一次就红。
 *
 * 帧长跟主循环走（收线期间 10ms，见 fishing.c 的 LOOP_REELING_MS），
 * 去抖帧数与帧长是绑在一起的：改帧长就要重算这里的期望值。
 *
 * 编译运行（需 cc）：
 *   cc -std=c11 -Wall -Wextra -Werror \
 *      tests/test_fishing_reel_hold.c main/fishing_logic.c -o /tmp/reel_hold
 *   /tmp/reel_hold
 */

#include <stdio.h>
#include <assert.h>
#include "fishing_logic.h"

#define FRAME_MS 10   /* 主循环在收线期间是 10ms 一帧，与 fishing.c 的 LOOP_REELING_MS 一致 */

static int g_now = 0;

/* 喂一帧：先给电平，再推进一个主循环周期（顺序与 game_task 里一致） */
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

/* 屏幕坐标：与 main/fishing.c 的轨道几何一致 ——
 *     top = TRK_Y + reel_bar_pos * TRK_H / 1000
 * 所以 bar_pos 越大 = 越靠屏幕下方。
 * 方向判据一律走这个函数，别直接比 bar_pos：这个映射被写反过一次，
 * 表现就是「按住反而往下钻」，光看 bar_pos 是看不出来的。 */
#define TRK_Y 46
#define TRK_H 224
static int bar_top_y(void) {
    return TRK_Y + bar_pos() * TRK_H / 1000;
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

    /* 3) 松手【1 帧】即生效（去抖已从 2 帧降到 1 帧）。
     *    为什么敢只留 1 帧：判电平的 ok_key_state() 用的是电压窗口，
     *    OK 是 447..1900mV、松开约 3300mV，中间隔着 1400mV 的空白带，
     *    而 bsp_button_read_mv() 是单次采样、噪声只有几十 mV —— 跨不过窗口边界，
     *    不存在"抖一帧就误判"。多留一帧只是把松手延迟翻倍（原来 2x20ms=40ms）。 */
    fishing_reel_hold_sample(false);
    assert(holding() == false);

    /* 4) 松手后立刻按回，同帧生效：两个方向都 0 延迟，手感才对称 */
    fishing_reel_hold_sample(true);
    assert(holding() == true);
    printf("[OK] 松手 1 帧（10ms）即生效，按回同样 0 延迟\n");

    /* 5) 双击序列（按下-松开-按下-松开）结束必须是松开 ——
     *    事件驱动时代这一串会被 DOUBLE_CLICK 吞掉，捕捉区贴顶下不来 */
    fishing_reel_hold_sample(true);
    fishing_reel_hold_sample(false);
    fishing_reel_hold_sample(true);
    fishing_reel_hold_sample(false);
    assert(holding() == false);
    printf("[OK] 双击序列结束是松开\n");

    /* 6) 长按 1000ms 后松手，捕捉区真的能落下来 —— 这是原来那个 bug 的主路径。
     *    1000ms 已经逼近 button 组件的 1500ms 长按阈值，是玩家的常态操作。
     *    同时钉死方向：按住 = 抬竿（屏幕 y 变小）、松手 = 落下（y 变大）。
     *    v2 里这一对符号写反过，按下去往下钻，玩起来就是「按了没反应 / 上不上下不下」。 */
    enter_reeling();
    const int y0 = bar_top_y();
    for (int i = 0; i < 100; i++) frame(true);      /* 按住 1000ms 往上抬 */
    const int y_hold = bar_top_y();
    assert(y_hold < y0);                            /* 抬竿：上沿 y 必须变小 */
    assert(holding() == true);
    frame(false);                                   /* 松手：1 帧去抖 */
    assert(holding() == false);
    for (int i = 0; i < 20; i++) frame(false);      /* 再 200ms */
    fishing_get_status(&st);
    assert(st.state == STATE_REELING);
    assert(bar_top_y() > y_hold);                   /* 落下：上沿 y 必须变大 */
    printf("[OK] 按住 1000ms 抬竿 y %d -> %d，松手回落到 y %d\n",
           y0, y_hold, bar_top_y());

    /* 7) 一直按住不会倒退：捕捉区单调上抬（y 单调不增），顶到轨道上端就停住 */
    enter_reeling();
    int prev_y = bar_top_y();
    for (int i = 0; i < 100; i++) {
        frame(true);
        const int cur = bar_top_y();
        assert(cur <= prev_y);
        prev_y = cur;
    }
    fishing_get_status(&st);
    assert(st.state == STATE_REELING);
    assert(prev_y <= TRK_Y + 1);                    /* 1000ms 足够顶到轨道上端 */
    printf("[OK] 持续按住 1000ms 捕捉区单调上抬至 y=%d（已到顶）\n", prev_y);

    /* 8) 跨局：上一局停在「按住」状态，新一局进来必须是没按住，
     *    且第一帧按下立即生效（去抖计数不能跨局残留） */
    enter_reeling();
    for (int i = 0; i < 50; i++) frame(true);
    assert(holding() == true);
    enter_reeling();
    assert(holding() == false);
    fishing_reel_hold_sample(true);
    assert(holding() == true);
    printf("[OK] 跨局复位：新一局未按住且按下立即生效\n");

    printf("\nALL TESTS PASSED\n");
    return 0;
}
