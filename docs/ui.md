# UI 架构

> 三层分工：**面板**（硬件）、**输入设备**（硬件）、**界面**（语义）。
> 依赖单向：`app_ui_core` 依赖 `app_ui_display` / `app_ui_touch`，反向一律禁止。

## 一、组件职责

| 组件 | 负责 | 依赖 | 不负责 |
| --- | --- | --- | --- |
| `app_ui_display` | CO5300 面板初始化、把面板描述交给 presenter（`esp_display_present_target_config_t`） | `esp_display_present`、`esp-mosaico-bsp`、`esp_lcd` | 触摸、GSP、任何界面语义 |
| `app_ui_touch` | CST9220 轮询（16 ms）、BOOT 键去抖、发布 `app_input_event_t` | `esp-mosaico-bsp`、`esp_lcd_touch`、`esp_driver_gpio` | 页面、手势判定、HID 报告、GSP |
| `app_ui_core` | 页面状态机、命中与手势、HID 上报、芯片显隐/置灰、overlay、GSP 场景与定时器 | `app_ui_display`、`app_ui_touch`、`app_cfg`、`app_usb_hid`、`app_log`、`esp-gsp` | 直接访问触摸/面板硬件 |

两个设备层只依赖 IDF 与 BSP（板级驱动），不再包含任何 `app_ui*.h`、`app_cfg.h`、`app_usb_hid.h`、`esp_gsp*.h`。

```
main ──> app_ui_core ──> app_ui_touch ──> esp_lcd_touch / BSP
 │             ├──────> app_ui_display ──> esp_display_present / BSP
 │             ├──────> app_cfg ──> (无 UI 依赖，见 §五)
 │             └──────> app_usb_hid / app_log / esp-gsp
 └──────> app_bt_* / app_usb_* / app_sensor / esp_iris（只连屏幕镜像）
```

`main` 只做装配：`app_ui_start()` 一次拿到整块 UI（面板 + 场景 + 输入），
再把 GSP 句柄交给 Iris 屏幕镜像。

## 二、输入事件模型

触摸 16 ms 采样、界面 16 ms 消费；输入层发布**事件**而不是坐标快照，
否则 40 ms 级的消费周期会吞掉快速轻点。

```c
typedef enum {
    APP_INPUT_TOUCH_DOWN,   /* 新触点按下 */
    APP_INPUT_TOUCH_MOVE,   /* 主动点位置变化 */
    APP_INPUT_TOUCH_UP,     /* 触点抬起，坐标保留最后一次位置 */
    APP_INPUT_KEY_DOWN,     /* BOOT 键按下 */
    APP_INPUT_KEY_UP,
} app_input_event_type_t;

typedef struct {
    app_input_event_type_t type;
    uint8_t id;         /* 触点编号 0/1；按键事件为 0 */
    int16_t x, y;       /* 触点坐标（px），按键事件为 0 */
    uint32_t seq;       /* 单调递增，用于丢弃/统计 */
} app_input_event_t;
```

- 队列深度 16（`app_ui_touch/app_input.c`），单生产者单消费者，`portMUX_TYPE` 自旋锁保护（本芯片是 FreeRTOS SMP，`taskENTER_CRITICAL()` 必须带锁参数）
- 满时丢最旧并累加 `dropped`；状态事件在 16 ms 周期内不会被挤掉
- 抬起事件保留最后坐标，鼠标的点击判定因此不再依赖「已清零的采样」

## 三、数据流

```
CST9220 (I2C, 16 ms) ─┐
BOOT 键 (GPIO, 16 ms) ─┴─ app_ui_touch ──> app_input_publish
                                              │
app_ui_core/app_ui_input.c (16 ms) <──────────┘
   ├─ 抽干队列：更新触点状态、置 overlay 脏标志
   ├─ BOOT 翻页
   ├─ mouse / keyboard / gamepad / setting 语义 → app_usb_hid_* 报告
   └─ overlay 重绘（50 ms 节流；抬手与按键高亮变化立即重绘）

app_ui_core/app_ui.c (渲染任务, 40 ms)
   ├─ 跑马灯（非 idle 页停到画布外）、终端日志（~4 Hz）
   ├─ 堆用量日志（10 s）
   └─ 设置页芯片刷新的入口（由 input 侧在切换后调用）
```

## 四、启动顺序与 overlay 修复

`app_ui_start()`：

1. `app_cfg_set_layout_rpc()` —— 把「当前页面/切页」的两个回调交给 RPC（见 §五）
2. `ui_bundle_open()` 打开 `ui_apps` 里的 GSP 场景
3. `app_ui_display_init()` 点屏，填 presenter 硬件描述
4. `esp_gsp_esp_lcd_start_prepared(..., ui_prepare, ...)`：在**渲染任务的首帧之前**执行 `ui_prepare`
5. `ui_prepare` 内完成：初始页面（idle）、跑马灯起点、`app_ui_refresh_chips()`、注册 overlay contributor、创建 40 ms 定时器
6. 返回后启动输入层：`app_ui_touch_start()`（触摸 + BOOT 键）→ `app_input` 消费任务

**关键点**：`esp_gsp_set_overlay_contributor()` 是「投递到渲染任务队列」的调用。
之前在 `app_main` 里注册，正好撞上 181 个对象的首帧合成，队列一直排不进去，
于是稳定返回 `ESP_ERR_TIMEOUT`（重试 40×50 ms 也无效），触摸层因此起不来。
改到 `start_prepared` 的 prepare 回调里注册后，日志固定出现 `overlay attached`。
输入侧仍保留兜底：注册未成功时每个 tick 重试一次，只在状态翻转时打日志。

## 五、页面状态与 RPC 的解耦

设置项、页面枚举都在 `app_ui_core`；但 RPC 服务在 `app_cfg`。原先
`app_cfg → app_ui_core → app_cfg` 是循环依赖。现在由 UI 在启动时注册两个回调：

```c
typedef struct {
    const char *(*name)(void);                  /* 当前页面名 */
    const char *(*apply)(const char *name);     /* 切页，未知返回 NULL */
} app_cfg_layout_rpc_t;
void app_cfg_set_layout_rpc(const app_cfg_layout_rpc_t *rpc);
```

`app_cfg` 不再包含 `app_ui.h`，依赖图无环。RPC 行为不变：
method 1 返回 `layout`，method 4 切页并回显名字。

## 六、验收（2026-11-18 真机）

- 开机日志：`input device started` → `overlay attached` → `input started (54 keys)`，**无** `touch input unavailable`
- 切页只靠 BOOT 键与 RPC（四角点击已移除）：`boot key -> …` 日志正常
- BOOT 键：`boot key -> gamepad/terminal/setting` 正常
- 鼠标单击：`click 1` 正常
- 设置页：usb / ds4·ds5·xbox / bt / ble 四行芯片、三处 `[default]` 与配置逐项一致（usb 默认含 sensor）；
  选中 = 青底 + 深色字，未选中 = 深底 + 浅色字，**没有第三种「置灰」状态**（灰色状态已删除，所有芯片都可点）
- idle / gamepad / keyboard / terminal / setting 五页渲染与重构前一致
- 堆：内部 RAM 剩余 ~118 KB，最大块 31 KB（新增一个任务与队列，约 11 KB）

待人工确认（需要真手指，无法自动化）：双指右键/滚动、键盘双指组合、触点圆环与按键高亮 overlay、连续快速轻点不丢。

## 六之一、手柄页按键布局（2026-11-18）

屏幕四角**不再**切页（只剩 BOOT 键与 RPC）。手柄页围绕两颗摇杆布置控件，右侧是左侧的镜像
（480 宽，镜像关系 `x' = 480 - x - w`）：

| 控件 | 位置（x,y,w,h） | 形态 | 作用 |
| --- | --- | --- | --- |
| LB | 60, 12, 120, 34 | 左摇杆正上方横条 | button bit 8 |
| RB | 300, 12, 120, 34 | 右摇杆正上方横条 | bit 9 |
| LS | 188, 60, 40, 120 | 左摇杆**正右方**竖条（样式同 LT，但是数字键） | bit 10 |
| RS | 252, 60, 40, 120 | 右摇杆正左方竖条 | bit 11 |
| SELECT | 60, 192, 120, 34 | 左摇杆**正下方**长条 | bit 12 |
| START | 300, 192, 120, 34 | 右摇杆正下方长条 | bit 13 |
| LT | 8, 60, 40, 120 | 左摇杆左侧竖条，**模拟滑条** | 扳机 0..255 |
| RT | 432, 60, 40, 120 | 右摇杆右侧竖条 | 扳机 0..255 |

十字键与 ABXY 都带 `#161616` 圆形外轮廓（半径 24 / 30，边框 `#2A2A2A`），按下时换成同形状的发光层。

实现要点：

- **触点归属在按下时锁存**：`s_in.capture[]` 记住这个手指落在哪个控件上，整个按压期间不变。
  所以从 LT 滑到摇杆区域时，仍然只在拖 LT，摇杆不会被带偏；反过来从摇杆滑到 LT 也一样。
- LT/RT 滑条：`value = (y - bar_y) * 255 / (bar_h - 1)`，**顶部 0、底部 255**，每 16 ms 更新一次
  `app_usb_hid_set_triggers()`；同一滑条两个触点取较大值；松开归 0。
- 锁存到普通按键的触点只上报对应 bit，不再参与摇杆/十字键/ABXY 的判定。
- 离开手柄页时（BOOT 翻页 / RPC 切页）调用 `gamepad_release_all()`：按键清零、摇杆回中、扳机回 0、
  所有发光层熄灭，避免按住不放就切页导致状态卡住。
- 扳机只在 **0 ↔ 非 0** 的边沿打日志（`trigger L=… R=…`），滑动过程不刷日志。
- 摇杆中心 (120,120)/(360,120)、半径 60，十字键 (120,360)，ABXY (360,360) 不变。

## 六之二、设置页语义（当前规则）

| 点击 | 结果 |
| --- | --- |
| `kbd` / `mouse` | 翻转对应位，其余不动 |
| `gamepad` | 清掉 `ds4/ds5/xbox`，再翻转 gamepad 位 |
| `sensor` | 清掉 `ds4/ds5/xbox`，再翻转 sensor 位（**gamepad 位不动**，两者可以共存） |
| `ds4` / `ds5` / `xbox` | 若是当前身份则关闭；否则清掉 `gamepad + sensor + 另外两个身份`，只置该位 |
| `audio`（usb/bt 行） | 翻转 usb 音频开关 / bt 音频掩码；ble 行的 audio 是占位，点了不动作 |
| `[default]`（每段一个） | usb：`APP_CLASS_USB_DEFAULT`（kbd+mouse+gamepad+**sensor**）+ audio 开；bt：bt_hid 全开 + 音频全开；ble：ble_hid 全开 |

所有改动都要重启生效（日志带 `reboot to apply`）。设置页芯片几何必须和 `ui/main.json` 一致：
`x = 12 + 92*i`、`86×38`，行 y = 84 / 128 / 218 / 308；`[default]` 在标题右侧 x=50,w=96,h=30。

## 七、已知权衡

| 取舍 | 说明 |
| --- | --- |
| 事件队列 vs 坐标快照 | 事件不丢状态，但需要消费端抽干；快照简单却会丢快速点击 |
| 两个任务 vs 一个任务 | 设备 16 ms、界面 16 ms 分开，职责清晰；代价约 3 KB 栈 |
| overlay 归 core | 绘制与命中共享同一份几何，避免两处维护；注册时机也随之正确 |
| BOOT 键归输入层 | 与触摸同为物理输入；页面切换语义仍在 core |
| 布局 RPC 回调 | 用 10 行回调换掉一个组件环，`app_cfg` 保持无 UI 依赖 |

---

# 八、当前状态与后续

## 8.1 代码库

- 基线提交：`e591e22`；此后全部改动仍未提交（`git diff` + 未跟踪文件即全部内容）。
- 构建（先 `. ../env.sh`，在仓库根目录）：

```bash
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
        --project projects/gamepad build
python3 mosaico.py iris system-update --project projects/gamepad   # 设备在 Vibe Mode 时先 iris test enter-recovery
python3 mosaico.py iris logs --snapshot --project projects/gamepad
python3 mosaico.py iris screenshot /tmp/page.png --project projects/gamepad
python3 mosaico.py iris rpc 0x4700 4 --payload '{"layout":"setting"}' --project projects/gamepad
```

- 设备 `iris memory` 返回 `not_supported`，任务栈高水位只能看日志/现象。

## 8.2 本次已完成

1. `app_input.c` 改 `portMUX_TYPE` + `portENTER_CRITICAL(&mux)`（SMP）
2. overlay 绘制与注册搬进 `app_ui_core/app_ui_input.c`，在 prepare 回调注册 → 修掉开机 `ESP_ERR_TIMEOUT`
3. `app_ui_touch` 只剩「读芯片 + BOOT 键 + 发事件」，不再含任何页面/HID/配置代码
4. `app_ui_display` 只点屏并交出 presenter 描述，不再创建触摸、不再依赖 esp-gsp
5. `app_ui_core` 新增 `app_ui_input.c`：翻页、mouse/keyboard/gamepad/setting、overlay
6. `app_cfg` 用 `app_cfg_layout_rpc_t` 去掉与 `app_ui_core` 的循环依赖
7. `main` 只调 `app_ui_start()` + 屏幕镜像；`app_sensor_start()` 恢复启用（此前为定位崩溃临时 `#if 0`）
8. USB 默认掩码含 sensor（`APP_CLASS_USB_DEFAULT`），BT/BLE 默认不含；`tud_hid_descriptor_report_cb()`
   补齐 sensor/Sony 实例（之前返回 NULL 会让主机一读报表描述符就 panic，连带 UAC2 与 Iris 一起失效）

## 8.3 仍需人工确认

- 双指手势（右键、滚动/横移）、键盘两指组合
- overlay：idle 页触点圆环、键盘页按键高亮
- 连续快速轻点 20 次，左键计数一致
- 手柄页八个控件：按下去只会亮自己（不再顺带把摇杆推到位）；LT/RT 滑条拖动输出 0→255；
  从 LT 滑到摇杆区域时摇杆必须保持不动（触点锁存）

## 8.4 与 UI 无关的未决项

- SCO 上行约 75% 取走率（eSCO 由手机侧决定），音调偏低
- 麦克风直连模式下 USB 采集与 SCO 争抢同一 I2S 流（`MIC_FANOUT_ENABLE=0`）
- 显示 TE 长跑偶发 `NO_MEM`（`psram_dma_direct=true` 已大幅缓解）
