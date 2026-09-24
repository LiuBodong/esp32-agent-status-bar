/*
 * SK6812 RGBW 状态灯实现
 *
 * 用的是托管组件 espressif/led_strip（RMT 后端，见 main/idf_component.yml），
 * 单灯珠、4 通道。硬件与踩坑记录见 docs/led_ctrol.md，关键几条：
 *   - ESP32-C3 无 DMA，RMT 每通道只有 48 word 且要求 mem_block_symbols 为偶数且 ≥48
 *   - RGBW 的字节序按批次可能是 GRBW / RGBW（点红亮绿就是序反了），用 Kconfig 开关切
 *   - 初始化别用 ESP_ERROR_CHECK（失败走 abort，反而看不到原因）
 *   - 工具链必须是 GCC：clang 下 RMT 传输完成中断不触发，会静默卡死在 refresh
 *
 * 灯效跑在独立的 FreeRTOS 任务里，刻意不挂 LVGL 定时器：后者跑在 LVGL 任务里，
 * 而整屏缓冲 + full_refresh 下屏幕本来就是 I2C 瓶颈，别把两者耦合在一起。
 */
#include "status_led.h"

#include <math.h>
#include <string.h>

#include "driver/rmt_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "status_model.h"
#include "ui.h"

/*
 * Kconfig 里的 bool 选项关闭时不生成宏，所以整份实现包在 #ifdef 里，
 * 关掉功能时下面那组空实现仍然满足链接。
 */
#ifdef CONFIG_STATUS_BAR_LED_ENABLE

static const char *TAG = "led";

#define LED_GPIO     CONFIG_STATUS_BAR_LED_GPIO
#define LED_COUNT    1                          /* 只做单灯 */
#define LED_TICK_MS  CONFIG_STATUS_BAR_LED_REFRESH_MS

#ifdef CONFIG_STATUS_BAR_LED_RGBW_ORDER
#define LED_COLOR_FMT LED_STRIP_COLOR_COMPONENT_FMT_RGBW
#define LED_COLOR_NAME "RGBW"
#else
#define LED_COLOR_FMT LED_STRIP_COLOR_COMPONENT_FMT_GRBW
#define LED_COLOR_NAME "GRBW"
#endif

#define PI_F 3.14159265f

/** 逻辑颜色：0..255，代表「想显示的强度」，还没做 gamma / 主亮度缩放 */
typedef struct {
    uint8_t r, g, b, w;
} led_rgbw_t;

static led_strip_handle_t s_strip;
static SemaphoreHandle_t  s_lock;         /* 保护 s_brightness / s_enabled（串口任务写、灯效任务读） */
static uint8_t            s_brightness = CONFIG_STATUS_BAR_LED_BRIGHTNESS;
static bool               s_enabled = true;

/* 上一次真正写出去的颜色，用来跳过重复刷新（长时间静态状态时省 RMT 流量） */
static led_rgbw_t s_last;
static bool       s_last_valid;
static bool       s_write_warned;

/* ------------------------------ 输出配置 ------------------------------ */

static void output_cfg_get(uint8_t *brightness, bool *enabled)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *brightness = s_brightness;
    *enabled = s_enabled;
    xSemaphoreGive(s_lock);
}

uint8_t status_led_brightness(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    uint8_t v;
    bool dummy;
    output_cfg_get(&v, &dummy);
    return v;
}

bool status_led_enabled(void)
{
    if (s_lock == NULL) {
        return false;
    }
    uint8_t dummy;
    bool v;
    output_cfg_get(&dummy, &v);
    return v;
}

void status_led_set_brightness(uint8_t level)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_brightness = level;
    xSemaphoreGive(s_lock);
}

void status_led_set_enabled(bool on)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = on;
    xSemaphoreGive(s_lock);
}

/* ------------------------------ 波形与配色 ------------------------------ */

static uint8_t to_u8(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    return (uint8_t)(v * 255.0f + 0.5f);
}

/** 升余弦呼吸：0 → 1 → 0，两端平滑，是「呼吸灯」最自然的形状 */
static float breathe(uint32_t t_ms, uint32_t period_ms)
{
    float phase = (float)(t_ms % period_ms) / (float)period_ms;
    return 0.5f * (1.0f - cosf(2.0f * PI_F * phase));
}

/** 三角脉冲：0 → 1 → 0，比正弦陡，用来做更「有劲」的跳动 */
static float pulse(uint32_t t_ms, uint32_t period_ms)
{
    uint32_t p = t_ms % period_ms;
    uint32_t half = period_ms / 2u;
    return (p < half) ? (float)p / (float)half
                      : (float)(period_ms - p) / (float)half;
}

/** 方波：每个 period 里有 on_ms 点亮 */
static bool blink(uint32_t t_ms, uint32_t period_ms, uint32_t on_ms)
{
    return (t_ms % period_ms) < on_ms;
}

/** HSV -> RGB，只用 RGB 三通道（W 留给白灯），h 单位是度 */
static led_rgbw_t hsv(float h_deg, float s, float v)
{
    float h = fmodf(h_deg, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (h < 60.0f) {
        r = c; g = x;
    } else if (h < 120.0f) {
        r = x; g = c;
    } else if (h < 180.0f) {
        g = c; b = x;
    } else if (h < 240.0f) {
        g = x; b = c;
    } else if (h < 300.0f) {
        r = x; b = c;
    } else {
        r = c; b = x;
    }
    led_rgbw_t out = { to_u8(r + m), to_u8(g + m), to_u8(b + m), 0 };
    return out;
}

/** 白灯走 W 通道，比 RGB 三通道叠出来的白更中性、也更省 */
static led_rgbw_t white(float level)
{
    led_rgbw_t out = { 0, 0, 0, to_u8(level) };
    return out;
}

/* ------------------------------ 灯效 ------------------------------ */

/** 上电未连 / 断链：白灯双拍心跳（每 1.5s 一组「咚-咚」） */
static led_rgbw_t fx_heartbeat(uint32_t t)
{
    uint32_t p = t % 1500u;
    float level = 0.0f;
    if (p < 150u) {
        level = pulse(p, 150u);
    } else if (p >= 250u && p < 400u) {
        level = pulse(p - 250u, 150u);
    }
    return white(level * 0.9f);
}

/** 有主机时的状态灯效 */
static led_rgbw_t fx_for_state(const agent_status_t *st, uint32_t t)
{
    switch (st->state) {
    case AGENT_STATE_IDLE:
        /* 空闲：青色极慢呼吸，整体压暗，安静待机 */
        return hsv(175.0f, 0.9f, 0.25f + 0.35f * breathe(t, 4000u));

    case AGENT_STATE_THINKING:
        /* 思考：色相 4s 转一圈（彩虹）+ 呼吸起伏 —— 最显眼的一档 */
        return hsv((float)(t % 4000u) * (360.0f / 4000.0f), 1.0f,
                   0.45f + 0.55f * breathe(t, 1800u));

    case AGENT_STATE_RUNNING:
        /* 生成中：蓝色 1s 脉冲 */
        return hsv(215.0f, 1.0f, pulse(t, 1000u));

    case AGENT_STATE_TOOL: {
        /* 跑工具：绿灯双闪后停顿，像磁盘活动灯 */
        uint32_t p = t % 1400u;
        bool on = (p < 120u) || (p >= 220u && p < 340u);
        return hsv(120.0f, 1.0f, on ? 0.85f : 0.05f);
    }

    case AGENT_STATE_WAITING:
        /* 在等用户确认/输入：琥珀色快呼吸，催一下 */
        return hsv(35.0f, 1.0f, 0.15f + 0.85f * breathe(t, 1200u));

    case AGENT_STATE_DONE: {
        /* 完成：三连闪之后渐隐 */
        uint32_t p = t % 2000u;
        if (p < 600u) {
            bool on = ((p / 100u) % 2u) == 0u;
            return hsv(120.0f, 1.0f, on ? 1.0f : 0.0f);
        }
        float fade = 1.0f - (float)(p - 600u) / 1400.0f;
        return hsv(120.0f, 1.0f, 0.8f * fade * fade);
    }

    case AGENT_STATE_ERROR:
        /* 出错：红色 2Hz 硬闪 */
        return hsv(0.0f, 1.0f, blink(t, 500u, 250u) ? 1.0f : 0.0f);

    default:
        /* host 传来的自定义状态（没有专用图标的那种）：青色慢呼吸 */
        return hsv(190.0f, 0.9f, 0.2f + 0.5f * breathe(t, 2500u));
    }
}

static led_rgbw_t render_color(const agent_status_t *st, uint32_t t)
{
    if (!st->host_seen || !st->link_up) {
        /* 没有主机（上电后从未连上，或断链）：白灯心跳 */
        return fx_heartbeat(t);
    }
    return fx_for_state(st, t);
}

/* ------------------------------ 输出 ------------------------------ */

/** gamma ≈ 2.0 的平方近似：SK6812 每通道只有 8bit，线性调光在低亮度区跳变很扎眼 */
static inline uint8_t gamma8(uint8_t v)
{
    return (uint8_t)(((uint16_t)v * (uint16_t)v) / 255u);
}

/** 通道值 -> 实际灰度：先 gamma 再乘主亮度。
 *  顺序很关键：先 gamma 后缩放，主亮度整体压低时各通道比例不变，
 *  低亮度区仍然平滑（反过来先缩放再 gamma，会把亮度压两次，暗得过分）。 */
static inline uint8_t scale_ch(uint8_t v, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)gamma8(v) * (uint16_t)brightness) / 255u);
}

static led_rgbw_t apply_output(const led_rgbw_t *color, uint8_t brightness)
{
    led_rgbw_t out = {
        .r = scale_ch(color->r, brightness),
        .g = scale_ch(color->g, brightness),
        .b = scale_ch(color->b, brightness),
        .w = scale_ch(color->w, brightness),
    };
    return out;
}

/** 写灯珠并刷新（不管缓存）；失败只提醒一次，免得刷屏 */
static void led_write(const led_rgbw_t *out)
{
    esp_err_t err = led_strip_set_pixel_rgbw(s_strip, 0, out->r, out->g, out->b, out->w);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    if (err != ESP_OK && !s_write_warned) {
        s_write_warned = true;
        ESP_LOGW(TAG, "灯带写入失败: %s（检查数据线接的是 Din、是否共地、工具链是否为 GCC）",
                 esp_err_to_name(err));
    }
}

/** 应用 gamma + 主亮度后写出；颜色和上次一样就跳过 */
static void led_flush(const led_rgbw_t *color, uint8_t brightness, bool enabled)
{
    led_rgbw_t zero = { 0, 0, 0, 0 };
    led_rgbw_t out = enabled ? apply_output(color, brightness) : zero;

    if (s_last_valid && memcmp(&out, &s_last, sizeof(out)) == 0) {
        return;
    }
    s_last = out;
    s_last_valid = true;
    led_write(&out);
}

/* ------------------------------ 灯效任务 ------------------------------ */

#ifdef CONFIG_STATUS_BAR_LED_BOOT_SELFTEST
/** 依次点亮 R/G/B/W 各 150ms：确认接线，也能一眼看出字节序对不对
 *  （点红亮绿 = 批次是 RGBW，去 menuconfig 打开 STATUS_BAR_LED_RGBW_ORDER）
 *
 *  刻意放在灯效任务里、而不是 status_led_init() 里：led_strip_refresh() 内部是
 *  rmt_tx_wait_all_done(chan, -1)（永久等待），工具链若是 clang 会永久卡在这。
 *  放任务里，最坏情况只是这个任务停摆，屏幕和串口链路照常工作；
 *  放 init 里则会把 app_main 一起卡死，等于一个可选外设把整机拖垮。 */
static void led_boot_selftest(void)
{
    const led_rgbw_t seq[] = {
        { .r = 255 },
        { .g = 255 },
        { .b = 255 },
        { .w = 255 },
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        led_rgbw_t out = apply_output(&seq[i], s_brightness);
        led_write(&out);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    const led_rgbw_t off = { 0, 0, 0, 0 };
    led_write(&off);
}
#endif

static void led_task(void *arg)
{
    (void)arg;

#ifdef CONFIG_STATUS_BAR_LED_BOOT_SELFTEST
    led_boot_selftest();
    s_last_valid = false;   /* 让下面第一帧一定真写一次 */
#endif

    uint32_t t = 0;

    while (true) {
        agent_status_t st;
        status_model_get(&st);

        uint8_t brightness;
        bool enabled;
        output_cfg_get(&brightness, &enabled);

        led_rgbw_t color = { 0, 0, 0, 0 };
        if (!ui_screen_off()) {
            /* 屏幕熄了（断链很久）就一起灭；关显示与灯带无关，
             * 这里只是复用 UI 已经算好的熄屏判定，别再维护一套断链计时 */
            color = render_color(&st, t);
        }
        led_flush(&color, brightness, enabled);

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
        t += LED_TICK_MS;
    }
}

/* ------------------------------ 初始化 ------------------------------ */

esp_err_t status_led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_SK6812,   /* 组件据此选时序：T0H=0.3us 等 */
        .color_component_format = LED_COLOR_FMT,
        .flags = { .invert_out = false },
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10MHz，1 tick = 0.1us，0.3us 刚好整除 */
        .mem_block_symbols = 48,             /* C3 无 DMA，每通道只有 48 word */
        .flags = { .with_dma = false },      /* C3 的 RMT 不支持 DMA */
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip 初始化失败: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        ESP_LOGE(TAG, "确认工具链是 GCC（IDF_TOOLCHAIN 不为 clang），否则 RMT 会静默卡死");
        return err;
    }

    ESP_LOGI(TAG, "SK6812 就绪: GPIO%d, %d 灯珠, %s, 默认亮度 %u",
             LED_GPIO, LED_COUNT, LED_COLOR_NAME, (unsigned)s_brightness);

    if (xTaskCreate(led_task, "status_led", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建灯效任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* !CONFIG_STATUS_BAR_LED_ENABLE */

/* 功能关闭：保留同名空实现，app_main / serial_link 不用做条件编译 */
esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set_brightness(uint8_t level) { (void)level; }
void status_led_set_enabled(bool on) { (void)on; }
uint8_t status_led_brightness(void) { return 0; }
bool status_led_enabled(void) { return false; }

#endif
