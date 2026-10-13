/* 魔法帝之路 —— 贴图绘制层
 *
 * 把 assets/mage_sprites.c 里由 tools/gen_mage_sprites.py 生成的 RGB565 像素数据
 * 包装成 LVGL 图片源，供标题页、城镇、战斗立绘、头像等处绘制。
 *
 * 素材与 HTML 原版（mage-game/index.html）1:1 同一批图，不新增、不重绘。
 * 透明像素已在生成阶段按背景色 0x0d0b1a 预乘烘焙，故这里只需喂 RGB565。
 */
#include "mage_draw.h"
#include "mage_sprites.h"

#include "lvgl.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "mage_draw";

const void *mage_sprite_def(const char *name)
{
    for (int i = 0; i < mage_sprite_count; i++) {
        if (strcmp(mage_sprites[i].name, name) == 0) {
            return &mage_sprites[i];
        }
    }
    return NULL;
}

/* 直接返回生成器编译期静态初始化好的 lv_image_dsc_t。
 * 绝不在运行时 memset/逐字段赋值 —— 钓鱼游戏（assets/sprites）已验证
 * 只有「编译期静态初始化 + const uint8_t 数据」这条路在 ESP32-C3 上不崩。 */
const lv_image_dsc_t *mage_img(const char *name)
{
    for (int i = 0; i < mage_sprite_count; i++) {
        if (strcmp(mage_sprites[i].name, name) != 0) {
            continue;
        }
        const mage_sprite_t *def = &mage_sprites[i];
        if (!def->dsc) {
            ESP_LOGE(TAG, "贴图描述符为空 %s", name);
            return NULL;
        }
        ESP_LOGI(TAG, "贴图 %s %ux%u", def->name,
                 (unsigned)def->dsc->header.w, (unsigned)def->dsc->header.h);
        return def->dsc;
    }
    ESP_LOGW(TAG, "找不到贴图 %s", name ? name : "(null)");
    return NULL;
}

/* 画一张贴图到 (cx, footY)，脚底中心为锚点，高度 hPix，垂直压扁 squash。
 * 与 HTML drawSprite(key,x,footY,hPix,squash,tilt) 同语义：
 *   squash > 1 表示纵向压扁（落地弹跳感），tilt_deg 为倾斜角（击退/突进）。
 * 返回 false 表示贴图不存在，调用方应走代码绘制的兜底图形。 */
bool mage_draw_sprite(lv_obj_t *parent, const char *name,
                      int cx, int foot_y, int h_pix,
                      float squash, float tilt_deg)
{
    return mage_draw_sprite_ret(parent, name, cx, foot_y, h_pix, squash, tilt_deg, NULL);
}

bool mage_draw_sprite_ret(lv_obj_t *parent, const char *name,
                          int cx, int foot_y, int h_pix,
                          float squash, float tilt_deg, lv_obj_t **out)
{
    if (out) {
        *out = NULL;
    }
    const lv_image_dsc_t *img = mage_img(name);
    if (!img) {
        return false;
    }

    lv_obj_t *o = lv_image_create(parent);
    if (!o) {
        return false;
    }

    const float sq = (squash <= 0.0f) ? 1.0f : squash;
    int h = (int)((float)h_pix * sq);
    /* 宽高要从 header 里取：lv_image_dsc_t 本身没有直接的 w/h 成员 */
    int w = (int)((float)h * (float)img->header.w / (float)img->header.h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* 铁律：ESP32-C3 上绝不让 LVGL 做缩放变换。LVGL 的缩放路径要额外分配一块
     * 与目标同尺寸的临时 draw buffer（LV_MEM 只有几十 KB），
     * 分配失败返回 NULL 后被解引用 = Load access fault（MTVAL≈0x4）。
     * 所以目标尺寸必须与原图逐像素一致，否则直接放弃这张图。 */
    if (w != (int)img->header.w || h != (int)img->header.h) {
        lv_obj_delete(o);
        ESP_LOGW(TAG, "尺寸不匹配拒绝缩放 %s 目标%dx%d 原图%ux%u",
                 name, w, h, (unsigned)img->header.w, (unsigned)img->header.h);
        return false;
    }

    lv_image_set_src(o, img);
    lv_obj_set_size(o, w, h);

    /* 锚点换算：脚底中心 -> 左上角 */
    const int x = cx - w / 2;
    const int y = foot_y - h;

    /* 不做任何 transform 旋转：ESP32-C3 软件变换会打满单核导致发烫死机。
     * 需要倾斜时由调用方改 pos 近似。tilt_deg 参数保留仅为接口兼容。 */
    (void)tilt_deg;

    lv_obj_set_pos(o, x, y);
    if (out) {
        *out = o;
    }
    return true;
}

/* 画一张贴图铺满指定区域（左上角 + 宽高），用于全屏背景与标题/城镇主视觉。 */
bool mage_draw_bg(lv_obj_t *parent, const char *name, int x, int y, int w, int h)
{
    return mage_draw_bg_ret(parent, name, x, y, w, h, NULL);
}

bool mage_draw_bg_ret(lv_obj_t *parent, const char *name, int x, int y, int w, int h, lv_obj_t **out)
{
    if (out) {
        *out = NULL;
    }
    const lv_image_dsc_t *img = mage_img(name);
    if (!img) {
        return false;
    }
    lv_obj_t *o = lv_image_create(parent);
    if (!o) {
        return false;
    }
    /* 同 mage_draw_sprite_ret：背景图也必须与原图 1:1，禁止缩放 */
    if (w != (int)img->header.w || h != (int)img->header.h) {
        lv_obj_delete(o);
        ESP_LOGW(TAG, "背景尺寸不匹配拒绝缩放 %s 目标%dx%d 原图%ux%u",
                 name, w, h, (unsigned)img->header.w, (unsigned)img->header.h);
        return false;
    }
    lv_image_set_src(o, img);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    if (out) {
        *out = o;
    }
    return true;
}

bool mage_draw_icon(lv_obj_t *parent, const char *name, int x, int y, int size)
{
    return mage_draw_bg(parent, name, x, y, size, size);
}
