# Gamepad 项目踩坑记录（USB 复合 HID + 经典/低功耗蓝牙 + 音频 + 显示）

> 记录原则：每条都写「现象 → 原因 → 修复 → 验证」，方便下次直接定位。
> 时间线：2026-11，ESP32-S31 / ESP-Mosaico V1.2 / IDF master `0e6b480f`。

---

## 零、组件与职责

| 组件 | 职责 |
| --- | --- |
| `app_cfg` | NVS 配置（USB/BT/BLE 各功能位、VID/PID、音频）、RPC 0x4700；页面切换通过 UI 注册的回调转发。BT/BLE 掩码允许 sensor 位（`APP_CLASS_BT_ALL`），只有 USB 掩码含 Sony/Xbox 身份 |
| `app_usb_core` | USB 端点分配（配合 Iris 的 console CDC） |
| `app_usb_hid` | USB HID 每个类一个接口（手柄/鼠标/键盘）+ 可选 Sony 或运动接口；BT 共用其报告描述符 |
| `app_usb_audio` | UAC2 播放 + 采集 |
| `app_bt_core` | 控制器/Bluedroid 启动、双身份（BLE + Classic）、报告路由 |
| `app_ble_hid` | BLE HOGP 设备 |
| `app_btclassic_hid` | BR/EDR HID（HIDP）设备 |
| `app_btclassic_audio` | A2DP sink + HFP HF |
| `app_audio_codec` | ES8311/I2S 播放与麦克风、音量策略（源自 `projects/audio`） |
| `app_sensor` | BMI270 100 Hz（mg / 0.1 dps）+ BMM150 磁力计，供 sensor 接口与 Sony 报文 |
| `app_ui_core` | 页面语义（翻页/鼠标/键盘/手柄/设置）、HID 上报、overlay、场景与定时器 |
| `app_ui_display` | 只点屏并给出 presenter 硬件描述（不碰触摸、不依赖 esp-gsp） |
| `app_ui_touch` | 只做设备：CST9220 轮询 + BOOT 键 → `app_input_event_t` 事件队列 |
| `app_log` | 自己的日志环形缓冲（供终端页） |

布局枚举：`blank / gamepad / mouse / keyboard / terminal / setting`，只靠 BOOT 键循环与 RPC 切换
（屏幕四角切换已移除）。手柄页在摇杆两侧各有 LB/LS/SELECT/LT 与镜像的 RB/RS/START/RT，布局见 `docs/app_ui.md`。

---

## 一、USB

### 1.1 Iris 只有 console，刷机不能走 data link

`sdkconfig.defaults` 里关闭了 data link（`# CONFIG_ESP_IRIS_DATA_LINK is not set`），所以：

- `mosaico.py iris system-update` 会报 `The device data link is not ready`
- 必须先 `iris test enter-recovery`（走 console CDC），再 `iris system-update`

### 1.2 端点分配必须动态

Iris console 占用 `0x81`（notify）、`0x82`（data IN），USB HID 与麦克风从 `0x83` 起顺延。硬编码 `0x82` 曾导致设备能枚举但功能全不绑定。

相关 API（ESP-Iris 侧已扩展）：`esp_iris_usb_first_free_interface()`、`esp_iris_usb_set_hid_ifaces()`、`esp_iris_usb_set_extra_interfaces()`、`esp_iris_usb_set_ids()`。

### 1.3 VID/PID

默认 `0x303A:0x4002`，Iris 主机工具是按这个 VID/PID 发现设备的；改成 Sony ID 后主机工具找不到设备（已知取舍）。可通过 RPC 设置 `usb_vid/usb_pid`，DS4/DS5 模式会自动用 Sony 的 ID。

### 1.4 热插拔

S31 上「开机后再插 USB」偶发不枚举，Iris 里加了看门狗：未挂载且未挂起持续 ~2 s 就 `tud_disconnect()` → 50 ms → `tud_connect()`，限流 5 s。

---

## 二、HID 报告

- 手柄：report ID 1，8 字节（2 字节按键 + X/Y/Rx/Ry + Z(LT)/Rz(RT)）
- 运动：report ID 4，18 字节（3×int16 加速度 / 3×int16 角速度 / 3×int16 磁力）
- 鼠标：report ID 2，5 字节（按键 + X + Y + 滚轮 + AC Pan）
- 键盘：report ID 3，**8 字节，布局必须是 `[修饰键][保留][6 键码]`**（键码从 index 2 开始；曾因布局错误导致完全不响应）

`tud_hid_n_ready(inst)` 必须按接口实例判断；鼠标增量只有在报文被接受后才清零（否则丢帧）。

`tud_hid_descriptor_report_cb()` 必须回答**每一个**已注册实例（含 sensor / Sony），否则 TinyUSB 取报表
描述符时会从空指针拷贝 → panic，连带 UAC2 与 Iris console 一起失效（§6.8）。

### 2.1 USB 与蓝牙彻底解耦（已改）

**原来的问题**：两个传输共用一份状态和一份鼠标增量，且增量只在 **USB 成功**时清零：

- 插着 USB：增量被 USB 每 8 ms 清一次，BT 也按 125 Hz 发 → 空口排不完，队列持续增长 → 越用越卡
- 没插 USB：增量永不清零 → 每 tick 重复发送 → 光标单向漂移

**现在的结构**（`app_usb_hid.c`，两个传输完全独立）：

| 项 | USB | 蓝牙 |
| --- | --- | --- |
| 手柄/键盘快照 | `usb_last_gp/kbd` | `bt_last_gp/kbd` |
| 鼠标增量游标 | `usb_sent_x/y/w/p` | `bt_sent_x/y/w/p` |
| 鼠标按键状态 | `usb_buttons` | `bt_buttons` |
| 鼠标发送节拍 | 每 tick（8 ms） | 每 2 tick（16 ms ≈ 62 Hz） |
| 门控条件 | `tud_mounted() && tud_hid_n_ready(inst)` | `bt_connected()` |

要点：

- 每条传输只在自己成功发出后才推进自己的游标/快照，互不影响
- 保留 250 ms 的 keepalive：把两个传输的快照都置为无效，强制重发当前状态
- 鼠标增量由触摸侧累加（`s_mouse_dx` 等），每条传输各自计算「未发送的差值」，快滑时按 127 截断，剩余量留在累加器里下次再发

### 2.2 HID usage 规范对照（2026-11-18）

参考：HID Usage Tables（Generic Desktop / Button 页）、HID Sensor Usages（Sensor 页 0x20，
[Linux `hid-sensor-ids.h`](https://github.com/torvalds/linux/blob/master/include/linux/hid-sensor-ids.h) 是最好查的编号表）、
Apple Accessory Design Guidelines 的 6 轴 IMU 与手柄示例、Android 的 Xbox `.kl` 映射。

| 项 | 我们原来的写法 | 规范/惯例 | 结论 |
| --- | --- | --- | --- |
| 手柄集合 | `Usage (Game Pad 0x05)` + Application | 同上 | ✅ 合规 |
| 手柄按键 | Button 页 `Usage Min 1 / Max 16`，Logical 0..1 | 同上 | ✅ |
| 左摇杆 | `X (0x30)` / `Y (0x31)` | X/Y = 左摇杆 | ✅ 与 Xbox/Linux/Android 一致 |
| 右摇杆 | `Rx (0x33)` / `Ry (0x34)` | Rx/Ry = 右摇杆 | ✅ |
| 扳机 | `Z (0x32)` / `Rz (0x35)`，0..255 绝对量 | Z/Rz = 扳机（Xbox 系） | ✅ 用法对，但**声明顺序**是 X,Y,**Rx,Ry,Z,Rz**，与常见 X,Y,Z,Rx,Ry,Rz 不同；按字段顺序解析的主机会把轴错位 |
| 摇杆范围 | 0..255（中心 128） | Xbox 用有符号 -32768..32767（中心 0） | ⚠️ 合法但不通用，按有符号解释的主机会偏到右下 |
| 十字键 | 4 个 Button bit | 通常用 `Hat switch (0x39)` | ⚠️ 合法，但 Apple/ADT 之类按 Hat 找方向键 |
| 运动接口 | **vendor 页 0xFF00，usage 1**，9×int16 一包 | Sensor 页 0x20：`Accelerometer 3D 0x73` / `Gyrometer 3D 0x76` / `Compass 3D 0x83`，数据 usage 0x453-0x455 / 0x457-0x459 / 0x485-0x487 | ❌ 已改（2026-11-18），仍共用 report ID 4 保持 18 字节线格式 |

**尚未做**（会改线格式，需要确认）：手柄摇杆改成有符号、usage 顺序改成 X,Y,Z,Rx,Ry,Rz、
十字键改 Hat switch、增加 `Physical Min/Max` 与 `Unit`/`Unit Exponent` 属性；
运动接口补 `Report Interval (0x30E)` / `Report State (0x316)` 等 Feature 属性（Windows 传感器栈需要）。

### 2.3 经典 HID 延迟的另一半：sniff（见 §4.2）

---

## 三、BLE HID（HOGP）

### 3.1 必须注册 GATTS 回调（最隐蔽的坑）

**现象**：手机能配对、能显示「已连接」一瞬间，随后变成「已保存」；设备侧完全没有 `BLE_HIDD` 日志。

**原因**：Bluedroid 只有一个 GATTS 回调槽，`esp_hid` 不会自己注册，需要应用调用：

```c
esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);   // 来自 esp_hidd_gatts.h
esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, cb, &dev);
```

没注册 → 属性表从未创建 → HID 服务不存在 → 手机服务发现为空 → 断开。

**验证**：注册后日志出现 `DBG creat attr tab[0] status=0x0 num=29 svc=52` 和 `DBG CONNECT[0]`。

### 3.2 静态随机地址的类型位

`esp_ble_gap_set_rand_addr()` 要求地址的最高两位为 `0b11`。Bluedroid 校验的是 `addr[0]`（把 index 0 当最高字节），而 LE 规范说的是最高字节。做法：**两端都置位**

```c
random_addr[0] = (random_addr[0] & 0x3F) | 0xC0;
random_addr[5] = (random_addr[5] & 0x3F) | 0xC0;
```

否则报 `BT_BTC: Invalid random address type` + `BT_BTM: No random address yet`，广播失败。

地址必须**永远不变**（从 MAC 派生），否则手机缓存旧地址后会连不上。改过两次生成方式后，手机里必须删除旧记录重新配对。

### 3.3 广播内容

- **flags 用 `0x06`（LE-only）**：我们用独立随机地址，没有对应的经典蓝牙。若写 `0x02`（支持 BR/EDR），Android 会判断为双模设备并**改走经典蓝牙**连接（表现为「点 BLE 却连到 Classic」）
- `esp_ble_adv_data_t` 的 `service_uuid_len` **必须是 16 的倍数**（只接受 128 位 UUID），所以改用 `esp_ble_gap_config_adv_data_raw()` 手工拼 16 位 UUID `0x1812`
- 广播载荷 31 字节不够放 flags + UUID + 名字时，名字放扫描响应（或用 raw 拼 29 字节）
- 广播参数用 `ADV_TYPE_IND` + `BLE_ADDR_TYPE_RANDOM`

### 3.4 报告格式

`esp_hidd_dev_input_set()` 直接把 buffer 发到 Report 特征值，**不要自己前置 Report ID**——特征值长度按 `input_len/8` 设置，ID 由 Report Reference 描述符告知主机。前置 ID 会让所有字段整体移位（鼠标方向错乱、键盘按键错位）。

### 3.5 断开后要重新广播

Bluedroid 连接时停止广播；`ESP_HIDD_DISCONNECT_EVENT` 不一定派发到回调。现在：每秒轮询 `esp_hidd_dev_connected()`，检测到「已连→未连」就重新广播；另外 `RPC 0x4700 / 8` 可手动重启广播。

### 3.6 配对

`ESP_LE_AUTH_REQ_SC_BOND` + `ESP_IO_CAP_NONE`（Just Works，不弹 PIN）+ 16 字节密钥 + ENC/ID 密钥分发。
认证失败时自动 `esp_ble_remove_bond_device()`；`RPC 0x4700 / 7` 可清空全部绑定。

---

## 四、经典蓝牙 HID

### 4.1 SDP 记录容量（HID 服务注册失败）

**现象**：`BT_SDP: the free pad for SDP record ... is full` → `BT_HIDD: HID_DevAddRecord: failed to complete SDP record` → `Setting hid parameters failed (1), now deint!`，经典 HID 完全不可用。

**原因**：HID 的 SDP 记录要内嵌整个 report map（现在 166 字节起、启用运动接口时 224 字节），
默认 `CONFIG_BT_SDP_PAD_LEN=300` 不够。

**修复**：`CONFIG_BT_SDP_ATTR_LEN=1024`、`CONFIG_BT_SDP_PAD_LEN=1024`。成功标志：`BT_HIDD: Setting hid parameters success!`。

### 4.2 延迟：Bluedroid 自己在请求 sniff（根因）

**现象**：鼠标能移动但明显滞后/拖尾，不打电话、A2DP 未推流时也存在。

**原因**：Bluedroid 的 BTA DM 电源管理对 **HD（HID Device）profile** 有一套默认策略，在
`components/bt/host/bluedroid/bta/dm/bta_dm_cfg.c` 的 `/* HD : 6 */` 表里：

- 连接后 5 s、idle 后 5 s 请求 sniff
- **每发一个 HID 报文（"busy" 事件）立即请求 sniff，延时 0**

sniff→active 往返约 **40–50 ms**，直接主导输入延迟，并且会覆盖应用设置的任何 QoS。
（参考社区补丁 [esp32-bthidbridge: disable HID device sniff](https://raw.githubusercontent.com/anarsoul/esp32-bthidbridge/master/patches/0001-bta-dm-disable-hid-device-sniff.patch)、
[IDF #5887](https://github.com/espressif/esp-idf/issues/5887)）

**修复**：把该表的 allowed modes 清零，使 `pm_action = allowed_modes & (PARK|SNIFF)` 恒为 `NO_ACTION`：

```c
/* HD : 6 */
{
    (BTA_DM_PM_NO_ACTION),   /* 原来是 (BTA_DM_PM_SNIFF | BTA_DM_PM_PARK) */
```

另外保留连接时的一次 `esp_bt_gap_set_qos(bda, 20)`（12.5 ms poll），让主动模式本身也更跟手。

**踩过的弯路**：只调 `esp_bt_gap_set_qos` 无效；「每 2 秒重申 QoS」也无效——因为报文发送路径里
的 sniff 请求触发得比重申更频繁。

**2026-11-19 新证据（待验证，尚未改代码）**：实测「通话中蓝牙鼠标完全不卡，挂断后立刻又卡」。
这和上面的根因完全吻合：

- 通话期间 §4.3 的 1 秒定时器持续把 ACL 顶回 active，HID 报文和 SCO 走同一条 ACL，
  所以 sniff 消失、鼠标顺滑
- 挂断后定时器停止，`HD : 6` 表又开始每发一个报文就请求 sniff（延时 0），卡顿立刻回来

也就是说**鼠标卡顿和通话没声音是同一个根因**。修法已经收敛到一个公共入口（`app_bt_core`）：

| 接口 | 语义 |
| --- | --- |
| `app_bt_core_kick_acl_sniff()` | 立刻把当前经典 ACL 踢出 sniff；**谁都可以随时调**，1 秒窗口内的重复调用直接丢弃（`APP_BT_KICK_MIN_INTERVAL_US`），返回本次是否真的踢了 |
| `app_bt_core_note_classic_peer(bda)` | 缓存对端地址，踢的时候不用遍历配对列表（HFP SLC 事件会带 remote_bda；经典 HID 连接事件不带） |
| `app_bt_core_kick_stats(kicks, throttled, age_ms)` | 计数器，进 `bt_classic: status` 的 `kick=<执行>/<节流>/<age ms>` |
| `app_bt_core` 里的 1 秒定时器 | 持续踢。**启动条件当前是桩**：只要 Classic 启用就开着；将来收窄为「经典已连接 且（HID 某个类启用/已连接 或 通话真正建立）」 |

调用点：经典 HID 连接、HFP SLC 建立、RING/CIND/audio-state 各事件，以及上面的周期定时器。
节流判断与时间戳写入在同一个 `portMUX` 临界区里完成，避免两个任务同时通过检查。

内部仍然调 `bta_dm_pm_active()`（IDF 导出、无公开头文件的符号）。
验证口径：`kick=` 计数在增长、`hci cmd send: sniff` 每分钟 0 条、鼠标手感与通话中一致。

---


### 4.3 通话没声音：sniff 把 eSCO 挤掉（2026-11-18，已修）

现象：手机已连上、`call=1`、`sco_try=1`、`sco_ok=1`，但 `sco=0`、`rx=0 tx=0`，通话双方都没声音。

日志顺序：

```
W BT_HCI: hci cmd send: sniff: hdl 0x801, intv(10 18)
W BT_HCI: hcif mode change: hdl 0x801, mode 2, intv 18
I bt_classic: link mode 2
E OLM_CONN: pend list full on link:0, refuse proc_type:0x800d     ← eSCO 建立被控制器拒绝
```

原因：Bluedroid 的电源管理表给 HFP（`bta_dm_cfg.c` 里 `/* HS : 4 */`）允许 sniff，而且
`sco open, active` 这一项本身就配成 `BTA_DM_PM_SNIFF3` —— 通话中都会请求 sniff。ACL 进 sniff 后，
控制器的挂起链表被占满，随后的 eSCO setup 直接被拒，通话要等下一次重试才有声音（“打了一会儿对方才听见”）。

**修法（2026-11-19，项目侧，不改 IDF）**：表是 `const`（放 flash）、没有 Kconfig、也没有公开 API
能关掉 host 主动发的 `HCI_Sniff_Mode`，但 BTA 电源管理器**导出**了（调用封装见 §4.2 的
`app_bt_core_kick_acl_sniff()`）：

```c
void bta_dm_pm_active(uint8_t peer_addr[6]);   /* bta_dm_pm.c，内部符号，无公开头文件 */
```

它内部就是 `BTM_SetPowerMode(..., BTM_PM_MD_ACTIVE)`，会把链路从 sniff 拉回 active。
`app_btclassic_audio.c` 里这样用：

- `force_link_active()`：遍历已配对设备（一般只有一个），逐个 `bta_dm_pm_active()`
- **收到 RING / CIND call 事件时先 `force_link_active()`，再 `esp_hf_client_connect_audio()`**
  —— 顺序很关键，晚于发起请求就没用了
- HFP 进入 `SLC_CONNECTED` 后起 **1 秒周期 `esp_timer`**，通话期间持续顶回 active
  （表格会在 SCO open 后 7 秒再次请求 sniff）；断链时停表

实测：改前“通话要等一会才出声”，改后 `sco_try=0 sco_ok=1` 一次建链、`amp≈2200`、对方立即能听到。

风险：`bta_dm_pm_active` 是 IDF 内部符号（本项目锁定 IDF SHA，暂时安全）。若将来升级 IDF 后它消失，
链接会报 undefined reference，届时再考虑回退到 `patches/` 方式（历史记录见 git log）。

## 五、音频（A2DP / HFP）

### 5.1 Voice-over-HCI：只有拉模式真正工作

`esp_hf_client_audio_data_send()` 被 `#if (BTM_SCO_HCI_INCLUDED && BTA_HFP_EXT_CODEC)` 包裹，`BTA_HFP_EXT_CODEC` 只有在 `CONFIG_BT_HFP_USE_EXTERNAL_CODEC=y` 时才为真。**没开这个选项时，该 API 永远返回 ESP_FAIL**（日志表现为 `serr` 增长、`tx=0`）。

真正供料的是：

```c
esp_hf_client_outgoing_data_ready();   // 通知底层来取
// 底层回调 esp_hf_client_outgoing_data_cb_t(uint8_t *buf, uint32_t len)
```

回调约定**非阻塞**，所以麦克风数据要先写进环形缓冲，由回调按需取走。

### 5.2 CVSD 是 8 kHz

`CONFIG_BT_HFP_WBS_ENABLE=n` 时走 CVSD 8 kHz（`s_sco_rate=8000`，`sco_factor()=6`）。按 16 kHz 处理会**音调升高一倍**。

打开 WBS（mSBC 16 kHz）后，当前这台手机协商出 `BT_APPL: bta_hf_client_sco_co_out_data invalid air mode: 255`，数据被丢弃 → 最终关回 CVSD。

### 5.3 上行音质：循环声与 75% 取走率（已解决循环声）

**症状一：对方听到「一小段循环」**

原因不是欠载，而是控制器**同步 TX 缓冲只有 2 个**（`CONFIG_BT_CTRL_BR_EDR_STATIC_SYNC_TX_BUF_NB=2`，是早期削减内存时设的，IDF 默认 4）。抖动一来缓冲就满，控制器丢包后重发上一包，听感就是一小段循环。

**修复**：`CONFIG_BT_CTRL_BR_EDR_STATIC_SYNC_TX_BUF_NB=4`，循环声消失。

**症状二：栈只取走 75% 的样本**

计数器（通话 152 秒）：

```
pull=15395/1847400/1833760/0   请求 1.85 MB，实际给出 1.83 MB（99.3%，不欠载）
mic=15225/2436000             生产 2.44 MB（16,000 B/s = 8 kHz × 2 B，实时正确）
peak=1025                     环形缓冲长期满
```

每次拉取固定 **120 字节（60 样本 = 7.5 ms）**、约 101 次/秒 → 手机每秒只拿到约 6.1 k 样本。理论上会让对端声音偏慢约 1/4；实际听感尚可接受（语音上不明显）。

**结论**：这个 120 字节由手机的 AG 在 eSCO 协商时决定，**从 HID/HF client 侧改不了**。实测把控制器支持的包型扩到 `3-EV3` / 3-slot（`CONFIG_BT_CTRL_BR_EDR_ESCO_EV3_3_SUPP`、`..._3_SLOTS_SUPP`）**完全没有效果**（每次拉取仍是 120 字节），已回退。

**踩过的弯路**：

- 试图「自己定节拍」给麦克风任务加 10 ms/5 ms 固定延时 → 前者导致欠载循环、后者直接没声音。正解是让阻塞式 I2S 读决定节奏（`MIC_READ_FRAMES=HFP_FRAMES*3`，10 ms）
- 打开 WBS（mSBC）后该手机协商出 `invalid air mode: 255`，数据被丢弃，已关回 CVSD

**若将来要彻底解决 75%**：需要让 AG 协商更大的 eSCO 包（设备侧主动权不在我们），或接受带宽做重采样适配（音调正确但频带变窄）。

**2026-11-18 复测（重要修正）**：另一台手机协商出的是 **5 ms / 120 字节**（≈207 次/秒 → 要求 24.8 kB/s），
而 CVSD 8 kHz 的 PCM 只有 16 kB/s（5 ms 只该要 80 B），所以 `pull_out/pull_req` 稳定在 **66.7%**。
但通话**听感正常**——这个比例是「请求帧大小 × 帧率」与 8 kHz PCM 的不匹配，**不能当作音质指标**，
真正要看的是 `mic rate ... amp=`（生产速率 + 麦克风峰值）。

这一轮真正的问题出在上行生产端，两个都要满足：

1. **`bt_mic` 优先级只有 3**，被 UI/render（4/6）压住 → 只跑 66 次/秒 → 少 1/3 → 对方听不清。改成 **5**。
2. **上行环没有余量**：`peak` 一直是 160 B（刚好一个生产块），栈的 120 B 拉取经常只拿到 80 B 并立刻重试。
   在 `hfp_sco_start()` 里预填 **20 ms 静音**（`sco_rate/50` 字节）后 `peak` 稳定 320 B，听感恢复。

另外：`MIC_READ_FRAMES` 改 5 ms（`HFP_FRAMES*3/2`）会让**对方完全听不到**，必须保持 10 ms（`HFP_FRAMES*3`）。

### 5.4 麦克风消费者：直连模式没有独立游标

`APP_AUDIO_MIC_FANOUT_ENABLE=0` 时，`app_audio_codec_mic_consumer_read()`：

- 不使用 `consumer->pos_q16`，每次直接从共享 I2S 通道读
- 总是返回请求的帧数（不够就补零）

因此 USB 采集与 BT SCO 两个消费者会**互抢样本**；而且该函数内有 `static int16_t stereo[240*2]` 共享缓冲，两个任务并发进入会竞争。

`audio` 项目里的 fan-out 实现（`MIC_FANOUT_RING=960` + 每消费者游标）也有已知缺陷：`pos_q16` 是 uint32，48 kHz 下约 **1.37 秒回绕一次**（见 `projects/audio/bug.md` P1-1）。

当前测试中 USB 采集从未启动（`usb_audio: mic on=0`），所以该问题尚未暴露。

### 5.5 不要自己定节拍

`projects/audio/gotcha.md` §五 记录：任何「自己定节拍」的中间层都会与 USB 1 ms 微帧 / I2S DMA 节奏错位。实测给麦克风任务加 10 ms 固定延时后，对端听到「一小段循环」（SCO 无重传，欠载时控制器重发上一包）。

现在麦克风任务的节奏由**阻塞式 I2S 读**决定（10 ms 一块），只在读不到数据时 `vTaskDelay(5 ms)`。

### 5.6 麦克风增益

`app_audio_codec_mic_start_at()` 里 `esp_codec_dev_set_in_gain(36.0f)` 偏高，容易削顶产生沙沙声，待调整（24–30 dB）。

### 5.7 播放音量

单一决策点 `audio_codec_apply_volume()`：蓝牙源 → 100%，其它 → 主机音量。`APP_AUDIO_BT_GAIN_Q16=0x18000`（×1.5）对蓝牙做数字增益（饱和处理）。

通话时对端声音小，多数情况是**手机侧通话音量**，不是本机问题。

### 5.8 通话排障顺序（照这个顺序查，别从日志里猜）

**第 0 步：确认链路真的通了。** 只看 `bt_classic: status` 这一行：

```
paired=1 a2dp_link=1 hfp=3 sco=1 call=1
```

`paired=0` 说明这台设备当前**没有已配对的手机**（刷机后手机常常不会自动重连），先让手机重新配对/连接；
`hfp=-1` 说明 HFP SLC 没建立；`call=1` 但 `sco=0` 才是 audio 链路的问题。别在没有链路的时候分析音频。

**第 1 步：ACL 有没有进 sniff。** 通话/音乐期间出现这些行就是问题：

```
W BT_HCI: hci cmd send: sniff: hdl 0x801, intv(10 18)
W BT_HCI: hcif mode change: hdl 0x801, mode 2, intv 18
E OLM_CONN: pend list full on link:0, refuse proc_type:0x800d   ← eSCO 建立被拒
```

原因是 Bluedroid 自己的电源管理表（`bta_dm_cfg.c`）。修法见 §4.3：把 `HS : 4`（HFP）和 `HD : 6`（HID）
的 allowed modes 改成 `BTA_DM_PM_NO_ACTION`（改在 IDF 里，重装会丢，见 §九）。
**判据**：补丁后一次启动里 `hci cmd send: sniff` 应为 0 条。

**第 2 步：上行生产端速率。** 看 5 秒一次的

```
bt_classic: mic rate: 100 iters/s, 16000 B/s, amp=2500
```

- `iters/s` 必须 ≈100（10 ms 一块）。约 66 次/秒 → 麦克风任务被抢占，检查 `bt_mic` 优先级（必须 5，不能是 3）
- `B/s` 必须 ≈16000（8 kHz × 2 B）。偏低同上
- `amp` 是这 5 秒的峰值：说话时应有几百~几千；**一直是 0 说明麦克风通路没声音**，与 SCO 链路无关
- `MIC_READ_FRAMES` 必须是 `HFP_FRAMES*3`（10 ms）；改成 5 ms 会让对方完全听不到

**第 3 步：上行环有没有余量。** `status` 里的 `peak`：稳定在 320 B 左右才算健康；
一直是 160 B（刚好一个生产块）说明栈每次拿不满，需要在 `hfp_sco_start()` 预填 20 ms 静音。

**第 4 步：`pull=…/…/…` 的比例不要当音质指标。** 66.7% / 75% 是**正常**的：手机按
120 B/帧 × 帧率要 24.8 kB/s，而 CVSD 8 kHz 的 PCM 只有 16 kB/s。判据只有听感和第 2 步的 `amp`。

**第 5 步：其它已知坑。**
`invalid air mode: 255` = WBS/mSBC 协商失败（保持 CVSD）；`SCO xmit Q overflow` = 同步 TX 缓冲不足
（`STATIC_SYNC_TX_BUF_NB=4`）；`rx/tx` 恒为 0 是正常的——语音走的是 legacy pull 路径，看 `lx/lt/pull`。

---

## 六、显示（GSP / CO5300）

### 6.1 TE 同步与两条黑线

关闭 `CONFIG_BSP_CO5300_ENABLE_TE` 后，终端页会出现两条水平黑线（肉眼可见、截图看不到，因为截图走独立 mirror 缓冲）。打开 TE 后消失。

TE 打开后需要一整块 PSRAM 合成缓冲（480×480×2 ≈ 460 KB），且合成路径在内存紧张时会报 `TE ticket N submit failed: ESP_ERR_NO_MEM`。

### 6.2 `psram_dma_direct`

BSP 原本 `io_config.flags.psram_dma_direct = false`，颜色数据在 PSRAM 时必须为每个 SPI 传输分配**内部 DMA 反弹缓冲**。改成 `true`（`submodule/esp-mosaico-bsp/.../onboard/display.c`）后内部空闲从 39 KB 升到 112 KB、DMA 从 9 KB 升到 80 KB。

### 6.3 跑马灯不要每帧重设位置

空闲页的跑马灯每 40 ms 调一次 `esp_gsp_component_set_position()` 会强制 25 fps 全屏重绘（旧代码在非空闲页也不断「停车」）。现在只在状态切换时调用一次。

### 6.4 分区绘制缓冲

`esp_display_present` 的 `drawbuf.in_psram = true`（`app_ui_display.c`），否则内部 DMA 会被绘制缓冲占满。

### 6.5 overlay 必须注册在首帧之前

`esp_gsp_set_overlay_contributor()` 是「投递到渲染任务队列」的调用。在 `app_main` 里、
首帧合成（181 个对象）还没结束时注册，队列一直排不进去，会**稳定**返回
`ESP_ERR_TIMEOUT`（试过 40×50 ms 重试，全部失败），表现为「触摸起不来」。

正解：用 `esp_gsp_esp_lcd_start_prepared()`，在渲染任务的首帧回调里注册 overlay
contributor、创建定时器、推初始芯片状态。日志应出现 `overlay attached`。

### 6.6 改 label 文字颜色必须先在场景里声明 color 绑定

`esp_gsp_component_set_color_rgb888()` 需要组件有「canonical color 属性」。label 的 `fg_color` 是**静态**字段，
默认不生成任何运行时属性，调用会返回错误而画面不变（这就是早期「置灰/选中变色」看着没生效的原因）。

做法：给 label 加

```json
"bind": "chip_text_bt_1_color",
"bind_target": "color",
```

编译后 `scene0/gamepad_objects.h` 会生成 `gsp_..._set_color_rgb888()`，通用的
`esp_gsp_component_set_color_rgb888(gsp, key, rgb)` 也会开始生效。

### 6.7 命中框必须和场景坐标逐像素对齐

设置页芯片是 `x = 12 + 92*i`、`86×38`。代码里曾写成 `12 + 78*i`、`72` 宽，导致第 4、5 个芯片点不到、
点「sensor」实际触发「audio/ds4」。改场景坐标时必须同步 `app_ui_input.c` 的常量。

### 6.8 `tud_hid_descriptor_report_cb` 必须回答每个 HID 实例（已修，教训重要）

现象：`usb_hid` 掩码包含 `APP_CLASS_SENSOR`（或 Sony 模式）并重启后，
主机侧 **UAC2 音频设备消失**、**ESP-Iris 不应答 HELLO**，但键盘/鼠标仍勉强可用、屏幕正常。

根因：`tud_hid_descriptor_report_cb()` 只遍历了 3 个通用接口的实例表：

```c
for (size_t i = 0; i < INTERFACE_COUNT; ++i)
    if (s_usb.instance[i] == instance) return s_interfaces[i].desc;
return NULL;   /* ← sensor / sony 的实例走到这里 */
```

而 TinyUSB 在 `GET_DESCRIPTOR(HID REPORT)` 时**不判空**：

```c
uint8_t const *desc_report = tud_hid_descriptor_report_cb(hid_itf);
tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_report, p_hid->report_desc_len);
```

于是主机枚举到第 4 个 HID 接口（sensor）时，从空指针拷贝 → panic → 重启，
整个 USB 复合设备（连同 UAC2 和 Iris console）一起完蛋。Sony 模式当年“Iris 摸不到”
大概率也是这个原因，而不是 VID/PID 过滤。

修法：为非通用实例补上描述符（`s_desc_sensor` / `s_sony_desc`）。顺带把 sensor 的
`ep_size` 改成 `SENSOR_REPORT_LEN + 1`：报表含 report ID 共 19 字节
（通用接口的 9 字节打 8 字节端点一直能跑，但严格来说也该 +1）。

### 6.9 终端字符网格

30 行 × 60 列：字体 `fonts/Mono.ttf`（macOS 的 Andale Mono，等宽 0.6 em），字号 13，行高 16 px，y=0..464。**注意 Andale Mono 是 Apple 系统字体，不可再分发**，发布固件前需换成 DejaVu Sans Mono / Noto Sans Mono。

---

## 七、内存

| 时刻 | internal | DMA | PSRAM |
| --- | --- | --- | --- |
| 开机（UI 前） | ~125 KB | ~93 KB | 12.59 MB |
| 蓝牙+UI 后 | ~104 KB | ~72 KB | 12.59 MB |
| 通话中 | ~104 KB | ~72 KB | 12.59 MB |

有效的配置：

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`、`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`
- `CONFIG_BT_CTRL_ALLOW_MALLOC_FROM_SPIRAM=y`（**仅控制器**；Bluedroid 主机栈没有 PSRAM 选项）
- 显示绘制缓冲与 GSP 渲染栈放 PSRAM

注意：

- `CONFIG_ESP_WIFI_ENABLED` 是**隐藏符号**（`default y if SOC_WIFI_SUPPORTED`），无法通过 sdkconfig 关闭；日志里的 `factory_network/wifi` 行来自 Vibe Mode 固件的残留日志
- 日志环很小，`SCO xmit Q overflow`、`TE ticket` 这类逐包/逐帧告警会在几十秒内把其它日志全部挤掉。排查时用 `esp_log_level_set("BT_BTM", ESP_LOG_ERROR)` 之类临时压掉，或把它们加到 `app_log` 的过滤表里
- `heap_caps_register_failed_alloc_callback()` 能打印失败的分配（`ALLOC FAIL <size> caps=<caps> from <func>`），排查 NO_MEM 很有用
- 本芯片是 FreeRTOS **SMP** 移植：`taskENTER_CRITICAL()` 必须带 `portMUX_TYPE`，否则编译报 `too few arguments to function 'vPortEnterCriticalMultiCore'`

---

## 八、构建、刷机与工具

```bash
cd esp-mosaico-vibe
. ../env.sh
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
        --project projects/gamepad build

P="--project projects/gamepad"
python3 mosaico.py iris test enter-recovery $P     # 先进入 Vibe Mode（console 通道）
python3 mosaico.py iris system-update $P           # 走 recovery 写入
python3 mosaico.py iris logs --snapshot $P         # 拉日志
python3 mosaico.py iris screenshot out.png $P      # 截图（不含 overlay/黑线类面板问题）
python3 mosaico.py iris rpc 0x4700 4 --payload '{"layout":"setting"}' $P  # 远程切页
python3 mosaico.py iris memory $P                  # 本机型返回 not_supported
```

- 改 `sdkconfig*.defaults` 后必须 `rm -f build/sdkconfig` 再构建，否则不生效
- 每次刷机都会重启设备、断开蓝牙；测通话前必须让手机重连（否则走手机本机音频，日志表现为 `paired=0 hfp=-1`）
- RPC 服务 `0x4700`：1 list / 2 enable / 3 disable / 4 layout / 5 reboot / 6 set / 7 clear_bonds / 8 restart BLE adv / 9 reset

---

## 九、本地修改（不在本仓库 git 内，重装依赖会丢）

**ESP-IDF 现在零改动**（`git -C $IDF_PATH status` 应为空）。曾经改过、现在都已移出 IDF：

| 曾经的 IDF 改动 | 现在怎么做 |
| --- | --- |
| `bt/host/bluedroid/bta/dm/bta_dm_cfg.c`：HS/HD 电源表禁用 sniff | 项目侧调 IDF **导出的内部符号** `bta_dm_pm_active()`，见 §4.3（`app_btclassic_audio.c`） |
| `esp_hid/src/bt_hidd.c`：连接时 `esp_bt_gap_set_qos()` | 公开 API 挪到 `app_btclassic_hid.c`（HID 连接事件里对已配对设备设置） |
| `esp_hid/src/ble_hidd.c`：3 条调试日志 | 删除，需要时用 `esp_log_level_set` |

仍然在 IDF 之外的改动（同一台机器上的依赖，重装会丢）：

| 文件 | 修改 |
| --- | --- |
| `submodule/esp-mosaico-bsp/.../onboard/display.c` | `psram_dma_direct = true` |
| `submodule/.../ESP-Iris/.../esp_iris_transport_usb.c` | USB 热插拔看门狗 `usb_hotplug_watchdog()` |
| `esp_hid`/`esp_lcd` 等受管组件 | 不要手改，升级会被覆盖 |

注意：`bta_dm_pm_active()` 没有公开头文件，靠的是 IDF 内部符号。本项目锁定 IDF SHA，暂时安全；
升级 IDF 后若链接报 undefined reference，就回到"改 `bta_dm_cfg.c` 电源表"的方式（git log 里有那次提交）。


---

## 十、已知未解决问题

1. **经典 HID（蓝牙鼠标）卡顿**：根因同 §4.2，已改成公共踢点 + `app_bt_core` 的 1 秒定时器。
   待办：定时器的启动条件仍是桩（Classic 启用即运行），需收窄为「经典已连接 且 HID 有类在用或通话中」；
   并在真机上确认 `kick=` 增长、`sniff` 为 0、手感与通话中一致
2. **通话上行取走率 66–75%**：不是音质指标（§5.3）。现在 `mic rate` 是 100 次/秒、16000 B/s，
   `amp` 正常，听感正常；`pull_out/pull_req` 的差额来自 AG 的 120 字节 × 5–7.5 ms 请求节奏
3. **`SCO xmit Q overflow`**：通话中控制器同步 TX 队列持续溢出（改前 120 秒 720 次），部分丢包
4. **麦克风双消费者**：USB 采集与 SCO 在直连模式下抢同一 I2S 流（当前 USB 未开，未暴露）
5. **BLE 事件不派发**：`ESP_HIDD_*` 事件有时收不到，只能用轮询 `esp_hidd_dev_connected()` 兜底
6. **显示长跑**：TE 合成路径在长时间运行后仍可能 `NO_MEM`（已用 `psram_dma_direct` 大幅缓解，未根除）
7. **USB 运动接口在 Windows 上**：结构已合法（Report ID 在顶层集合内），但没有导出 `Unit` /
   Feature 属性（Reporting State `0x0316`、Report Interval `0x030E`），Windows 传感器类驱动可能
   拒绝绑定；Android/WebHID 侧按 report ID 4 读取，18 字节载荷
8. **`app_usb_hid.c` 里的 `s_usb.report_map[512]`** 只写不读，属于死代码，可以删除
