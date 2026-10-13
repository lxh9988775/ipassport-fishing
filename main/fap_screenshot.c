/*
 * main/fap_screenshot.c
 *
 * FAP_SCREENSHOT_V1 串口截屏协议固件侧实现。
 *
 * 主机（publisher.py capture-screen）经 USB-CDC 发送 ASCII 行：
 *     FAP_SCREENSHOT_V1\n
 * 设备回应：
 *     FAP_SCREENSHOT_V1 <w> <h> RGB565LE <bytes>\n
 *     紧接 <bytes> 个小端 RGB565 像素（行主序）。
 *
 * 通道说明（关键改动，解决 IDF 5.5.3 组件名问题）：
 *  本固件已开启 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG，控制台在启动时已把
 *  USB-CDC 挂载为 VFS 的 stdin/stdout。本模块直接对 fd 0 读、fd 1 写，
 *  等价使用同一根 CDC 通道，且无需 REQUIRES 那个无法被 CMake 解析的
 *  usb_serial_jtag 组件名。
 *
 * 设计要点（来自 docs/reference/y2lin/serial-screenshot-protocol.md）：
 *  - reader 任务优先级 3（低于 LVGL 的 4），空闲必 vTaskDelay，出错退避 200ms，绝不忙等。
 *  - 命令以滑动窗口做子串匹配，兼容 Windows 终端吞掉末尾换行的情况。
 *  - 帧缓冲静态 64 字节对齐、240x320x2=153600 字节，永不动用 LVGL/系统堆。
 *  - 仅渲染那一刻持有 LVGL 锁；回传以 512 字节块（< 1024 tx ring）流式写入。
 *  - 二进制窗口前静默全量日志（esp_log_level_set("*",NONE)），结束恢复 INFO，
 *    防止日志字节混入像素流导致主机按声明长度读偏。
 *  - 命令只读：任何失败只记录、不回任何字节，主机得到干净超时而非损坏流。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "others/snapshot/lv_snapshot.h"
#include "esp_log.h"
#include "bsp_display.h"
#include "fap_screenshot.h"

#include <unistd.h>
#include <string.h>

static const char *TAG = "fap_ss";

#define SS_W                240
#define SS_H                320
#define SS_CMD              "FAP_SCREENSHOT_V1"
#define SS_CMD_LEN          (sizeof(SS_CMD) - 1)
#define SS_BUF_BYTES        (SS_W * SS_H * 2)
#define SS_CHUNK            512
#define SS_TASK_PRIO        (3)          /* 低于 LVGL 的 4 */
#define SS_TASK_STACK       (8192)

/* 静态全屏 RGB565 缓冲（64 字节对齐，永不在运行时分配） */
static __attribute__((aligned(64))) uint8_t s_fb[SS_BUF_BYTES];
static lv_draw_buf_t s_drawbuf;
static bool s_buf_inited = false;

/* 滑动窗口匹配：保存最近 SS_CMD_LEN 个字节 */
static char s_win[SS_CMD_LEN];
static size_t s_wpos;

static void ss_send_bytes(const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > SS_CHUNK) chunk = SS_CHUNK;
        ssize_t r = write(STDOUT_FILENO, p + off, chunk);
        if (r <= 0) return;               /* 主机拔线：放弃传输但保留任务 */
        off += (size_t)r;
    }
}

/* 抓当前活动屏并以 RGB565LE 回传；任何失败都静默返回 */
static void ss_capture(void)
{
    if (!s_buf_inited) {
        ESP_LOGW(TAG, "drawbuf 未初始化，跳过截屏");
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "无默认显示，跳过截屏");
        return;
    }

    /* 仅渲染这一刻持锁 */
    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "LVGL 锁超时，跳过截屏");
        return;
    }
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_res_t res = lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &s_drawbuf);
    bsp_lvgl_unlock();

    if (res != LV_RES_OK) {
        ESP_LOGW(TAG, "快照渲染失败");
        return;
    }
    /* 防御性校验：确认真是我们那块全屏紧密 RGB565 缓冲 */
    if (s_drawbuf.data != (void *)s_fb ||
        s_drawbuf.header.cf != LV_COLOR_FORMAT_RGB565 ||
        (size_t)s_drawbuf.header.w != SS_W ||
        (size_t)s_drawbuf.header.h != SS_H) {
        ESP_LOGW(TAG, "快照缓冲异常，跳过");
        return;
    }

    uint32_t total = (uint32_t)SS_W * SS_H * 2;

    /* 进入二进制窗口：关闭全部日志，避免字节混入像素流 */
    esp_log_level_set("*", ESP_LOG_NONE);

    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr),
                     "FAP_SCREENSHOT_V1 %d %d RGB565LE %lu\n",
                     SS_W, SS_H, (unsigned long)total);
    ss_send_bytes((const uint8_t *)hdr, (size_t)hl);
    ss_send_bytes(s_fb, total);

    /* 退出窗口：恢复日志 */
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "已回传截屏 %dx%d RGB565LE", SS_W, SS_H);
}

static void ss_task(void *arg)
{
    (void)arg;
    uint8_t rb[64];
    while (1) {
        ssize_t n = read(STDIN_FILENO, rb, sizeof(rb));
        if (n < 0) {                      /* 错误：退避，绝不忙等 */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = (char)rb[i];
            if (c == '\n' || c == '\r') { /* 行终止符重置窗口 */
                s_wpos = 0;
                memset(s_win, 0, sizeof(s_win));
                continue;
            }
            s_win[s_wpos % SS_CMD_LEN] = c;
            s_wpos++;
            if (s_wpos >= SS_CMD_LEN &&
                memcmp(s_win, SS_CMD, SS_CMD_LEN) == 0) {
                s_wpos = 0;              /* 防残字节重复触发 */
                memset(s_win, 0, sizeof(s_win));
                ss_capture();
            }
        }
    }
}

void fap_screenshot_init(void)
{
    /* 初始化静态绘制缓冲（一次性，永不释放） */
    lv_draw_buf_init(&s_drawbuf, LV_COLOR_FORMAT_RGB565, SS_W, SS_H, s_fb, SS_BUF_BYTES);
    s_buf_inited = true;

    BaseType_t ok = xTaskCreate(ss_task, "fap_ss", SS_TASK_STACK, NULL, SS_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "无法创建截屏任务");
        return;
    }
    ESP_LOGI(TAG, "FAP_SCREENSHOT_V1 串口截屏就绪 (USB-CDC stdin/stdout, %dx%d)", SS_W, SS_H);
}
