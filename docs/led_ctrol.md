# ESP32-C3 驱动 SK6812 RGBW：从零搭建记录

本文记录这个工程从零搭起来的过程，以及过程中真正踩到的坑。照着走一遍应该能复现。

---

## 0. 目标

用 ESP32-C3 Super Mini，通过**一根数据线**控制 SK6812 RGBW 灯珠：

- 基础颜色切换（红 / 绿 / 蓝 / 白 / 灭）
- 呼吸调光（带 gamma 校正）
- 彩虹循环

---

## 1. 硬件

### 1.1 器件清单

| 器件 | 说明 |
| --- | --- |
| ESP32-C3 Super Mini | 主控，3.3V 逻辑电平 |
| SK6812 RGBW 灯珠 / 模块 | 数字可寻址 LED，每颗 32 bit（R/G/B/W 各 8 bit） |
| 330Ω 电阻 | 串在数据线上，抑制反射与尖峰 |
| 杜邦线若干 | |

### 1.2 接线

| ESP32-C3 | 方向 | SK6812 | 说明 |
| --- | --- | --- | --- |
| `GPIO4` | → | `Din` | 数据线，中间串 330Ω |
| `5V` | → | `5V` | 供电，**不要接 3V3** |
| `GND` | — | `GND` | **必须共地** |
| — | — | `Dout` | 单颗使用时悬空；级联时接下一颗的 `Din` |

接线示意（可用 draw.io 打开）：`docs/接线图.drawio`

> 注意：`Din` / `Dout` 接反不会烧坏，但灯完全不亮，是很容易忽略的问题。

### 1.3 为什么选 GPIO4

ESP32-C3 上并非所有 GPIO 都能随便用：

| 引脚 | 为什么避开 |
| --- | --- |
| `GPIO2` / `GPIO8` / `GPIO9` | strapping 脚，影响启动模式；`GPIO8` 还常接板载 LED |
| `GPIO20` / `GPIO21` | UART0 默认日志口 |
| `GPIO11` | 内部 VDD_SPI，未引出 |
| `GPIO12` ~ `GPIO17` | 内部 SPI Flash，未引出 |
| `GPIO18` / `GPIO19` | USB D- / D+ |

剩下 `GPIO0~7`、`GPIO10` 都可以。选 `GPIO4`：无 strapping 功能、不与串口/USB 冲突。

---

## 2. 环境准备

### 2.1 ESP-IDF

```bash
# 假设 IDF 已在 /path/to/esp-idf
. /path/to/esp-idf/export.sh
idf.py --version
```

本工程实际使用的版本是 **master 开发分支**（组件管理器报告为 `6.2.0`，`git describe` 为
`v6.1-dev-8142-g188e3e55bb`）。

### 2.2 工具链：必须用 GCC（重要）

**这一条是本工程最大的坑，详见第 5 节坑 1。** 编译前务必确认：

```bash
unset IDF_TOOLCHAIN          # 或显式 export IDF_TOOLCHAIN=gcc
echo $IDF_TOOLCHAIN          # 应该是空或 gcc，不能是 clang
```

如果从 clang 切到 gcc，**必须** `idf.py fullclean`，否则 CMake 会沿用旧编译器缓存。

### 2.3 组件下载需要代理

`led_strip` 是托管组件，构建时由组件管理器从网上拉取。国内网络需要代理：

```bash
export HTTPS_PROXY=http://localhost:7897
export HTTP_PROXY=http://localhost:7897
```

---

## 3. 从零创建工程

### 3.1 生成工程骨架

```bash
idf.py create-project esp32-sk6812-demo
cd esp32-sk6812-demo
```

得到：

```
esp32-sk6812-demo/
├── CMakeLists.txt          # 顶层，一般不用改
├── main/
│   ├── CMakeLists.txt      # 组件注册
│   └── main.c              # 入口
└── sdkconfig               # 配置
```

### 3.2 设置目标芯片

```bash
idf.py set-target esp32c3
```

### 3.3 声明 led_strip 依赖

新建 `main/idf_component.yml`：

```yaml
dependencies:
  espressif/led_strip: "^3.0.3"
```

构建时组件管理器会自动把组件下载到 `managed_components/`，并生成 `dependencies.lock`。

### 3.4 给 main 组件补上 RMT 依赖

`main/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_rmt)
```

加 `esp_driver_rmt` 是因为 `led_strip_rmt.h` 里要用到 `driver/rmt_types.h`（如 `RMT_CLK_SRC_DEFAULT`）。

### 3.5 编译

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

---

## 4. 代码说明

完整代码见 `main/main.c`，这里只说关键部分。

### 4.1 初始化

```c
led_strip_config_t strip_config = {
    .strip_gpio_num = 4,                  /* 数据脚 */
    .max_leds       = 1,                  /* 灯珠数量 */
    .led_model      = LED_MODEL_SK6812,   /* 决定时序参数 */
    .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,
    .flags = { .invert_out = false },
};

led_strip_rmt_config_t rmt_config = {
    .clk_src           = RMT_CLK_SRC_DEFAULT,
    .resolution_hz     = 10 * 1000 * 1000,  /* 10MHz，1 tick = 0.1us */
    .mem_block_symbols = 48,                /* C3 每通道 48 个符号 */
    .flags = { .with_dma = false },
};

led_strip_handle_t strip;
led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
```

几个参数为什么这么填：

| 参数 | 取值 | 原因 |
| --- | --- | --- |
| `led_model` | `LED_MODEL_SK6812` | 组件据此选择时序：`T0H=0.3us / T0L=0.9us / T1H=0.6us / T1L=0.6us`，复位码 280us |
| `color_component_format` | `..._FMT_GRBW` | SK6812 RGBW 最常见的字节顺序，见坑 2 |
| `resolution_hz` | 10MHz | 与组件默认一致，`0.3us = 3 tick` 刚好整除 |
| `mem_block_symbols` | 48 | C3 无 DMA，每通道只有 48 word，且驱动要求 ≥48 的偶数 |
| `with_dma` | `false` | C3 的 RMT 不支持 DMA |

### 4.2 取色与刷新

RGBW 灯珠必须用 4 通道的接口：

```c
led_strip_set_pixel_rgbw(strip, i, r, g, b, w);   /* 不要用 3 参数的 set_pixel */
led_strip_refresh(strip);                          /* 修改后必须刷新才生效 */
```

### 4.3 调光与 gamma

SK6812 每通道只有 8 bit（256 级）。直接线性调光在低亮度区跳变非常明显，加个平方近似即可显著改善：

```c
static uint32_t gamma_correct(uint32_t v)
{
    return (v * v) / 255;   /* 近似 gamma≈2.0 */
}
```

### 4.4 错误处理

不要用 `ESP_ERROR_CHECK`，改成显式判断并打印，原因见坑 3。

---

## 5. 踩过的坑

### 坑 1（最严重）：esp-clang 下 RMT 传输完成中断永不触发

**现象**

程序启动后串口日志停在初始化那一行，**没有任何 panic、没有错误码、没有重启**，灯不亮：

```
I (247) main_task: Calling app_main()
I (247) sk6812-demo: [boot] app_main entered
I (257) sk6812-demo: [1/3] init: gpio=4, leds=1, ...
        ← 到此为止
```

灯的表现是**常绿**（不是全灭）。

**定位过程**

这类"静默卡死"最难查，因为没有任何线索。有效的办法是**把粒度打到单个底层调用**——
写一段绕过 `led_strip`、直接用裸 RMT API 的探针，每一步都打日志：

```c
ESP_LOGI(TAG, "probe[1] rmt_new_tx_channel ...");
err = rmt_new_tx_channel(&chan_cfg, &chan);
ESP_LOGI(TAG, "probe[1] -> %s", esp_err_to_name(err));
/* probe[2] rmt_new_copy_encoder ... 依次类推 */
```

结果一目了然：

```
probe[1] rmt_new_tx_channel   -> ESP_OK
probe[2] rmt_new_copy_encoder -> ESP_OK
probe[3] rmt_enable           -> ESP_OK
probe[4] rmt_transmit         -> ESP_OK        ← 数据成功入队
probe[5] rmt_tx_wait_all_done -> ESP_ERR_TIMEOUT
E (1297) rmt: rmt_tx_wait_all_done(590): flush timeout
```

**根因**

`rmt_transmit()` 入队成功后，**RMT 的传输完成中断从不触发**，于是
`rmt_tx_wait_all_done()` 等满超时。数据发到一半卡住，GPIO 停在高电平，SK6812 把这串
高电平解析成 `G=0xFF`，所以灯显示常绿。

问题出在工具链：当时环境里 `IDF_TOOLCHAIN=clang`，而 ESP-IDF 自己在构建时就警告过：

```
Building ESP-IDF with clang is an experimental feature and is not yet officially supported.
```

**结论 / 规避**

```bash
unset IDF_TOOLCHAIN        # 或 export IDF_TOOLCHAIN=gcc
idf.py fullclean           # 切换工具链后必须清理！
idf.py build
```

换成 GCC 后立刻正常。建议把这个变量在 shell 里永久改成 `gcc`，否则换终端又会踩。

**经验**：在这类"master 开发版 + 实验性工具链"的组合上，遇到静默卡死要优先怀疑工具链和
驱动，而不是自己的代码。

---

### 坑 2：SK6812 RGBW 的字节顺序不是唯一的

**现象**

灯亮，但颜色全错（点红亮绿）。

**原因**

SK6812 RGBW 的 32 bit 数据里，各通道的排列顺序取决于厂家批次。市面上主流是
**GRBW**，但也有 **RGBW**、**WRGB** 的版本。顺序不对就会出现"点红亮绿"。

**规避**

```c
.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,   /* 先用这个 */
```

不对就依次试：

```c
LED_STRIP_COLOR_COMPONENT_FMT_RGBW
```

再不对就自定义（WRGB）：

```c
.color_component_format = {
    .format = { .r_pos = 1, .g_pos = 2, .b_pos = 3, .w_pos = 0,
                .bytes_per_color = 1, .num_components = 4 },
},
```

另外代码里的 `demo_basic_colors()` 会依次点亮红/绿/蓝/白，就是专门用来快速验证顺序的。

> 顺带记一个判断技巧：如果实际是 **RGB** 灯珠（3 通道）而你按 GRBW 发 4 字节，
> 单颗灯**仍然能正确显示**——因为它读走的正是前 24 bit（G/R/B），多出来的 W 会传给下一颗。

---

### 坑 3：`ESP_ERROR_CHECK` 会把错误"吞掉"

**现象**

初始化失败时串口里看不到任何原因，程序就停在那儿。

**原因**

`ESP_ERROR_CHECK` 失败后走 `abort()`。在某些配置下这条路径不会留下有用的输出，
反而让"初始化失败"看起来像"程序卡死"。

**规避**

初始化里改成显式判断并打印：

```c
esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "new_rmt_device FAILED: %s (0x%x)", esp_err_to_name(err), err);
    return err;
}
```

`main.c` 里所有可能失败的地方都用了这种写法，失败时串口一定说话。

---

### 坑 4：ESP32-C3 无 DMA，RMT 内存只有 48 个符号

**现象**

参数填大了会创建失败。

**原因**

C3 的 RMT 只有 4 个通道、每个通道 48 word（`SOC_RMT_MEM_WORDS_PER_CHANNEL = 48`），
且**不支持 DMA**。驱动要求 `mem_block_symbols` 为**偶数且 ≥ 48**。

`led_strip` 组件传 `0` 时会自动落到 48，但写死更明确：

```c
.mem_block_symbols = 48,
```

另外 C3 的 RMT 共 4 个通道，其中**只有 2 个能用作 TX**。以后如果要再加一路灯带、
红外收发之类的，注意别把 TX 通道抢完。

---

### 坑 5：托管组件需要代理

**现象**

`idf.py build` 卡在 `Solving dependencies` 或直接报网络错误。

**原因**

`led_strip` 不在 IDF 源码里，是组件管理器从 `components.espressif.com` 拉取的。

**规避**

构建前带上代理环境变量：

```bash
export HTTPS_PROXY=http://localhost:7897 HTTP_PROXY=http://localhost:7897
idf.py build
```

---

### 坑 6：IDF 版本号的迷惑

`git describe --tags` 报的是 `v6.1-dev-8142-...`，但组件管理器报告的是 `idf (6.2.0)`，
`sdkconfig` 里也看不出准确版本。判断组件兼容性时以**组件管理器报告的版本**为准。

本环境是 master 开发分支（`6.2.0-dev`），API 可能比 v5.x LTS 新，也可能有回归。

---

## 6. 概念澄清：SK6812 不需要 PWM 调光

一个容易走弯路的地方：**SK6812 是数字可寻址 LED，调光不靠 PWM 占空比。**

- 亮度是把灰度值（`0~255`）编进单总线数据包发出去，芯片内部自己完成恒流 PWM 驱动
- 不需要 `LEDC` / PWM 外设
- "调光"就是改数据包里的数值：`0` 全灭 → `255` 最亮
- 数据脚和"调光"是同一根线

只有当你要驱动**普通 LED**（非可寻址）时，才需要 LEDC。ESP32-C3 有 6 个 LEDC 通道、
4 个定时器，且输出可路由到任意 GPIO。

---

## 7. 灯不亮时的排查清单

按可能性从高到低：

1. **数据线接的是 `Din` 还是 `Dout`？** 接反完全不亮，且不会损坏，极易忽略
2. **有没有共地？** LED 的 `GND` 必须和 ESP32 的 `GND` 连在一起
3. **电压**：万用表测 LED 的 `5V` 与 `GND` 焊盘之间，应约 5V（接 3V3 会不亮或极暗）
4. **引脚插错**：确认是丝印标 `4` 的排针
5. **电阻阻值**：应该是 300~500Ω；kΩ 级会衰减信号导致不亮
6. **电平裕量**：SK6812 的高电平门限典型为 `0.7 × VDD = 3.5V`，而 ESP32-C3 只能输出
   3.3V，属于临界状态。多数批次能跑，少数不认。软件全正常仍不亮时，可把 LED 供电降到
   4.3V 左右（5V 串一只 1N4148 二极管），门限随之降到约 3.0V
7. **先看串口日志**：如果日志停在初始化，那是软件/工具链问题，直接对照第 5 节坑 1

---

## 8. 参考资料

- ESP-IDF RMT 文档：<https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/api-reference/peripherals/rmt.html>
- led_strip 组件：<https://components.espressif.com/components/espressif/led_strip>
- led_strip 组件文档：<https://espressif.github.io/idf-extra-components/latest/led_strip/index.html>
- 工程内接线图：`docs/接线图.drawio`
