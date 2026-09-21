/*
 * 屏幕初始化：I2C + SSD1315/SSD1306 面板 + esp_lvgl_port
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2C 总线、单色 OLED 面板与 LVGL 移植层
 *
 * @param[out] out_disp 返回 LVGL display 句柄
 */
esp_err_t display_init(lv_display_t **out_disp);

/**
 * @brief 开关面板显示（熄屏 / 亮屏）
 *
 * 走 SSD1306 的关显示命令，只关掉驱动输出：GDDRAM 内容和 I2C 刷新都不受影响，
 * 因此重新打开时面板上就是 LVGL 最新的那一帧，不会先闪一下旧内容。
 *
 * @param[in] on true = 亮屏，false = 熄屏
 */
void display_set_on(bool on);

#ifdef __cplusplus
}
#endif
