// 起音检测（设计文档 §3.4 阶段 2 的前半）。
//
//   16 段能量 → spectral flux → 自适应阈值 → onset → onset 速率
//
// 后半（6 秒窗自相关求周期、锁相跟拍）在 lamp_beat.h。
//
// flux 取自**已经跨档归一化的 16 段能量**（lamp_bands.h），不是原始频谱。
// 这样 flux 的分子已经与 (N, 窗) 无关，只剩帧间隔那一个尺度因子要处理。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_bands.h"
#include "lamp_envelope.h"   // envCoeff
#include "lamp_time.h"

namespace lamp {

// Spectral flux：相邻两帧 16 段能量的**正向**差之和，再除以帧间隔。
//
// 半波整流的理由：负向变化是音符衰减，不是起音。把它算进去会让长音的尾巴
// 和真正的击打混在一起。
//
// 除以 Δt 的理由：差分的大小天然正比于帧间隔，hop 减半则同一段音乐的每帧
// 差分也减半。设计 §3.3.4 第 2 条说「换档后失效的只是 flux 的尺度」——
// 除掉 Δt 就把那个尺度也消掉了，换档后自适应阈值只需微调而不是从头爬。
inline float spectralFlux(const float *prev, const float *cur, float dt_ms) {
    if (!(dt_ms > 0.0f)) return 0.0f;          // 含 NaN
    float s = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float d = cur[i] - prev[i];
        if (isfinite(d) && d > 0.0f) s += d;   // 单段的 NaN 只丢那一段
    }
    return s * 1000.0f / dt_ms;                // 每秒变化量
}

struct OnsetConfig {
    // 阈值 = 滑动均值 × mult + delta。
    // delta 是绝对偏置，负责静音时不对底噪敏感 —— 只靠乘性阈值的话，
    // 均值趋零时阈值也趋零，底噪抖动就会被当成起音。
    float    mult          = 1.6f;
    float    delta         = 0.30f;
    float    mean_tau_ms   = 400.0f;    // 滑动均值的时间常数
    uint32_t refractory_ms = 60;        // 两个 onset 的最小间隔（60ms ≈ 1000BPM）
    float    rate_tau_ms   = 3000.0f;   // onset 速率的平滑
};

struct OnsetDetector {
    float    prev[NUM_BANDS] = {0};
    bool     has_prev        = false;
    float    mean            = 0.0f;    // flux 的滑动均值
    float    rate            = 0.0f;    // 每秒 onset 数
    bool     armed           = false;   // 上一帧是否已在阈值之上（上升沿判据）
    uint32_t last_onset_ms   = 0;
    bool     has_onset       = false;   // 独立标志位，不拿 last_onset_ms==0 当哨兵
    float    a_mean = 0.0f, a_rate = 0.0f;
    float    dt_ms  = 0.0f;
};

// 不应期是否已过。单独成函数是为了能直接测 —— 它藏在 onsetUpdate 里的话，
// 「间隔 27.9 天」这种场景要靠跑几百万帧才能触达。
inline bool refractoryExpired(const OnsetDetector &d, const OnsetConfig &c, uint32_t now_ms) {
    if (!d.has_onset) return true;
    return elapsedAtLeast(now_ms, d.last_onset_ms, c.refractory_ms);
}

// 换档：只换系数，**保留 rate、prev、mean**。
//
// §3.3.4 第 2 条：全量重新锁定要 6 秒，用户会看到灯「发呆」。
// flux 已按 Δt 归一化，所以 mean 的**尺度**换档后仍然成立，不必清零；
// 需要换的只是两个 IIR 系数。
inline void onsetRetime(OnsetDetector &d, const OnsetConfig &c, float dt_ms) {
    d.dt_ms  = dt_ms;
    d.a_mean = envCoeff(c.mean_tau_ms, dt_ms);
    d.a_rate = envCoeff(c.rate_tau_ms, dt_ms);
}

inline void onsetInit(OnsetDetector &d, const OnsetConfig &c, float dt_ms) {
    for (int i = 0; i < NUM_BANDS; ++i) d.prev[i] = 0.0f;
    d.has_prev = false;
    d.mean = 0.0f; d.rate = 0.0f;
    d.armed = false; d.has_onset = false; d.last_onset_ms = 0;
    onsetRetime(d, c, dt_ms);
}

// 喂一帧 16 段能量，返回本帧是否是一个 onset。
inline bool onsetUpdate(OnsetDetector &d, const OnsetConfig &c,
                        const float *bands, uint32_t now_ms) {
    if (!d.has_prev) {
        for (int i = 0; i < NUM_BANDS; ++i) d.prev[i] = bands[i];
        d.has_prev = true;
        return false;                       // 没有前一帧可比，绝不凭空报
    }

    const float flux = spectralFlux(d.prev, bands, d.dt_ms);
    for (int i = 0; i < NUM_BANDS; ++i) d.prev[i] = bands[i];

    const float threshold = d.mean * c.mult + c.delta;
    // 均值在阈值判定**之后**更新，否则本帧的尖峰会先把自己的阈值抬高
    d.mean += d.a_mean * (flux - d.mean);

    bool fired = false;
    if (flux > threshold) {
        // 上升沿 + 不应期：一个起音的上升沿会连续几帧超阈值，只认第一帧
        if (!d.armed && refractoryExpired(d, c, now_ms)) {
            fired = true;
            d.last_onset_ms = now_ms;
            d.has_onset = true;
        }
        d.armed = true;
    } else {
        d.armed = false;
    }

    // 速率：本帧是 onset 则瞬时速率为 1/Δt（每秒个数），否则 0，再做 IIR 平均。
    // 除以 Δt 让结果是「每秒」而不是「每帧」—— 这是跨 hop 一致的关键，
    // 而它正是 StyleFeatures 的主判据，漂了会让档位切换自激。
    const float instant = fired ? (1000.0f / d.dt_ms) : 0.0f;
    d.rate += d.a_rate * (instant - d.rate);

    return fired;
}

} // namespace lamp
