# LampSender — macOS 端

两件事，两个入口：

- **菜单栏**（音频面）：把这台 Mac 正在播放的声音分析成灯效数据推给灯，替代
  [`tools/wled_sync_sender.py`](../../../tools/wled_sync_sender.py) 与它那套
  launchd 常驻。
- **控制台窗口**（控制面）：灯本身怎么亮 —— 开关、亮度、效果、预设、分段、
  定时、配网、OTA。从菜单栏的「打开控制台…」进入。

控制面用 [LampKit](../../../../LampKit)（独立仓库，iOS App 也依赖同一份），
音频面用 liblamp。

## 为什么不在 iOS App 那个仓库里

这个 App 要直接编译 `usermods/lamp` 那批 C++ 头文件（与固件同一份源码），
跨仓库引用 C++ 源码树没法干净做，所以它住在固件仓库。控制面（选灯、调参、
预设）仍归 `ambient-light` 的 LampKit / LampApp，这边只负责发包。

## 构建、安装、运行

```bash
./build.sh              # 编译出 build/LampSender.app
./build.sh install      # 装到 ~/Applications（会先停掉正在跑的实例）
open ~/Applications/LampSender.app
```

菜单里的「开机自动启动」走 `SMAppService`，注册的是**当前这份 app 的位置**，
所以必须先 `install` 再开自启 —— 从 `build/` 里注册，下次重新构建挪走就指空了。
没有 UI 的场合（ssh）可以这样核实状态：

```bash
defaults read com.hjma.lamp.sender launchAtLoginStatus   # enabled / requiresApproval / notFound
```

**必须用 `open` 启动。** 直接执行 `Contents/MacOS/LampSender`（比如从 ssh）
会被系统判为不属于图形会话：tap 照样建得成、IOProc 照样回调，但**数据全是 0**，
且不报任何错误码、不写任何日志。这一条踩过，排查了很久。

构建不需要 XcodeGen 也不需要 SwiftPM —— SwiftPM 的 target 源码不能跳出 package
目录，而我们要编仓库另一头的 C++。`swiftc` 直接编没有这个限制，有 Xcode 就够。

## 相对 Python 版的变化

| | Python 版 | 原生版 |
|---|---|---|
| 音频来源 | BlackHole 虚拟声卡 | Core Audio Process Tap |
| 前置条件 | 装驱动 + 把系统输出切成「多输出设备」 | 无 |
| 常驻方式 | launchd 用户代理 | 菜单栏 App + 系统登录项 |
| 进程守护 | `KeepAlive`，崩了自动重启 | **没有**，见下 |
| 依赖体积 | 2.9 GB venv | 120 MB（.app，其中 CLAP 模型 120 MB） |
| 语义情绪 | CLAP 零样本（1.7 GB 权重） | Core ML fp32，模型打进 .app |

分析管线本身没有变：两边都调同一个 `liblamp`，跑的是固件那份 C++。

### 崩了不会自动重启

Python 版那份 plist 带 `KeepAlive`，进程挂掉 launchd 立刻拉起来。`SMAppService`
的登录项只负责**登录时启动一次**，之后崩了就是崩了 —— 表现是灯不再跟着音乐，
但菜单栏图标也没了，至少不会静悄悄地错。

要补这个能力得换成 app bundle 内嵌 LaunchAgent（`SMAppService.agent`）加
`KeepAlive`，但那条路上 `ProgramArguments` 直接指可执行文件会撞上前面说的
图形会话问题，指 `open` 又会让 launchd 以为进程立刻退出而反复重启。暂时不做。

### 一处行为差异值得知道

Process Tap 抓的是**全系统**混音 —— 视频会议、系统提示音、任何 App 的声音都会
进去。Python 版靠 BlackHole 时，只有用户显式路由过去的音频才会被抓，等于自带
一个筛子。`CATapDescription` 支持按进程或按 bundle ID 圈定范围，要做选择性
抓取，扩展点在 `AudioTap.start()`。

### CLAP 语义情绪

每 4 秒对最近 10 秒音频跑一次零样本推理，八锚点 valence×energy 加权，
结果融进 LAMP1：`mood = 0.5 × liblamp.mood + 0.5 × CLAP.energy`，与 Python
版同式。模型是 Core ML fp32，连同 42 KB 的锚点常量一起打进 .app 的
Resources，**不读外部路径**。

与 Python 版的两处差异：

- **窗口 10 秒而不是 8 秒**。Python 版送 8 秒进去，CLAP 内部 repeatpad 到
  10 秒；这里直接给满 10 秒真实音频，上下文更完整。
- **静音时整组退回缺省**。Python 版静音时只把 energy 置空，valence 和
  emo_id 留着上一次的值；这里整个置 nil，发包侧退回固件给 v1 包的 0 / 255。
  安静时段挂着上一首歌的情绪没有意义。

模型加载在后台，不挡音频链路起步。加载不出来（没打包、或加载失败）就退回
纯 liblamp 的 mood，**并在菜单栏标出来** —— 一个看不见的降级比没有这个功能
更糟。生成模型见 [clap/README.md](clap/README.md)。

## 控制台窗口

侧边栏选灯，右边四个面板：控制 / 预设 / 分段 / 设置。功能与 iOS App 对齐，
外加一块 Mac 端独有的实时仪表（频谱、RMS、BPM、情绪）—— 窗口够大放得下，
而且这台机器往往就是 LAMP1 的来源，出问题时能当场看出是发端没数据还是灯
没收到。

**视图不与 iOS 共享，逻辑共享。** 手机是竖屏分页，Mac 是侧边栏加窗口，硬凑
一套两头别扭；`LampViewModel` 在 LampKit 里，两端共用同一个状态机。

菜单栏 App 平时不该弹窗口，所以默认不自动打开。要开机就见到控制台：

```bash
defaults write com.hjma.lamp.sender openConsoleOnLaunch -bool true
```

## 线程模型

```
[HAL 实时线程]  tap IOProc → 转单声道 → 写无锁环     ← 不分配、不加锁、不打日志
[工作线程]      读环 → 重采样 48k→22.05k → liblamp → 打包 → UDP
[主线程]        每秒收一次统计，刷菜单栏
```

Python 版是在音频回调里直接跑完 FFT 再 `sendto` 的 —— GIL 下本来就不实时，
不算错。原生版没有这个借口，所以拆成三段。

重采样这一步是 Python 版没有的：它直接向 CoreAudio 要 22050 Hz 的输入流，
重采样由驱动层代劳；tap 只给设备原生率（这台机器是 48 kHz），得自己补上。
用 `AVAudioConverter` 而不是线性插值 —— 降采样不先抗混叠低通，11 kHz 以上的
能量会折回来污染频段分析。

## 已验证（2026-09-15，20.3 / macOS 26.5.2 / M4 Max / Xcode 26.4）

- 板子 `192.168.24.123`：启动 App 后 `/lampdata` 的 `src` 从 1（本地麦克风）
  翻成 2（LAMP1 远端），`rms` 0.13、`vocal` 0.56、`bands` 有真实频谱
- 停掉 App 约 500 ms 后 `src` 回落到 1（固件的 `kRemoteFreshMs`）
- 不装 BlackHole、不改系统默认输出（实测时默认输出是一副 AirPods，全程没动）
- 运行时 CPU 5.8%、常驻内存 78.7 MB

## 两个 Core Audio 的坑

1. **`CATapDescription(stereoMixdownOfProcesses: [])` 的参数是「要包含的进程」。**
   空数组等于「混合零个进程」：tap 建得成、格式报 48000Hz/2ch、`AudioDeviceStart`
   返回 `noErr`，但 IOProc **一次都不回调**。全系统要用
   `stereoGlobalTapButExcludeProcesses([])`。两者差一个词，症状是彻底静默。

2. **进程必须归属图形会话**，否则静默全零 —— 见上面「必须用 open 启动」。
   TCC 表里 `kTCCServiceAudioCapture` 的授权是自动给的（`auth_value=2`，从未
   弹框），所以这不是「没授权」，是会话归属判定。

## 替换 Python 版之后的遗留

停用旧的常驻（可逆，plist 只是改名不是删）：

```bash
launchctl bootout gui/$(id -u)/com.hjma.wled-sync
mv ~/Library/LaunchAgents/com.hjma.wled-sync.plist{,.disabled}
```

这些东西不再有人用了，**没有删**，要回收自己决定：

| 路径 | 占用 | 说明 |
|---|---|---|
| `tools/mlenv/` | 2.9 GB | Python sender 的 venv，在仓库目录里 |
| └ `laion_clap/630k-audioset-best.pt` | 1.7 GB | CLAP 权重，就在 venv 包目录内 |
| `~/.local/wled-sync/` | 39 MB | 其中 `sender.log` 单独占 36 MB |

（`~/.cache/torch/hub` 那 409 MB 是 utmos22，别的项目的语音 MOS 模型，与这里无关。）

留着它们唯一的理由是 CLAP —— 原生版还没有语义情绪，真要跑 CLAP 只能回到
Python 那套。那 1.7 GB 的 checkpoint 里真正有用的部分其实很小：

| 部分 | 体积 | App 要吗 |
|---|---|---|
| optimizer 状态 | 1228 MB | 不要，训练残留 |
| `text_branch`（RoBERTa） | 499 MB | **不要** —— 8 个锚点的 embedding 是常量，离线算好存 16 KB |
| audio 塔 + projection | 132 MB (fp32) / 66 MB (fp16) | 要 |

## 还没做

- 选择性抓取（只 tap 指定 App）
- 进程守护（见上）
- CLAP 在真实音乐上的主观效果比对（数值一致性已验，见 clap/README.md）
