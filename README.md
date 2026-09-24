# ESP32 Vibe Coding 状态栏

给终端里的编码 agent 配一块**实体状态灯**：ESP32-C3 + 0.96 寸 128x32 单色 OLED，
由 Pi 上的扩展通过 USB 串口把「在想 / 在写 / 在跑工具 / 上下文占用 / token 数 / TPS / 耗时」
实时推过来，屏幕轮播展示。不用切窗口就能扫一眼知道 agent 现在在干什么。

```
┌─────────────────────────────┐
│  ▶  │ 28K/200K     94.9%    │   CTX 页：已用 / 窗口 + 缓存命中率
│     │ ▓▓▓▓▓▓░░░░░░░░ 14.2%  │           进度条 + 上下文占比
├─────────────────────────────┤
│  ▶  │ ↑ 1.2k  ↓ 567         │   TOK 页：输入 / 输出 token
│     │ 42.5t/s  12.4s        │           速度 / 耗时
└─────────────────────────────┘
   状态图标        108px 文本区
```

左侧图标常驻表示状态（▶ 运行、齿轮 工具、⏳ 等待、✔ 完成、✖ 报错，`thinking` 是转动的弧），
右侧 2 页自动轮播；链路断了会固定显示 `NO HOST`（副行 `lost Ns` 是超时，`host exit` 是
主机主动退出）。

ESP32 旁边那颗 SK6812 状态灯用灯效复述同一份状态，不用回头盯屏幕也能扫一眼：

| 场景 | 颜色 | 灯效 |
|---|---|---|
| 没主机（未连 / 断链） | 白 | 双拍心跳，1.5s 一组 |
| 空闲 | 青 | 极慢呼吸 |
| 思考 | 彩虹 | 色相 4s 转一圈 + 呼吸起伏 |
| 生成 | 蓝 | 1s 脉冲 |
| 跑工具 | 绿 | 双闪后停顿（像磁盘活动灯） |
| 等确认 / 等输入 | 琥珀 | 快呼吸 |
| 完成 | 绿 | 三连闪后渐隐 |
| 出错 | 红 | 2Hz 硬闪 |

没主机持续到熄屏（默认 60s）时，灯和屏幕一起灭。

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-C3，2MB flash（单 app 分区 1MB，固件约占 78%） |
| 屏幕 | SSD1315（寄存器兼容 SSD1306），128x32 单色 OLED，I2C |
| 屏幕接线 | **SDA = GPIO8，SCL = GPIO9**，400kHz，从机地址 `0x3C`，模块无 RESET 脚 |
| 状态灯 | SK6812 RGBW 单灯珠，数据脚 **GPIO4**（`Status LED → GPIO` 可改），RMT 驱动 |
| 供电 | 屏 VCC 接 3V3；灯珠接 **5V**、GND 共地，数据线中间串 330Ω |

ESP32-C3 的 USB 占用 GPIO18/19，这两脚别接别的东西。

灯珠三根线：`5V` → 5V（接 3V3 会不亮或极暗），`Din` → GPIO4（**接成 `Dout` 完全不亮**，
但不会烧），`GND` → 必须和 ESP32 共地。上电会依次点亮红/绿/蓝/白各 150ms 做自检
（`Status LED → Sweep R/G/B/W once at boot`），点红亮绿说明字节序是 RGBW，打开
`Status LED → Use RGBW byte order` 即可。完整接线/排查记录见 `docs/led_ctrol.md`。

状态灯默认亮度只有 **40/255**（约 16%），跑起来是个安静的小指示灯；嫌亮/嫌暗改
`Status LED → Default brightness`，或者用串口/`/statusbar led` 临时热调。

## 快速开始

### 1. 编译烧录

需要 ESP-IDF **v6.1**（或更新）。`idf.py` 会自动按 `main/idf_component.yml` 拉取
LVGL 9.5 / esp_lvgl_port 2.9 / cjson / led_strip 依赖。首次构建需要联网下载托管组件，
国内建议先挂代理（`export HTTPS_PROXY=http://localhost:7897 HTTP_PROXY=http://localhost:7897`）；
另外确保工具链是 **GCC**（`unset IDF_TOOLCHAIN`），clang 下 RMT 会静默卡死，见
`docs/led_ctrol.md` 坑 1。

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
| `/statusbar led 24` | 状态灯亮度调到 24/255（`on` / `off` 开关整灯） |

### 3. 不装 Pi 也能测

```bash
uv run tools/mock_status.py              # 跑一遍完整的思考→生成→调工具→完成
uv run tools/mock_status.py --mode loop  # 循环
uv run tools/mock_status.py --send '{"state":"error","tps":0}'
uv run tools/mock_status.py --led-brightness 24   # 顺便把状态灯调暗（或 --led-off）
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
{"state":"thinking","ctx":12000,"ctx_max":200000,"ctx_pct":6.0,"cache_pct":94.9,
 "in":1234,"out":567,"tps":42.5,"elapsed":3.2}
```

| 字段 | 含义 | 别名 |
|---|---|---|
| `state` | `idle` `thinking` `running` `tool` `waiting` `done` `error`，也接受任意自定义字符串 | `status` |
| `ctx` | 上下文已用 token | `ctx_used` `context` `context_tokens` `context_used` |
| `ctx_max` | 上下文窗口 | `ctx_limit` `ctx_size` `ctx_window` `context_max` … |
| `ctx_pct` | 上下文占用百分比（保留 1 位小数，屏幕直接显示这个数），**负数 = 未知** | `ctx_percent` `context_pct` `context_percent` |
| `cache_pct` | 缓存命中百分比（保留 1 位小数），**不给 / 负数 = 屏幕不显示这一栏** | `cache_percent` `cache_hit` `cache_hit_rate` |
| `in` | 累计输入 token | `in_tokens` `input` `input_tokens` `tokens_in` `prompt_tokens` |
| `out` | 累计输出 token | `out_tokens` `output` `output_tokens` `tokens_out` `completion_tokens` |
| `tps` | token/s | `tok_s` `tokens_per_second` `speed` |
| `elapsed` | 当前步骤耗时（秒） | `elapsed_s` `duration` `dur` |

没有 `state` 里的标准值时显示原始字符串（过滤成可打印的大写 ASCII）。
数值写成字符串也能解析。

`ctx_pct` / `cache_pct` 是**为了两个屏幕对得上**才由主机直接下发的：主机自己算好它显示的
那两个数（Pi 扩展取 `getContextUsage().percent` 和底栏的 `CH`）原样给过来，ESP 不再重算 ——
一边是 JS 按 double 舍入、一边是整数/F 运算，总会差一档。`ctx_pct` 没给时 ESP 会退回用
`ctx/ctx_max` 自己算（`tools/mock_status.py` 就是这种）；`cache_pct` 推不出来，不给就一直空着。

### 控制命令（host → ESP32）

```json
{"cmd":"ping"}                       // → {"evt":"pong","up_ms":...,"page":...,"rx":...,"bad":...}
{"cmd":"page","index":1,"hold":10}   // 切到第 1 页并保持 10 秒；index=-1 恢复自动轮播
{"cmd":"clear"}                      // 清空统计
{"cmd":"led","brightness":40,"on":true}  // 调状态灯；字段可选，都不给就是查询
{"cmd":"bye"}                        // host 要退出了：立刻按断链渲染（副行 host exit），不用等超时
```

`led` 命令回 `{"evt":"ack","cmd":"led","brightness":40,"on":1}`。`brightness` 夹到 0..255，
`on` 开关整灯。**这个设置不落盘**：断电重启回到 Kconfig 的 `Default brightness`，
免得调暗之后找不回来。

`bye` 由 Pi 扩展在真正退出时（`session_shutdown` 且 `reason=quit`）发出。切换/新建/分叉
会话也会触发同一个事件，那几种情况不发，否则屏幕会白闪一下。超时断链仍然显示
`lost Ns`，两者在副行上区分开；下一次收到任何主机数据时 `host_gone` 自动清除。

### 下行事件（ESP32 → host）

```json
{"evt":"boot","fw":"vibe-status-bar","ver":1,"hres":128,"vres":32,"page_ms":4000,"pages":2}
{"evt":"hb","up_ms":123456,"page":0,"rx":42,"bad":0}   // 默认每 10s
```

ESP32 默认每 10s 发一次心跳；超过 10s 没收到 host 数据，屏幕显示 `NO HOST`。

> 协议是**双向**的：Pi 扩展用 `O_RDWR` 打开串口并持续读走 ESP 的下行（心跳/事件），
> 还要把 tty 设成 `raw`。只写不读会让主机的 tty 缓冲涨满，反过来把 ESP 的 RX 一起
> 拖死 —— 症状就是「agent 明明连着却偶现 `NO HOST`」。

## 目录结构

| 路径 | 作用 |
|---|---|
| `main/esp32-vibe-coding-status-bar.c` | 入口：初始化模型 → 显示 → UI → 串口 |
| `main/display.c` | I2C 总线 + SSD1306 面板 + esp_lvgl_port 初始化 |
| `main/ui.c` | LVGL 界面：状态图标、两行文本、2 页轮播、上电自检 |
| `main/status_model.c` | 状态模型 + cJSON 字段解析（互斥锁保护，供 UI 任务读） |
| `main/status_led.c` | SK6812 状态灯：RMT 驱动、亮度、灯效任务、上电自检 |
| `main/serial_link.c` | USB Serial/JTAG 收发、命令处理、心跳 |
| `main/fonts/` | 副行用的思源黑体 13px ASCII 子集（lv_font_conv 生成） |
| `pi-extension/esp32-status-bar.ts` | Pi 扩展：监听 agent 事件 → 推 JSON |
| `tools/mock_status.py` | 不依赖 Pi 的模拟数据发送器 |
| `AGENTS.md` | 给 AI 编码助手的工程笔记（踩过的坑都在这） |

## 配置项

`idf.py menuconfig` → **Vibe Coding Status Bar**：

- **Display**：分辨率、I2C 引脚 / 频率 / 地址、`Invert colors`（黑白反了就打开）、镜像
- **Status LED**：开关、数据脚（默认 GPIO4）、默认亮度（默认 40/255）、RGBW 字节序、
  灯效刷新周期、上电 R/G/B/W 自检
- **UI**：轮播间隔（默认 4000ms，0 关闭）、刷新周期（默认 200ms）、链路超时（默认 10000ms，
  建议 ≥ 主机保活间隔的 3 倍）、
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
| 明明连着却偶现 `NO HOST` | 串口**下行没人排空**（`TIOCINQ` 顶在 3920/4096 就是它），或 tty 没设 `raw` 导致回显污染。关掉 `idf.py monitor` 再看，别被它掩盖 |
| 屏幕来回跳 | mock 脚本和 Pi 扩展同时在写串口，只能留一个 |
| 串口打不开 | USB Serial/JTAG 需为副控制台（UART0 主），见 `sdkconfig.defaults` |
| 状态灯完全不亮 | 数据线接的是 `Din` 还是 `Dout`？有没有共地？灯珠供电实测是不是 5V？见 `docs/led_ctrol.md` 第 7 节 |
| 状态灯颜色不对（点红亮绿） | 字节序，打开 `Status LED → Use RGBW byte order` |
| 屏幕和串口都正常，但状态灯不亮/常绿、也不做上电自检 | 工具链是 clang（RMT 传输完成中断不触发，`refresh` 会永久等待）：`unset IDF_TOOLCHAIN && idf.py fullclean` 后用 GCC 重编 |
| 状态灯太亮 / 太暗 | `Status LED → Default brightness`，或运行时 `/statusbar led <0-255>` |
| 构建卡在 `Solving dependencies` | 拉 `led_strip` 托管组件需要代理，见「快速开始 → 编译烧录」 |
| `idf.py size` 分区吃紧 | CJK 字库占 ~157KB，换小字库或关掉不用的字体 |
