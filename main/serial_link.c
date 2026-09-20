/*
 * USB Serial/JTAG 通信
 *
 * 线路格式：一行一条 JSON，'\n' 结尾。
 * 只有以 '{' 开头的行才会被解析，其它内容（比如日志回显）会被忽略。
 *
 * 状态字段（host -> ESP，全部可选，字段别名见 status_model.c）：
 *   {"state":"thinking","ctx":12000,"ctx_max":200000,"ctx_pct":6.0,"cache_pct":94.9,
 *    "in":1234,"out":567,"tps":42.5,"elapsed":3.2}
 *   ctx_pct / cache_pct 由 host 直接给（它自己显示的那两个百分比，1 位小数；负数 = 未知），
 *   免得两边各算一遍算不到一起。
 *
 * 控制命令（host -> ESP）：
 *   {"cmd":"ping"}                         -> {"evt":"pong",...}
 *   {"cmd":"page","index":1,"hold":10}     -> 切到第 1 页并保持 10 秒；index=-1 恢复自动轮播
 *                                             （共 2 页：0=CTX，1=TOK；超出会取模）
 *   {"cmd":"clear"}                        -> 清空统计
 *   {"cmd":"bye"}                          -> host 要退出了：立刻按断链渲染（副行显示 host exit），
 *                                             不必等 STATUS_BAR_LINK_TIMEOUT_MS 超时。不回 ack
 *
 * 下行事件（ESP -> host）：
 *   {"evt":"boot",...}   上电/复位后
 *   {"evt":"hb",...}     周期性心跳（可用 CONFIG_STATUS_BAR_HEARTBEAT_MS 关闭）
 *
 * 主机存活的判定：只有携带主机字段（state/ctx/in/out/tps/... 或 cmd）的行才算数，
 * 光「是个合法 JSON」不够。原因：如果主机侧 tty 没关 ECHO，本机的下行事件会被
 * 回灌进自己的 RX，此时若把它当主机数据，链路就永远不会判成断开。
 *
 * 主机侧注意事项（Pi 扩展已按此实现）：
 *   串口要用 O_RDWR 打开并持续读干净，且把 tty 设成 raw（关掉 icanon/echo/ixon）。
 *   只写不读会让 host 的 tty 输入队列（4KB）涨满，USB IN 方向没人消费 → 本机 TX 环
 *   堵死 → 主机侧的写也跟着失败，表现为「agent 连着却偶现 NO HOST」。
 */
#include "serial_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_model.h"
#include "ui.h"

static const char *TAG = "link";

#define LINE_MAX_LEN  CONFIG_STATUS_BAR_MAX_LINE_LEN

static char     s_line[LINE_MAX_LEN];
static size_t   s_line_len;
static bool     s_line_drop;

static uint32_t s_rx_lines;   /* 成功解析的 JSON 行数 */
static uint32_t s_rx_bad;     /* 解析失败/超长的行数 */
static int64_t  s_boot_us;

/* ------------------------------ 发送 ------------------------------ */

/*
 * 发送一行。
 *
 * 这里刻意只等很短的时间（TX_TIMEOUT_MS）：
 *   1. 本函数会被接收任务调用（心跳、pong/ack），一旦长时间阻塞，接收任务就停摆，
 *      而主机的数据还在往里灌，主机那边的写就会失败 —— 屏幕上的表现是主机明明在发，
 *      却闪 NO HOST。所以宁可丢这一帧，也不能把接收任务卡住。
 *   2. 失败也不要打 WARN：日志走的是同一个 USB TX 环，环满时它自己也会被拦，
 *      刷屏只会让情况更糟（而且 VFS 日志是逐字节写的，会把整行截断在中间）。
 */
#define TX_TIMEOUT_MS 20

static uint32_t s_tx_dropped;   /* 因为 TX 环满而丢掉的帧数 */

void serial_link_send_line(const char *line)
{
    if (line == NULL) {
        return;
    }
    size_t len = strlen(line);
    if (len == 0) {
        return;
    }

    /* 一次写入，避免与其他任务输出交错在行中间 */
    char buf[256];
    if (len + 1 > sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, line, len);
    buf[len] = '\n';

    int written = usb_serial_jtag_write_bytes(buf, len + 1, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    if (written != (int)(len + 1)) {
        s_tx_dropped++;
        ESP_LOGD(TAG, "USB 发送失败 (%d)，已丢帧 %u 次", written, (unsigned)s_tx_dropped);
    }
}

static void send_jsonf(const char *fmt, ...)
{
    char line[224];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    serial_link_send_line(line);
}

/* ------------------------------ 命令处理 ------------------------------ */

static uint32_t uptime_ms(void)
{
    int64_t now = esp_timer_get_time();
    int64_t up = now - s_boot_us;
    if (up < 0) {
        up = 0;
    }
    return (uint32_t)(up / 1000);
}

static void handle_command(const cJSON *obj)
{
    const cJSON *cmd = cJSON_GetObjectItem(obj, "cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL) {
        return;
    }

    if (strcmp(cmd->valuestring, "ping") == 0) {
        send_jsonf("{\"evt\":\"pong\",\"up_ms\":%u,\"page\":%d,\"rx\":%u,\"bad\":%u}",
                   (unsigned)uptime_ms(), ui_current_page(),
                   (unsigned)s_rx_lines, (unsigned)s_rx_bad);
        return;
    }

    if (strcmp(cmd->valuestring, "page") == 0) {
        const cJSON *index = cJSON_GetObjectItem(obj, "index");
        if (index == NULL) {
            index = cJSON_GetObjectItem(obj, "page");
        }
        const cJSON *hold = cJSON_GetObjectItem(obj, "hold");

        int page = -1;
        if (cJSON_IsNumber(index)) {
            page = (int)index->valuedouble;
        }
        uint32_t hold_ms = 0;
        if (cJSON_IsNumber(hold) && hold->valuedouble > 0) {
            hold_ms = (uint32_t)(hold->valuedouble * 1000.0);
        }

        ui_show_page(page, hold_ms);
        send_jsonf("{\"evt\":\"ack\",\"cmd\":\"page\",\"index\":%d,\"hold_ms\":%u}",
                   page, (unsigned)hold_ms);
        return;
    }

    if (strcmp(cmd->valuestring, "clear") == 0) {
        status_model_reset();
        send_jsonf("{\"evt\":\"ack\",\"cmd\":\"clear\"}");
        return;
    }

    if (strcmp(cmd->valuestring, "bye") == 0) {
        /* host 要退出了（Pi 的 session_shutdown/reason=quit）：
         * 立刻按断链渲染，省得屏幕把最后一次状态挂到超时为止。
         * 刻意不回 ack —— 对面马上就要关串口了，回包没人读；
         * 下一次收到任何主机数据时 host_gone 会自动清掉。 */
        status_model_mark_host_gone();
        return;
    }

    ESP_LOGW(TAG, "未知命令: %s", cmd->valuestring);
    send_jsonf("{\"evt\":\"error\",\"msg\":\"unknown cmd\"}");
}

static void handle_json_line(char *line)
{
    s_rx_lines++;

    cJSON *obj = cJSON_Parse(line);
    if (obj == NULL) {
        s_rx_bad++;
        ESP_LOGW(TAG, "JSON 解析失败: %.60s", line);
        return;
    }

#ifdef CONFIG_STATUS_BAR_LOG_RX
    ESP_LOGI(TAG, "recv %s", line);
#endif

    /* 只有「真的带主机字段」的行才算主机还活着。
     * 不能无脑把任何合法 JSON 都算进去：tty 的 ECHO 会把本机下行事件回灌回来
     * （{"evt":"hb",...}），那样 ESP 拿自己的心跳当主机存活，断链检测就永远是 true。 */
    const cJSON *cmd = cJSON_GetObjectItem(obj, "cmd");
    if (cJSON_IsString(cmd)) {
        status_model_mark_rx();
        handle_command(obj);
    } else if (status_model_apply_json(obj)) {
        status_model_mark_rx();
    } else {
        ESP_LOGD(TAG, "没有可用字段: %.60s", line);
    }

    cJSON_Delete(obj);
}

/* ------------------------------ 接收 ------------------------------ */

static void feed_char(char c)
{
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        if (!s_line_drop && s_line_len > 0) {
            s_line[s_line_len] = '\0';
            if (s_line[0] == '{') {
                handle_json_line(s_line);
            }
        }
        s_line_len = 0;
        s_line_drop = false;
        return;
    }

    if (s_line_drop) {
        return;
    }
    if (s_line_len + 1 >= sizeof(s_line)) {
        s_line_drop = true;
        s_rx_bad++;
        ESP_LOGW(TAG, "单行超过 %u 字节，已丢弃", (unsigned)sizeof(s_line));
        return;
    }
    s_line[s_line_len++] = c;
}

static void rx_task(void *arg)
{
    (void)arg;

    uint8_t buf[128];
    int64_t next_heartbeat_us = 0;
    if (CONFIG_STATUS_BAR_HEARTBEAT_MS > 0) {
        next_heartbeat_us = esp_timer_get_time() + (int64_t)CONFIG_STATUS_BAR_HEARTBEAT_MS * 1000;
    }

    while (true) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            feed_char((char)buf[i]);
        }

        if (CONFIG_STATUS_BAR_HEARTBEAT_MS > 0 && esp_timer_get_time() >= next_heartbeat_us) {
            next_heartbeat_us = esp_timer_get_time() + (int64_t)CONFIG_STATUS_BAR_HEARTBEAT_MS * 1000;
            send_jsonf("{\"evt\":\"hb\",\"up_ms\":%u,\"page\":%d,\"rx\":%u,\"bad\":%u}",
                       (unsigned)uptime_ms(), ui_current_page(),
                       (unsigned)s_rx_lines, (unsigned)s_rx_bad);
        }
    }
}

/* ------------------------------ 初始化 ------------------------------ */

esp_err_t serial_link_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = CONFIG_STATUS_BAR_TX_BUFFER_SIZE,
        .rx_buffer_size = CONFIG_STATUS_BAR_RX_BUFFER_SIZE,
    };

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG 驱动安装失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "请确认 menuconfig -> Component config -> ESP System Settings -> Channel for console output");
        ESP_LOGE(TAG, "没有把 USB Serial/JTAG 设为『主控制台』（应为 UART0 主 + USB Serial/JTAG 副）");
        return err;
    }

    /* 让日志也走驱动的发送环形缓冲，避免与 JSON 输出在字节层面交错 */
    usb_serial_jtag_vfs_use_driver();

    s_boot_us = esp_timer_get_time();

    BaseType_t ok = xTaskCreate(rx_task, "usb_rx", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建接收任务失败");

    ESP_LOGI(TAG, "USB Serial/JTAG 已就绪 (rx_buf=%d tx_buf=%d)",
             CONFIG_STATUS_BAR_RX_BUFFER_SIZE, CONFIG_STATUS_BAR_TX_BUFFER_SIZE);
    return ESP_OK;
}

void serial_link_send_boot_event(void)
{
    send_jsonf("{\"evt\":\"boot\",\"fw\":\"vibe-status-bar\",\"ver\":1,"
               "\"hres\":%d,\"vres\":%d,\"page_ms\":%d,\"pages\":%d}",
               CONFIG_STATUS_BAR_LCD_H_RES, CONFIG_STATUS_BAR_LCD_V_RES,
               CONFIG_STATUS_BAR_PAGE_INTERVAL_MS, (int)UI_PAGE_COUNT);
}
