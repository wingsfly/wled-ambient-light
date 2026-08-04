# 0001 · `PS Dancing Shadows` 用 `uint32_t` 接了 `sprayEmit()` 的 `-1`

- **文件**：`wled00/FX.cpp`，`mode_particleDancingShadows()`
- **基线**：v16.0.1（`d619d469` 时的 upstream/main 仍有此问题）
- **状态**：待上游修复。上游修了就撤掉这个补丁。

## 缺陷

```cpp
uint32_t partidx = PartSys->sprayEmit(PartSys->sources[0]);
PartSys->particles[partidx].ttl = ttl;
```

`ParticleSystem1D::sprayEmit()` 的返回类型是 `int32_t`，**找不到死粒子时返回 `-1`**
（`FXparticleSystem.cpp:1273`）。这里用 `uint32_t` 接，`-1` 变成 `0xFFFFFFFF`，
直接拿去当下标。

哨兵一定会被踩到，不是偶发：准入门槛是 `deadparticles > 5`（即**至少 6 颗**），
而循环要发 `width = hw_random16(1, 10)` 颗、**最多 9 颗**。`width ≥ 7` 时必然踩空。

实测崩溃现场（96 颗一维灯带，主机端 ASan）：

```
deadparticles = 6    width = 9    usedParticles = 49
EXC_BAD_ACCESS  FX.cpp  mode_particleDancingShadows()
```

## 为什么真机上看不出来

指针宽度不同，后果完全不同：

| | `particles[0xFFFFFFFF]` 落在哪 | 现象 |
|---|---|---|
| ESP32（32 位） | 指针算术模 2³² 回绕 → **`particles[-1]`** | 不崩。悄悄改写 `ParticleSystem1D` 结构体尾部 8 字节 |
| 主机（64 位） | 零扩展 → 基址 **+34 GB** | 立刻段错误 |

真机上是**静默的内存损坏**，所以上游一直没发现。这也是为什么值得在这里修：
它不是「只有模拟器才有的问题」。

## 修法

与上游自己在另外两处的写法一致（`FX.cpp:9840` 的 `if (idx < 0) break;`、
`FX.cpp:10555` 的 `if (partindex >= 0)`）：

```cpp
int32_t partidx = PartSys->sprayEmit(PartSys->sources[0]);
if (partidx >= 0) PartSys->particles[partidx].ttl = ttl;
```

**只补哨兵判断，不动 `deadparticles > 5` 那个门槛。** 发不出粒子时少发一颗
就是 `sprayEmit()` 返回 `-1` 的本意，画面上看不出区别；改门槛要重排循环结构，
改动面大得多，跟上游合并时也更容易冲突。

## 上游反馈

**暂不提 issue/PR**（项目主人的决定）。理由与可直接粘贴的英文报告草稿见
[`upstream-findings.md`](upstream-findings.md) 的 U1 —— 想提的时候不用重查一遍。

## 顺带发现（**没有**修）

`FXparticleSystem.cpp:217` `ParticleSystem2D::flameEmit()` 写的是
`if (emitIndex > 0)` 而不是 `>= 0`，于是 0 号粒子拿不到 ttl 加成。
良性差一错误，不涉内存安全，且是二维专用（这台灯用不到）。
记在 [`upstream-findings.md`](upstream-findings.md) 的 U2。

## 验证

修之前 `PS Dancing Shadows` 在主机端必崩，是现成的红灯：

```bash
./tools/build_wled.sh
# 220 个效果各渲 240 帧，ASan 全量扫描
```

修之后该效果解除屏蔽，220 个全绿。
