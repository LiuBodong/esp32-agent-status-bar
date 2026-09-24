/*
 * SK6812 RGBW 状态灯
 *
 * 把 agent 状态映射成一颗 RGBW 灯珠的灯效（呼吸 / 色相旋转 / 脉冲 / 快闪），
 * 屏幕之外也能一眼看出 agent 在干什么。
 *
 * 亮度有两套入口：
 *   - 编译期默认值：menuconfig -> Vibe Coding Status Bar -> Status LED -> brightness
 *   - 运行时热调：串口 {"cmd":"led","brightness":40,"on":true}（不落盘，重启回 Kconfig 默认）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RMT 灯带、跑上电自检、启动灯效任务
 *
 * 失败时只返回错误码、不 abort：led_strip 的初始化错误用 ESP_ERROR_CHECK 会被吞掉
 * （见 docs/led_ctrol.md 坑 3），调用方拿到错误码后自行决定要不要继续。
 *
 * @return ESP_OK 成功；其它值表示状态灯不可用（屏幕功能不受影响）
 */
esp_err_t status_led_init(void);

/** @brief 运行时设置亮度（0..255，0 = 灭） */
void status_led_set_brightness(uint8_t level);

/** @brief 运行时开关整灯（false = 灭，但灯效任务照常运行） */
void status_led_set_enabled(bool on);

/** @brief 当前亮度（0..255） */
uint8_t status_led_brightness(void);

/** @brief 当前是否开着 */
bool status_led_enabled(void);

#ifdef __cplusplus
}
#endif
