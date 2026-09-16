# CLAP 语义情绪能不能进原生 App

结论：**能**，代价是 126 MB 模型 + 每次 85 ms 推理，换来与 Python 版
**逐位一致**的结果（余弦 1.000000）。跑 `export_clap.py` 就能生成两个文件。

2026-09-15 在 20.3（macOS 26.5.2 / M4 Max / Xcode 26.4，coremltools 9.0 +
torch 2.13）实测。

## 1.86 GB 里真正需要的部分

| 部分 | 体积 | 去留 |
|---|---|---|
| optimizer 状态 | 1228 MB | 丢弃，训练残留 |
| `text_branch`（RoBERTa） | 499 MB | **丢弃** |
| audio 塔 + projection | 132 MB | 保留 |

**文本塔不用上 App**，这是最大的一处简化。我们的用法里 8 个锚点的 embedding
是常量 —— Python 版 sender 也只在启动时算一次 —— 离线算好存成 42 KB 的
`anchors.json` 就够了。

## 必须用 fp32

Core ML 默认的 fp16 在这个模型上不能用。同一份精确转换，只差量化精度：

| 精度 | 体积 | 推理 | 与 PyTorch 余弦 | valence 偏差 |
|---|---|---|---|---|
| **fp32** | 126 MB | 85 ms | **1.000000** | **0.0000** |
| fp16 | 63 MB | 23 ms | 0.954071 | 0.2469 |

valence 值域是 -1..1，0.25 的偏差是 12% 满量程 —— 语义情绪会明显跑偏。省下
的 63 MB 不值这个代价。85 ms 每 4 秒跑一次是 2% 占空比。

这个结论是绕了一圈才拿到的：一开始 fp16 在白噪声上测出的误差只有 3e-6，看
着完全无害；换真实音频才暴露成 4.6e-2。**拿白噪声测神经网络的数值精度会得出
错误的结论** —— 它在 8 个音乐锚点上的响应本来就平，掩盖了真实素材会踩到的
动态范围。

## Core ML 没有 bicubic

HTSAT 在 `reshape_wav2img` 里用 `upsample_bicubic2d` 把 mel 谱图插值到固定
尺寸（实测只触发一次：时间轴 1001 → 1024，频率轴不触发）。coremltools 对这
个算子直接报 `NotImplementedError`。

两条路走过，只有第二条对：

**用 bilinear 顶替（不行）。** 注册一个转换函数改用 `resize_bilinear`，转是
转出来了，但真实音频上余弦只有 0.945、valence 偏差 0.202。

**精确矩阵化（对）。** bicubic 沿单一维度插值是**线性算子** —— 权重只取决于
位置，不取决于数据。对单位矩阵做一次同样的插值就得到它的矩阵，之后用
matmul 算，数值上完全等价（纯 PyTorch 下实测最大绝对差 5.6e-08）。

实现要在 **coremltools 层**接管，不要 patch PyTorch 的 `nn.functional.interpolate`：
那条路上插值矩阵是懒构造的，trace 期缓存一旦没命中，**构造矩阵时调的那次原始
bicubic 会被记进图**，转换照样失败。补丁本身是生效的，泄漏的是它的初始化。

## 集成结果（已完成）

模型和锚点都在 `.app/Contents/Resources/` 里，运行时**不读外部路径**。
`build.sh` 第 3 步用 `xcrun coremlc compile` 把 `.mlpackage` 编成 `.mlmodelc`
放进去，只在模型比产物新时重编。

| | 集成前 | 集成后 |
|---|---|---|
| `.app` 体积 | 380 KB | 123 MB |
| 常驻内存（启动后 1 分钟） | 77 MB | 240 MB |
| 常驻内存（连续运行 7 小时） | — | **488 MB** |
| CPU（放音时） | 4.9% | 2.8~5.0% |

CPU 基本没变 —— 85 ms 每 4 秒跑一次是 2% 占空比，淹在噪声里。

**内存会从 240 MB 爬到 488 MB 再稳住。** 2026-09-16 重建后实测：启动 45 秒
240 MB，运行 6 小时 46 分后 488 MB，此后 40 秒采样纹丝不动 —— 是爬到稳态，
不是泄漏。增长来源没有进一步定位（可能是 Core ML 的推理缓存、控制台窗口的
SwiftUI 状态、或仪表历史）。只看启动值会低估它常驻的分量，所以两个都记。

**端到端验证**（板子 192.168.24.123 的 `/lampdata`）：

- 放音时 `emo_id` 从 255 变成真实锚点、`valence` 跟着内容走
- 停止放音后 `src` 回落 1、`valence` +0.000、`emo` 255
- ssh 下查状态：`defaults read com.hjma.lamp.sender clapStatus` → `ready`

**与 PyTorch 原版交叉验证**（`crosscheck.py`，两段性质相反的合成音频）：

| 素材 | PyTorch 原版 | App（板子读回） |
|---|---|---|
| happy | `1(energetic)` +0.770 | `2(cheerful)`+0.62 → `1(energetic)`+0.70 |
| sad | `2(cheerful)` +0.684 | `2(cheerful)` +0.60 |

两边落在同一对候选上、权重接近，集成没有引入偏差。注意合成的「悲伤」片段
被**两边一致**判成 cheerful —— 纯正弦波的小调琶音确实不像悲伤音乐，这是
素材的局限，不是实现的问题。要评模型的实际效果得拿真实音乐。

## 接进 App 要改什么

固件一行都不用改 —— `valence`/`emo_id` 本来就在 LAMP1 协议里，现在发的是
固件给 v1 包的缺省值（0 / 255）。

| | |
|---|---|
| 模型输入 | `MLMultiArray [1, 480000]` FLOAT32（10 秒 @48 kHz 单声道） |
| 模型输出 | `[1, 512]` FLOAT32，已 L2 归一化 |
| 打包 | `xcrun coremlc compile` 成 `.mlmodelc` 放进 Resources |
| 数据流 | `Pipeline.run` 里 `ring.read` 之后分一路 48 kHz 进 10 秒环（1.9 MB）；重采样那一路不动 |
| 推理时机 | 后台队列每 4 秒一次，与 Python 版同频 |
| 打分 | `embedding · anchors` → `softmax(×25)` → 加权得 valence/energy，argmax 得 emo_id |
| 融合 | `mood = 0.5 * liblamp.mood + 0.5 * energy`，与 Python 版同式 |

静音时要跳过推理并把 `emo_id` 退回 255 —— 和 Python 版一样，不然安静时段会
拿上一段音乐的情绪一直发。

## 没做的

- 真实音乐素材上的主观效果比对（只验了与 PyTorch 的数值/判定一致性）
- 混合精度（只让敏感层留 fp32）能不能把 126 MB 压下来，没试
- 模型加载耗时没单独测（已经放在后台队列，不挡音频链路起步）

## 重建笔记（2026-09-16）

模型和 `anchors.json` 都是生成物、不入库（见 `Resources/.gitignore`），临时目录
一清就没了，得照本文重新跑一次。这次重建踩到的：

**`laion_clap` 有未声明的依赖。** 它的 metadata 里没有 `torchvision`，但
`clap_module/utils.py` 直接 `from torchvision.ops.misc import FrozenBatchNorm2d`
—— 装完一切正常，`import laion_clap` 时才炸。补装即可。

**`wget` 把下载临时文件写在当前工作目录，不是目标目录。** `load_ckpt()` 最终要
把 1.86 GB 的 checkpoint 放进 `site-packages/laion_clap/`，但下载途中的文件叫
`<cwd>/630k-audioset-best.pt<随机>.tmp`。在目标目录里找不到任何东西、进程又
静默几十分钟，很容易误判成卡死 —— 它只是在慢慢下（实测 660 KB/s，44 分钟）。
判断它是否在动：看 cwd 下的 `.tmp` 文件在不在长大。

**版本警告可以忽略。** `coremltools 9.0` 会警告「torch 2.14.0 未经测试，最高测到
2.7.0」，`scikit-learn 1.9.1` 也超出支持范围。实际转换没问题 —— 与 PyTorch 的
余弦仍是 1.000000。

**在构建机上原地构建。** 模型 126 MB、`.app` 123 MB，在本机导出再传过去意味着两
次上百 MB 的来回。按 `build.sh` 的期望把固件仓库与 `LampKit` 并排放好，原地跑
`./build.sh` 就行。

端到端复验（灯 `/lampdata`）：播 C 大调 140 BPM 的合成素材，`emo` 从 255 变成 1
（`energetic happy dance music`），`val` 收敛到 +0.740 —— 锚点标定的 valence 是
+0.8。第一次采样会因为窗口里还混着静音而落在别的锚点上，属正常冷启动。
