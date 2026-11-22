# todo.md — 待办与候选功能

放「还没做、但已经想清楚方向」的事情。已经踩过的坑和排障细节在
[`gotcha.md`](gotcha.md)，接口语义在各 `app_*.md`。

---

## 1. LE Audio（候选，2026-11-22 记录）

**现状：硬件和 IDF 都支持，本项目没启用。**

| 层面 | 状态 |
| --- | --- |
| 芯片 | `soc_caps.h`: `SOC_BLE_ISO_SUPPORTED = 1`、`SOC_BLE_AUDIO_SUPPORTED = 1` |
| IDF | `components/bt/esp_ble_iso`（ISO 通道）+ `components/bt/esp_ble_audio`（Zephyr 移植：AICS/ASCS/BAP/PACS/CAP） |
| 本项目 | 未开任何 `BT_BAP_*` / `BT_ASCS_*` / `BT_ISO` 配置，无 ISO/BAP 初始化代码 |
| 主机看来 | 只是 BLE HID + 经典音频，不是 LE Audio 设备 |

**要开工的话，顺序建议：**

1. 可行性验证：只开 Kconfig（`CONFIG_BT_BAP_UNICAST_SERVER` + ASCS + PACS，`BT_AUDIO_RX/TX` 会被自动 select），
   加最小 BAP sink 骨架，先确认手机能发现设备并成功建立 CIS —— 不接音频
2. 接音频：ASE 状态机（codec config / QoS / enable / start / stop / release）+ `esp_ble_iso` 收发，
   PCM 接到现有 `app_audio_codec` 的 playback / mic 路径
3. **LC3 编解码**：IDF 只给框架，实现要额外引入（组件库或移植）——这是最大不确定项，先确认方案再排期
4. 若要广播（Auracast）再考虑 `CONFIG_BT_BAP_BROADCAST_SINK`

**主机侧要确认：** Android 13+ 且厂商开启 LE Audio（Pixel 8、部分三星/小米）；
iPhone 只对 AirPods 这类自家配件开放 LE Audio，**通用 LE Audio 设备在 iOS 上还不行**。

**顺带说明：** LE Audio 是「两台 Mosaico 互相通话」（见第 3 条）最干净的方案——BAP unicast
的 sink + source 可以同时存在，LC3 延迟约 20–50 ms。

---

## 2. 双角色（dual role）：既能当从，也能当主

**现状：栈支持，应用层只写了从/外设一侧。**

| 传输 | 现在的角色 | 栈里同样具备的另一半 |
| --- | --- | --- |
| 经典 A2DP | sink（收音频） | `esp_a2d_source_init()` 发送 |
| 经典 HFP | HF client（免提端） | `esp_hf_ag_api.h`（AG/车载端） |
| 经典 HID | HID device | HID host（需 `CONFIG_BT_HID_HOST_ENABLED=y`，当前 off） |
| 经典 ACL | 永远 slave（我们从不主动连出） | 发起连接即成为 master；也支持 scatternet |
| BLE | peripheral（HOGP） | central / observer / broadcaster（`esp_ble_gap_start_discovery` + GATTC 都没用） |

**关键约束：**

- `CONFIG_BT_ACL_CONNECTIONS=2`（经典 + BLE 合计上限）。要「同时：一台对讲设备 + 一个手柄输入 + 手机音频」时需要提到 3–4
- BLE 只开了 `CONFIG_BT_BLE_42_FEATURES_SUPPORTED`，BLE 5 特性（扩展广播/周期广播/ISO）全关；LE Audio 依赖的 `BT_ISO` 挂在 `BT_BLE_FEAT_ISO_EN` 上
- 主动连出需要自己处理：发现 → 连接 → profile 打开 → 重连策略；现在是永远被动等主机连

**能解锁的场景：**

1. 两台 Mosaico 对讲（见下一节，需要 A2DP source 或 HFP AG）
2. 把 Mosaico 当主机接真实蓝牙手柄/键鼠（HID host）
3. 一台 Mosaico 主动连另一台做数据传输 / 联动

---

## 3. 两台 Mosaico 互相连接做对讲（候选）

**现状：不行。** 本项目在经典蓝牙上是 **A2DP sink**（收音频）+ **HFP HF**（被手机呼叫）+ HID device，
BLE 上只有 HOGP；两台设备都是「被推音频的一方」，没有 A2DP source / HFP AG 角色，也没有角色协商入口。

**三条可选路线：**

| 路线 | 做法 | 代价 |
| --- | --- | --- |
| A. HFP AG + HF 双向 | 一侧跑 `esp_hf_ag`（模拟车载主机），另一侧仍是 HF；SCO 双向 8 kHz CVSD | 语音质量差但延迟低、改动最小；需要 AT 命令状态机 + 模拟来电 |
| B. A2DP source + sink 双向 | 两台同时提供 source 与 sink 记录，两个方向各一条 SBC 流 | 延迟 150–250 ms，像对讲机；带宽/回声压力大 |
| C. LE Audio unicast 双向 | 见第 1 条，BAP sink + source，LC3 | 依赖 LE Audio 落地；效果最好 |

**不管走哪条都要先解决：**

- **回声/啸叫**：同机扬声器+麦克风，必须半双工（按住说话）或加 AEC
- **角色与配对**：谁连谁、谁做 AG/source，需要 UI（设置页或 sensor 页以外的入口）
- **音频路径复用**：`app_audio_codec` 目前是「USB / 经典蓝牙」两个源，要再加一个「对讲」源并处理抢占

---

## 4. 其它已知项

细节和验证方法都在 [`gotcha.md` §十](gotcha.md)：

- 经典蓝牙鼠标卡顿：kick 定时器的启动条件仍是桩（Classic 启用即运行），需收窄为「经典已连接 且 HID 在用时」
- 通话上行取走率 66–75%：不是音质指标，听感正常
- USB 运动接口在 Windows 上缺少 `Unit` / Feature 属性，Windows 传感器驱动可能不绑定
- `app_usb_hid.c` 里的 `s_usb.report_map[512]` 是死代码，可删
