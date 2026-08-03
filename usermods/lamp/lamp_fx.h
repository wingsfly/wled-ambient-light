// 灯效（设计文档 §5 的最小起点）。
//
// ⚠️ 这只是**三个示例效果**，不是最终的特效集。它们存在的理由是让模拟页面
// 有东西可画、让 AudioFrame 的每个字段都有一个消费者 —— 真正的效果设计是
// 阶段 4 的事。
//
// 三个效果各自只吃 AudioFrame 的一部分，合起来把主要字段都覆盖了：
//   spectrumBars  ← bands[16]           频段是否正确分层
//   beatPulse     ← phase / beat_locked 拍点是否对齐
//   levelSweep    ← rms_fast / peak     包络与峰值的动态
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_geometry.h"
#include "lamp_color.h"
#include "lamp_pipeline.h"
#include "lamp_envelope.h"   // envCoeff

namespace lamp {

enum FxId : uint8_t {
    FX_SPECTRUM_BARS = 0,
    FX_BEAT_PULSE    = 1,
    FX_LEVEL_SWEEP   = 2,
    FX_BAR_IMPACT    = 3,
    FX_BEAT_RUNNER   = 4,
    FX_SPLIT_BANDS   = 5,
    FX_COUNT         = 6,
};

inline const char *fxName(FxId f) {
    switch (f) {
        case FX_BEAT_PULSE:  return "beat-pulse";
        case FX_LEVEL_SWEEP: return "level-sweep";
        case FX_BAR_IMPACT:  return "bar-impact";
        case FX_BEAT_RUNNER: return "beat-runner";
        case FX_SPLIT_BANDS: return "split-bands";
        case FX_SPECTRUM_BARS:
        default:             return "spectrum-bars";
    }
}

// 有时间演化的效果需要状态。前三个效果是无状态的纯函数（同一帧输入永远画出
// 同一幅图），冲击/拖尾这类做不到 —— 它们的当前亮度取决于之前发生过什么。
struct FxState {
    float bar[NUM_BANDS] = {0};   // 每段的冲击包络
    float runner = 0.0f;          // 光点位置 [0,1)
    float prev_phase = 0.0f;
    bool  has_phase = false;
};

struct FxConfig {
    // 冲击的衰减长度 = 拍周期 × 这个系数。**不能写成固定毫秒**：
    // 174BPM 一拍只有 345ms，而 90BPM 有 667ms —— 固定值要么在快歌里糊成一片，
    // 要么在慢歌里早早熄灭。0.55 拍意味着落到 1/e 时下一拍还没到。
    float decay_beats   = 0.55f;
    float decay_free_ms = 260.0f;   // 没锁上节拍时的退路
    float runner_tail   = 0.28f;    // 光点拖尾长度，占管长的比例
};

// HSV→RGB，色相 [0,1)。特效常用色相环，写一次省得三处重复。
inline Rgb hsv(float h, float s, float v) {
    h -= floorf(h);
    if (!(s >= 0.0f)) s = 0.0f; if (s > 1.0f) s = 1.0f;
    if (!(v >= 0.0f)) v = 0.0f; if (v > 1.0f) v = 1.0f;
    const float c = v * s, x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0, g = 0, b = 0;
    const int seg = (int)(h * 6.0f) % 6;
    switch (seg) {
        case 0: r = c; g = x; break;   case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;   case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;   default: r = c; b = x; break;
    }
    return Rgb{(uint8_t)((r + m) * 255.0f + 0.5f),
               (uint8_t)((g + m) * 255.0f + 0.5f),
               (uint8_t)((b + m) * 255.0f + 0.5f)};
}

// 显示缩放。AGC 把**总** RMS 拉到 target=0.25，而能量分散在 16 段里，
// 单段落在 0.02–0.3 量级 —— 直接送进 perceptual() 会几乎全黑。
// 这个系数是经验值，真机点亮后要重新标定。
constexpr float kFxBandScale  = 6.0f;
constexpr float kFxLevelScale = 3.0f;

// 把 [0,1] 的能量压成亮度。感知亮度近似平方关系，直接线性映射会让
// 中低电平段全挤在一起看不出层次。
inline float perceptual(float e) {
    if (!(e > 0.0f)) return 0.0f;
    if (e > 1.0f) e = 1.0f;
    return e * e;
}

// ── 效果 ──────────────────────────────────────────────────

// 频段柱：把 16 段铺满一根管，色相按段号走色轮，亮度按该段能量。
// 两根管镜像，这样从正面看是对称的。
inline void fxSpectrumBars(const AudioFrame &f, const Geometry &g, Rgb *out) {
    for (int s = 0; s < 2; ++s) {
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u   = (float)i / (float)(LEDS_PER_TUBE - 1);
            const int   band = (int)(u * (NUM_BANDS - 1) + 0.5f);
            const float e    = perceptual(f.bands[band] * kFxBandScale);
            out[mapPixel(g, (Side)s, u)] = hsv((float)band / NUM_BANDS, 0.9f, e);
        }
    }
}

// 拍点脉冲：整管在拍点亮起，随相位衰减。未锁定时退回呼吸，
// 免得没节奏的音乐下灯完全不动 —— 那看起来像坏了。
inline void fxBeatPulse(const AudioFrame &f, const Geometry &g, Rgb *out) {
    const float env = f.beat_locked ? expf(-f.phase * 4.0f)
                                    : 0.35f + 0.25f * f.rms_fast;
    const float hue = f.beat_locked ? fmodf(f.bpm / 240.0f, 1.0f) : 0.55f;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.75f, perceptual(env));
        }
}

// 电平扫描：快包络决定填充高度，峰值留一条保持线。
// 这是最直观的「音量表」，用来眼看 AGC 是否把动态压在合理范围内。
inline void fxLevelSweep(const AudioFrame &f, const Geometry &g, Rgb *out) {
    const float lvl = perceptual(f.rms_fast * kFxLevelScale);
    const float pk  = perceptual(f.peak * kFxLevelScale);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            Rgb c{0, 0, 0};
            // 峰值保持线。pk 太小就不画 —— 否则静音时管底会留一段白，
            // 那看起来像「灯没关干净」而不是「没有信号」。
            if (pk > 0.02f && fabsf(u - pk) < 0.02f) c = Rgb{255, 255, 255};
            // 填充区间取左闭右开：写成 u <= lvl 的话，lvl=0（静音）时 u=0
            // 那一颗仍会亮，两根管各留一点绿。
            else if (u < lvl)                        c = hsv(0.33f - 0.33f * u, 0.95f, 1.0f);
            out[mapPixel(g, (Side)s, u)] = c;
        }
}

// ── 有状态的效果 ──────────────────────────────────────────

// 推进冲击包络。**上升沿瞬时跟随、衰减按拍周期缩放** —— 这两条合起来
// 才是「冲击峰的宽度和声音一致，同时跟得上 BPM」：
// 峰出现的时刻与高度完全由声音决定，而它落下去的快慢由节拍决定。
inline void fxAdvance(FxState &st, const FxConfig &c, const AudioFrame &f, float dt_ms) {
    if (!isfinite(dt_ms) || dt_ms <= 0.0f) return;

    const float decay_ms = (f.beat_locked && f.bpm > 1.0f)
                         ? (60000.0f / f.bpm) * c.decay_beats
                         : c.decay_free_ms;
    const float a = envCoeff(decay_ms, dt_ms);

    for (int i = 0; i < NUM_BANDS; ++i) {
        float e = f.bands[i] * kFxBandScale;
        if (!isfinite(e) || e < 0.0f) e = 0.0f;
        if (e > 1.0f) e = 1.0f;
        if (e > st.bar[i]) st.bar[i] = e;              // 冲高：立刻，不做平滑
        else               st.bar[i] += a * (e - st.bar[i]);   // 掉落：按拍
    }

    // 光点：锁上节拍时直接跟相位走，一拍跑完一趟；没锁上就匀速漂
    if (f.beat_locked) { st.runner = f.phase; st.has_phase = true; }
    else {
        st.runner += dt_ms / 1400.0f;
        if (st.runner >= 1.0f) st.runner -= 1.0f;
    }
    st.prev_phase = f.phase;
}

// 冲击柱：16 段各自一根，冲高瞬时、掉落跟拍。用户要的就是这个。
inline void fxBarImpact(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const int   band = (int)(u * (NUM_BANDS - 1) + 0.5f);
            // 段内位置：越靠段顶越暗，这样每段看起来是一根有高度的柱子
            const float within = u * (NUM_BANDS - 1) - (float)band + 0.5f;
            const float lvl = st.bar[band];
            float v = (within <= lvl) ? 1.0f : 0.0f;
            v *= perceptual(lvl);
            // 拍点整体提亮一档，让节奏在视觉上更实
            if (f.beat_locked) v *= 0.72f + 0.28f * expf(-f.phase * 5.0f);
            out[mapPixel(g, (Side)s, u)] = hsv((float)band / NUM_BANDS, 0.88f, v);
        }
}

// 拍点光点：每拍从管底发到管顶，带拖尾。速度直接由 BPM 决定。
inline void fxBeatRunner(const FxState &st, const FxConfig &c, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    const float hue = f.beat_locked ? fmodf(f.bpm / 240.0f, 1.0f) : 0.55f;
    // 没有信号就彻底熄灭。光点本身带 25% 的底光（否则安静段落里看不见节拍），
    // 但那个底光不该在真正的静音里还亮着 —— 那看起来像灯没关干净。
    if (!(f.rms_fast > 0.003f)) return;
    const float amp = perceptual(f.rms_fast * kFxLevelScale);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            float d = st.runner - u;              // 拖尾只朝身后
            if (d < 0.0f) d += 1.0f;
            const float tail = (d < c.runner_tail) ? (1.0f - d / c.runner_tail) : 0.0f;
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.8f, perceptual(tail) * (0.25f + 0.75f * amp));
        }
}

// 低频从底往上、高频从顶往下，在中间相遇。看的是频谱重心怎么移动。
inline void fxSplitBands(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    float lo = 0.0f, hi = 0.0f;
    for (int i = 0; i < 5; ++i)             lo += st.bar[i];
    for (int i = NUM_BANDS - 6; i < NUM_BANDS; ++i) hi += st.bar[i];
    lo /= 5.0f; hi /= 6.0f;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            Rgb c{0, 0, 0};
            if (u < lo * 0.5f)              c = hsv(0.02f, 0.95f, perceptual(lo));
            else if (u > 1.0f - hi * 0.5f)  c = hsv(0.55f, 0.9f,  perceptual(hi));
            out[mapPixel(g, (Side)s, u)] = c;
        }
    (void)f;
}

// 统一入口。渲染后**统一施加白平衡** —— 各效果自己不碰它，
// 否则总有一个会忘，而忘了的那个偏色，看起来像效果设计得难看。
inline void fxRender(FxId id, FxState &st, const FxConfig &c, const AudioFrame &f,
                     const Geometry &g, bool white_balance, float dt_ms, Rgb *out) {
    fxAdvance(st, c, f, dt_ms);      // 状态先推进，无状态的效果不受影响
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) out[i] = Rgb{0, 0, 0};
    switch (id) {
        case FX_BEAT_PULSE:  fxBeatPulse(f, g, out);         break;
        case FX_LEVEL_SWEEP: fxLevelSweep(f, g, out);        break;
        case FX_BAR_IMPACT:  fxBarImpact(st, f, g, out);     break;
        case FX_BEAT_RUNNER: fxBeatRunner(st, c, f, g, out); break;
        case FX_SPLIT_BANDS: fxSplitBands(st, f, g, out);    break;
        case FX_SPECTRUM_BARS:
        default:             fxSpectrumBars(f, g, out);      break;
    }
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        out[i] = applyWhiteBalance(out[i], white_balance);
}

} // namespace lamp
