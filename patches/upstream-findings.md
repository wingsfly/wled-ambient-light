# 在上游发现、但**暂不向上游反馈**的问题

在主机端移植 WLED 灯效的过程中（见 `tools/hostwled/README.md`），
ASan 顺带查出几个上游自己的缺陷。**当前决定：不提 issue、不提 PR，只在这里记录。**

这个文件存在的理由：这类东西查一次很贵、写一次报告也很贵，但一旦不写下来，
过几个月连「当时是怎么复现的」都想不起来了。所以连**可以直接粘贴的英文报告草稿**
一起留着 —— 将来想提的时候是几分钟的事，不是重查一遍。

| # | 位置 | 严重度 | 我们的处置 | 上游反馈 |
|---|---|---|---|---|
| [U1](#u1) | `FX.cpp` `mode_particleDancingShadows()` | **内存损坏** | 已在 fork 里修，见 [`0001`](0001-ps-dancing-shadows-sprayemit.md) | 暂不提 |
| [U2](#u2) | `FXparticleSystem.cpp` `ParticleSystem2D::flameEmit()` | 良性差一错误 | **不修**（二维专用，这台灯用不到） | 暂不提 |

基线：`v16.0.1`；`d619d469`（当时的 `upstream/main`）上两条都还在。

---

<a id="u1"></a>
## U1 · `mode_particleDancingShadows()` 用 `uint32_t` 接了 `sprayEmit()` 的 `-1`

技术细节、复现现场、为什么这么修，全在 [`0001-ps-dancing-shadows-sprayemit.md`](0001-ps-dancing-shadows-sprayemit.md)。
这里只补「要不要告诉上游」这一面。

**值得告诉上游的理由**：在 ESP32 上它**不崩**。32 位指针算术把
`particles[0xFFFFFFFF]` 回绕成 `particles[-1]`，写坏的是 `ParticleSystem1D`
结构体尾部 8 字节 —— 静默的内存损坏，症状是偶发的粒子行为异常，
没有人会把它和这个效果联系起来。这也正是它能一直留着的原因。

**暂不提的理由**：这是项目主人的决定，不是技术判断。

### 可直接粘贴的报告草稿（英文）

> **Title:** `PS Dancing Shadows`: `sprayEmit()`'s `-1` sentinel is assigned to a `uint32_t` and used as an index
>
> **Version:** v16.0.1 (still present on `main` as of `d619d469`)
>
> In `mode_particleDancingShadows()` (`wled00/FX.cpp`):
>
> ```cpp
> uint32_t partidx = PartSys->sprayEmit(PartSys->sources[0]);
> PartSys->particles[partidx].ttl = ttl;
> ```
>
> `ParticleSystem1D::sprayEmit()` is declared `int32_t` and returns `-1` when no
> dead particle is available (`FXparticleSystem.cpp`). Assigning that to a
> `uint32_t` makes it `0xFFFFFFFF`, which is then used directly as an index.
>
> This is not a rare edge case. The guard for the emit block is
> `deadparticles > 5` (i.e. at least 6 free particles), but the loop emits
> `width = hw_random16(1, 10)` particles — up to 9. Whenever `width >= 7` the
> sentinel is guaranteed to be hit.
>
> Observed on a 96-LED 1D segment (host build with ASan):
> `deadparticles = 6`, `width = 9`, `usedParticles = 49`.
>
> On a 64-bit host the index is zero-extended, so the write lands ~34 GB past
> the buffer and faults immediately. **On ESP32 (32-bit pointers) the arithmetic
> wraps, so the write goes to `particles[-1]`** — it does not crash, it silently
> overwrites the last 8 bytes of the `ParticleSystem1D` object itself. I suspect
> that is why this has gone unnoticed.
>
> Suggested fix, matching how the other two call sites that use the return value
> already handle it (`FX.cpp:9840` `if (idx < 0) break;`, `FX.cpp:10555`
> `if (partindex >= 0)`):
>
> ```cpp
> int32_t partidx = PartSys->sprayEmit(PartSys->sources[0]);
> if (partidx >= 0) PartSys->particles[partidx].ttl = ttl;
> ```
>
> Skipping the particle is exactly what the `-1` return means, and there is no
> visible difference in the effect. Tightening the `deadparticles > 5` guard
> instead would require restructuring the loop.
>
> (While looking at this I also checked the other 18 `sprayEmit()` call sites —
> the rest either discard the return value or test it correctly. One unrelated
> nit noted separately.)

---

<a id="u2"></a>
## U2 · `ParticleSystem2D::flameEmit()` 用 `> 0` 而不是 `>= 0`

`wled00/FXparticleSystem.cpp`：

```cpp
void ParticleSystem2D::flameEmit(const PSsource &emitter) {
  int emitIndex = sprayEmit(emitter);
  if (emitIndex > 0)  particles[emitIndex].ttl += emitter.source.ttl;
}
```

`ParticleSystem2D::sprayEmit()` 同样是失败返回 `-1`、成功返回下标（`0` 是合法下标）。
写成 `> 0` 于是**漏掉 0 号粒子** —— 它照常被发射，只是拿不到那份 ttl 加成，
寿命比别的火苗粒子短。

**不涉及内存安全**，只是一颗粒子的行为不一致，肉眼基本看不出来。

**为什么不修**：唯一的调用者是 `FX.cpp:8442`（二维火焰类效果），
而这台灯是一维灯管，跑不到。多改一行上游代码就多一处将来合并时的冲突点，
换不到任何东西。

真要修就是 `emitIndex > 0` → `emitIndex >= 0`。

---

## 将来要提的话

1. 先按 `patches/README.md` 确认补丁还没被上游修掉（拉一次 `upstream`，
   看 `mode_particleDancingShadows()` 现在长什么样）。
2. U1 直接用上面的草稿开 issue；WLED 的仓库是 <https://github.com/wled/WLED>。
3. 上游合入之后，**把 `patches/0001` 撤掉**，别让本地补丁无限期留着。
