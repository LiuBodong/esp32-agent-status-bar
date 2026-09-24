/*
 * ESP32-C3 + SSD1315(128x32 单色 OLED) Agent 状态指示器
 *
 * 数据链路：Pi Agent --USB Serial/JTAG--> ESP32-C3（一行一条 JSON）
 * 显示：esp_lvgl_port + LVGL9，左侧状态图标 + 右侧两行文本，2 个页面轮播
 * 状态灯：GPIO4 上一颗 SK6812 RGBW，按 agent 状态跑灯效（见 status_led.c）
 */
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "display.h"
#include "serial_link.h"
#include "status_led.h"
#include "status_model.h"
#include "ui.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "vibe coding status bar starting");

    status_model_init();

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));
    ui_init(disp);

    /* 屏幕自检（可用 CONFIG_STATUS_BAR_BOOT_SELFTEST 关闭） */
    ui_boot_selftest();

    /* SK6812 状态灯：初始化 + R/G/B/W 自检 + 起灯效任务。
     * 失败只告警不中断 —— 灯不亮不该把屏幕和串口链路一起拖垮。 */
    esp_err_t led_err = status_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "状态灯不可用: %s（屏幕与串口链路不受影响）", esp_err_to_name(led_err));
    }

    esp_err_t err = serial_link_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "串口链路启动失败，屏幕仍会显示 WAITING HOST");
    } else {
        serial_link_send_boot_event();
    }

    ESP_LOGI(TAG, "ready, waiting for host JSON lines on USB Serial/JTAG");
}
