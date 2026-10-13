/* 魔法帝之路 —— 战斗演出层接口（1:1 复刻 HTML 实时战斗演出） */
#ifndef MAGE_FX_H
#define MAGE_FX_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 粒子类型 */
#define FX_K_PARTICLE 0
#define FX_K_RING     1
#define FX_K_CIRCLE   2
#define FX_K_CHARGE   3

/* 把演出层挂到指定父对象（一般是战斗专用容器），只做一次对象池构建 */
void mage_fx_attach(lv_obj_t *parent);

/* 演出层是否已构建 */
bool mage_fx_ready(void);

/* 进入/离开战斗时显示或隐藏（离开必须隐藏，否则会盖在后续页面上持续重绘） */
void mage_fx_show(bool on);

/* 每帧推进时间轴（背景滚动、跳跃、粒子、突进、受击），dt 单位秒 */
void mage_fx_tick(float dt);

/* 每帧绘制：只改对象属性，不新建对象（ESP32-C3 防闪屏铁律） */
void mage_fx_render(void);

/* 生成一个粒子 */
void mage_fx_spawn(int kind, float x, float y, int color, float r, float life);

/* 生成一条伤害飘字（1:1 复刻 HTML bt.nums）
 * worldX/worldY 为世界坐标（会减去 cam），value 为伤害数字，color 为字色。
 * 英雄受击传 (BTPX, 200)；敌人受击传 (e->x, 250-34)。 */
void mage_fx_num(int worldX, int worldY, int value, uint32_t color);

#ifdef __cplusplus
}
#endif
#endif
