/*
 * 与主机（Pi Agent）之间的 USB Serial/JTAG 通信
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 安装 USB Serial/JTAG 驱动并启动接收任务 */
esp_err_t serial_link_start(void);

/** @brief 向主机发送固件启动事件 */
void serial_link_send_boot_event(void);

/** @brief 直接发送一行文本（会自动补 '\n'） */
void serial_link_send_line(const char *line);

#ifdef __cplusplus
}
#endif
