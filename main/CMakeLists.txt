idf_component_register(
    SRCS
        "esp32-vibe-coding-status-bar.c"
        "display.c"
        "ui.c"
        "status_model.c"
        "serial_link.c"
        "fonts/lv_font_source_han_sans_cn_13_ascii.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES
        esp_lcd
        esp_timer
        esp_driver_i2c
        esp_driver_usb_serial_jtag
)

# lv_font_conv 生成的字体文件默认 #include "lvgl/lvgl.h"，
# 该路径在组件管理器下载的目录结构下不存在，改成 #include "lvgl.h"
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
