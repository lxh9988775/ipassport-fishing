/*
 * main/fap_screenshot.h
 *
 * FAP_SCREENSHOT_V1 串口截屏协议（固件侧）。
 * 主机通过 USB-CDC 发送 ASCII 行 "FAP_SCREENSHOT_V1\n"，
 * 设备用静态 240x320 RGB565LE 缓冲抓取当前屏，并以 512 字节块回传。
 *
 * 协议细节见 docs/reference/y2lin/serial-screenshot-protocol.md
 * （落地本实现的踩坑与坑位均已按该文档规避）。
 */
#ifndef FAP_SCREENSHOT_H
#define FAP_SCREENSHOT_H

/* 在 app_main 完成显示/LVGL 初始化后调用一次。 */
void fap_screenshot_init(void);

#endif /* FAP_SCREENSHOT_H */
