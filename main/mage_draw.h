/* 魔法帝之路 —— 贴图绘制层接口 */
#ifndef MAGE_DRAW_H
#define MAGE_DRAW_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 必须 ≥ 实际贴图数（当前 10 张），用于静态数组容量 */
#define MAGE_SPRITE_COUNT 16

/* 按名字取 LVGL 图片描述符；不存在返回 NULL */
const lv_image_dsc_t *mage_img(const char *name);

/* 按名字取原始贴图定义（宽高 + RGB565 指针） */
const void *mage_sprite_def(const char *name);

/* 画立绘：脚底中心 (cx, foot_y) 为锚点，高 h_pix，垂直压扁 squash，倾斜 tilt_deg。
 * 返回 false = 贴图缺失，调用方走代码兜底。 */
bool mage_draw_sprite(lv_obj_t *parent, const char *name,
                      int cx, int foot_y, int h_pix,
                      float squash, float tilt_deg);

/* 铺图到 (x,y,w,h) 区域，用于全屏背景 */
bool mage_draw_bg(lv_obj_t *parent, const char *name, int x, int y, int w, int h);

/* 画正方形图标 */
bool mage_draw_icon(lv_obj_t *parent, const char *name, int x, int y, int size);

#ifdef __cplusplus
}
#endif
#endif
