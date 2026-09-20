/*
 * 状态栏界面：128x32 单色屏
 * 左侧 18px 状态图标 + 右侧两行文本，2 个页面轮播
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 页面编号 */
typedef enum {
    UI_PAGE_CTX = 0,    /* 上下文占用 + 进度条 */
    UI_PAGE_TOK,        /* 输入/输出 token + 速度 + 轮次 + 耗时 */
    UI_PAGE_COUNT,
} ui_page_t;

/**
 * @brief 构建界面并启动刷新定时器
 *
 * @param[in] disp display_init() 返回的 LVGL display
 */
void ui_init(lv_display_t *disp);

/**
 * @brief 上电自检：整屏点亮一小会儿再熄灭
 *
 * 用于确认「屏幕 + I2C + 刷新链路 + 单色转换」是否正常，
 * 由 CONFIG_STATUS_BAR_BOOT_SELFTEST 控制是否生效。
 */
void ui_boot_selftest(void);

/**
 * @brief 强制显示某一页
 *
 * @param[in] index   页面编号；传 -1 恢复自动轮播
 * @param[in] hold_ms 保持在指定页的时长（毫秒），0 表示不额外保持
 */
void ui_show_page(int index, uint32_t hold_ms);

/** @brief 当前正在显示的页面编号 */
int ui_current_page(void);

#ifdef __cplusplus
}
#endif
