[English](README.md) | 简体中文

# UniXNU-piano

在小米平板 8 Pro（`piano`，骁龙 8 至尊版 SM8750）上原生运行 macOS 27。本仓库是 [UniXNU](https://github.com/UniXNU/UniXNU) 的设备仓库：
https://github.com/lenzhang/UniXNU-xiaomipad8pro（原名 lenzhang/UniXNU-piano，本地检出仍沿用 UniXNU-piano 这个名字）。
27.3 线的 k273 XNU（k27 源码树加上 `patches/k27` 0053-0076 和 `patches/k273`）作为 HLOS 运行在 EL1，Gunyah 仍在 EL2。
启动链：ABL → BootShim → Mu-Silicium UEFI（FD，即 piano-uefi 的 `pianoPkg` 加上本仓库的改动）→ UniXNU 载荷与菜单 → DarwinBoot → XNU。
UniXNU 核心、Mu-Silicium、piano-uefi、DarwinBoot 和 SoC 模块都是固定版本的依赖（`unixnu.lock`、`feeds.toml`），不是分叉。

相关公开仓库：[UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) 是今后共享的 SM8750 部分的占位仓库
（代码目前仍在本仓库，构建固定使用另一个 SM8750 模块）；[veil](https://github.com/lenzhang/veil) 是早期 Linux + Gunyah 客户机路线的冻结存档。
仓库、本地目录和规则的总览见 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)。

## 当前状态（2026-11-27）

原生 macOS **27.3（26C5035r）** 既能开机从磁盘启动，也能以 RAM 镜像启动。当前日常安装版本是 **v13.1y12u**（2026-11-27，基于 `v13.1y12q` 仅更新 ESP），默认开启 **120 Hz** 日常刷新率（144 Hz 保留手动启用）、**DPU M2** WC 缓存清理跳过（`piano-dpu-clean-skip=1`）、**夜览** DSPP PCC 路由、**磁盘 M5 写回**（`data-store-write-back=1`）加 M4 Data 并发模式，以及 USB 调试网络发送流控。原生面板休眠（`piano-dpu-panel=0`）和动态刷新率切换保持关闭。
系统识别为 Mac17,11，显示为 "Xiaomi Pad 8 Pro"，芯片为 "Snapdragon 8 Elite"。根卷在 `linux_rootfs`，机主的 Data 存储在 `linux_data`。
日常能耗模式为“高功率”。与 Android 对齐的 CPU/GPU 频率曲线和 PD 降压充电（9 V）默认开启，电荷泵保持关闭。

已安装系统上目前可用的功能：从磁盘启动并保留机主数据、**120 Hz** 显示（含夜览与 DPU M2 清理跳过）、原生触摸屏、键盘保护套及其触控板、Wi-Fi（开机自动连接、开关、断线后自动重连）、扬声器和麦克风（含主动全双工）、电池电量百分比、5 V 和 9 V PD 充电及 80 % 充电上限、自动亮度、Adreno 830 上的 Metal 及分页 GPU 内存、4 GiB GPU 窗口（4032 像素照片上的 Core Image）、Photo Booth 与摄像头录像、QuickTime 和 Safari 中的硬件视频解码（H.264 最高 1080p）、Safari 的 WebGL 2 和 WebGPU，开启 GPU 沙盒的 Chrome GPU 加速（WebGL、GPU 合成、流畅滚动和视频），以及自 v13.1y12m 起无需启动参数的 Chrome WebGPU（内核补丁 0075，47 位用户地址空间）。自 v13.1y12m 起，日常版本还包括保护套和电源键睡眠策略、显示驱动的原生截图和低功耗蓝牙事件掩码修复。RAM 资格测试模拟了接着充电器时合上保护套（只锁屏）和按电源键（睡眠和唤醒）。物理按键和用电池时合上保护套仍待现场检查。已安装系统使用 13 GiB 内存窗口。蓝牙键盘和鼠标配对、电荷泵快充、USB 主机设备和外接显示器仍待解决。自动旋转以及安装指南在其他机器上的验证仍在推进。

11 月 27 日，四项日常新特性在合并易失 Data 的 RAM 资格测试 **`v131y12u-daily1`** 中全部通过（168 项通过、0 代码失败，含 `video-v6 --qualify` 13/13）；随后完成新鲜的机主 Data 存储备份与 SHA-256 回读校验，并执行仅写 ESP 的日常安装 **`v13.1y12u`**（`inst-v131y12u-20261127-094439`，75 项通过 / 0 项失败；装后验证免参数 Chrome WebGPU 6/6 与 Safari WebGPU 52.15 fps，且磁盘重启恢复正常）。同日早些时候，已安装 `v13.1y12q` 通过 10 分钟 Metal 3D 持续负载（967–1050 MHz 下 59.98 fps）、蓝牙预检（32/32）和零 TCC 绕过的已安装麦克风+主动全双工（`inst-y12q-obs6`）；RAM 测试通过 480p 与 1080p 的 Iris H.264 硬件编码自检（`v131y12u-e1fix1`、`v131y12u-e1-1080p`），并证实 M5 唤醒黑屏中 `CTRL=0x1f7` 仅为次因（`v131y12u-m5ab1`，M5 维持 **HOLD**）。主机侧 27.3 `powerd` 与内核 0076 分析结合实机 `ioreg` 读数定位关闭了 BH-2 根因（`ArmDTPlatform::excludeList()` 导致 `IODeviceTree:/options` 被 `IOPlatformDevice` 遮蔽）。
[11 月 27 日测试结果](docs/results-2026-11-27.md) · [BH-2 分析](docs/drivers/power-bh2-powerd-analysis.md)

内核补丁 0076 让 USB 调试 shell 等已更新的辅助程序读取完整启动参数，包括旧接口在 1024 字节处截断的部分。
CI 现已使用固定版本的公开套件运行 Adreno830 GPU 主机测试。
[下午测试结果](docs/results-2026-11-26.md#afternoon-storage-power-cut-tests-120-hz-ci)

### 硬件支持

所有状态都来自平板上的实际运行。“可用（已安装）”指在安装到平板磁盘的系统上验证过（版本见该行）；
“仅 RAM”指只在 RAM 启动镜像上验证过。

✅ 完整支持 · 🟡 部分支持 / 进行中 · ❌ 尚不支持

| 功能 | 状态 | 验证（版本，日期） | 说明 |
| --- | --- | --- | --- |
| 从磁盘启动、持久安装 | ✅ 可用（已安装） | v13.1y12u，2026-11-27 | 已安装 **v13.1y12u**（`inst-v131y12u-20261127-094439`，75 项通过 / 0 项失败）在完成新鲜的 SHA-256 回读校验机主 Data 备份后仅写入变更的 ESP 文件，写后回读校验通过，保持 `v13.1y12q` 用户态层不变，并顺利通过磁盘重启且机主 Data 恢复挂载。此前 `v13.1y12q` 磁盘启动已确认 +32 秒登录及 `cs-native4k-skip=1`（跳过 896 个文件共 7496 MiB）。[已安装 v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) |
| 显示 | ✅ 可用（已安装），120 Hz | 已安装 v13.1y12u，2026-11-27；RAM v131y12u-daily1 与 m5ab1，2026-11-27 | 日常 **v13.1y12u** 运行固定 **120 Hz** 双链路扫描（`120.016 Hz`）、**DPU M2** WC 清理跳过（`piano-dpu-clean-skip=1`，6,177 次翻页仅 3 次初始化清理、0 欠载、最大锁存 `8.375 ms`）、**夜览** DSPP0/1 PCC 路由开启（`native-gop-display-night-shift=1`）及原生截图。合并 RAM `v131y12u-daily1` 在 120 Hz 下同时验证了屏幕解锁夜览 `off→warm→off`（0 失败、关闭恢复 `1.000/0.999/0.999`）与 100% M2 跳过（C920 光学检查 `transient_frames=0`）。原生面板休眠保持关闭（`piano-dpu-panel=0`，`v131y12u-m5ab1` 消除 `CMD_MODE_EN` 争用后唤醒光学仍黑屏 `YAVG 21.5`，M5 维持 **KEEP HOLD**）。[已安装 v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) · [合并 RAM 资格测试](docs/results-2026-11-27.md#v131y12u-daily1-2026-11-27) · [M5 唤醒结果](docs/results-2026-11-27.md#v131y12u-m5ab1-2026-11-27) |
| 120 Hz / 144 Hz | 🟡 可用（已安装）120 Hz；144 Hz 手动启用 | 已安装 v13.1y12u，2026-11-27；RAM m120c-144，2026-11-26 | **120 Hz** 已成为已安装 **v13.1y12u** 的日常默认刷新率（`piano-dpu-modes=120`，双链路实测 `120.016 Hz`、`IOMFBDisplayRefresh` 全部 6 项字段精确匹配、0 欠载、重启保持，`OWNER-0748`）。**144 Hz** 保留手动启用（`m120c-144` RAM 实测 `144.146 Hz`、0 欠载）。高刷应用呈现节奏（`v131y12t-m120d` 上 `pipe120` 511/511 回退、50.71 次回调/秒、`presentedTime` 为零）维持 **HOLD** 待后续分析。[已安装 v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) · [120 Hz RAM 探测](docs/results-2026-11-27.md#120-hz-pipe--presented-time-probe-ram-v131y12t-m120d) |
| 触摸屏（原生） | ✅ 可用（已安装） | v13.1y12l，2026-11-26；手势在 RAM touch-native3，2026-11-23 | 绑定到显示器的原生内置触摸屏：点按、长按（右键）、在滚动视图中拖动与滚动、双指缩放、旋转、智能缩放、防误触、90 度坐标（RAM 上 44 项通过 / 0 项失败）。已安装系统：开机即发布并被系统接管。原生路径的长期手动使用仍待验证。 |
| 键盘保护套与触控板 | ✅ 可用（已安装） | 机主实测，v12，2026-11-19；v13.1y12l，2026-11-26 | 打字和触控板可用；登录窗口能识别保护套键盘。 |
| Wi-Fi | 🟡 可用（已安装），部分 | v13.1y12l，2026-11-26；v13.1x，2026-11-21 | 开机自动连接（WPA2、DHCP、DNS）；开关关闭再打开后重新连接；强制断线后几秒内重连；国家码已设置。待验证：手动点击网络列表中的某一行加入、热点消失又恢复后的自动重连、多接入点的办公室网络。 |
| 蓝牙 | ❌ 尚未支持（进行中） | v13.1y12q，2026-11-27；机主在已安装的 v13.1 版本上测试，2026-11-20 | 已安装系统的预检报告 BT_PREFLIGHT READY（等待 48 秒后 32/32 项通过），控制器已发布并上电，低功耗蓝牙事件掩码第 6 和 11 位已设置。机主此前测试时能发现设备，但低功耗蓝牙连接超时（HCI 0x08）。键盘和鼠标配对仍待机主进行 20 分钟实机测试。与 Wi-Fi 的射频共存正在分析。蓝牙音频尚待实现。 |
| 扬声器 | ✅ 可用（已安装） | v13.1u，2026-11-21（有声测试音）；v13.1y12q，2026-11-27（`inst-y12q-obs6` 实机全双工）；RAM v131y12u-ram2，2026-11-27 | 播放、音量和静音可用。扬声器保护（增益 243）和原厂杜比 “Movie” 音效在 DSP 上运行。已安装 `v13.1y12q` 的 `inst-y12q-obs6` 通过了录音期间并发 `afplay` 播放（`underruns=0`、`failed=0`、`fallbacks=0`，音量和静音恢复一致）。手动启用的静音探测 `spk-probe1` 在连续两次 RAM 资格测试中均通过（四路功放 `ok`，25.0 °C，测完关闭）。[已安装全双工结果](docs/results-2026-11-27.md#installed-microphone-and-active-duplex-inst-y12q-obs6) · [静音探测结果](docs/results-2026-11-27.md#second-silent-speaker-and-emission-free-ir-passes) |
| 麦克风 | ✅ 可用（已安装） | 已安装 v13.1y12q（`inst-y12q-obs6`），2026-11-27；RAM y12t-obs1，2026-11-27；降噪在 RAM mic-ns4b，2026-11-23 | 已安装系统提供原始 “Mic”（默认输入）和 “Mic (noise reduced)”（RNNoise）。在已安装 `v13.1y12q`（`inst-y12q-obs6`，零 TCC 绕过）上，直接采集通过安静底噪（`-44.45 dBFS`）、硬件静音全零（`3.13 s` 内 `0` 个非零采样）及扬声器+麦克风并发全双工（`994.93 Hz`、`28.16 dB SNR`、`-4.26 dBFS`、`0` 削波、前后对比度 `63 dB`、两次冷释放干净；仅在 `duplex-hold=0` 冷启扬声器时出现一次 `37 ms` FIFO 间隔）。RAM `y12t-obs1` 也通过了 −36 dBFS AVFoundation 录音和 C920 声学回放；QuickTime/语音备忘录界面权限弹窗留作可选现场检查。[已安装麦克风与全双工](docs/results-2026-11-27.md#installed-microphone-and-active-duplex-inst-y12q-obs6) · [麦克风 RAM 结果](docs/results-2026-11-27.md#calibrated-microphone-and-c920-playback) |
| 摄像头：Photo Booth | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 前置摄像头实时预览，无授权弹窗；特效网格可以打开。 |
| 摄像头：QuickTime | 🟡 部分可用 | v13.1y12l，2026-11-26 | 前置和后置摄像头都能录 10 秒 H.264 视频（AVFoundation，QuickTime 高质量预设）。手动“新建影片录制”并保存回放仍待验证。编码为软件编码。 |
| 摄像头：FaceTime | 🟡 尚未验证 | 权限已预先授予，v13.1y12l，2026-11-26 | 两个摄像头都已列出，FaceTime 已有摄像头和麦克风权限；尚未检查 FaceTime 自拍预览。 |
| 充电：5 V | ✅ 可用（已安装） | v13.1y12u，2026-11-27；v13.1y12d，2026-11-24 | 3 A 电源下最高 3 A（`ICLReadback=3000 mA`、`AppliedFCC=1500 mA`）。在 Mac mini 5 V USB-C 接口上，硬件 AICL 稳在 `1650 mA`（`IBUS=1644 mA @ 4.72 V`，输入 `7.77 W`），唤醒状态与息屏+低功耗模式下均可保持 **`+2.0–2.7 W`**（`+514 至 +701 mA`）净正向充入电池。[充电实测](docs/results-2026-11-27.md#live-5-v-usb-c-charging-and-low-power-check) |
| 充电：PD 9 V（降压） | ✅ 可用（已安装） | v13.1y12d，2026-11-24 | 默认开启。原装充电器上的固定 9 V PD：9 V / 1.6 A（约 14 W，即充电器固件请求的电流，与 Ubuntu 相同）。PPS 9 V（最高 2.5 A）已构建但未运行。 |
| 充电：80 % 上限 | ✅ 可用 | RAM chg-buck6，2026-11-17；此后每个已安装配置都包含 | 充到 80 % 停止，降到 75 % 恢复。 |
| 充电：电荷泵 / MiPPS | ❌ 尚未支持 | 首次有人值守尝试，2026-11-24 | 默认关闭。首次尝试时输入限流停在 100 mA（一个未映射的厂商电源对象）；已在版本中修复，下一次有人值守测试待进行。 |
| 电池电量百分比 | ✅ 可用（已安装） | 已安装 v13.1y12u 与 RAM v131y12u-daily1，2026-11-27 | 已安装系统显示电池电量百分比和电源状态。`m120c-120` 电池注册表报告 122 次循环、9200 mAh 设计容量和 8781 mAh 满充容量，系统信息显示“建议维修”且未显示最大容量。主机侧 27.3 `powerd` 与内核 0076 反汇编结合实机 `ioreg`（`OWNER-0748`）已定位根因：`ArmDTPlatform::excludeList()` 返回 `"()"`，使 `IODTPlatformExpert::processTopLevel()` 在 `IODeviceTree:/options` 创建了遮蔽真实 `IODTNVRAM` 的 `IOPlatformDevice` 节点（`kIOReturnUnsupported` → `0xe00002ca`）；BH-2 保持默认关闭，待独立内核构建与 RAM 资格测试。[BH-2 分析](docs/drivers/power-bh2-powerd-analysis.md) |
| 睡眠与唤醒 | 🟡 部分可用 | v13.1y12m，2026-11-26（已安装策略及 RAM 测试）；RAM v131y12t-qual1，2026-11-27 | v13.1y12m 的测试记录了唤醒后 Wi-Fi 和 USB 链路恢复并返回锁屏界面。已安装版本启用了保护套和电源键策略。最新 RAM 资格测试通过了挂起到空闲并定时唤醒，以及模拟电源键和保护套开合策略。此前 RAM 测试覆盖了接着充电器时合上保护套（锁屏但不挂起）和模拟按电源键睡眠及唤醒。物理电源键和用电池时合上保护套仍待现场检查。 |
| 亮度：手动 | 🟡 部分可用 | 按键在已安装 v12，2026-11-19；滑块路径在 v13.1i | 亮度键可用。通过系统亮度控制的滑块路径可用；在控制中心手动拖动仍待验证。 |
| 亮度：自动 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 光线传感器驱动自动亮度；手动调整不会退出自动模式。真实光线变化范围的测试仍待进行。 |
| GPU：Metal（Adreno 830） | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 我们的 Metal 插件驱动 WindowServer、桌面和应用；ImageIO、基于 Metal 的 Core Image、MetalKit 纹理加载和快速查看缩略图全部通过：30 条评分路径 30 条通过，包括此前内存不足的 4032 像素 Core Image 和 HEIC 纹理路径；预览能打开 4032 像素的照片。 |
| GPU：分页内存 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 4 KiB 分页 GPU 内存（上限 7168 MiB，无固定池），无拒绝、无缺页错误；默认开启重放式挂起恢复。 |
| GPU：Android 级频率曲线 | ✅ 可用（已安装） | v13.1y12q，2026-11-27；v13.1y12l，2026-11-26 | 采用 Android 的 14 级 GPU 频率表，最高 1100 MHz，带表面温度和热控限频及 KGSL 式调频器，并采用 Android 的 CPU 频率表。已安装系统上的 10 分钟 Metal 3D 测试平均 59.98 fps，GPU 持续运行在 967–1050 MHz，GPU 故障 0 次。帧率按 Metal 完成事件计算（`presented=0`）。[Metal 3D 持续测试结果](docs/results-2026-11-27.md#sustained-metal-3d-installed-v131y12q) |
| GPU：4 GiB 窗口 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 256 GiB 地址空间中 64 个 4 GiB 客户端窗口，单个对象 1 GiB，没有被拒绝的 GPU 打开请求。修复了 4032 像素 Core Image 内存不足的路径（Metal 图像检查 30/30）。 |
| Safari：WebGL 2 和 WebGPU | ✅ 可用（已安装） | v13.1y12o，2026-11-26 | WebGL 2 和 WebGPU 复测通过。v13.1y12m 上的 WebGPU 适配器为 “adreno”，测试页面约 52 fps，回读正确。 |
| Safari：硬件视频解码 | ✅ 可用（已安装） | v13.1y12o，2026-11-26；v13.1y12i，2026-11-25 | 1920x1088 以内的 H.264 在 Iris 视频核心上解码。1080p 复测仍为零丢帧。 |
| Safari：HEVC 和 4K | ✅ 可用（已安装），软件解码 | v13.1y12j，2026-11-25；HEVC 在 v13.1y12o 上复测 | v13.1y12j 上的 HEVC 2160p 解码 600 帧、丢 0 帧（29.9 fps），H.264 2160p 解码 591 帧、丢 5 帧，均由 Apple 的软件解码器完成。不支持 VP9 和 AV1。 |
| Chrome：GPU 加速 | ✅ 可用（已安装） | v13.1y12q，2026-11-27；v13.1y12o，2026-11-26；此前在 v13.1y12m 和 v13.1y12l 测试 | 无需启动参数，WebGL 2 经 ANGLE Metal（Adreno 830，`GraphiteDawnMetal`）在 3200 × 1738 下实测 37.89 fps。Chrome 播放 1080p 和 4K 的 H.264 与 HEVC、1080p 的 VP9 与 AV1，丢帧 0–1。HEVC 经 VideoToolbox 播放。本次运行未记录到崩溃、PianoA830 故障或沙盒拒绝。此前在开启 GPU 沙盒时验证了 GPU 合成、2D canvas 56 fps、滚动 60 fps 且没有超过 34 ms 的帧。RAM 启动下 Chrome 可能耗尽基于内存的 Data 盘。 |
| Chrome：WebGPU | ✅ 可用（已安装） | v13.1y12u，2026-11-27；v13.1y12q，2026-11-27；v13.1y12o，2026-11-26 | 已在安装的 **v13.1y12u**（`post-y12u-20261127-100037`）上复验通过：未使用 GPU、WebGPU 或沙盒启动参数，`qualcomm` / `adreno-8xx` 适配器（`fallback_adapter = false`）上 6/6 个上下文创建成功，计算结果正确，离屏和 canvas 的 `bgra8unorm` 像素回读完全一致，WebGL `37.4 fps`，上下文/环形/传输错误均为零，内核补丁 0075 提供 47 位用户地址空间，17/17 个 Chrome 进程处于安全 V8 沙盒模式（bucket 0，预留 1152 GB）。 |
| 视频播放（QuickTime） | ✅ 可用（已安装） | 已安装 v13.1u/v13.1x，2026-11-21；已安装 v13.1y12u 与 RAM v131y12u-daily1，2026-11-27 | AVC High 和 HEVC Main10 硬件解码；各播放约 100 秒，摄像头记录中没有闪烁或黑帧。VideoToolbox 待处理帧取消修复（`a426ce70`）在连续 RAM 运行（含 `v131y12u-daily1`）中通过 `video-v6 --qualify` 13/13（`V6_TOOL_EXIT=0`、4/4 终止 `Demt`、`encs=0`）后已随已安装 **v13.1y12u** 生效。[合并 RAM 资格测试](docs/results-2026-11-27.md#v131y12u-daily1-2026-11-27) |
| 能耗模式 | 🟡 部分可用 | v13.1y12l，2026-11-26 | 用 `pmset` 设置的低功耗、自动和高功率（日常设置）都能传到电源和热控策略。系统设置中的选择器尚未手动验证。 |
| 登录窗口空闲 CPU | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 已修复：一个音频守护进程在登录窗口反复循环（约占 1.3 个核心）；现在它会被暂停到登录之后。 |
| 应用启动 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 计算器、文本编辑和备忘录的第一个窗口都在 2.5 秒内打开。 |
| USB 调试（控制台、root shell、ssh） | ✅ 可用（已安装） | 已安装 v13.1y12q 和 RAM v131y12t-qual1，2026-11-27；已安装 v13.1y12q，2026-11-26 | USB-C 调试控制台、root shell 和有线 SSH 可用。补丁 0076 让 shell 读到旧接口在 1024 字节处截断的启动参数。发送流控自 v13.1y12q 起默认开启，11 月 27 日的磁盘启动检查确认已启用。同日的 RAM 矩阵测试中，66/66 次传输数据完全一致，驱动零丢包。 |
| 系统切换（Android / macOS） | 🟡 仅 RAM，只读 | RAM v13.1y12e，2026-11-24 | 启动标志的 sysctl 能读取标志；写入按设计被拒绝。尚未做过完整切换。 |
| USB 主机 / OTG | ❌ 尚未支持 | RAM usb-a2d，2026-11-18（空端口） | 主机模式能在空端口上启动；VBUS 供电和真实设备尚未完成。 |
| 外接显示器（USB-C DP） | ❌ 尚未支持 | 无 | 仅有设计。 |
| 自动旋转及其他传感器 | ❌ 尚未支持 | RAM v131y12t-qual1，2026-11-27 | 光线传感器可用。RAM 测试通过了加速度计数据流和 `piano-rotated` 读取 1024 字节之后的启动参数。日常配置仍关闭自动旋转，相关工作仍在分析。 |
| 硬件视频编码（Iris VPU 3.5） | 🟡 E1 自检通过（480p 与 1080p，仅 RAM） | 已安装 `v13.1y12q`；RAM `v131y12u-e1fix1` 与 `v131y12u-e1-1080p`，2026-11-27 | 已安装版录像使用软件编码，日常 `v13.1y12q` 默认关闭（`piano-video-enc=0`、`encs=0`）。手动启用的 RAM 内核自检已通过 **640×480@30**（`v131y12u-e1fix1`，105/0，`22,439 B`、`314 ms`、平均 PSNR-Y `51.6 dB`）和 **1920×1080@30**（`v131y12u-e1-1080p`，113/0，14 项 `56,180,736 B`、`88,882 B`、`358 ms`、平均 PSNR-Y `44.3 dB`）H.264 硬件编码，两次均在同启复用常驻 Iris 核心通过 13/13 阶段 AVC 解码。VideoToolbox 编码器注册（E2+）维持 **HOLD**。[480p E1](docs/results-2026-11-27.md#v131y12u-e1fix1-2026-11-27) · [1080p E1](docs/results-2026-11-27.md#v131y12u-e1-1080p-2026-11-27) |
| 红外发射、手写笔、手电筒 | 🟡 红外仅 RAM 验证；手电筒主机验证（待现场点亮） | RAM `v131y12u-ram2`（红外）；RAM T2 `flip4`/`hands1`（手电筒 20/50 mA）；400 mA 默认点亮与阶梯主机测试，2026-11-27 | SE15 红外连续两次 RAM 资格测试通过（0 次探查写入、381 µs 通过 64/64 字节 DMA 回环、GPIO29 无发射、0 次发送；实际发射待现场接收测试）。后置手电筒在 macOS 上此前仅试过 20 mA 和 50 mA（`flip4`/`hands1`，12/12 写入回读正常、LED 未亮）；机主于 2026-11-27 批准 400 mA 总电流（2×200 mA）原厂安全上限（`OWNER-0717`）并将 **每通道 200 mA（总计 400 mA）** 设为默认点亮电流（不仅限于阶梯顶档，`OWNER-0801`），保留全部温控保护、回读校验和默认关闭启动行为（已通过主机测试，点亮测试待机主在场）。手写笔仍未支持。[红外 RAM 结果](docs/results-2026-11-27.md#second-silent-speaker-and-emission-free-ir-passes) · [手电筒清单](docs/drivers/torch.md#attended-t4-ram-qualification-checklist) |
| 内存（16 GB） | 🟡 部分可用 | 已安装 v13.1y12l 和 RAM m120c-120/ram16g-hevc1，2026-11-26；RAM ram16g-hctl1/tier3w/tctl3，2026-11-27 | 已安装系统使用 13 GiB（13310 MiB，空闲 47% 可用，无交换或压力）。EFI 审计将 322 块 2 MiB 内存中的 319 块划为非运行时（释放 0 MiB，待机主核验）。684 MiB 混合 HEVC 区在 `ram16g-hevc1` 通过 24/24 帧 4K Main10 金样本；对照组 `ram16g-hctl1` 通过 1080p HEVC 上限与 −131 MiB 差值并排除了混合区与 AVC 取消超时的因果关系；`ram16g-tier3w` 验证了 **126 MiB** 私有 GPU `region1` 峰值驻留（1,512 MiB 拷贝、0 故障/拒绝、全部回收），配对关闭对照 `ram16g-tctl3` 通过 Metal 图像 30/30。日常保持 `piano-extra-ram=0`、`piano-gpu-tier=0`。[对照组](docs/results-2026-11-27.md#ram16g-hevc-control-ram-ram16g-hctl1) · [GPU 分层](docs/results-2026-11-27.md#ram16g-private-gpu-tier-ram-ram16g-tier3w) |
| 内置存储（UFS） | ✅ 可用（已安装） | 已安装 v13.1y12u，2026-11-27；临时测试区 A/B 与断电测试，2026-11-26–27 | 已安装 **v13.1y12u** 默认运行 **M4 并发模式**（`mode=concurrent depth=8`）加 **磁盘 M5 写回**（`data-store-write-back=1`，`OWNER-0748`，在完成新鲜的 SHA-256 回读校验机主 Data 备份后启用）：实机磁盘启动记录 `write_back=1`、`fua=0`、`cached=31,298`（均写约 `27.9 µs`）、`flushes=1,311`、`errors=0`，并干净通过重启（`cached=3,966`、`flushes=137`、`errors=0`）。此前 A/B 测得 4.9–5.6 倍随机读 IOPS（M4）、1.71–4.12 倍写吞吐且同步延迟 p50 降 42%–58%（M5-wb），断电测试 32/32 通过。`data-store-queue=0` 保持关闭（不适合日常）。[已安装 v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) · [临时测试区结果](docs/results-2026-11-27.md#disk-m5-scratch-window-ab) |
| 指纹、NPU | ❌ 暂缓 | 无 | 没有安全隔区（无法使用触控 ID）；此 SoC 上没有 Core ML 后端。 |

详细说明（状态定义、已知缺口、效果展示）：[docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)。
平板测试结果（英文）见 [11 月 27 日](docs/results-2026-11-27.md)和 [11 月 26 日](docs/results-2026-11-26.md)。

**效果展示：** 各项功能的截图和短片（在平板上直接截取）收录在 [docs/STATUS.zh-CN.md#效果展示](docs/STATUS.zh-CN.md#效果展示)
（第一批原生截图来自已安装的 v13.1y12m：关于本机、显示器、电池、访达、活动监视器、Metal 3D、Safari 视频）。

USB-C 调试控制台（`package/PianoDwc3`、`tooling/native/usb-console.py`）和通过数据线的 ssh 都可用；控制台监视器的
`reset fastboot` 命令无需按键即可让平板回到 ABL。日常开发使用 `fastboot boot` 启动 RAM 镜像，它优先于已安装的系统。

各驱动的说明、启动参数和测试级别：[docs/drivers/README.md](docs/drivers/README.md) 和 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)。
已安装系统的使用、更新和恢复：[docs/USING.md](docs/USING.md)。更早的公开记录包括
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) 和 [docs/results-2026-11-17.md](docs/results-2026-11-17.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `patches/k27-tree/` | 我们在 k27 源码树固定上游版本（macOS-DroidVM）之上的改动：构建脚本、KC 构建器、内核补丁 0050-0054 |
| `patches/k27/` | k27 内核补丁 0053-0076，基于 k27 源码树（xnu-12377 + 基线补丁 0001-0052） |
| `patches/k273/` | 27.3 线在 `patches/k27` 之上的内核补丁 |
| `patches/k27-kc/` | KC 构建器的原生布局（最佳适配、`__TEXT` 中无重定位常量），需手动应用到 k27 树（[docs/kc-space.md](docs/kc-space.md)） |
| `patches/xnu/` | UniXNU macOS 27.3 线的设备补丁（目前为空） |
| `package/` | 原生 kext；没有对应启动参数时都不生效 |
| `configs/` | 原生 kext 列表（`k27-native-kexts.txt`）、ADT 标识与显示节点（`k27-adt.toml`、`k27-gop-display.toml`）、27.3 线的内核配置 |
| `dts/piano.dts` | 用于 ADT 的 piano 硬件描述 |
| `.github/`、`scripts/` | CI（检查、每月依赖检查、构建、手动发布：[docs/ci.md](docs/ci.md)）与 `fetch-pins.py` |
| `tooling/` | `k273/build-k273-image.sh`（27.3 kext → 内核集合 → ADT → RAM 镜像，`K273_KERNEL`）、`k273/build-k273-esp.py`（已安装系统的 27.3 ESP）、`kc/`（KC 构建）、`adt/`、`bootimg/`（RAM 镜像及其校验）、`native/`（测试循环、USB 控制台、输入）、`persist/`（安装到磁盘）、`firmware/`（FD） |
| `firmware/` | 对 piano-uefi 的 `pianoPkg`、Mu-Silicium 和 UniXNU 的改动 |
| `docs/` | 移植笔记、驱动说明（`drivers/`）、计划和证据 |
| `skills/`、`tooling/repro/` | 面向其他设备的代理指南及工具：Apple 输入文件的固定版本、自行下载、根卷重建校验、主机检查 |
| `logs/`、`artifacts/ARTIFACTS.md` | 已提交的验证日志；构建产物和输入的 SHA-256 |
| `private/`、`out/` | 被忽略：密钥、单机校准数据；构建输出 |

## 构建

新设备的完整流程（要求、输入、构建、RAM 启动验证、受保护的安装、回滚、检查）见安装指南
[skills/install-piano-macos/SKILL.zh-CN.md](skills/install-piano-macos/SKILL.zh-CN.md)（中文版；英文原版
[SKILL.md](skills/install-piano-macos/SKILL.md) 面向编程代理）。本节是摘要，以该指南为准。

### 主机要求

- **必须是 Apple 芯片的 Mac（M1 或更新）。不支持 Intel Mac。** Xcode 27 的编译器和链接器以及 macOS 27 自带的工具只有 arm64 版本，
  而且有两个构建步骤会运行自己编译出的 arm64e 程序。证据见安装指南 §1.1。
- 已测试：macOS 27.2.1 加 **Xcode 27.2**（需要完整的 Xcode，仅有命令行工具不够：内核需要 iphoneos SDK 和 `Kernel.framework`，
  并用 Xcode 的 `ld-classic` 链接）。Homebrew 装在 `/opt/homebrew`，并安装安装指南 §1.2 列出的软件包、`fastboot`、Python 3.11 或更新版本；
  约 90 GiB 可用磁盘空间；根卷步骤需要在前台终端里使用 `sudo`。
- `bash tooling/repro/check-host.sh` 只读检查主机，每项输出 `ok` / `WARN` / `FAIL` / `GAP`；只有 `0 FAIL` 时才继续。
  请在原生 arm64 shell 中运行，不要在 Rosetta 下运行。

### 源码与固定版本

- 本仓库，加上 `unixnu.lock` / `feeds.toml` 中固定版本的依赖（UniXNU 核心、Mu-Silicium、piano-uefi、DarwinBoot、SoC 模块），
  以及 k27 内核源码树。将 `PIANO_K27_TREE` 设为 `unixnu.lock` 中固定版本的源码树。
- **拉取固定版本：** `python3 scripts/fetch-pins.py` 按提交把 `unixnu.lock` 里的每个依赖拉到 `$UNIXNU_RESEARCH`
  （`--verify` 只做校验；由 SorasakiHinaHusband 贡献）。k27 内核源码树是固定版本的上游
  [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) 加上我们的 [patches/k27-tree](patches/k27-tree/README.md)：
  `tooling/k27/fetch-k27-tree.sh DEST`，然后设 `PIANO_K27_TREE=DEST`。
- 本项目从不分发 Apple 的文件。`tooling/repro/fetch-ipsw.py` 从 Apple 的 CDN 下载固定版本 27.3 IPSW 的组成文件，从 Apple 获取 AEA 密钥，
  并按 `tooling/repro/pins.json` 校验每个文件。
- 有些输入目前仍只存在于原始环境（k27 内核源码树、基础信任包、Data 种子、Turnip 源码树、写入工具中的单机固定值）：
  指南的 "Known gaps" 一节逐项列出并给出可选方案。

### 构建步骤

```sh
bash tooling/repro/fetch-and-build.sh --dry-run   # 列出将要执行的每条命令；不下载、不构建、不写入
bash tooling/repro/fetch-and-build.sh             # host、inputs、systemos、root、kernel、fd、image（也可按名字只跑一步）
bash tooling/repro/fetch-and-build.sh --check     # 按固定值检查每一步的输出（不联网）
```

它执行的步骤，也可以逐个运行：

```sh
bash tooling/k273/prepare-k27-tree.sh          # 27.3 内核源码树及重新链接的内核；打印 K273_KERNEL 的路径
bash tooling/firmware/build-fd.sh               # 从 unixnu.lock 的检出构建 UEFI FD
K273_KERNEL=<上一步打印的路径> bash tooling/k273/build-k273-image.sh   # kext → 内核集合 → ADT → RAM 镜像
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

- **kext 与内核集合：** KC 只链接本仓库的 kext（`configs/k27-native-kexts.txt`）；`tooling/k27/check-native-kexts.py` 在链接前后都会强制检查。
  KC 日志中的通过标志：`native kext rule: ... all this repository's`。
- **根卷镜像在本地生成，从不分发。** 20.9 GB 的根卷由 IPSW 的 OS 卷（逐字节一致，版本固定）加上由 `tooling/k273/make-systemos.sh`
  和 `make-root-image.sh` 重建的 SystemOS 窗口组成，并由 `tooling/repro/verify-root.py` 按固定的文件级清单校验（指南 §3.1）。
- 只构建 27.3 线；已退役的 27.0.1 路线（`tooling/k27/build-native-image.sh`）不再使用。根卷位置：新设备先把根卷放在 `linux_data`
  （`K273_ROOT_PART=linux_data`，指南 §3.2）；参考设备的已安装 ESP 使用 `build-k273-esp.py --root-part linux_rootfs`
  （[docs/persistent-boot.md](docs/persistent-boot.md) §13.8）。

### CI 与发布

FD、k27 原生内核和我们的 kext 也可以在 GitHub 托管的 runner 上构建（[docs/ci.md](docs/ci.md)）：拉取请求会运行依赖拉取、
自测和隐私扫描；发布版本（在平板验证通过后手动打标签）只包含我们自己的二进制、构建配方、SHA-256 清单和安装 skill。
根卷镜像和内核集合始终在用户本地生成。

### RAM 镜像的大小上限

RAM 启动镜像**最多 250,000,000 字节**。ABL 会把整个镜像载入 UEFI 的 256 MiB 内核区域；一个 294,952,960 字节的镜像在下载后一直停在
fastboot，而 247,767,040 字节的镜像可以启动（[docs/k273-line.md](docs/k273-line.md) §4）。该上限是 `tooling/bootimg/build-ram-images.py`
中的 `IMAGE_MAX_BYTES`（镜像将要超限时它会压缩载荷的空闲空间），`tooling/bootimg/verify-ram-images.py` 会再检查一次
（两者都读取 `PIANO_IMAGE_MAX_BYTES`；不要调大）。

### 安装前先做 RAM 启动

每个镜像都要先用 `fastboot boot` 启动（RAM 启动不写任何分区），在那里通过后才能写入磁盘：控制台能走到 XNU，USB 控制台有响应，
启动能通过复位保护回到 ABL；桌面镜像还要能正确显示登录窗口，并且触摸、键盘保护套、ssh、Wi-Fi 和电池读数都正常（指南 §4）。
安装只能通过 `tooling/persist/maint-session.py` 进行，每一步都有备份、回读校验和回滚
（[docs/persistent-boot.md](docs/persistent-boot.md)、[docs/install-esp-v3.md](docs/install-esp-v3.md)）。

更多：启动参数与无屏调试见 [docs/bringup-debugging.md](docs/bringup-debugging.md)（§4 说明从 k27 树读取哪些内容）；
当前可用功能见 [docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)。

## 启动与测试

日常开发使用 RAM 启动：`fastboot boot` 一个 RAM 镜像（它优先于已安装的系统），由无人值守的循环启动测试、复位并读回测试日志
（`tooling/native/native-loop.sh`，[docs/bringup-debugging.md](docs/bringup-debugging.md) §3）。安装到磁盘
（[docs/persistent-boot.md](docs/persistent-boot.md)）只写该文档列出的区域，每个区域先做经过校验的备份，每次写入后回读校验，
每一步都有回滚；从不写固件分区。请勿手动刷写 boot 或固件分区。

## 原生 kext 规则

原生内核集合只链接本仓库的 kext（`configs/k27-native-kexts.txt`）。`tooling/k27/check-native-kexts.py` 在每次构建的链接前后都强制检查：
不允许来自别处的描述文件，也不允许虚拟机时代的半虚拟化 kext。

## 延伸阅读

- [docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)：逐项功能状态，以及每项验证时的版本和日期。
- [docs/USING.md](docs/USING.md)：已安装系统的使用、更新和恢复。
- [skills/install-piano-macos/SKILL.zh-CN.md](skills/install-piano-macos/SKILL.zh-CN.md)：在其他设备上构建和安装（安装指南，中文版）。
- 驱动设计、启动参数及资格测试记录。[驱动索引](docs/drivers/README.md)
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)：仓库、本地布局、硬性规则。
- [docs/bringup-debugging.md](docs/bringup-debugging.md)：启动链、无屏调试、测试循环、重新构建。
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md)：尚待适配的硬件。
- [docs/kc-space.md](docs/kc-space.md)：内核集合中留给原生 kext 的空间、运行时固件加载器。

（以上文档除注明外均为英文。）

## 许可与内容

除非某个软件包的 `LICENSE`、文件的 SPDX 头或 `UPSTREAM` 文件另有说明，本仓库代码采用 BSD-2-Clause-Patent，
Copyright (c) 2026 Leo Zhang（第三方部分保留其原有版权人和许可）。
本仓库从不包含厂商固件、分区镜像和 Apple 载荷（内核集合、信任缓存、由 IPSW 派生的文件），也不包含机密信息、密钥、单机校准数据、
本地网络或存储细节以及机器名。

`scripts/fetch-pins.py` 最初由 SorasakiHinaHusband 编写（拉取请求 #2）。
