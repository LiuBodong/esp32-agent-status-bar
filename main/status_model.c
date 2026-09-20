/*
 * Agent 状态模型实现
 *
 * 接收的 JSON 字段（全部可选，大小写不敏感）：
 *   state     : "idle"|"thinking"|"running"|"tool"|"waiting"|"done"|"error"（也接受任意自定义字符串）
 *   ctx       : 上下文已用 token            别名 ctx_used/context/context_tokens
 *   ctx_max   : 上下文窗口                  别名 ctx_limit/ctx_size/ctx_window/context_max
 *   in        : 输入 token                  别名 in_tokens/input/input_tokens/tokens_in
 *   out       : 输出 token                  别名 out_tokens/output/output_tokens/tokens_out
 *   tps       : token/s                     别名 tok_s/tokens_per_second
 *   elapsed   : 当前步骤耗时(秒)            别名 elapsed_s/duration/dur
 *   turn      : 轮次/步骤序号               别名 step/round/iteration
 */
#include "status_model.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    agent_status_t st;
    int64_t  last_rx_us;     /* 最近一次收到 host 数据的时刻 */
    int64_t  state_change_us;/* 最近一次状态变化的时刻（本地计时基准） */
    float    host_elapsed_s; /* <0 表示 host 没有提供 elapsed */
} model_t;

static model_t s_m;
static SemaphoreHandle_t s_lock;

static const struct {
    const char *name;
    agent_state_t state;
} k_state_table[] = {
    { "IDLE",     AGENT_STATE_IDLE     },
    { "THINKING", AGENT_STATE_THINKING },
    { "RUNNING",  AGENT_STATE_RUNNING  },
    { "WORKING",  AGENT_STATE_RUNNING  },
    { "TOOL",     AGENT_STATE_TOOL     },
    { "TOOL_USE", AGENT_STATE_TOOL     },
    { "WAITING",  AGENT_STATE_WAITING  },
    { "BLOCKED",  AGENT_STATE_WAITING  },
    { "DONE",     AGENT_STATE_DONE     },
    { "COMPLETE", AGENT_STATE_DONE     },
    { "SUCCESS",  AGENT_STATE_DONE     },
    { "ERROR",    AGENT_STATE_ERROR    },
    { "FAILED",   AGENT_STATE_ERROR    },
};

const char *status_state_name(agent_state_t state)
{
    for (size_t i = 0; i < sizeof(k_state_table) / sizeof(k_state_table[0]); i++) {
        if (k_state_table[i].state == state && state != AGENT_STATE_UNKNOWN) {
            return k_state_table[i].name;
        }
    }
    return "UNKNOWN";
}

/* ------------------------------ 小工具 ------------------------------ */

/** 拷贝字符串并保证以 '\0' 结尾 */
static void copy_str(char *dst, const char *src, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_size; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/** 把字符串转成大写 ASCII 并过滤不可打印字符 */
static void to_safe_upper(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_size; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        dst[i] = (c >= 0x20 && c < 0x7f) ? c : '_';
    }
    dst[i] = '\0';
}

static uint32_t to_u32(double v)
{
    if (v <= 0) {
        return 0;
    }
    if (v > 4294967040.0) {
        return 4294967040u;
    }
    return (uint32_t)(v + 0.5);
}

/* cJSON_GetObjectItem 大小写不敏感，因此别名只需覆盖不同拼写 */
static bool json_num_any(const cJSON *obj, const char *const *keys, size_t key_cnt, double *out)
{
    for (size_t i = 0; i < key_cnt; i++) {
        const cJSON *item = cJSON_GetObjectItem(obj, keys[i]);
        if (cJSON_IsNumber(item)) {
            *out = item->valuedouble;
            return true;
        }
        if (cJSON_IsString(item)) {
            /* 兼容 host 把数值写成字符串的情况 */
            char *end = NULL;
            double v = strtod(item->valuestring, &end);
            if (end != NULL && end != item->valuestring) {
                *out = v;
                return true;
            }
        }
    }
    return false;
}

static const char *json_str_any(const cJSON *obj, const char *const *keys, size_t key_cnt)
{
    for (size_t i = 0; i < key_cnt; i++) {
        const cJSON *item = cJSON_GetObjectItem(obj, keys[i]);
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            return item->valuestring;
        }
    }
    return NULL;
}

#define NUM_KEYS(keys) (sizeof(keys) / sizeof((keys)[0]))

static bool get_ctx_used(const cJSON *o, double *out)
{
    static const char *const keys[] = { "ctx", "ctx_used", "context", "context_tokens", "context_used" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

static bool get_ctx_max(const cJSON *o, double *out)
{
    static const char *const keys[] = { "ctx_max", "ctx_limit", "ctx_size", "ctx_window",
                                        "context_max", "context_limit", "context_size", "context_window" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

static bool get_tok_in(const cJSON *o, double *out)
{
    static const char *const keys[] = { "in", "in_tokens", "input", "input_tokens", "tokens_in", "prompt_tokens" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

static bool get_tok_out(const cJSON *o, double *out)
{
    static const char *const keys[] = { "out", "out_tokens", "output", "output_tokens", "tokens_out", "completion_tokens" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

static bool get_tps(const cJSON *o, double *out)
{
    static const char *const keys[] = { "tps", "tok_s", "tokens_per_second", "tokens_per_sec", "speed" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

static bool get_elapsed(const cJSON *o, double *out)
{
    static const char *const keys[] = { "elapsed", "elapsed_s", "elapsed_sec", "duration", "duration_s", "dur" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

static bool get_turn(const cJSON *o, double *out)
{
    static const char *const keys[] = { "turn", "step", "round", "iteration" };
    return json_num_any(o, keys, NUM_KEYS(keys), out);
}

/* ------------------------------ 更新逻辑 ------------------------------ */

static void set_state_locked(const char *raw)
{
    char upper[AGENT_STATE_NAME_MAX];
    to_safe_upper(raw, upper, sizeof(upper));
    if (upper[0] == '\0') {
        return;
    }

    agent_state_t state = AGENT_STATE_UNKNOWN;
    for (size_t i = 0; i < sizeof(k_state_table) / sizeof(k_state_table[0]); i++) {
        if (strcmp(upper, k_state_table[i].name) == 0) {
            state = k_state_table[i].state;
            break;
        }
    }

    if (strcmp(s_m.st.state_name, upper) != 0) {
        copy_str(s_m.st.state_name, upper, sizeof(s_m.st.state_name));
        s_m.state_change_us = esp_timer_get_time();
        s_m.host_elapsed_s = -1.0f;  /* 新状态，本地重新计时 */
    }
    s_m.st.state = state;
}

void status_model_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    memset(&s_m, 0, sizeof(s_m));
    s_m.st.tps = -1.0f;
    s_m.st.state = AGENT_STATE_UNKNOWN;
    copy_str(s_m.st.state_name, "UNKNOWN", sizeof(s_m.st.state_name));
    s_m.host_elapsed_s = -1.0f;
    s_m.state_change_us = esp_timer_get_time();
}

void status_model_mark_rx(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_m.last_rx_us = esp_timer_get_time();
    s_m.st.host_seen = true;
    xSemaphoreGive(s_lock);
}

void status_model_reset(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool seen = s_m.st.host_seen;
    int64_t last_rx = s_m.last_rx_us;
    uint32_t rev = s_m.st.rev;

    memset(&s_m.st, 0, sizeof(s_m.st));
    s_m.st.tps = -1.0f;
    s_m.st.host_seen = seen;
    s_m.st.rev = rev;
    copy_str(s_m.st.state_name, "UNKNOWN", sizeof(s_m.st.state_name));
    s_m.last_rx_us = last_rx;
    s_m.host_elapsed_s = -1.0f;
    s_m.state_change_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

bool status_model_apply_json(const cJSON *obj)
{
    if (s_lock == NULL || !cJSON_IsObject(obj)) {
        return false;
    }

    double v = 0;
    bool touched = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const char *state = json_str_any(obj, (const char *const[]) { "state", "status" }, 2);
    if (state != NULL) {
        set_state_locked(state);
        touched = true;
    }

    if (get_ctx_used(obj, &v))  { s_m.st.ctx_used = to_u32(v); touched = true; }
    if (get_ctx_max(obj, &v))   { s_m.st.ctx_max  = to_u32(v); touched = true; }
    if (get_tok_in(obj, &v))    { s_m.st.tok_in   = to_u32(v); touched = true; }
    if (get_tok_out(obj, &v))   { s_m.st.tok_out  = to_u32(v); touched = true; }
    if (get_tps(obj, &v))       { s_m.st.tps      = (float)v;  touched = true; }
    if (get_elapsed(obj, &v))   { s_m.host_elapsed_s = (float)v; touched = true; }
    if (get_turn(obj, &v))      { s_m.st.turn     = to_u32(v); touched = true; }

    if (touched) {
        s_m.st.rev++;
    }
    xSemaphoreGive(s_lock);
    return touched;
}

void status_model_get(agent_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_m.st;

    const int64_t now = esp_timer_get_time();
    if (s_m.st.host_seen) {
        int64_t age_us = now - s_m.last_rx_us;
        if (age_us < 0) {
            age_us = 0;
        }
        out->age_ms = (uint32_t)(age_us / 1000);
        out->link_up = age_us <= (int64_t)CONFIG_STATUS_BAR_LINK_TIMEOUT_MS * 1000;
    }

    if (s_m.host_elapsed_s >= 0.0f) {
        out->elapsed_s = s_m.host_elapsed_s;
    } else {
        out->elapsed_s = (float)(now - s_m.state_change_us) / 1000000.0f;
    }
    xSemaphoreGive(s_lock);
}
