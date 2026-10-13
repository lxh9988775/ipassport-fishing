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

/* 每张贴图对应的运行时 LVGL 图片描述符（惰性构建，用过即常驻） */
typedef struct {
    lv_image_dsc_t dsc;
    bool           ready;
} mage_img_t;

static mage_img_t s_imgs[MAGE_SPRITE_COUNT];

const mage_sprite_t *mage_sprite_def(const char *name)
{
    for (int i = 0; i < mage_sprite_count; i++) {
        if (strcmp(mage_sprites[i].name, name) == 0) {
            return &mage_sprites[i];
        }
    }
    return NULL;
}

const lv_image_dsc_t *mage_img(const char *name)
{
    for (int i = 0; i < mage_sprite_count; i++) {
        const mage_sprite_t *def = &mage_sprites[i];
        if (strcmp(def->name, name) != 0) {
            continue;
        }
        if (!s_imgs[i].ready) {
            mage_img_t *im = &s_imgs[i];
            memset(&im->dsc, 0, sizeof(im->dsc));
            im->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            im->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            im->dsc.header.flags  = LV_IMAGE_FLAGS_NONE;
            im->dsc.header.w      = def->w;
            im->dsc.header.h      = def->h;
            im->dsc.header.stride = LV_DRAW_BUF_STRIDE(def->w, LV_COLOR_FORMAT_RGB565);
            im->dsc.data_size     = (uint32_t)((size_t)def->w * def->h * sizeof(uint16_t));
            /* 生成器已输出小端 RGB565，可直接作为图像数据，无需逐像素转换 */
            im->dsc.data          = (const uint8_t *)def->rgb;
            im->ready             = true;
            ESP_LOGI(TAG, "贴图就绪 %s %ux%u", def->name, def->w, def->h);
        }
        return &s_imgs[i].dsc;
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
    const lv_image_dsc_t *img = mage_img(name);
    if (!img) {
        return false;
    }

    lv_obj_t *o = lv_image_create(parent);
    if (!o) {
        return false;
    }

    const float sq = (squash <= 0.0f) ? 1.0f : squash;
    const int h = (int)((float)h_pix * sq);
    int w = (int)((float)h * (float)img->w / (float)img->h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    lv_image_set_src(o, img);
    lv_obj_set_size(o, w, h);

    /* 锚点换算：脚底中心 -> 左上角 */
    const int x = cx - w / 2;
    const int y = foot_y - h;

    if (tilt_deg != 0.0f) {
        /* 旋转 pivot 放在脚底中心，模拟 HTML 里 translate(x,footY) 后再 rotate */
        lv_obj_set_style_transform_rotation(o, (int32_t)(tilt_deg * 10), 0);
        lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(o, h, 0);
        lv_obj_set_style_transform_angle(o, 0, 0);
    }

    lv_obj_set_pos(o, x, y);
    return true;
}

/* 画一张贴图铺满指定区域（左上角 + 宽高），用于全屏背景与标题/城镇主视觉。 */
bool mage_draw_bg(lv_obj_t *parent, const char *name, int x, int y, int w, int h)
{
    const lv_image_dsc_t *img = mage_img(name);
    if (!img) {
        return false;
    }
    lv_obj_t *o = lv_image_create(parent);
    if (!o) {
        return false;
    }
    lv_image_set_src(o, img);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    return true;
}

bool mage_draw_icon(lv_obj_t *parent, const char *name, int x, int y, int size)
{
    return mage_draw_bg(parent, name, x, y, size, size);
}
