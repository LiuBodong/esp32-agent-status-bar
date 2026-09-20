/*
 * Agent 状态模型：串口任务写入，UI 任务读取，用互斥锁保护。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGENT_STATE_UNKNOWN = 0,
    AGENT_STATE_IDLE,
    AGENT_STATE_THINKING,
    AGENT_STATE_RUNNING,
    AGENT_STATE_TOOL,
    AGENT_STATE_WAITING,
    AGENT_STATE_DONE,
    AGENT_STATE_ERROR,
} agent_state_t;

#define AGENT_STATE_NAME_MAX 12

typedef struct {
    agent_state_t state;
    char     state_name[AGENT_STATE_NAME_MAX]; /* 已转大写，host 未识别的名字原样保留 */
    uint32_t ctx_used;                          /* 上下文已用 token */
    uint32_t ctx_max;                           /* 上下文窗口大小，0 = 未知 */
    uint32_t tok_in;                            /* 最近一次请求输入 token */
    uint32_t tok_out;                           /* 最近一次回复输出 token */
    float    tps;                               /* token/s，<0 表示未知 */
    float    elapsed_s;                         /* 当前步骤耗时：host 值优先，否则本地计时 */
    uint32_t turn;                              /* 轮次 / 步骤序号 */
    bool     link_up;                           /* 最近 STATUS_BAR_LINK_TIMEOUT_MS 内收到过数据 */
    bool     host_seen;                         /* 上电后是否收到过任何数据 */
    uint32_t age_ms;                            /* 距上一次收到 host 数据的时间 */
    uint32_t rev;                               /* 每次成功更新 +1 */
} agent_status_t;

/** @brief 初始化（只需调用一次） */
void status_model_init(void);

/** @brief 取一份状态快照（线程安全） */
void status_model_get(agent_status_t *out);

/** @brief 复位所有状态（保留链路信息） */
void status_model_reset(void);

/** @brief 标记「刚收到 host 数据」，用于链路存活判定 */
void status_model_mark_rx(void);

/**
 * @brief 用一条已解析的 JSON 对象更新状态
 *
 * @param[in] obj cJSON 对象
 * @return true 表示至少更新了一个字段
 */
bool status_model_apply_json(const cJSON *obj);

/** @brief 状态枚举对应的内置名称 */
const char *status_state_name(agent_state_t state);

#ifdef __cplusplus
}
#endif
