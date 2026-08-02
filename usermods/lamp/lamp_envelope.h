// 响度包络与自动增益（设计文档 §3.2）。
//
// 快 ~30ms / 慢 ~2s 双时间常数 + 峰值 + AGC。
//
// 贯穿本文件的一条纪律：**所有时间常数按物理时间定义，不按帧数**。
// hop 随风格档位变（§3.3），把 IIR 系数写成常数就等于时间常数跟着档位漂，
// 用户看到的是「换了个档，灯的反应快慢也变了」。系数一律由 (τ, Δt) 现算。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stddef.h>
#include <math.h>

namespace lamp {

// 一帧 PCM 的 RMS。
inline float frameRms(const float *x, size_t n) {
    if (n == 0) return 0.0f;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += (double)x[i] * (double)x[i];
    return (float)sqrt(s / (double)n);
}

// 一阶 IIR 的系数：a = 1 − exp(−Δt/τ)。
//
// 这是零阶保持的**精确**离散化，不是常见的 Δt/τ 近似。近似式在 Δt 接近 τ 时
// 会算出 a>1，包络当场发散 —— 而 Δt 在「氛围档 2048/512」下是 23ms，
// 对 30ms 的快包络来说已经不算小了。
inline float envCoeff(float tau_ms, float dt_ms) {
    if (!(dt_ms > 0.0f)) return 0.0f;      // 含 NaN
    if (!(tau_ms > 0.0f)) return 1.0f;     // τ=0 → 直通
    return 1.0f - expf(-dt_ms / tau_ms);
}

// ── 包络 ──────────────────────────────────────────────────

struct EnvelopeConfig {
    float fast_tau_ms = 30.0f;     // 瞬态跟随，特效直接消费
    float slow_tau_ms = 2000.0f;   // 长期平均，AGC 的分母
    float peak_tau_ms = 400.0f;    // 峰值回落
};

struct Envelope {
    float fast = 0.0f, slow = 0.0f, peak = 0.0f;
    float a_fast = 0.0f, a_slow = 0.0f, a_peak = 0.0f;
    float dt_ms = 0.0f;
};

// 只换系数，**保留状态**。切档位时用这个。
//
// 丢状态的话包络归零，灯会在切档瞬间暗一下 —— 与 §3.3.4 第 2 条
// 「不要丢掉 BPM 锁定」是同一个道理：换的是观察方式，不是被观察的东西。
inline void envelopeRetime(Envelope &e, const EnvelopeConfig &c, float dt_ms) {
    e.dt_ms  = dt_ms;
    e.a_fast = envCoeff(c.fast_tau_ms, dt_ms);
    e.a_slow = envCoeff(c.slow_tau_ms, dt_ms);
    e.a_peak = envCoeff(c.peak_tau_ms, dt_ms);
}

inline void envelopeInit(Envelope &e, const EnvelopeConfig &c, float dt_ms) {
    e.fast = e.slow = e.peak = 0.0f;
    envelopeRetime(e, c, dt_ms);
}

// rms 为本帧的 RMS 响度。非有限值整帧丢弃 —— ADC 掉线或上游除零都可能送 NaN，
// 一旦渗进 IIR 状态就永远出不来了。
inline void envelopeUpdate(Envelope &e, float rms) {
    if (!isfinite(rms) || rms < 0.0f) return;
    e.fast += e.a_fast * (rms - e.fast);
    e.slow += e.a_slow * (rms - e.slow);
    // 峰值：瞬时攻击、指数回落
    if (rms > e.peak) e.peak = rms;
    else              e.peak += e.a_peak * (rms - e.peak);
}

// ── AGC ───────────────────────────────────────────────────

struct AgcConfig {
    float target     = 0.25f;      // 归一化后的目标 RMS
    float squelch    = 0.002f;     // 低于此视为静音
    float gate_hyst  = 0.5f;       // 关门阈 = squelch × 此值，开门阈 = squelch
    float gain_min   = 1.0f;
    float gain_max   = 40.0f;
    float attack_ms  = 120.0f;     // 增益**下降**（信号变响）
    float release_ms = 6000.0f;    // 增益**上升**（信号变轻）
};

struct Agc {
    float gain    = 1.0f;          // 从 1.0 起步，不是从 gain_max
    bool  gated   = false;
    float a_attack = 0.0f, a_release = 0.0f;
};

inline void agcRetime(Agc &g, const AgcConfig &c, float dt_ms) {
    g.a_attack  = envCoeff(c.attack_ms,  dt_ms);
    g.a_release = envCoeff(c.release_ms, dt_ms);
}

inline void agcInit(Agc &g, const AgcConfig &c, float dt_ms) {
    g.gain  = 1.0f;
    g.gated = false;
    agcRetime(g, c, dt_ms);
}

// slow_rms 取包络的慢支路。返回当前增益。
//
// 两件事分开做：
//   **门限**决定「这帧算不算有声音」，滞回防止阈值附近抖动导致灯闪。
//   **增益**只在未门限时更新 —— 静音时冻结，否则底噪会被一路放大到 gain_max，
//   安静的房间里灯反而开始闪，这正是 §4 验收表里那条「AGC 不失控放大底噪」。
//
// attack 必须远快于 release：变响要立刻压下去防削顶，变轻要慢慢放上来，
// 否则歌曲间隙那两秒增益就冲上去了，下一段主歌进来先炸一下。
inline float agcUpdate(Agc &g, const AgcConfig &c, float slow_rms) {
    if (!isfinite(slow_rms)) return g.gain;

    // 负的 slow_rms 不需要单独夹零：负数小于任何非负 squelch，下面的门限必先拦下。
    // 即便 squelch 被配成负数，want 的夹取也会把负增益顶回 gain_min。
    const float close_at = c.squelch * c.gate_hyst;
    if (g.gated) { if (slow_rms >= c.squelch) g.gated = false; }
    else         { if (slow_rms <  close_at)  g.gated = true;  }
    if (g.gated) return g.gain;

    // 除零保护。严格说它是冗余的：squelch=0 且 slow_rms=0 时 want 会是 +inf，
    // 而 inf > gain_max 成立，下面的夹取照样兜得住。留着是为了不让 inf 出现在
    // 中间量里 —— 它不掩盖任何其他检查，这一点与被删掉的「事后再夹一次 gain」不同。
    float want = c.target / (slow_rms > 1e-9f ? slow_rms : 1e-9f);
    if (want < c.gain_min) want = c.gain_min;
    if (want > c.gain_max) want = c.gain_max;

    // want 已夹进 [min,max]，且系数 a∈[0,1]（envCoeff 的值域），
    // 于是这一步是 gain 与 want 的**凸组合** —— 结果必然仍在 [min,max] 内。
    // 事后不再夹一次：那层冗余保护会把 want 的夹取失效掩盖成「输出还是对的」。
    g.gain += (want < g.gain ? g.a_attack : g.a_release) * (want - g.gain);
    return g.gain;
}

} // namespace lamp
