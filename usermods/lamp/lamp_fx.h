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

namespace lamp {

enum FxId : uint8_t {
    FX_SPECTRUM_BARS = 0,
    FX_BEAT_PULSE    = 1,
    FX_LEVEL_SWEEP   = 2,
    FX_COUNT         = 3,
};

inline const char *fxName(FxId f) {
    switch (f) {
        case FX_BEAT_PULSE:  return "beat-pulse";
        case FX_LEVEL_SWEEP: return "level-sweep";
        case FX_SPECTRUM_BARS:
        default:             return "spectrum-bars";
    }
}

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

// 统一入口。渲染后**统一施加白平衡** —— 各效果自己不碰它，
// 否则总有一个会忘，而忘了的那个偏色，看起来像效果设计得难看。
inline void fxRender(FxId id, const AudioFrame &f, const Geometry &g,
                     bool white_balance, Rgb *out) {
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) out[i] = Rgb{0, 0, 0};
    switch (id) {
        case FX_BEAT_PULSE:  fxBeatPulse(f, g, out);    break;
        case FX_LEVEL_SWEEP: fxLevelSweep(f, g, out);   break;
        case FX_SPECTRUM_BARS:
        default:             fxSpectrumBars(f, g, out); break;
    }
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        out[i] = applyWhiteBalance(out[i], white_balance);
}

} // namespace lamp
