# ESP32 Vibe Coding 状态栏

给终端里的编码 agent 配一块**实体状态灯**：ESP32-C3 + 0.96 寸 128x32 单色 OLED，
由 Pi 上的扩展通过 USB 串口把「在想 / 在写 / 在跑工具 / 上下文占用 / token 数 / TPS / 耗时」
实时推过来，屏幕轮播展示。不用切窗口就能扫一眼知道 agent 现在在干什么。

```
┌─────────────────────────────┐
│  ▶  │ 28K/200K              │   CTX 页：上下文已用 / 窗口
│     │ ▓▓▓▓▓▓░░░░░░░░ 14%    │           进度条 + 百分比
├─────────────────────────────┤
│  ▶  │ ↑ 1.2k  ↓ 567         │   TOK 页：输入 / 输出 token
│     │ 42.5 t/s  T3  12.4s   │           速度 / 轮次 / 耗时
└─────────────────────────────┘
   状态图标        108px 文本区
```

左侧图标常驻表示状态（▶ 运行、齿轮 工具、⏳ 等待、✔ 完成、✖ 报错，`thinking` 是转动的弧），
右侧 2 页自动轮播；链路断了会固定显示 `NO HOST`。

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-C3，2MB flash（单 app 分区 1MB，固件约占 78%） |
| 屏幕 | SSD1315（寄存器兼容 SSD1306），128x32 单色 OLED，I2C |
| 接线 | **SDA = GPIO8，SCL = GPIO9**，400kHz，从机地址 `0x3C`，模块无 RESET 脚 |
| 供电 | 屏 VCC 接 3V3，GND 共地 |

ESP32-C3 的 USB 占用 GPIO18/19，这两脚别接别的东西。

## 快速开始

### 1. 编译烧录

需要 ESP-IDF **v6.1**（或更新）。`idf.py` 会自动按 `main/idf_component.yml` 拉取
LVGL 9.5 / esp_lvgl_port 2.9 / cjson 依赖。

```bash
idf.py set-target esp32c3          # 首次
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

上电会先整屏点亮 400ms 做自检（`CONFIG_STATUS_BAR_BOOT_SELFTEST`）。
**这一步看不到亮，就是接线/I2C 问题，不用怀疑字体。**

### 2. 装 Pi 扩展

```bash
ln -s "$PWD/pi-extension/esp32-status-bar.ts" ~/.pi/agent/extensions/
```

串口选择顺序：`--statusbar-port` 参数 → `PI_STATUSBAR_PORT` 环境变量 → 自动探测 `/dev/ttyACM*`、`/dev/ttyUSB*`。
拔掉设备不会报错，扩展每 5s 重试一次；插回来会自动接上。

跑起来后：

| 命令 | 作用 |
|---|---|
| `/statusbar` | 看连接状态（已连接 / 重试中 / 已关闭） |
| `/statusbar test` | 发一段演示数据，不依赖 agent 状态，用来验证屏幕 |
| `/statusbar port /dev/ttyACM1` | 换串口 |
| `/statusbar on` / `off` | 临时开关推流 |

### 3. 不装 Pi 也能测

```bash
uv run tools/mock_status.py              # 跑一遍完整的思考→生成→调工具→完成
uv run tools/mock_status.py --mode loop  # 循环
uv run tools/mock_status.py --send '{"state":"error","tps":0}'
```

脚本是 PEP 723 格式，`uv` 会自己准备 pyserial 环境，无需手动装依赖。
它会同时打印 ESP32 回传的 `boot` / `hb` / `pong` 事件。

> **别同时跑 mock 脚本和 Pi 扩展**——两边都在往同一个串口写，屏幕会来回跳。
> 另外 `idf.py monitor` 常驻不影响 Pi 扩展：monitor 只读，扩展只写。

## 通信协议

一行一条 JSON，`\n` 结尾；**只有以 `{` 开头的行会被解析**，所以日志回显不会干扰。
字段全部可选、大小写不敏感，没给的字段保持原值。

### 状态上报（host → ESP32）

```json
{"state":"thinking","ctx":12000,"ctx_max":200000,"in":1234,"out":567,"tps":42.5,"elapsed":3.2,"turn":2}
```

| 字段 | 含义 | 别名 |
|---|---|---|
| `state` | `idle` `thinking` `running` `tool` `waiting` `done` `error`，也接受任意自定义字符串 | `status` |
| `ctx` | 上下文已用 token | `ctx_used` `context` `context_tokens` `context_used` |
| `ctx_max` | 上下文窗口 | `ctx_limit` `ctx_size` `ctx_window` `context_max` … |
| `in` | 累计输入 token | `in_tokens` `input` `input_tokens` `tokens_in` `prompt_tokens` |
| `out` | 累计输出 token | `out_tokens` `output` `output_tokens` `tokens_out` `completion_tokens` |
| `tps` | token/s | `tok_s` `tokens_per_second` `speed` |
| `elapsed` | 当前步骤耗时（秒） | `elapsed_s` `duration` `dur` |
| `turn` | 轮次 / 步骤序号 | `step` `round` `iteration` |

没有 `state` 里的标准值时显示原始字符串（过滤成可打印的大写 ASCII）。
数值写成字符串也能解析。

### 控制命令（host → ESP32）

```json
{"cmd":"ping"}                       // → {"evt":"pong","up_ms":...,"page":...,"rx":...,"bad":...}
{"cmd":"page","index":1,"hold":10}   // 切到第 1 页并保持 10 秒；index=-1 恢复自动轮播
{"cmd":"clear"}                      // 清空统计
```

### 下行事件（ESP32 → host）

```json
{"evt":"boot","fw":"vibe-status-bar","ver":1,"hres":128,"vres":32,"page_ms":4000,"pages":2}
{"evt":"hb","up_ms":123456,"page":0,"rx":42,"bad":0}   // 默认每 10s
```

ESP32 默认每 10s 发一次心跳；超过 5s 没收到 host 数据，屏幕显示 `NO HOST`。

## 目录结构

| 路径 | 作用 |
|---|---|
| `main/esp32-vibe-coding-status-bar.c` | 入口：初始化模型 → 显示 → UI → 串口 |
| `main/display.c` | I2C 总线 + SSD1306 面板 + esp_lvgl_port 初始化 |
| `main/ui.c` | LVGL 界面：状态图标、两行文本、2 页轮播、上电自检 |
| `main/status_model.c` | 状态模型 + cJSON 字段解析（互斥锁保护，供 UI 任务读） |
| `main/serial_link.c` | USB Serial/JTAG 收发、命令处理、心跳 |
| `main/fonts/` | 副行用的思源黑体 13px ASCII 子集（lv_font_conv 生成） |
| `pi-extension/esp32-status-bar.ts` | Pi 扩展：监听 agent 事件 → 推 JSON |
| `tools/mock_status.py` | 不依赖 Pi 的模拟数据发送器 |
| `AGENTS.md` | 给 AI 编码助手的工程笔记（踩过的坑都在这） |

## 配置项

`idf.py menuconfig` → **Vibe Coding Status Bar**：

- **Display**：分辨率、I2C 引脚 / 频率 / 地址、`Invert colors`（黑白反了就打开）、镜像
- **UI**：轮播间隔（默认 4000ms，0 关闭）、刷新周期（默认 200ms）、链路超时（默认 5000ms）、
  开机自检开关
- **USB Serial/JTAG link**：收发缓冲大小、单行最大长度、心跳间隔、是否打印每条收到的 JSON

反锯齿灰度转 1bpp 的阈值在 `sdkconfig.defaults` 里（`CONFIG_LV_DRAW_SW_I1_LUM_THRESHOLD`，
调小字更粗），注释掉默认用 127。

## 排错

| 现象 | 排查方向 |
|---|---|
| 上电不自检、屏幕全黑 | 接线 / 供电 / `0x3C` 地址；试 `idf.py monitor` 看 I2C 是否 ACK |
| 显示内容黑白反了 | menuconfig 打开 `Display → Invert colors` |
| 屏幕一直 `NO HOST` | host 没在发数据；mock 脚本要记得先停 Pi 扩展 |
| 屏幕来回跳 | mock 脚本和 Pi 扩展同时在写串口，只能留一个 |
| 串口打不开 | USB Serial/JTAG 需为副控制台（UART0 主），见 `sdkconfig.defaults` |
| `idf.py size` 分区吃紧 | CJK 字库占 ~157KB，换小字库或关掉不用的字体 |
