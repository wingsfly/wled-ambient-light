// 16 段对数频段映射（设计文档 §3.2 / §3.3.4）。
//
// 边界以 **Hz** 为规范定义，按 N 换算成 bin —— 而不是给三种 N 各写一张 bin 表。
// 三张手写表要手工保持一致，Hz 规范只有一份真相。
//
// 这组 Hz 取自 WLED 上游 512 点映射的换算值，所以 N=512 时逐 bin 复现上游，
// 默认档的观感与上游一致。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "lamp_window.h"

namespace lamp {

constexpr int NUM_BANDS = 16;

constexpr float kBandEdgeHz[NUM_BANDS + 1] = {
      43.07f,   86.13f,  129.20f,  215.33f,  301.46f,  430.66f,
     559.86f,  818.26f, 1119.73f, 1421.19f, 1894.92f, 2411.72f,
    3014.65f, 3703.71f, 4478.91f, 7105.96f, 9259.28f,
};

// 第 i 条边界在长度 N 的 FFT 里落在哪个 bin。
// 四舍五入而非截断：截断会系统性把每条边界往低频拉最多一个 bin。
inline uint16_t binEdge(int i, size_t n) {
    const float b = kBandEdgeHz[i] * (float)n / kSampleRate;
    return (uint16_t)(b + 0.5f);
}

// 一个窗的主瓣**半宽**，单位 bin。段内 bin 数不到它的两倍时，
// 主瓣会糊过段边界，那一段的读数就不再跨窗可比 —— 见 bandIsResolved()。
inline uint16_t mainLobeHalfWidth(WindowType wt) {
    switch (wt) {
        case WIN_BLACKMAN_HARRIS: return 4;
        case WIN_FLATTOP:         return 6;
        case WIN_HANN:
        default:                  return 2;
    }
}

// 这一段在这个 (N, 窗) 下是否分得开。
//
// 这不是保护性检查，是**规格的一部分**：43Hz 的段宽在 N=512 下只有 1 个 bin，
// 而 Hann 主瓣就是 2 bin —— 物理上分不开，跟实现无关。设计 §3.3 让
// 「极限打点」档用 N=512 正是接受了低频分辨率的损失。
inline bool bandIsResolved(int i, size_t n, WindowType wt) {
    const uint16_t cnt = (uint16_t)(binEdge(i + 1, n) - binEdge(i, n));
    return cnt >= (uint16_t)(2 * mainLobeHalfWidth(wt));
}

constexpr size_t kMaxFftLen = 2048;

// 一次分析配置：窗系数、它的噪声功率增益、以及帧长，三者绑在一起。
//
// 绑在一起有两个理由。一是**不可能用错长度**：增益必须按同一个 n 算，
// 分开传参就有写死成 1024 的机会，而那种错只在 n≠1024 时发作，
// 还会被上一次调用残留在缓冲里的数据掩盖。二是**每帧不必重算**：
// 100Hz 下重复 fillWindow(2048) 是两千次 cos 的白扔。
//
// 8KB，别放栈上 —— 调用方按静态或堆对象持有。
struct Analysis {
    size_t     n  = 0;
    WindowType wt = WIN_HANN;
    float      ng = 0.0f;      // mean(w²)
    float      w[kMaxFftLen];
};

// 切换档位时调用；同一档位内每帧复用。
//
// 非法帧长返回 false 并**保持 a 不变** —— 不静默裁剪到 kMaxFftLen：
// 那样会拿 2048 点的窗去处理一个声称 4096 点的谱，读出来的东西没有意义，
// 而调用方还以为配置成功了。
//
// 尾部清零不是洁癖。增益按 a.n 算，若哪天有人把长度写死（比如 noisePowerGain
// 里写成 1024），残留的旧窗系数会让结果看着仍然合理，缺陷就藏住了。
inline bool analysisInit(Analysis &a, size_t n, WindowType wt) {
    if (n < 64 || n > kMaxFftLen || (n & (n - 1)) != 0) return false;
    a.n = n; a.wt = wt;
    fillWindow(wt, a.w, n);
    for (size_t i = n; i < kMaxFftLen; ++i) a.w[i] = 0.0f;
    a.ng = noisePowerGain(a.w, n);
    return true;
}

// 从幅度谱算 16 段能量，**结果与 (N, 窗) 无关**（在 bandIsResolved 的段上）。
//
// mag 是长度 a.n/2+1 的幅度谱（不是功率谱），来自对加了 a.w 窗的 a.n 点实数序列做 FFT。
//
// 归一化按 Parseval 走：
//     Σ_{k∈段} |Y[k]|² · 2 / (n² · mean(w²))  =  该段内信号的功率
// 三个因子都不能少：
//   1) ×2 —— 实数 FFT 只取了单边谱
//   2) n² —— DFT 未归一化，幅度正比于 n，功率正比于 n²
//   3) 窗的**噪声功率增益** mean(w²)，不是相干增益 mean(w)。
//      用相干增益会让 Hann 与 BH 差 0.5²/0.359² = 1.94 倍。
//
// **不要再除段内 bin 数。** 除 bin 数得到的是「每 bin 平均功率」，那个量对宽带
// 噪声跨 N 一致，对纯音却不一致：纯音的能量集中在 1 个 bin，而段内 bin 数随 N 变。
// Parseval 形式对两者都成立。
//
// 段区间是左闭右开 [binEdge(i), binEdge(i+1))，相邻段既不重叠也不留缝。
//
// 输出是 RMS 幅度量纲（对功率开方），所以输入幅度加倍时输出也加倍。
inline void computeBandEnergy(const Analysis &a, const float *mag, float *out) {
    for (int i = 0; i < NUM_BANDS; ++i) {
        const uint16_t lo = binEdge(i, a.n);
        const uint16_t hi = binEdge(i + 1, a.n);

        double p = 0.0;
        for (uint16_t k = lo; k < hi; ++k) p += (double)mag[k] * (double)mag[k];

        const double power = 2.0 * p / ((double)a.n * (double)a.n * (double)a.ng);
        out[i] = (float)sqrt(power);
    }
}

// ── 由频段能量派生的两个谱形特征 ──────────────────────────
//
// 它们是 StyleFeatures 的后两个判据（§3.3.3）。放在这里而不是 lamp_style.h：
// 它们是 16 段能量的派生量，与档位切换的策略无关。
//
// 之前这两项没有生产者，恒为 0，占掉 30% 的权重 —— speedDemand 的上限因此
// 只有 0.70，而「极限打点」档的阈值是 0.78，**那个档位永远进不去**。
// 四个档位实际只能用三个。

// 第 i 段的几何中心频率。对数频段用几何中心，不是算术中心。
inline float bandCenterHz(int i) {
    return sqrtf(kBandEdgeHz[i] * kBandEdgeHz[i + 1]);
}

// 频谱质心：能量的加权重心，单位 Hz。亮/暗的直接度量。
inline float spectralCentroid(const float *bands) {
    double num = 0.0, den = 0.0;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float e = bands[i];
        if (!isfinite(e) || e <= 0.0f) continue;
        num += (double)bandCenterHz(i) * e;
        den += e;
    }
    if (!(den > 0.0)) return 0.0f;          // 静音：返回 0，由调用方当作「无信息」
    return (float)(num / den);
}

// 频谱平坦度（Wiener entropy）：几何平均 / 算术平均，落在 [0,1]。
// 白噪声各段等能量 → 1；单音集中在一段 → 趋近 0。
//
// 用 log 求和再 exp，不要连乘：16 个 0.01 量级的数直接相乘会下溢到 0。
inline float spectralFlatness(const float *bands) {
    double log_sum = 0.0, lin_sum = 0.0;
    int n = 0;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float e = bands[i];
        if (!isfinite(e) || e < 0.0f) continue;
        // 加一个极小的地板，否则任何一段为 0 都会让几何平均整体归零，
        // 而那在音乐里很常见（高频段常常真的没能量）
        const double v = (double)e + 1e-9;
        log_sum += log(v);
        lin_sum += v;
        ++n;
    }
    if (n == 0 || !(lin_sum > 0.0)) return 0.0f;
    const double ari = lin_sum / (double)n;
    // 静音要报 0，不能报 1。地板项让全零输入看起来「完美平坦」——
    // 那样静音与白噪声就分不开了，而 StyleFeatures 会拿这个 1.0 去投满票。
    // 阈值取 1e-6：比地板项 1e-9 高三个数量级，又远低于任何真实信号。
    if (ari < 1e-6) return 0.0f;
    const double geo = exp(log_sum / (double)n);
    double f = geo / ari;
    if (!(f > 0.0)) return 0.0f;            // 含 NaN
    return (f > 1.0) ? 1.0f : (float)f;
}

} // namespace lamp
