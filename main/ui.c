/*
 * 状态栏界面
 *
 * 128x32 分成左右两块：左边 18px 放状态图标，右边 108px 用两行文字放数字。
 * 状态改用图形表示后，省下的横向空间就能容纳更多数字。
 *
 *   +---------+------------------------------------------+
 *   |         | 主行 (y=0..15)  思源黑体 14px             |
 *   |  18x16  |                                          |
 *   |  图标   | 副行 (y=16..31) 13px ASCII 子集           |
 *   +---------+------------------------------------------+
 *     x=0..17   x=20..127
 *
 * 文字一律左对齐：数值位数变化时整行不会左右抖动。
 *
 * 共 2 个页面自动轮播（原来的 STATE 页已取消，状态由左侧图标常驻表示）：
 *   0 CTX : 上下文 已用/全部 + 进度条
 *   1 TOK : ↑ 输入 ↓ 输出 token + 速度 + 轮次 + 耗时
 *
 * 主行有两种版式：
 *   CTX 页：整行一个 label，不写 CTX 前缀，数值按 1000 进制阶梯退化
 *           （999 → 999，1000 → 1K，1000000 → 1M），所以永远是 "28K/200K" 这种形式
 *   TOK 页：↑ 1.2k ↓ 567 —— 箭头来自 montserrat 符号、数字用主字体，
 *           同一行没法混排两种字体，所以放在一个 flex 容器里各占一个 label
 *
 * 没有收到过数据 / 链路超时 时，固定显示提示页。
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_model.h"

static const char *TAG = "ui";

/*
 * 字库
 *
 * 主行：LVGL 自带的 lv_font_source_han_sans_sc_14_cjk（思源黑体，含 ASCII + 常用汉字，
 *       ~120KB flash，由 sdkconfig 的 CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK 打开）。
 * 副行：用同一款字体生成的 13px ASCII 子集（main/fonts 下，lv_font_conv 生成）。
 * 图标：LVGL 自带的 lv_font_montserrat_14，只取其中的 FontAwesome 符号字形。
 *
 * 为什么不用 LVGL 内置 montserrat 排正文：它的很多字形墨迹宽度大于 advance（负边距），
 * 小字号下相邻字符会贴在一起；思源黑体的拉丁字形边距正常。
 * 图标是「单个符号字形 + 独立 label」，不存在相邻字符挤压，所以可以放心用它的符号表。
 */
extern const lv_font_t lv_font_source_han_sans_cn_13_ascii;

#define FONT_MAIN  (&lv_font_source_han_sans_sc_14_cjk)
#define FONT_SUB   (&lv_font_source_han_sans_cn_13_ascii)
#define FONT_ICON  (&lv_font_montserrat_14)

/*
 * 单色屏颜色映射
 *
 * esp_lvgl_port 把 LVGL 的 I1 位图转成 SSD1315 的「按页纵向排列」格式时做了取反：
 * LVGL 中的亮像素 -> OLED 熄灭，LVGL 中的暗像素 -> OLED 点亮。
 * 因此这里「在 LVGL 里画黑 = 在屏幕上点亮」。
 *
 * 如果实机显示反了（例如变成亮底暗字），把 menuconfig 里的
 *   Vibe Coding Status Bar -> Display -> Invert colors
 * 打开即可整体翻转。
 */
#define COLOR_LIT  lv_color_black()   /* 屏幕上点亮的部分：文字、图标、进度条 */
#define COLOR_OFF  lv_color_white()   /* 屏幕上不亮的部分：背景 */

#define SCREEN_W   CONFIG_STATUS_BAR_LCD_H_RES
#define SCREEN_H   CONFIG_STATUS_BAR_LCD_V_RES

/* 左列：状态图标（符号字形的墨迹最大 16px 宽，留 1px 余量） */
#define ICON_Y     8                      /* (32-16)/2：纵向居中 */
#define ICON_W     18
#define ICON_H     16
#define SPIN_Y     7                      /* (32-18)/2 */
#define SPIN_SIZE  18

/* 右列：两行文字，左对齐 */
#define COL_X      (ICON_W + 2)           /* 20：图标列 + 2px 间隔 */
#define COL_W      (SCREEN_W - COL_X)     /* 108 */

#define ROW_MAIN_Y 0
#define ROW_SUB_Y  16
#define ROW_MAIN_H 16
#define ROW_SUB_H  16

/* TOK 页主行：↑ 1.2k ↓ 567
 * 箭头字形 13x8、advance 12.25px，数字最多 4 字符（"999k"）30.6px */
#define TOK_ARROW_W 14
#define TOK_VAL_W   34
#define TOK_GAP     3    /* 合计 14+3+34+3+14+3+34 = 105px，右列放得下 */

/* 进度条 + 百分比，一起铺满右列副行 */
#define BAR_X      COL_X
#define BAR_Y      18
#define BAR_W      68
#define BAR_H      12
#define PCT_X      (COL_X + BAR_W + 2)    /* 90 */
#define PCT_W      (SCREEN_W - PCT_X)     /* 38 */

static lv_obj_t *s_icon;        /* 状态图标（符号字形） */
static lv_obj_t *s_spin;        /* thinking 专用的旋转弧 */
static lv_obj_t *s_top;         /* 主行：CTX 页的整行文字 */
static lv_obj_t *s_tok_row;     /* 主行：TOK 页的 flex 容器 */
static lv_obj_t *s_tok_in;      /*   ↑ 后面的输入 token */
static lv_obj_t *s_tok_out;     /*   ↓ 后面的输出 token */
static lv_obj_t *s_bot;         /* 副行 */
static lv_obj_t *s_bar;
static lv_obj_t *s_selftest;

static const char *s_icon_symbol;  /* 当前图标字形；NULL 表示显示旋转弧 */

static ui_page_t s_page = UI_PAGE_CTX;
static uint32_t  s_page_start_ms;
static uint32_t  s_hold_until_ms;
static bool      s_bar_visible;
static bool      s_tok_row_shown;

/* ------------------------------ 文本工具 ------------------------------ */

static void set_label_text(lv_obj_t *label, const char *text)
{
    const char *old = lv_label_get_text(label);
    if (old == NULL || strcmp(old, text) != 0) {
        lv_label_set_text(label, text);
    }
}

/** 上下文用量：每级超过 1000 就退化一级（999 → 999，1000 → 1K，1000000 → 1M）
 *
 * 1000 进制而不是 1024：模型的上下文窗口都是十进制标称的（200000 / 1000000），
 * 用 1000 进制才能显示成 "200K" / "1M"。
 * 取整用截断：999999 还是 999K，到 1000000 才变 1M，跟「超出即退化」的语义一致。
 */
static void fmt_ctx(uint32_t value, bool known, char *out, size_t out_size)
{
    if (!known) {
        snprintf(out, out_size, "--");
    } else if (value < 1000u) {
        snprintf(out, out_size, "%u", (unsigned)value);
    } else if (value < 1000000u) {
        snprintf(out, out_size, "%uK", (unsigned)(value / 1000u));
    } else {
        snprintf(out, out_size, "%uM", (unsigned)(value / 1000000u));
    }
}

/** 大数值压缩显示：999 / 1.2k / 12k / 999k / 1.2M（最长 4 字符，保证 TOK 页放得下） */
static void fmt_count(uint32_t value, bool known, char *out, size_t out_size)
{
    if (!known) {
        snprintf(out, out_size, "--");
    } else if (value < 1000) {
        snprintf(out, out_size, "%u", (unsigned)value);
    } else if (value < 10000) {
        snprintf(out, out_size, "%.1fk", (double)value / 1000.0);
    } else if (value < 1000000) {
        snprintf(out, out_size, "%uk", (unsigned)(value / 1000));
    } else {
        snprintf(out, out_size, "%.1fM", (double)value / 1000000.0);
    }
}

/** 秒 -> mm:ss / hh:mm（超过 1 小时用 1h05 这种紧凑写法，副行才放得下） */
static void fmt_duration(float seconds, char *out, size_t out_size)
{
    if (seconds < 0.0f) {
        seconds = 0.0f;
    }
    uint32_t total = (uint32_t)seconds;
    uint32_t h = total / 3600;
    uint32_t m = (total % 3600) / 60;
    uint32_t s = total % 60;
    if (h > 0) {
        snprintf(out, out_size, "%uh%02u", (unsigned)h, (unsigned)m);
    } else {
        snprintf(out, out_size, "%02u:%02u", (unsigned)m, (unsigned)s);
    }
}

/* ------------------------------ 状态图标 ------------------------------ */

/** 状态 -> 图标字形；thinking 返回 NULL，表示改用旋转弧 */
static const char *state_symbol(agent_state_t state)
{
    switch (state) {
    case AGENT_STATE_THINKING:
        return NULL;
    case AGENT_STATE_IDLE:
        return LV_SYMBOL_PAUSE;
    case AGENT_STATE_RUNNING:
        return LV_SYMBOL_PLAY;
    case AGENT_STATE_TOOL:
        return LV_SYMBOL_SETTINGS;
    case AGENT_STATE_WAITING:
        return LV_SYMBOL_BELL;      /* 在等用户确认/输入 */
    case AGENT_STATE_DONE:
        return LV_SYMBOL_OK;
    case AGENT_STATE_ERROR:
        return LV_SYMBOL_WARNING;
    default:
        return LV_SYMBOL_BULLET;    /* host 传来的自定义状态，没有专用图标 */
    }
}

/**
 * @brief 更新左侧图标
 *
 * @param[in] symbol 图标字形；NULL 表示旋转弧（thinking）
 * @param[in] now_ms 当前 tick，用来推进旋转弧的角度
 */
static void set_icon(const char *symbol, uint32_t now_ms)
{
    if (symbol != s_icon_symbol) {
        s_icon_symbol = symbol;
        if (symbol == NULL) {
            lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_label_text(s_icon, symbol);
            lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (symbol == NULL) {
        /* 旋转弧每个刷新周期转 45°（8 档，默认 200ms 一档 ≈ 1.6s 一圈）。
         * 刻意不用 lv_anim：显示是整屏缓冲 + full_refresh，常驻动画会让
         * I2C 以 30fps 整帧刷屏，而这里跟着 UI 定时器走就够用了。 */
        int start = (int)((now_ms / CONFIG_STATUS_BAR_REFRESH_MS) % 8) * 45;
        lv_arc_set_angles(s_spin, start, start + 90);
    }
}

/* ------------------------------ 布局控制 ------------------------------ */

static void set_bar_visible(bool visible)
{
    if (visible == s_bar_visible) {
        return;
    }
    s_bar_visible = visible;

    if (visible) {
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_bot, PCT_X, ROW_SUB_Y);
        lv_obj_set_width(s_bot, PCT_W);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_bot, COL_X, ROW_SUB_Y);
        lv_obj_set_width(s_bot, COL_W);
    }
}

/** 主行两套版式二选一：false = CTX 页整行文字，true = TOK 页的 ↑↓ 行 */
static void set_main_row(bool tok_row)
{
    if (tok_row == s_tok_row_shown) {
        return;
    }
    s_tok_row_shown = tok_row;

    if (tok_row) {
        lv_obj_add_flag(s_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_tok_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tok_row, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------ 页面渲染 ------------------------------ */

static void render_pending(void)
{
    set_icon(LV_SYMBOL_USB, 0);
    set_main_row(false);
    set_bar_visible(false);
    set_label_text(s_top, "WAITING HOST");
    set_label_text(s_bot, "no data on usb");
}

static void render_lost(const agent_status_t *st)
{
    char sub[32];
    set_icon(LV_SYMBOL_CLOSE, 0);
    set_main_row(false);
    set_bar_visible(false);
    set_label_text(s_top, "NO HOST");
    if (st->age_ms >= 60000u) {
        snprintf(sub, sizeof(sub), "lost %um", (unsigned)(st->age_ms / 60000u));
    } else {
        snprintf(sub, sizeof(sub), "lost %us", (unsigned)(st->age_ms / 1000u));
    }
    set_label_text(s_bot, sub);
}

static void render_page(const agent_status_t *st, uint32_t now_ms)
{
    char main_text[40];
    char sub_text[32];

    set_icon(state_symbol(st->state), now_ms);

    switch (s_page) {
    case UI_PAGE_TOK: {
        set_main_row(true);
        set_bar_visible(false);

        char in[12];
        char out[12];
        fmt_count(st->tok_in, true, in, sizeof(in));
        fmt_count(st->tok_out, true, out, sizeof(out));
        set_label_text(s_tok_in, in);
        set_label_text(s_tok_out, out);

        /* 速度：三位数以上就不带小数了，免得副行超宽 */
        char speed[12];
        if (st->tps < 0.0f) {
            snprintf(speed, sizeof(speed), "--");
        } else if (st->tps >= 100.0f) {
            snprintf(speed, sizeof(speed), "%d", (int)(st->tps + 0.5f));
        } else {
            snprintf(speed, sizeof(speed), "%.1f", (double)st->tps);
        }

        char elapsed[16];
        fmt_duration(st->elapsed_s, elapsed, sizeof(elapsed));
        if (st->turn > 0) {
            snprintf(sub_text, sizeof(sub_text), "%st/s  #%u  %s",
                     speed, (unsigned)st->turn, elapsed);
        } else {
            snprintf(sub_text, sizeof(sub_text), "%st/s  %s", speed, elapsed);
        }
        set_label_text(s_bot, sub_text);
        break;
    }

    case UI_PAGE_CTX:
    default: {
        set_main_row(false);
        set_bar_visible(true);

        bool pct_known = st->ctx_max > 0;

        char used[12];
        char total[12];
        fmt_ctx(st->ctx_used, true, used, sizeof(used));
        fmt_ctx(st->ctx_max, pct_known, total, sizeof(total));

        if (st->state == AGENT_STATE_UNKNOWN && st->state_name[0] != '\0') {
            /* 自定义状态没有对应图标，主行退回显示它的名字 */
            snprintf(main_text, sizeof(main_text), "%s", st->state_name);
        } else {
            /* 最长 "4294M/4294M"，实测 89px，恒在右列宽度内 */
            snprintf(main_text, sizeof(main_text), "%s/%s", used, total);
        }
        set_label_text(s_top, main_text);

        uint32_t pct = 0;
        if (pct_known) {
            uint32_t clamped = st->ctx_used > st->ctx_max ? st->ctx_max : st->ctx_used;
            pct = (clamped * 100u) / st->ctx_max;
        }
        lv_bar_set_value(s_bar, (int32_t)pct, LV_ANIM_OFF);

        if (pct_known) {
            snprintf(sub_text, sizeof(sub_text), "%u%%", (unsigned)pct);
        } else {
            snprintf(sub_text, sizeof(sub_text), "--");
        }
        set_label_text(s_bot, sub_text);
        break;
    }
    }
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    const uint32_t now = lv_tick_get();

    if (CONFIG_STATUS_BAR_PAGE_INTERVAL_MS > 0 && now >= s_hold_until_ms) {
        if (now - s_page_start_ms >= CONFIG_STATUS_BAR_PAGE_INTERVAL_MS) {
            s_page = (ui_page_t)((s_page + 1) % UI_PAGE_COUNT);
            s_page_start_ms = now;
        }
    }

    agent_status_t st;
    status_model_get(&st);

    if (!st.host_seen) {
        render_pending();
        return;
    }
    if (!st.link_up) {
        render_lost(&st);
        return;
    }
    render_page(&st, now);
}

/* ------------------------------ 初始化 ------------------------------ */

/** 建一个定宽定高的 label；位置由调用方给（flex 子项则由布局器排） */
static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font,
                              int w, int h, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, COLOR_LIT, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(label, "");
    return label;
}

void ui_init(lv_display_t *disp)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return;
    }

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, COLOR_OFF, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_text_color(scr, COLOR_LIT, 0);

    s_icon = create_label(scr, FONT_ICON, ICON_W, ICON_H, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_icon, 0, ICON_Y);

    s_top = create_label(scr, FONT_MAIN, COL_W, ROW_MAIN_H, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_top, COL_X, ROW_MAIN_Y);

    s_bot = create_label(scr, FONT_SUB, COL_W, ROW_SUB_H, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_bot, COL_X, ROW_SUB_Y);

    s_icon_symbol = "";   /* 和 set_icon() 的入参永远不等，保证首次也会真正刷一遍 */

    /* TOK 页主行：箭头是 montserrat 符号、数字是主字体，一行里混不了两种字体，
     * 所以各占一个固定宽度的 label，交给 flex 排成「↑ 1.2k ↓ 567」 */
    s_tok_row = lv_obj_create(scr);
    lv_obj_remove_style_all(s_tok_row);          /* 透明、无边框、无内边距 */
    lv_obj_remove_flag(s_tok_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_tok_row, COL_X, ROW_MAIN_Y);
    lv_obj_set_size(s_tok_row, COL_W, ROW_MAIN_H);
    lv_obj_set_flex_flow(s_tok_row, LV_FLEX_FLOW_ROW);   /* 必须在 remove_style_all 之后 */
    lv_obj_set_flex_align(s_tok_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_tok_row, TOK_GAP, 0);

    lv_obj_t *arrow = create_label(s_tok_row, FONT_ICON, TOK_ARROW_W, ROW_MAIN_H, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(arrow, LV_SYMBOL_UP);
    s_tok_in = create_label(s_tok_row, FONT_MAIN, TOK_VAL_W, ROW_MAIN_H, LV_TEXT_ALIGN_LEFT);
    arrow = create_label(s_tok_row, FONT_ICON, TOK_ARROW_W, ROW_MAIN_H, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(arrow, LV_SYMBOL_DOWN);
    s_tok_out = create_label(s_tok_row, FONT_MAIN, TOK_VAL_W, ROW_MAIN_H, LV_TEXT_ALIGN_LEFT);

    lv_obj_add_flag(s_tok_row, LV_OBJ_FLAG_HIDDEN);   /* 开机停在 CTX 页 */
    s_tok_row_shown = false;

    /* thinking 用的旋转弧：只画 indicator，底环和末端圆点都去掉 */
    s_spin = lv_arc_create(scr);
    lv_obj_set_size(s_spin, SPIN_SIZE, SPIN_SIZE);
    lv_obj_set_pos(s_spin, 0, SPIN_Y);
    lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_spin, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, COLOR_LIT, LV_PART_INDICATOR);
    lv_arc_set_rotation(s_spin, 270);      /* 0° 从正上方开始 */
    lv_arc_set_bg_angles(s_spin, 0, 360);
    lv_arc_set_angles(s_spin, 0, 90);
    lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_set_pos(s_bar, BAR_X, BAR_Y);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, COLOR_LIT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COLOR_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COLOR_LIT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    s_bar_visible = false;

    /* 上电自检用的整屏色块（默认隐藏） */
    s_selftest = lv_obj_create(scr);
    lv_obj_remove_style_all(s_selftest);
    lv_obj_set_pos(s_selftest, 0, 0);
    lv_obj_set_size(s_selftest, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_selftest, COLOR_LIT, 0);
    lv_obj_set_style_bg_opa(s_selftest, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_selftest, LV_OBJ_FLAG_HIDDEN);

    /* 先画一帧提示，等 host 数据到来 */
    render_pending();

    s_page_start_ms = lv_tick_get();
    s_hold_until_ms = 0;
    lv_timer_create(ui_timer_cb, CONFIG_STATUS_BAR_REFRESH_MS, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready (%dx%d, 图标列 %dpx + 文字列 %dpx, %d pages)",
             SCREEN_W, SCREEN_H, ICON_W, COL_W, (int)UI_PAGE_COUNT);
    ESP_LOGI(TAG, "字体: 主行 %p line_height=%d, 副行 %p line_height=%d, 图标 %p",
             (const void *)FONT_MAIN, (int)FONT_MAIN->line_height,
             (const void *)FONT_SUB, (int)FONT_SUB->line_height,
             (const void *)FONT_ICON);
    ESP_LOGI(TAG, "空闲堆: %u 字节, 空闲内部堆: %u 字节",
             (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void ui_boot_selftest(void)
{
#ifndef CONFIG_STATUS_BAR_BOOT_SELFTEST
    return;
#else
    if (s_selftest == NULL) {
        return;
    }

    /* 点亮整屏 400ms：能看到说明 I2C 面板、刷新链路、单色转换都正常；
     * 看不到说明问题在屏幕接线/供电/面板初始化，而不在字体或文字渲染。 */
    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_obj_remove_flag(s_selftest, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(400));

    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_obj_add_flag(s_selftest, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "屏幕自检完成（如果刚才没看到整屏点亮，请检查 I2C 接线/供电）");
#endif
}

void ui_show_page(int index, uint32_t hold_ms)
{
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (index < 0) {
        s_hold_until_ms = 0;
    } else {
        s_page = (ui_page_t)(index % UI_PAGE_COUNT);
        s_hold_until_ms = lv_tick_get() + hold_ms;   /* 0 表示不额外保持 */
    }
    s_page_start_ms = lv_tick_get();
    lvgl_port_unlock();
}

int ui_current_page(void)
{
    return (int)s_page;
}
