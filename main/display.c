/*
 * 屏幕初始化：I2C + SSD1315/SSD1306 面板 + esp_lvgl_port
 *
 * SSD1315 与 SSD1306 寄存器兼容，因此直接使用 IDF esp_lcd 中的 SSD1306 驱动。
 * 单色屏在 LVGL9 下使用 LV_COLOR_FORMAT_I1，esp_lvgl_port 会负责
 * 「LVGL I1 位图 -> SSD1306 按页(Page)纵向排列」的转换。
 */
#include "display.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

#define LCD_H_RES CONFIG_STATUS_BAR_LCD_H_RES
#define LCD_V_RES CONFIG_STATUS_BAR_LCD_V_RES

/* 单色屏要求使用整屏缓冲：buffer_size 单位是像素 */
#define LCD_BUFFER_PIXELS (LCD_H_RES * LCD_V_RES)

/* Kconfig 中的 bool 选项关闭时不会生成宏，这里统一成常量 */
#ifdef CONFIG_STATUS_BAR_LCD_MIRROR_X
#define LCD_MIRROR_X true
#else
#define LCD_MIRROR_X false
#endif

#ifdef CONFIG_STATUS_BAR_LCD_MIRROR_Y
#define LCD_MIRROR_Y true
#else
#define LCD_MIRROR_Y false
#endif

esp_err_t display_init(lv_display_t **out_disp)
{
    ESP_RETURN_ON_FALSE(out_disp != NULL, ESP_ERR_INVALID_ARG, TAG, "out_disp is NULL");

    ESP_LOGI(TAG, "I2C: SDA=GPIO%d SCL=GPIO%d %dHz addr=0x%02X",
             CONFIG_STATUS_BAR_I2C_SDA_GPIO, CONFIG_STATUS_BAR_I2C_SCL_GPIO,
             CONFIG_STATUS_BAR_I2C_FREQ_HZ, CONFIG_STATUS_BAR_I2C_ADDR);

    i2c_master_bus_handle_t i2c_bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_STATUS_BAR_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_STATUS_BAR_I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c_new_master_bus failed");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = CONFIG_STATUS_BAR_I2C_ADDR,
        .scl_speed_hz = CONFIG_STATUS_BAR_I2C_FREQ_HZ,
        .control_phase_bytes = 1,   /* SSD1306: 1 字节控制字，bit6 = D/C# */
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io), TAG, "new panel io failed");

    const esp_lcd_panel_ssd1306_config_t oled_cfg = {
        .height = LCD_V_RES,   /* 0.96 寸横条屏为 32 行 */
        .contrast = 0x7F,      /* 0 表示使用驱动默认值 128 */
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_STATUS_BAR_LCD_RST_GPIO,
        .bits_per_pixel = 1,
        .vendor_config = (void *)&oled_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(io, &panel_cfg, &panel), TAG, "new ssd1306 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init failed");
#ifdef CONFIG_STATUS_BAR_LCD_INVERT
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "panel invert failed");
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on failed");

    /* LVGL 任务栈给宽裕一点，LVGL9 渲染时栈开销比 LVGL8 大 */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_BUFFER_PIXELS,  /* 单色屏必须整屏缓冲 */
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = true,
        .color_format = LV_COLOR_FORMAT_I1,
        .rotation = {
            .swap_xy = false,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
        .flags = {
            .swap_bytes = false,
            .sw_rotate = false,
        },
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");

    *out_disp = disp;
    ESP_LOGI(TAG, "OLED %dx%d ready", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
