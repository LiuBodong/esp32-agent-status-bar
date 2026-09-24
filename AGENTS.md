# ESP32 Vibe Coding 状态栏

ESP32-C3 + SSD1315（0.96 寸 128x32 单色 OLED）的 Agent 状态指示器：Pi（终端编码 agent）
通过 USB Serial/JTAG 把运行状态实时推给 ESP32，屏幕轮播展示。

## 构建与烧录：必须由用户手动执行

- **不要自动跑 `idf.py build` / `idf.py flash` / `idf.py monitor`，也不要放后台跑**。
  改完代码就停下、汇报改动，编译和烧录由用户自己决定什么时候做。
- 不要为了「验证一下能不能编过」而擅自构建：编译失败、占满 CPU、重刷设备都由用户承担。
  需要编译期确认时，在回复里说明该跑什么命令，等用户自己跑。
- 静态检查（如 `tsc --strict` 校验 Pi 扩展）不属于构建，可以照常做。
- 改完 `pi-extension/esp32-status-bar.ts` 只是改了仓库里的源文件，用户要自己 `cp` 到
  `~/.pi/agent/extensions/` 并重开会话才生效。

## 硬件

- 芯片：ESP32-C3，2MB flash（单 app 分区 1MB，目前固件约占 78%）
- 屏幕：SSD1315（寄存器兼容 SSD1306），128x32，**I2C**
  - **SDA = GPIO8，SCL = GPIO9**，400kHz，从机地址 0x3C，模块无 RESET 引脚
  - C3 的 USB 占用 GPIO18/19，选引脚时避开
- 状态灯：SK6812 **RGBW 单灯珠**，数据脚 **GPIO4**（`Status LED → GPIO` 可改），RMT 驱动
  - `5V` 供电（接 3V3 会不亮/极暗）、数据线串 330Ω、必须共地；`Din` 接反完全不亮但不烧
  - 为什么不选别的脚：见 `docs/led_ctrol.md` 1.3（GPIO2/8/9 是 strapping，20/21 是 UART0）
- 通信：USB Serial/JTAG（Espressif VID 303a，枚举为 `/dev/ttyACM0`）
  - 日志走 UART0（USB 是副控制台，不会抢 USJ 驱动），所以 Pi 扩展可以独占 USJ 收发

## 技术栈（版本不要随便动）

- ESP-IDF **6.2.0**（master；`git describe` 是 `v6.1-dev-8142-g188e3e55bb`，版本以
  `tools/cmake/version.cmake` 的 6/2/0 为准）。v6 已移除内置的 `json`/cJSON 组件，所以用 `espressif/cjson`
- LVGL **~9.5.0** + `espressif/esp_lvgl_port ^2.9.0`（这个组合验证过；9.6 太新没用过）
- 状态灯用 `espressif/led_strip ^3.0.3`（RMT 后端）。托管组件，构建时要联网（挂代理）
- Pi 扩展用 Pi 0.85.x 的 Extension API，已用其官方类型做过 `tsc --strict` 校验
- CodeBuddy 侧没有进程内扩展 API，用 hooks（`SessionStart`/`SessionEnd`）+ 常驻 daemon，
  协议细节见下面的「CodeBuddy 接入」

## 目录

| 路径                                 | 作用                                                                                       |
| ------------------------------------ | ------------------------------------------------------------------------------------------ |
| `main/display.c`                   | I2C 总线 + SSD1306 面板 + esp_lvgl_port 初始化、面板开关（熄屏/亮屏）                      |
| `main/ui.c`                        | LVGL 界面：左状态图标 + 右两行文本、2 页轮播、上电自检、断链熄屏                           |
| `main/status_model.c`              | 状态模型 + cJSON 字段解析（互斥锁保护）                                                    |
| `main/status_led.c`                | SK6812 状态灯：RMT 驱动、亮度、灯效任务、上电 R/G/B/W 自检                                 |
| `main/serial_link.c`               | USB Serial/JTAG 收发、命令处理、心跳；协议说明在文件头注释                                 |
| `main/fonts/`                      | 副行用的思源黑体 13px ASCII 子集（lv_font_conv 生成）                                      |
| `tools/mock_status.py`             | 不依赖 Pi，直接给屏幕灌模拟数据（PEP 723，`uv run`）                                     |
| `tools/cb-status-daemon.py`        | CodeBuddy 侧的常驻 daemon：tail 会话记录 + 状态机 + 独占串口（PEP 723，`uv run`）        |
| `tools/cb-status-hook.py`          | CodeBuddy hook：只往握手文件写`transcript_path` / 会话结束事件，不碰串口                 |
| `pi-extension/esp32-status-bar.ts` | Pi 扩展；用`cp` 装到 `~/.pi/agent/extensions/`，**改完要重新拷并重开会话才生效** |

## 关键约定与坑

### 组件与工具链

- **托管组件只能由组件管理器下载，不能把上游 git 仓库整体拷进 `managed_components/`**。
  手拷的目录没有 `.component_hash`（registry 包另外还带 `CHECKSUMS.json`），IDF 6.x 在 cmake
  配置阶段直接报 `File .component_hash or CHECKSUMS.json ... does not exist or cannot be parsed`
  并中止。默认非 strict 模式（`IDF_COMPONENT_STRICT_CHECKSUM` 没开）只比对 `.component_hash`
  的**内容**、不逐文件校验，所以缺这个文件就是硬错误、没得商量。修法：删掉该组件目录
  （`managed_components/` 和 `dependencies.lock` 都在 `.gitignore` 里，可安全重建），挂代理
  重跑 `idf.py reconfigure`（build 目录残缺就先删掉再跑）。一眼分辨来源：目录里带
  `.devcontainer` / `.pre-commit-config.yaml` / `SConscript` 的是 git 版，不是 registry 包
- **GCC 16 的 `-Wformat-truncation` 在 `-Werror` 下会因 `snprintf` 拼接多个 `%s` 而报错**。
  它按实参的**缓冲区上限**推最坏长度：`"%s t/s  %s"` 配 `speed[12]` + `elapsed[16]` 就是
  11 + 6 + 15 + 1（结束符）= 33 字节，目标缓冲 32 就过不了（实际内容远短，是保守误报）。
  修法：把目标缓冲配到 ≥「各子串上限之和 + 1」（`ui.c:render_page()` 的 `sub_text` 因此取 40）。
  `%f`（如 `%.1f`）不在推断范围内、走参数化 `out_size` 的也不报，别去动那些

### 显示

- 单色屏走 LVGL `LV_COLOR_FORMAT_I1`，**必须用整屏缓冲**（`hres*vres`），这是 esp_lvgl_port 的硬要求
- esp_lvgl_port 把 I1 位图转成 SSD1315 页格式时会取反：**LVGL 里画黑色 = 屏幕上点亮**。
  所以 `ui.c` 用 `COLOR_LIT = lv_color_black()`。实机黑白反了就打开 `Display → Invert colors`
- 版式：左侧 `x=0..17` 是 18x16 的状态图标，右侧 `x=20..127` 两行文字（**左对齐**，数值位数
  变化时整行不会左右抖）。主行 `y=0..15` 用 LVGL 自带 `lv_font_source_han_sans_sc_14_cjk`
  （行高 17，字形占 0..14，约 157KB），副行 `y=16..31` 用自造的 13px ASCII 子集（行高 15）
- 主行有两套版式，都是 flex 容器（`ui.c:set_main_row()`，同一行混不了两种字体，所以各段独立 label）：
  - CTX 页：`28K/200K`（14px 主字体）+ `95.4%` 缓存命中率（13px 副字体）。不写 `CTX` 前缀，
    数值按 **1000 进制阶梯**退化（`fmt_ctx()`：999 → 999，1000 → 1K，1000000 → 1M，截断取整）。
    用 1000 而不是 1024，是因为模型窗口都是十进制标称的（200000 / 1000000），
    这样才显示得成 `200K` / `1M`。缓存命中率主机不给就把右段收起来、左段撑满整行
  - TOK 页：`↑ 1.2k ↓ 567`。箭头是 montserrat 符号、数字是思源黑体
- `fmt_count()` 的分段和取整必须和 Pi 底栏 `formatTokens()`（`footer.js`）**逐条对齐**：
  `<1000` 原样 / `<10k` 一位小数 / `<1M` 四舍五入到 k / `<10M` 一位小数 M / 其余四舍五入到 M。
  改任一边都要同步，否则会出现「屏幕 29k、底栏 30k」这种对不上的情况
- 上下文百分比 / 缓存命中率**都由主机下发**（`ctx_pct` / `cache_pct`，1 位小数，
  负数 = 未知），ESP 不重算。JS 那边是 `toFixed(1)`（按 double 舍入），一边整数一边浮点必然
  在某些值上差一档（例：300/200000 精确等于 0.15%，JS 的 double 是 0.14999… → `0.1%`，
  精确十进制会给 `0.2%`）。`ctx_pct` 没给时才退回用 `ctx/ctx_max` 自己算
  （`tools/mock_status.py` 走这条）；`cache_pct` 推不出来，不给就不显示
- 右列只有 108px，改文案前先按 `.adv_w/16` 量一遍（两套字体都能从字库源文件里解析出来）：
  14px：数字 7.6875 / `/` 5.5 / `K` 8.94 / `%` 12.81 / 空格 3.125；
  13px：数字 7.1875 / `.` 3.625 / `:` 3.625 / `%` 12 / 空格 2.94。
  现在卡得最紧的是 CTX 页这两处，都是按「最坏取值」算出来的：
  - 主行：最宽文本 `199K/200K` = 69.5px、缓存 `99.9%` = 37.2px → `CTX_MAIN_W=70`
    + `CTX_CACHE_W=38` = 108px 刚好，中间不留间隔（靠字形侧边距分开）。
      满命中只写 `100%`（`100.0%` 要 44.4px，会撑破右段）
  - 副行：进度条 60px + 上下文占比 46px（`100.0%` = 44.4px）
- TOK 页的 `↑/↓` 口径 = **整个会话累计**，并且和 Pi 底栏一样把 assistant 消息、toolResult
  消息、`type:"usage"` 条目（cache_warm 等）以及 compaction/branch_summary 的 usage 全算进去
  （Pi 扩展里的 `collectUsageTotals()`）。**不要改成「本轮累计」** —— 那是自造口径，只会和底栏打架
- 累计值的刷新时机有坑：pi **先跑扩展 handler、再 `appendMessage`**（`agent-session.js:408-420`），
  所以在 `message_end` 里立刻扫 entries **必然少掉刚结束的那条**，屏幕会一直慢一轮。
  正确做法是落盘之后再算一次（扩展里 `scheduleStatsRefresh()` 延迟 30ms 防抖）+
  `turn_end`（`agent-session.js:447` 注释说明了此时消息和 tool results 都已落盘）兜底。
  这种「补算」必须绕开 `CTX_REFRESH_MS` 节流，否则会被吃掉
- 状态改用图标表示：`lv_font_montserrat_14` 里带全套 FontAwesome 符号（`LV_SYMBOL_PLAY` 等），
  映射见 `ui.c:state_symbol()`；`thinking` 用 `lv_arc` 画的 90° 弧逐档旋转。
  **不要用 LVGL 内置 montserrat 排正文**：它的字形墨迹常宽于 advance（负边距），小字号会挤在一起；
  图标是单字形独立 label，不受影响
- 动效一律跟着 UI 定时器走，**别用 `lv_anim`**：整屏缓冲 + full_refresh 下常驻动画会让 I2C 以 30fps 整帧刷屏
- 断链熄屏：`link_up` 为假（含上电后一直没连上）持续 `CONFIG_STATUS_BAR_SCREEN_OFF_MS`
  （默认 60s，0 = 常亮）就调 `display_set_on(false)` 发 SSD1306 的关显示命令，收到任何 host
  数据（`mark_rx()` → `link_up` 变真）下一拍就亮回来。判断放在 `ui_timer_cb` 里，用
  `s_link_down_since_ms` 自己计时 —— 别用 `host_gone` 或 `age_ms`，它们要么只覆盖 bye 场景，
  要么在上电从未连上时是 0。**熄屏期间照常渲染**（不跳过 `render_*`）：关显示只关掉驱动输出，
  GDDRAM 写入照样生效，这样唤醒那帧面板上已经是新内容，不会先闪一下过期的 `NO HOST`
- `main/fonts/*.c` 由 lv_font_conv 生成，需要 `-DLV_LVGL_H_INCLUDE_SIMPLE`（见 `main/CMakeLists.txt`）
- `main/Kconfig.projbuild` 里的 **bool 选项关闭时不生成宏**，判断要用 `#ifdef`

### 状态灯（SK6812 RGBW）

- `main/status_led.c` 用托管组件 `espressif/led_strip`（RMT 后端）。它是**托管组件**，
  首次构建要挂代理下载（`docs/led_ctrol.md` 坑 5）；`main/CMakeLists.txt` 还要显式
  `REQUIRES esp_driver_rmt`（`led_strip_rmt.h` 里用到 `RMT_CLK_SRC_DEFAULT`）
- C3 的 RMT 只有 4 个通道、**其中 2 个能做 TX**，led_strip 占掉 1 个。以后再加灯带 /
  红外收发之类的注意别把 TX 抢完
- **初始化不要用 `ESP_ERROR_CHECK`**（坑 3：失败走 abort，反而看不到原因）。`status_led_init()`
  失败只返回错误码，`app_main` 里 `ESP_LOGW` 一声继续跑 —— 灯坏了不该把屏幕和串口链路一起拖垮
- **工具链必须是 GCC**（坑 1）：clang 下 RMT 传输完成中断不触发，而 `led_strip_refresh()` 内部是
  `rmt_tx_wait_all_done(chan, -1)`（**永久等待**），会无声卡死。所以**上电自检和灯效都跑在灯效
  任务里、不在 `status_led_init()`（= `app_main`）里** —— 最坏情况只是这个任务停摆，屏幕和串口
  链路照常工作；放 init 里会让一个可选外设把整机启动卡死。换工具链后必须 `idf.py fullclean`
- 字节序按批次可能是 GRBW（默认）或 RGBW，点红亮绿就开 `Status LED → Use RGBW byte order`。
  上电自检依次点 R/G/B/W 就是为了一眼看出这个（也是确认接线的手段）
- 亮度管线是 **先 gamma 再乘主亮度**：`out = (v*v/255) * brightness / 255`（`scale_ch()`）。
  顺序别反 —— 先缩放再 gamma 等于把亮度压两次，会暗到看不见。默认 40/255（约 16%）
- 白灯走 **W 通道**（`white()`），彩色走 RGB（`hsv()` 只在 RGB 上算）
- 灯效跑在**独立 FreeRTOS 任务**里（`CONFIG_STATUS_BAR_LED_REFRESH_MS`，默认 40ms ≈ 25fps），
  **不要挂 LVGL 定时器** —— 那个跑在 LVGL 任务里，而屏幕整屏缓冲 + full_refresh 已经把 I2C 占满
- 断链 / 熄屏联动直接复用 `ui_screen_off()`（`ui.c` 里那份断链计时）：屏幕熄了灯一起灭。
  **不要再维护一套断链计时**，否则两边会不同步
- 亮度 / 开关是「串口任务写、灯效任务读」，用互斥锁保护（`output_cfg_get()`）；
  `{"cmd":"led"}` 改的亮度**不落盘**，重启回 Kconfig 默认
- 灯效映射见 `fx_for_state()`：idle 青呼吸 / thinking 彩虹转 / running 蓝脉冲 / tool 绿双闪 /
  waiting 琥珀快呼吸 / done 绿三连闪渐隐 / error 红 2Hz 闪；没主机（含上电未连）是白灯双拍心跳。
  单灯珠做不了尾焰/流水，所以「炫」全押在色相旋转 + 呼吸上

### 通信协议

一行一条 JSON、`\n` 结尾，只有以 `{` 开头的行会被解析；字段全部可选、大小写不敏感：

```json
{"state":"thinking","ctx":12000,"ctx_max":200000,"ctx_pct":6.0,"cache_pct":94.9,"in":1234,"out":567,"tps":42.5,"elapsed":3.2}
```

- `state`：`idle|thinking|running|tool|waiting|done|error`，也接受任意自定义字符串
- 字段别名、命令（`ping`/`page`/`clear`/`led`/`bye`）与下行事件（`boot`/`hb`/`pong`/`ack`）
  见 `main/serial_link.c` 头部注释
- `{"cmd":"led","brightness":40,"on":true}` 调状态灯，两个字段都可选（都不给 = 查询当前值），
  回 `{"evt":"ack","cmd":"led","brightness":40,"on":1}`。亮度夹到 0..255，**不落盘**
- `{"cmd":"bye"}` 是主机主动退出（Pi 扩展在 `session_shutdown` 且 `reason=quit` 时发），
  ESP 收到后把 `host_gone` 置位 → 立刻按断链渲染（`NO HOST` + 副行 `host exit`），
  不必等 10s 超时。**`session_shutdown` 在切换/新建/分叉会话时也会发（`reason` 不是 `quit`），
  那几种情况不能发 bye**，否则屏幕会白闪。下次收到任何主机数据时 `mark_rx()` 会清掉 `host_gone`
- ESP 每 10s 发一次心跳；10s 收不到 host 数据就显示 `NO HOST`
  （`STATUS_BAR_LINK_TIMEOUT_MS`，要 ≥ 主机保活间隔的 3 倍）
- **主机侧必须「双工 + raw」**：用 `O_RDWR` 打开串口并持续排空下行，且把 tty 设成
  `raw`（关 icanon/echo/ixon）。只写不读会让 host 的 tty 输入队列（canonical 上限 4KB）
  涨满 → USB IN 方向没人消费 → ESP 的 TX 环（512B）堵死 → 主机自己的写也跟着失败，
  表现就是 **agent 明明连着却偶现 `NO HOST`**（`TIOCINQ` 顶在 3920/4096 钉死不降）。
  没关 ECHO 更糟：ESP 会收到自己下行的回显，和真数据按字节粘成半行（
  `bad` 计数每个心跳 +1），还会把回显当成主机存活。所以 `mark_rx()` 只在真的带
  主机字段（或 `cmd`）的行上调用。

### CodeBuddy 接入（tail 会话记录）

CodeBuddy **没有 Pi 那样的进程内扩展 API**（插件只能声明 skills/commands/agents/hooks/MCP/LSP），
hook 又是「一件事 spawn 一个一次性进程」，做不到 Pi 扩展那种 2s 保活 + 100ms 排空串口的循环。
所以拆成两个进程：**daemon 独占串口**（排空/保活/状态机），**hook 只写一个握手文件**递路径。
串口始终只有 daemon 碰 —— hook 自己去开 tty 会把 raw 设置和字节流一起搅乱。

- 数据源：`~/.codebuddy/projects/<cwd 斜杠换横线>/<session>.jsonl`。实测**行级即时 flush**
  （条目落盘延迟 4~60ms，不用等一轮结束）
- **粒度是「块」不是 token**：块内部文件完全静默 —— 实测一次推理 21.7s 静默、一次 8 秒的工具
  期间 0 字节。所以 **`thinking` 和 `running` 分不开**（都是一段静默），`running` 这个状态暂时用不上；
  tps 只能按「一次响应」算平均且滞后一步；纯文本回答期间屏幕上没有任何进度可显示
- 拿到的：`ctx` = 最后一次响应的 `inputTokens`；`in`/`out` 累计 = **所有带 usage 的条目相加**；
  `cache_pct` = `cached_tokens / inputTokens`；`model` = `providerData.model`；
  工具名与工具耗时（`function_call` → `function_call_result`，实测精度 ~30ms）；`done` = `turn-metrics`
- **usage 挂在「本次模型响应的最后一条条目」上**：响应以工具调用收尾就挂在 `function_call`
  （并行调用时只有最后一条带），以纯文本收尾就挂在 `message`。所以收集时两种类型都要看，
  且见一条加一条 —— `conversationRequestId` 是「用户轮」级别的（一个 ID 底下挂过 50 条带 usage
  的条目），**不能拿来去重**
- **孤儿调用会钉死状态**：被中断的调用（`function_call` 自带 `status:"incomplete"`，或者干脆没有
  结果条目）永远等不到 `function_call_result`，只按 callId 配对的话状态会永久停在 `tool`。
  正确做法是「出现任何新响应的条目（reasoning/message/summary/turn-metrics）就结束工具阶段」
- `turn-metrics` 带 `source` 字段的是旁支轮次（`background-task`），**不是主轮结束**，要跳过
- `ctx_max` 不在 transcript 里（只有 ctx），只能 `--ctx-max` / `CB_STATUSBAR_CTX_MAX`；
  未知时发 0，屏幕显示 `83K/--` + 副行 `--`，不会显示错数字
- 刻意**不发 `elapsed`**（ESP 自己从状态变化时刻计时，比每 2 秒下一帧顺）和 **`ctx_pct`**
  （让 ESP 用 `ctx/ctx_max` 自己算，`ui.c:436` 那条兜底路径就是为它写的）
- hook 侧两条铁律：**绝不能往 stdout 写东西**（`SessionStart` 的 stdout 会被灌进模型上下文，
  打印个 "ok" 都会污染对话）；`SessionEnd` 的 `reason=clear` **不能发 bye**
  （紧接着就有新会话，发了屏幕会白闪一下）
- daemon 启动时读到的是历史状态（上次会话已经 `end` 了），只**采纳**不补发 bye，
  否则手动启动 daemon 会立刻把屏幕打成 `NO HOST`

安装（和 Pi 扩展一样，仓库里只是源文件，要手动装）：

```bash
mkdir -p ~/.codebuddy/hooks
cp tools/cb-status-hook.py ~/.codebuddy/hooks/

```json ~/.codebuddy/settings.json
{
"hooks": {
  "SessionStart": [{ "hooks": [{ "type": "command",
    "command": "python3 \"$HOME\"/.codebuddy/hooks/cb-status-hook.py" }] }],
  "SessionEnd":   [{ "hooks": [{ "type": "command",
    "command": "python3 \"$HOME\"/.codebuddy/hooks/cb-status-hook.py" }] }]
}
```

hook 只负责递路径和收工通知，**不负责拉起 daemon**：daemon 由用户手动跑（跑起来才占串口）。
没装 hook 也能用，daemon 会退化成扫描项目目录里最新的 `.jsonl`，代价是同一个项目开两个会话
时会挑错文件。

### 调试

- 上电自检：整屏点亮 400ms（`CONFIG_STATUS_BAR_BOOT_SELFTEST=y`）。看不到 → 查接线/供电，不是字体问题
- 状态灯上电自检：R/G/B/W 各 150ms（`Status LED → Sweep R/G/B/W once at boot`）。
  灯全不亮 → 查 `Din`/共地/供电；颜色错 → 字节序；
  屏幕串口都正常但灯常绿且不做自检 → 工具链是 clang（见上面「状态灯」一节）
- 直接用串口测灯：`{"cmd":"led","brightness":120}` / `{"cmd":"led","on":false}`（回 ack 带实际值），
  或 `uv run tools/mock_status.py --send '{"cmd":"led","brightness":120}'`
- `idf.py monitor` 常驻占用串口不影响 Pi 扩展（它只读、扩展只写）。但要注意 monitor
  会顺带把 tty 设成 raw 并把积压读走，所以**开着 monitor 时现象会消失**，别被它骗了
- 查链路是否健康：`TIOCINQ` 应该只有几十字节；ESP 心跳里的 `rx`/`bad` 里
  `bad` 不该随时间单调涨（涨 = 有回显污染或半行）
- **别同时跑 `tools/mock_status.py`、`tools/cb-status-daemon.py` 和 Pi 扩展**，几边都在写同一个
  串口，屏幕会来回跳
- 排查渲染问题时用 `idf.py size` 看分区余量（CJK 字库占 ~157KB）

### 代码风格

- C 代码注释写中文，遵循 ESP-IDF 惯用法（`ESP_RETURN_ON_ERROR` / `ESP_ERROR_CHECK`）
- Pi 扩展注释写中文，保持和 C 端一致的术语（state/ctx/tps 等）
- `tools/*.py` 注释同样写中文；统一走 PEP 723 头 + `uv run`，改完过一遍 `ruff format`

## 常用命令（以下都由用户手动执行）

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # 刷机并看日志
uv run tools/mock_status.py              # 灌模拟数据（先停掉 Pi 扩展）
uv run tools/mock_status.py --led-brightness 24   # 顺手把状态灯调暗（或 --led-off）
```

CodeBuddy 侧（daemon 和 mock_status 抢同一个串口，别同时跑；也别和 Pi 扩展同时跑）：

```bash
uv run tools/cb-status-daemon.py --dry-run          # 先不占串口，看要发的帧对不对
uv run tools/cb-status-daemon.py --ctx-max 200000   # 真机；窗口大小 transcript 里没有，必须给
uv run tools/cb-status-daemon.py --replay -t <会话.jsonl>   # 离线快放，验证状态机
uv run tools/cb-status-daemon.py --led-brightness 24       # 顺手把状态灯调暗（或 --led-off）
```

Pi 交互里：`/statusbar`（看连接状态，含状态灯亮度）、`/statusbar test`（发演示数据）、
`/statusbar port <设备>`、`/statusbar on|off`、`/statusbar led <0-255|on|off>`
