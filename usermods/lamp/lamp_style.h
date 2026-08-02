// 风格档位与自动切换（设计文档 §3.3.1 / §3.3.3）。
//
// 四个判据（onset 密度、BPM+置信度、频谱质心、频谱平坦度）先合成一个
// **速度需求**标量，再量化成四个档位之一。两步分开有两个好处：
// 判据怎么加权与档位边界在哪里可以各自调；标量本身也是个能直接看的量。
//
// 切换纪律与 §3.1 的音频源仲裁同一模式：手动锁定最高、候选需连续胜出、
// 时间比较一律走回绕安全的 elapsedAtLeast。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_window.h"
#include "lamp_time.h"

namespace lamp {

// 序号按「越快越大」排列 —— quantize 的阈值比较依赖这个次序。
enum StylePreset : uint8_t {
    STYLE_AMBIENT    = 0,   // 弦乐、人声、慢曲
    STYLE_GENERAL    = 1,   // 流行、摇滚（默认）
    STYLE_EDM        = 2,   // 打击强、BPM 高
    STYLE_PERCUSSIVE = 3,   // 快速瞬态，放弃频率精度
    STYLE_COUNT      = 4,   // 兼作「未锁定」的哨兵：它不是合法档位，不会与真值撞车
};

struct PresetParams {
    uint16_t   n;
    WindowType wt;
    uint16_t   hop;
};

// 设计 §3.3.1 的表。约束 H ≥ N/8：大 N 配极小 hop 两头不讨好 ——
// 93ms 的涂抹每秒采样 172 次，绝大部分是冗余，CPU 却翻八倍。
inline PresetParams presetParams(StylePreset p) {
    switch (p) {
        case STYLE_AMBIENT:    return {2048, WIN_BLACKMAN_HARRIS, 1024};
        case STYLE_EDM:        return {1024, WIN_HANN,             256};
        case STYLE_PERCUSSIVE: return { 512, WIN_HANN,             128};
        case STYLE_GENERAL:
        default:               return {1024, WIN_HANN,             512};
    }
}

// 帧间隔，毫秒。包络与 AGC 切档时要用它 retime（见 lamp_envelope.h）。
inline float presetHopMs(StylePreset p) {
    return (float)presetParams(p).hop * 1000.0f / kSampleRate;
}

// ── 速度需求 ──────────────────────────────────────────────

struct StyleFeatures {
    float onset_rate  = 0.0f;    // 每秒 onset 数
    float bpm         = 0.0f;
    float bpm_conf    = 0.0f;    // 0..1
    float centroid_hz = 0.0f;
    float flatness    = 0.0f;    // 0..1，谐波型接近 0、噪声型接近 1
};

struct StyleConfig {
    // 权重之和为 1。onset 密度是主判据 —— 它直接量「打击有多密」，
    // 而档位的本质区别正是需要多快的瞬态响应。
    float w_onset    = 0.45f;
    float w_bpm      = 0.25f;
    float w_centroid = 0.20f;
    float w_flatness = 0.10f;

    float thresholds[3] = {0.30f, 0.55f, 0.78f};   // AMBIENT|GENERAL|EDM|PERCUSSIVE
    float hysteresis    = 0.05f;

    uint32_t hold_ms  = 3000;    // 新档位需连续胜出
    uint32_t dwell_ms = 8000;    // 距上次切换
    StylePreset manual_lock = STYLE_COUNT;   // COUNT = 未锁定
};

inline float clamp01(float v) {
    if (!(v > 0.0f)) return 0.0f;     // 含 NaN
    return v > 1.0f ? 1.0f : v;
}

inline float speedDemand(const StyleConfig &c, const StyleFeatures &f) {
    const float onset = clamp01(f.onset_rate / 8.0f);

    // 置信度低时 BPM 项退回**中性值 0.5**，不是退回 0。
    // 退回 0 等于断言「没节奏就是慢曲」，但白噪声和纯人声都没有稳定节奏，
    // 前者该快后者该慢 —— BPM 在这种时候本就不该有话语权，而不是投反对票。
    const float conf  = clamp01(f.bpm_conf);
    const float bpm   = conf * clamp01((f.bpm - 60.0f) / 120.0f) + (1.0f - conf) * 0.5f;

    // 质心按对数归一化：200Hz–6kHz 对应 0–1。听感上倍频程才是等距的。
    const float cent  = (f.centroid_hz > 200.0f && isfinite(f.centroid_hz))
                      ? clamp01(log2f(f.centroid_hz / 200.0f) / 4.9f) : 0.0f;

    const float flat  = clamp01(f.flatness);

    return clamp01(c.w_onset * onset + c.w_bpm * bpm
                 + c.w_centroid * cent + c.w_flatness * flat);
}

// ── 量化（带滞回）───────────────────────────────────────

// 需求值 → 档位。滞回让**升档要多走 hysteresis、降档要多退 hysteresis**。
//
// 这层滞回不是装饰。下面 styleUpdate 要求候选连续胜出 3 秒，而需求值在阈值上下
// 抖动时候选会一直变，永远凑不满 3 秒 —— 音乐明明变了却不切档，比抖动更糟。
inline StylePreset quantize(const StyleConfig &c, float demand, StylePreset current) {
    int level = 0;
    for (int i = 0; i < STYLE_COUNT - 1; ++i) {
        // 已经在这条边界之上时，要跌破 (th - hyst) 才算下来；否则要越过 (th + hyst) 才算上去
        const bool above_now = ((int)current > i);
        const float edge = c.thresholds[i] + (above_now ? -c.hysteresis : c.hysteresis);
        if (demand >= edge) level = i + 1;
    }
    return (StylePreset)level;
}

// ── 切换纪律 ──────────────────────────────────────────────

struct StyleSelector {
    StylePreset current    = STYLE_GENERAL;
    StylePreset candidate  = STYLE_GENERAL;
    uint32_t    cand_since = 0;
    uint32_t    last_switch = 0;
    // 独立标志位而不是拿 last_switch==0 当哨兵：0 是合法的 millis 值，
    // 回绕后真的会走到那里，那时首切判定会被误触发。
    bool        has_switched = false;
};

// 返回本次是否发生了档位切换。切换时调用方要做 §3.3.4 的三件事：
// 重算频段归一化（已由 analysisInit 覆盖）、保留 BPM 锁定只重置自适应阈值、
// 300ms 交叉淡入。
inline bool styleUpdate(StyleSelector &s, const StyleConfig &c,
                        const StyleFeatures &f, uint32_t now_ms) {
    const StylePreset want = quantize(c, speedDemand(c, f), s.current);

    // 候选跟踪照常进行，**手动锁定期间也不停**：解锁那一刻要有一个新鲜的候选可用，
    // 否则会先用一个几分钟前的过时判断，再花 3 秒纠正过来。
    if (want != s.candidate) { s.candidate = want; s.cand_since = now_ms; }

    if (c.manual_lock < STYLE_COUNT) {
        if (s.current == c.manual_lock) return false;
        s.current      = c.manual_lock;
        s.last_switch  = now_ms;
        s.has_switched = true;
        return true;
    }

    if (s.candidate == s.current) return false;
    if (!elapsedAtLeast(now_ms, s.cand_since, c.hold_ms)) return false;
    // 首次切换不受驻留限制 —— 否则开机要空等 8 秒才肯离开默认档
    if (s.has_switched && !elapsedAtLeast(now_ms, s.last_switch, c.dwell_ms)) return false;

    s.current      = s.candidate;
    s.last_switch  = now_ms;
    s.has_switched = true;
    return true;
}

} // namespace lamp
