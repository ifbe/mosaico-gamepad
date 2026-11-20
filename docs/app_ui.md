# UI 架构

这个项目只有一个 UI 文档，覆盖三个 UI 组件（面板、输入设备、界面）的职责、
契约与页面行为。

> 三层分工：**面板**（硬件）、**输入设备**（硬件）、**界面**（语义）。
> 依赖单向：`app_ui_core` 依赖 `app_ui_display` / `app_ui_touch`，反向一律禁止。

## 一、组件职责

| 组件 | 负责 | 依赖 | 不负责 |
| --- | --- | --- | --- |
| `app_ui_display` | CO5300 面板初始化、把面板描述交给 presenter（`esp_display_present_target_config_t`） | `esp_display_present`、`esp-mosaico-bsp`、`esp_lcd` | 触摸、GSP、任何界面语义 |
| `app_ui_touch` | CST9220 轮询（16 ms）、BOOT 键去抖、发布 `app_input_event_t` | `esp-mosaico-bsp`、`esp_lcd_touch`、`esp_driver_gpio` | 页面、手势判定、HID 报告、GSP |
| `app_ui_core` | 页面状态机、命中与手势、HID 上报、芯片显隐、overlay、GSP 场景与定时器 | 上面两个 + `app_cfg`、`app_usb_hid`、`app_log`、`esp-gsp` | 直接访问触摸/面板硬件 |

两个设备层只依赖 IDF 与 BSP（板级驱动），不包含任何 `app_ui*.h`、`app_cfg.h`、
`app_usb_hid.h`、`esp_gsp*.h`。

```
main ──> app_ui_core ──> app_ui_touch ──> esp_lcd_touch / BSP
 │             ├──────> app_ui_display ──> esp_display_present / BSP
 │             ├──────> app_cfg ──> (无 UI 依赖，见 §五)
 │             └──────> app_usb_hid / app_log / esp-gsp
 └──────> app_bt_* / app_usb_* / app_sensor / esp_iris（只连屏幕镜像）
```

`main` 只做装配：`app_ui_start()` 一次拿到整块 UI（面板 + 场景 + 输入），
再把 GSP 句柄交给 Iris 屏幕镜像。

## 二、app_ui_display：只点屏

- `bsp_display_new()` 用 BSP 默认配置点屏，回读 panel 与 SPI IO
- 填 presenter 描述：RGB565 输入、rotation 0、byte swap；`CONFIG_BSP_CO5300_ENABLE_TE`
  时给 TE（GPIO、总线时钟、位宽）；`fb.mode = AUTO`、`drawbuf.in_psram = true`
- 打一行分辨率与 TE 状态日志

不创建触摸控制器（那是 `app_ui_touch`），不引用 GSP 类型，不依赖任何应用组件。

**所有权**：panel、SPI IO、PSRAM draw buffer 归 BSP，整个运行期不释放。
GSP 侧策略由 `app_ui_core` 决定：touch adapter 恒为 NULL（触摸设备是唯一读者）、
渲染对齐 4 px（QSPI 面板）、渲染任务栈放在 PSRAM。

```c
esp_err_t app_ui_display_init(esp_display_present_target_config_t *out_target);
```

## 三、app_ui_touch：只做设备

- CST9220 每 16 ms 读一次（I2C，BSP 已配好），BOOT 键同周期采样，两次采样一致才认
- 只发布事件，不做页面/手势判定；队列见 §五
- 触摸控制器缺失只打日志，不阻塞启动

```c
esp_err_t app_ui_touch_start(void);   /* 触摸 + BOOT 键 + 事件队列 */
void app_ui_touch_stop(void);
```

任务栈 3072、优先级 6；`app_input_publish()` 可从中断/任意任务调用。

## 四、app_ui_core：页面与场景

| 文件 | 内容 |
| --- | --- |
| `app_ui.c` | `app_ui_start()`（bundle → 面板 → GSP → prepare）、页面显隐、设置页芯片、摇杆帽、发光层、终端文本、跑马灯定时器 |
| `app_ui_input.c` | 消费事件：页面语义（mouse / keyboard / gamepad / setting）、overlay 绘制 |
| `ui/main.json` | 480×480 场景（六个页面层） |

启动顺序：

1. `app_cfg_set_layout_rpc()`：把「当前页面 / 切页」两个回调交给 RPC（见 §七）
2. `ui_bundle_open()` 打开 `ui_apps` 分区里的场景
3. `app_ui_display_init()` 点屏并拿到 presenter 描述
4. `esp_gsp_esp_lcd_start_prepared(..., ui_prepare, ...)`：在**渲染任务首帧之前**执行
   `ui_prepare` —— 初始页面（idle）、跑马灯起点、`app_ui_refresh_chips()`、
   注册 overlay contributor、创建 40 ms 定时器
5. 返回后启动输入层：`app_ui_touch_start()` → `app_ui_input_start()` 消费任务

**overlay 必须在 prepare 回调里注册**：`esp_gsp_set_overlay_contributor()` 是投递到渲染
任务队列的调用，在 `app_main` 里注册正好撞上 181 个对象的首帧合成，稳定返回
`ESP_ERR_TIMEOUT`（重试 40×50 ms 也无效），触摸层因此起不来。输入侧仍保留兜底：
未成功时每个 tick 重试一次，只在状态翻转时打日志。

公开 API（`app_ui.h`）：

| 函数 | 契约 |
| --- | --- |
| `app_ui_start()` | 面板 + 场景 + 定时器 + overlay + 输入设备，一次性；返回 bundle/display/GSP 错误 |
| `app_ui_handle()` | GSP 句柄，启动前为 NULL（供 Iris 屏幕镜像） |
| `app_ui_set_layout()` / `app_ui_layout()` / `app_ui_layout_name()` / `app_ui_layout_from_name()` | 当前页面，任意任务可调 |
| `app_ui_refresh_chips()` / `app_ui_set_chip()` | 设置页芯片，数据源是 `app_cfg` |
| `app_ui_set_button_glow()` / `app_ui_set_dpad_glow()` / `app_ui_set_side_glow()` / `app_ui_set_stick_knob()` | 手柄页反馈 |
| `app_ui_set_terminal_line()` | 终端页文本 |

GSP setter 是排队到渲染任务的，渲染忙时可能被拒；`app_ui.c` 会记录首批失败并在
下一个 tick 重刷一次芯片状态。

## 五、输入事件模型与数据流

触摸 16 ms 采样、界面 16 ms 消费。输入层发布**事件**而不是坐标快照，否则 40 ms 级
的消费周期会吞掉快速轻点。

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
    uint32_t seq;       /* 单调递增 */
} app_input_event_t;
```

- 队列深度 16，单生产者单消费者，`portMUX_TYPE` 自旋锁（本芯片是 FreeRTOS SMP，
  `taskENTER_CRITICAL()` 必须带锁参数）
- 满时丢最旧并累加 `dropped`；状态事件在一个 16 ms 周期内不会被挤掉
- 抬起事件保留最后坐标，鼠标点击判定不依赖「已清零的采样」

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
   └─ 设置页芯片刷新入口
```

## 六、页面与布局

页面：`blank`（idle）/ `mouse` / `keyboard` / `gamepad` / `sensor` / `terminal` /
`setting`，同一时刻只显示一层。切页只有 BOOT 键与 RPC（BOOT 循环顺序即此顺序），
**屏幕四角不再切页**。

### 6.1 手势与页面语义

| 页面 | 行为 |
| --- | --- |
| mouse | 相对移动光标；单指轻点（<200 ms、位移 <8 px）= 左键；双指轻点 = 右键；双指上下滑动 = 滚轮 + 横移 |
| keyboard | 12×5 键表，最多两触点，发 8 字节报告 |
| gamepad | 双摇杆、十字键、ABXY，外加 §6.2 的八个侧控 |
| sensor | 三行 gyro/accl/mag 实时值 + 底部陀螺仪鼠标开关，见 §6.3 |
| setting | 三段（usb / bt / ble）各一个 `[default]`，芯片点击写 `app_cfg`（重启生效） |
| terminal | `app_log` 最新 30 行，4 Hz 刷新 |

### 6.2 手柄页侧控（坐标相对 480×480）

右侧是左侧镜像（`x' = 480 - x - w`）：

| 控件 | 位置（x,y,w,h） | 形态 | 作用 |
| --- | --- | --- | --- |
| LB | 60, 12, 120, 34 | 左摇杆正上方横条 | bit 8 |
| RB | 300, 12, 120, 34 | 右摇杆正上方横条 | bit 9 |
| LS | 188, 60, 40, 120 | 左摇杆**正右方**竖条（数字键） | bit 10 |
| RS | 252, 60, 40, 120 | 右摇杆正左方竖条 | bit 11 |
| SELECT | 60, 192, 120, 34 | 左摇杆**正下方**长条 | bit 12 |
| START | 300, 192, 120, 34 | 右摇杆正下方长条 | bit 13 |
| LT | 8, 60, 40, 120 | 左摇杆左侧竖条，**模拟滑条** | 扳机 0..255 |
| RT | 432, 60, 40, 120 | 右摇杆右侧竖条 | 扳机 0..255 |

十字键与 ABXY 都带 `#161616` 圆形外轮廓（半径 24 / 30，边框 `#2A2A2A`），按下时换成
同形状的发光层。摇杆中心 (120,120)/(360,120)、半径 60，十字键 (120,360)，ABXY (360,360)。

实现要点：

- **触点归属在按下时锁存**（`s_in.capture[]`）：整个按压期间不变。所以从 LT 滑到摇杆
  区域时仍然只在拖 LT，摇杆不会被带偏；反过来也一样
- LT/RT：`value = (y - bar_y) * 255 / (bar_h - 1)`，顶部 0、底部 255，每 16 ms 调一次
  `app_usb_hid_set_triggers()`；同一滑条两个触点取较大值，松开归 0
- 锁存到普通按键的触点只上报对应 bit，不再参与摇杆/十字键/ABXY 判定
- 离开手柄页时 `gamepad_release_all()`：按键清零、摇杆回中、扳机回 0、发光层全灭
- 扳机只在 0 ↔ 非 0 边沿打日志（`trigger L=… R=…`）

### 6.3 传感器页与陀螺仪鼠标

页面从上到下：`SENSOR` 标题、三行数值（`gyro` 0.1 °/s、`accl` mg、`mag` 0.1 µT，X/Y/Z 整数，
约 10 Hz 刷新，没有磁力计时第三行显示 `n/a`）、单位提示、**左/右键**、**GYRO MOUSE 开关**。
所有坐标与 `ui/main.json` 一致：

| 控件 | 位置（x,y,w,h） | 行为 |
| --- | --- | --- |
| `LEFT` | 48, 272, 176, 64 | 按下即报左键，抬起释放；按下时青底深字 |
| `RIGHT` | 256, 272, 176, 64 | 同上，右键 |
| `GYRO MOUSE` | 48, 356, 384, 64 | 点一次开、再点关；开=青底深字 + `GYRO MOUSE  ON` |

按键细节：

- **两指可以同时按住 L 和 R**：每个触点用 `s_sensor_grab[]` 记住自己抓的是哪个键，抬起只释放自己那个
- **拖拽不丢键**：陀螺仪位移上报时把当前按键位一起带上（`app_usb_hid_mouse(s_sensor_click, dx, dy, …)`），
  所以按住 LEFT 转动手腕就是拖动
- 按键与 gyro 开关**互相独立**：开关关着也能当普通鼠标键用
- 离开页面 `sensor_release_clicks()` + `sensor_set_mouse(false)`，不会把键卡住

开关细节：

- **只在有鼠标类可用时显示**：判定 `(usb_hid | bt_hid | ble_hid) & APP_CLASS_MOUSE`；
  否则整块隐藏，日志打 `sensor page (no mouse class)`
- 状态**不持久化**，且**离开该页自动关闭**
- 打开后把转动变成指针位移，走 `app_usb_hid_mouse()`，USB / 经典蓝牙 / BLE 哪个连着就发给哪个

映射与手感参数（`app_ui_input.c`）：

| 项 | 值 |
| --- | --- |
| 轴映射 | `gyro Z → 光标 X`，`gyro X → 光标 Y`，两轴都取负（`dx = -gyro_z`、`dy = -gyro_x`） |
| 灵敏度 | `px/s = rate(0.1 °/s) × 2.0`，即 100 °/s ≈ 2000 px/s（`SENSOR_PX_NUM 20`） |
| 死区 | ±20（2 °/s）以内不产生位移，静止不漂 |
| 余数 | 不足 1 px 的部分留在 1/1000 px 累加器里，慢速移动也不丢 |
| 单次增量 | 钳位 ±127（报文是 int8） |

数值来源是 `app_sensor_latest()`：它**不消费**新鲜度，因此不会和 16 ms 的报文路径抢采样。
页面与运动报文互不影响：运动接口（report ID 4）在任何页面都以 ~62 Hz 持续上报，
陀螺仪鼠标只是**额外**的消费者。

### 6.4 设置页语义

| 点击 | 结果 |
| --- | --- |
| `kbd` / `mouse` | 翻转对应位，其余不动 |
| `gamepad` | 清掉 `ds4/ds5/xbox`，再翻转 gamepad 位 |
| `sensor` | 清掉 `ds4/ds5/xbox`，再翻转 sensor 位（**gamepad 位不动**，两者可共存） |
| `ds4` / `ds5` / `xbox` | 若是当前身份则关闭；否则清掉 `gamepad + sensor + 另外两个身份`，只置该位 |
| `audio`（usb/bt 行） | 翻转 usb 音频开关 / bt 音频掩码；ble 行的 audio 是占位 |
| `[default]`（每段一个） | usb：`APP_CLASS_USB_DEFAULT`（kbd+mouse+gamepad+**sensor**）+ audio 开；bt：bt_hid 全开 + 音频全开；ble：ble_hid 全开 |

选中 = 青底 + 深色字，未选中 = 深底 + 浅色字，**没有第三种「置灰」状态**。
所有改动重启生效（日志带 `reboot to apply`）。

芯片几何必须与 `ui/main.json` 一致：`x = 12 + 92*i`、`86×38`，行 y = 84 / 128 / 218 / 308；
`[default]` 在标题右侧 x=50,w=96,h=30。

## 七、页面状态与 RPC 的解耦

页面枚举与设置项在 `app_ui_core`，RPC 服务在 `app_cfg`。原先 `app_cfg → app_ui_core
→ app_cfg` 是循环依赖。现在由 UI 在启动时注册两个回调：

```c
typedef struct {
    const char *(*name)(void);                  /* 当前页面名 */
    const char *(*apply)(const char *name);     /* 切页，未知返回 NULL */
} app_cfg_layout_rpc_t;
void app_cfg_set_layout_rpc(const app_cfg_layout_rpc_t *rpc);
```

`app_cfg` 不包含 `app_ui.h`，依赖图无环。RPC 行为不变：method 1 返回 `layout`，
method 4 切页并回显名字。

## 八、真机验收清单

开机日志：`input device started` → `overlay attached` → `input started (54 keys)`，
**无** `touch input unavailable`。

- 切页：BOOT 键循环（`boot key -> gamepad/terminal/setting`）、RPC method 4 都正常
- 设置页：三行芯片与配置逐项一致；`[default]` 恢复对应段的默认值
- 鼠标：单击计数正确；连续快速轻点 20 次不丢
- 键盘：单键、两指组合正确
- 手柄页：八个控件按下去只亮自己（不再顺带把摇杆推到位）；LT/RT 输出 0→255；
  从 LT 滑到摇杆区域时摇杆保持不动（触点锁存）
- 双指手势：右键、滚动/横移
- overlay：idle 页触点圆环、键盘页按键高亮

## 九、已知权衡与限制

| 取舍 | 说明 |
| --- | --- |
| 事件队列 vs 坐标快照 | 事件不丢状态，但需要消费端抽干；快照简单却会丢快速点击 |
| 两个任务 vs 一个任务 | 设备 16 ms、界面 16 ms 分开，职责清晰；代价约 3 KB 栈 |
| overlay 归 core | 绘制与命中共享同一份几何；注册时机也随之正确 |
| BOOT 键归输入层 | 与触摸同为物理输入；页面切换语义仍在 core |
| 布局 RPC 回调 | 用 10 行回调换掉一个组件环，`app_cfg` 保持无 UI 依赖 |

UI 之外的已知问题（通话、蓝牙鼠标卡顿、显示长跑）见 `docs/gotcha.md` §十。
