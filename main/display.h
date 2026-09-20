/*
 * 屏幕初始化：I2C + SSD1315/SSD1306 面板 + esp_lvgl_port
 */
#pragma once

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

#ifdef __cplusplus
}
#endif
