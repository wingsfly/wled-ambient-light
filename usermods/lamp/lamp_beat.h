// 节拍跟踪（设计文档 §3.4 阶段 2 的后半）。
//
//   flux → 固定速率 ODF → 6 秒窗自相关 → BPM + 置信度 → 锁相 → 相位预测
//
// 前半（spectral flux、自适应阈值、onset）在 lamp_onset.h。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_onset.h"
#include "lamp_time.h"

namespace lamp {

// ODF（onset detection function）的采样率**固定**，与输入帧率无关。
//
// 43.066 Hz = 22050/512，正好是上游 512 点 FFT 的帧率，也正好让 258 点覆盖 6 秒
// ——设计 §3.4 说「自相关每秒一次，258 点」，指的就是这个。
//
// 为什么必须固定：BPM 由 lag 换算而来（bpm = 60·rate/lag）。若 ODF 跟着档位的
// hop 走，换档后同一个 lag 会对应不同的 BPM，§3.3.4 第 2 条要求的「换档后 BPM
// 与相位仍然有效」就无从谈起。
constexpr float kOdfRateHz   = 22050.0f / 512.0f;      // 43.066
constexpr float kOdfPeriodMs = 1000.0f / kOdfRateHz;   // 23.22
constexpr int   kOdfLen      = 258;                    // 6 秒

constexpr int kMinBpm = 60;
constexpr int kMaxBpm = 200;
// lag 边界：60BPM → 43.07，200BPM → 12.92。留出插值要用的邻居。
constexpr int kMinLag = 13;
constexpr int kMaxLag = 43;

struct BeatConfig {
    float    lock_conf     = 0.35f;    // 置信度高于此才算锁定
    float    unlock_conf   = 0.20f;    // 低于此才解锁（滞回）
    float    center_bpm    = 120.0f;   // 倍频加权的中心
    float    octave_sigma  = 0.9f;     // 加权的对数标准差（倍频程）
    float    octave_fold   = 0.85f;    // r(lag/2) 高过这个比例就折向快的那个
    float    pll_gain      = 0.15f;    // 相位环增益
    uint32_t analyze_ms    = 1000;     // 自相关的重算间隔
};

struct BeatTracker {
    float    odf[kOdfLen] = {0};
    uint16_t odf_head  = 0;
    uint16_t odf_count = 0;

    float    pending   = 0.0f;   // 重采样窗内的峰值
    float    carry_ms  = 0.0f;

    float    bpm       = 0.0f;
    float    conf      = 0.0f;
    bool     locked    = false;

    float    period_ms   = 0.0f;
    uint32_t next_beat   = 0;    // 下一拍的预测时刻
    bool     has_phase   = false;
    uint32_t last_analyze = 0;
    bool     has_analyzed = false;
};

inline void beatInit(BeatTracker &b, const BeatConfig &) {
    for (int i = 0; i < kOdfLen; ++i) b.odf[i] = 0.0f;
    b.odf_head = 0; b.odf_count = 0;
    b.pending = 0.0f; b.carry_ms = 0.0f;
    b.bpm = 0.0f; b.conf = 0.0f; b.locked = false;
    b.period_ms = 0.0f; b.next_beat = 0; b.has_phase = false;
    b.last_analyze = 0; b.has_analyzed = false;
}

// 换档时调用。**什么都不用改。**
//
// ODF 本来就是固定速率的，自相关的 lag→BPM 换算与档位无关，周期与相位是时域量。
// 这个函数存在只是为了把「换档时不该动 BeatTracker」这件事写成代码里可见的一笔
// ——§3.3.4 第 2 条的原话是「保留 BPM/相位、只重置自适应阈值」，而自适应阈值
// 在 lamp_onset.h 那边，由 onsetRetime 负责。
inline void beatRetime(BeatTracker &, const BeatConfig &) {}

// ── 自相关 ────────────────────────────────────────────────

// 倍频加权：以 center_bpm 为中心的对数高斯。
//
// 没有它，120BPM 的自相关在 lag=43（60BPM，每两拍一次相关）也有一个几乎同高的峰，
// 选谁全看噪声。听感上人对 120 的偏好远高于 60，加权把这个先验写进来。
inline float octaveWeight(const BeatConfig &c, float bpm) {
    const float z = log2f(bpm / c.center_bpm) / c.octave_sigma;
    return expf(-0.5f * z * z);
}

// 读环形缓冲：i=0 是最老的样本。
inline float odfAt(const BeatTracker &b, int i) {
    const int n = (b.odf_count < kOdfLen) ? b.odf_count : kOdfLen;
    const int start = (b.odf_count < kOdfLen) ? 0 : b.odf_head;
    return b.odf[(start + i) % kOdfLen < 0 ? 0 : (start + i) % kOdfLen];
}

// 归一化自相关 + 抛物线插值。返回是否得到有效估计。
//
// 归一化（除以两段的能量）是必须的：不归一化的话 r(lag) 随 lag 增大天然衰减
// （重叠区变短），长周期永远竞争不过短周期。
inline bool beatAnalyze(BeatTracker &b, const BeatConfig &c) {
    const int n = (b.odf_count < kOdfLen) ? (int)b.odf_count : kOdfLen;
    if (n < kMaxLag * 3) return false;          // 数据不够，别猜

    // 去均值：ODF 恒为正，不去均值的话自相关处处接近 1，峰淹没在直流里
    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += odfAt(b, i);
    mean /= (double)n;

    float r[kMaxLag + 2] = {0};
    float best = -2.0f, best_w = -2.0f;
    int   best_lag = 0;
    double r_sum = 0.0; int r_cnt = 0;

    for (int lag = kMinLag; lag <= kMaxLag; ++lag) {
        double num = 0.0, e0 = 0.0, e1 = 0.0;
        for (int i = 0; i + lag < n; ++i) {
            const double a = odfAt(b, i) - mean;
            const double d = odfAt(b, i + lag) - mean;
            num += a * d; e0 += a * a; e1 += d * d;
        }
        const double den = sqrt(e0 * e1);
        r[lag] = (den > 1e-12) ? (float)(num / den) : 0.0f;
        r_sum += r[lag]; ++r_cnt;

        const float w = r[lag] * octaveWeight(c, 60.0f * kOdfRateHz / (float)lag);
        if (w > best_w) { best_w = w; best = r[lag]; best_lag = lag; }
    }
    if (best_lag == 0) return false;

    // 倍频折叠。
    //
    // 光靠 octaveWeight 压不住半速：对纯周期信号 r(2T) 与 r(T) 几乎同高，
    // 而 180BPM 的加权（0.810）反而低于 90BPM（0.899），于是半速胜出 ——
    // 实测 180 会报成 89.8。
    //
    // 真正可用的不对称在别处：**长周期的峰是短周期的副产品**（每两拍也相关
    // 一次），反之不成立。所以两者接近时，短的那个才是真周期。
    // 这一步只在 r(lag/2) 确实几乎同高时才折，不会把 90 误折成 180 ——
    // 90BPM 在 lag/2 处正好是半拍的低谷，r 很低。
    {
        const int half = best_lag / 2;
        if (half >= kMinLag && r[half] > c.octave_fold * r[best_lag]) best_lag = half;
    }

    // 抛物线插值。43Hz 下整数 lag 的分辨率是 4.7%（lag 21↔22 是 122.9↔117.3 BPM），
    // 而验收线是 2% —— 插值不是优化，是达标的前提。
    float lag_f = (float)best_lag;
    if (best_lag > kMinLag && best_lag < kMaxLag) {
        const float y0 = r[best_lag - 1], y1 = r[best_lag], y2 = r[best_lag + 1];
        const float den = y0 - 2.0f * y1 + y2;
        if (fabsf(den) > 1e-9f) {
            float d = 0.5f * (y0 - y2) / den;
            if (d > 0.5f) d = 0.5f; else if (d < -0.5f) d = -0.5f;
            lag_f += d;
        }
    }

    const float r_mean = (r_cnt > 0) ? (float)(r_sum / r_cnt) : 0.0f;
    float conf = (best - r_mean) / (1.0f - r_mean + 1e-6f);
    if (!(conf > 0.0f)) conf = 0.0f;            // 含 NaN
    if (conf > 1.0f) conf = 1.0f;

    b.conf = conf;
    // 滞回：锁上要 lock_conf，掉下来要低于 unlock_conf
    if (b.locked) { if (conf < c.unlock_conf) b.locked = false; }
    else          { if (conf >= c.lock_conf)  b.locked = true;  }

    if (b.locked) {
        b.bpm       = 60.0f * kOdfRateHz / lag_f;
        b.period_ms = 60000.0f / b.bpm;
    }
    return true;
}

// ── 主循环 ────────────────────────────────────────────────

// flux 来自 spectralFlux()，is_onset 来自 onsetUpdate()。
inline void beatUpdate(BeatTracker &b, const BeatConfig &c,
                       float flux, bool is_onset, float dt_ms, uint32_t now_ms) {
    if (!isfinite(flux) || flux < 0.0f) flux = 0.0f;

    // 重采样到固定的 43.066 Hz。
    // 窗内取**最大值**而非平均：ODF 的价值全在峰的锐度上，平均会把它抹平。
    if (flux > b.pending) b.pending = flux;
    if (isfinite(dt_ms) && dt_ms > 0.0f) b.carry_ms += dt_ms;

    bool pushed = false;
    // dt 大于 ODF 周期时（氛围档 46.44ms）一帧要产出两个样本，两个都填 pending。
    // 补零会在 ODF 里造出伪造的静音，把自相关的峰削掉一半。
    while (b.carry_ms >= kOdfPeriodMs) {
        b.odf[b.odf_head] = b.pending;
        b.odf_head = (uint16_t)((b.odf_head + 1) % kOdfLen);
        if (b.odf_count < 0xFFFF) ++b.odf_count;
        b.carry_ms -= kOdfPeriodMs;
        pushed = true;
    }
    if (pushed) b.pending = 0.0f;

    // 自相关每秒一次即可（§3.4：258 点 O(n²) 约 66k 次乘加，S3 上不到 1ms）
    if (!b.has_analyzed || elapsedAtLeast(now_ms, b.last_analyze, c.analyze_ms)) {
        beatAnalyze(b, c);
        b.last_analyze = now_ms;
        b.has_analyzed = true;
    }

    if (!b.locked || !(b.period_ms > 0.0f)) { b.has_phase = false; return; }

    // 相位：先把预测拍点推到未来，再用 onset 做 PLL 微调
    if (!b.has_phase) { b.next_beat = now_ms + (uint32_t)b.period_ms; b.has_phase = true; }

    // 把已经过去的预测点推到未来。
    //
    // ⚠️ 这里**不能**用 elapsedAtLeast(next_beat, now_ms, 0)：它展开是
    // `(uint32_t)(next_beat - now_ms) >= 0`，对无符号数恒为真，循环一次都不会跑。
    // 第一版就是这么写的，`next_beat` 从此一路留在过去。
    //
    // 「时刻在过去还是未来」问的是差值的**符号**，与 docs/17 第 18 条讲的
    // 「间隔是否超过某个阈值」是两件事：后者必须无符号（差值可以很大），
    // 前者必须有符号（差值的正负才是答案）。这里差值不超过一个拍周期，
    // 离 2^31 远得很，转 int32 安全。
    while ((int32_t)(b.next_beat - now_ms) < 0)
        b.next_beat += (uint32_t)b.period_ms;

    if (is_onset) {
        // onset 到上一个/下一个预测拍点的偏差，折算到 [-T/2, T/2]
        float err = (float)(int32_t)(now_ms - b.next_beat);
        const float T = b.period_ms;
        err = fmodf(err, T);
        if (err >  0.5f * T) err -= T;
        if (err < -0.5f * T) err += T;
        b.next_beat = (uint32_t)((float)b.next_beat + c.pll_gain * err);
    }
}

// 相位 [0,1)，0 是拍点。ahead_ms 把时间往前推，用来抵消流水线延迟（§3.5）。
inline float beatPhaseAhead(const BeatTracker &b, uint32_t now_ms, float ahead_ms) {
    if (!b.has_phase || !(b.period_ms > 0.0f)) return 0.0f;
    const float T = b.period_ms;
    // next_beat 之前 T 毫秒是上一个拍点；相位 = 距上一个拍点的比例
    float since = (float)(int32_t)(now_ms - (b.next_beat - (uint32_t)T)) + ahead_ms;
    float ph = fmodf(since / T, 1.0f);
    if (ph < 0.0f) ph += 1.0f;
    if (!(ph >= 0.0f && ph < 1.0f)) return 0.0f;     // 含 NaN
    return ph;
}

} // namespace lamp
