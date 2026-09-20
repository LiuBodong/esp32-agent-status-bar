# ESP32 Vibe Coding 状态栏

ESP32-C3 + SSD1315（0.96 寸 128x32 单色 OLED）的 Agent 状态指示器：Pi（终端编码 agent）
通过 USB Serial/JTAG 把运行状态实时推给 ESP32，屏幕轮播展示。

## 硬件

- 芯片：ESP32-C3，2MB flash（单 app 分区 1MB，目前固件约占 78%）
- 屏幕：SSD1315（寄存器兼容 SSD1306），128x32，**I2C**
  - **SDA = GPIO8，SCL = GPIO9**，400kHz，从机地址 0x3C，模块无 RESET 引脚
  - C3 的 USB 占用 GPIO18/19，选引脚时避开
- 通信：USB Serial/JTAG（Espressif VID 303a，枚举为 `/dev/ttyACM0`）
  - 日志走 UART0（USB 是副控制台，不会抢 USJ 驱动），所以 Pi 扩展可以独占 USJ 收发

## 技术栈（版本不要随便动）

- ESP-IDF **v6.1-dev**。v6 已移除内置的 `json`/cJSON 组件，所以用 `espressif/cjson`
- LVGL **~9.5.0** + `espressif/esp_lvgl_port ^2.9.0`（这个组合验证过；9.6 太新没用过）
- Pi 扩展用 Pi 0.85.x 的 Extension API，已用其官方类型做过 `tsc --strict` 校验

## 目录

| 路径 | 作用 |
|---|---|
| `main/display.c` | I2C 总线 + SSD1306 面板 + esp_lvgl_port 初始化 |
| `main/ui.c` | LVGL 界面：左状态图标 + 右两行文本、2 页轮播、上电自检 |
| `main/status_model.c` | 状态模型 + cJSON 字段解析（互斥锁保护） |
| `main/serial_link.c` | USB Serial/JTAG 收发、命令处理、心跳；协议说明在文件头注释 |
| `main/fonts/` | 副行用的思源黑体 13px ASCII 子集（lv_font_conv 生成） |
| `tools/mock_status.py` | 不依赖 Pi，直接给屏幕灌模拟数据（PEP 723，`uv run`） |
| `pi-extension/esp32-status-bar.ts` | Pi 扩展；已软链到 `~/.pi/agent/extensions/` |

## 关键约定与坑

### 显示

- 单色屏走 LVGL `LV_COLOR_FORMAT_I1`，**必须用整屏缓冲**（`hres*vres`），这是 esp_lvgl_port 的硬要求
- esp_lvgl_port 把 I1 位图转成 SSD1315 页格式时会取反：**LVGL 里画黑色 = 屏幕上点亮**。
  所以 `ui.c` 用 `COLOR_LIT = lv_color_black()`。实机黑白反了就打开 `Display → Invert colors`
- 版式：左侧 `x=0..17` 是 18x16 的状态图标，右侧 `x=20..127` 两行文字（**左对齐**，数值位数
  变化时整行不会左右抖）。主行 `y=0..15` 用 LVGL 自带 `lv_font_source_han_sans_sc_14_cjk`
  （行高 17，字形占 0..14，约 157KB），副行 `y=16..31` 用自造的 13px ASCII 子集（行高 15）
- 右列只有 108px，改文案前先按字宽算一遍：14px 数字 7.69px / 空格 3.12px，
  13px 数字 7.19px / 空格 2.94px / `:` 3.62px（`%` 有 12px 宽，容易撑爆）
- 主行有两套版式（`ui.c:set_main_row()`）：
  - CTX 页：整行一个 label，不写 `CTX` 前缀，数值按 **1000 进制阶梯**退化
    （`fmt_ctx()`：999 → 999，1000 → 1K，1000000 → 1M，截断取整）。
    用 1000 而不是 1024，是因为模型窗口都是十进制标称的（200000 / 1000000），
    这样才显示得成 `200K` / `1M`
  - TOK 页：`↑ 1.2k ↓ 567`。箭头是 montserrat 符号、数字是思源黑体，**同一行混不了两种字体**，
    所以塞进一个 flex 容器（`s_tok_row`）各占一个定宽 label
- 状态改用图标表示：`lv_font_montserrat_14` 里带全套 FontAwesome 符号（`LV_SYMBOL_PLAY` 等），
  映射见 `ui.c:state_symbol()`；`thinking` 用 `lv_arc` 画的 90° 弧逐档旋转。
  **不要用 LVGL 内置 montserrat 排正文**：它的字形墨迹常宽于 advance（负边距），小字号会挤在一起；
  图标是单字形独立 label，不受影响
- 动效一律跟着 UI 定时器走，**别用 `lv_anim`**：整屏缓冲 + full_refresh 下常驻动画会让 I2C 以 30fps 整帧刷屏
- `main/fonts/*.c` 由 lv_font_conv 生成，需要 `-DLV_LVGL_H_INCLUDE_SIMPLE`（见 `main/CMakeLists.txt`）
- `main/Kconfig.projbuild` 里的 **bool 选项关闭时不生成宏**，判断要用 `#ifdef`

### 通信协议

一行一条 JSON、`\n` 结尾，只有以 `{` 开头的行会被解析；字段全部可选、大小写不敏感：

```json
{"state":"thinking","ctx":12000,"ctx_max":200000,"in":1234,"out":567,"tps":42.5,"elapsed":3.2,"turn":2}
```

- `state`：`idle|thinking|running|tool|waiting|done|error`，也接受任意自定义字符串
- 字段别名、命令（`ping`/`page`/`clear`）与下行事件（`boot`/`hb`/`pong`）见 `main/serial_link.c` 头部注释
- ESP 每 10s 发一次心跳；5s 收不到 host 数据就显示 `NO HOST`

### 调试

- 上电自检：整屏点亮 400ms（`CONFIG_STATUS_BAR_BOOT_SELFTEST=y`）。看不到 → 查接线/供电，不是字体问题
- `idf.py monitor` 常驻占用串口不影响 Pi 扩展（它只读、扩展只写）
- **别同时跑 `tools/mock_status.py` 和 Pi 扩展**，两边都在写同一个串口，屏幕会来回跳
- 排查渲染问题时用 `idf.py size` 看分区余量（CJK 字库占 ~157KB）

### 代码风格

- C 代码注释写中文，遵循 ESP-IDF 惯用法（`ESP_RETURN_ON_ERROR` / `ESP_ERROR_CHECK`）
- Pi 扩展注释写中文，保持和 C 端一致的术语（state/ctx/tps 等）

## 常用命令

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # 刷机并看日志
uv run tools/mock_status.py              # 灌模拟数据（先停掉 Pi 扩展）
```

Pi 交互里：`/statusbar`（看连接状态）、`/statusbar test`（发演示数据）、`/statusbar port <设备>`、`/statusbar on|off`
