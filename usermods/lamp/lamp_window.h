// 分析窗（设计文档 §3.3.2）。
//
// 用**周期性**定义 w[n] = f(2πn/N) 而不是对称定义 f(2πn/(N-1))。
// 谱分析必须用周期定义：对称窗在 DFT 下引入半个 bin 的偏差。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <math.h>

namespace lamp {

// 整条音频管线的采样率。放在这里而不是 lamp_bands.h：频段映射、档位 hop 换算、
// 将来的 BeatTracker 都要用它，它不属于其中任何一个。
// 22.05kHz 与 WLED 上游 AudioReactive 的默认值一致（设计 §3.2）。
constexpr float kSampleRate = 22050.0f;

enum WindowType : uint8_t {
    WIN_HANN            = 0,  // 主瓣窄，旁瓣 −31dB。频段总能量用它，默认
    WIN_BLACKMAN_HARRIS = 1,  // 主瓣约 2× 宽，旁瓣 −92dB。单峰定位用它
    WIN_FLATTOP         = 2,  // 主瓣最宽，幅度精度 ±0.01dB。仅手动模式
};

inline void fillWindow(WindowType type, float *w, size_t n) {
    const double k = 6.283185307179586 / (double)n;   // 2π/N，周期定义
    for (size_t i = 0; i < n; ++i) {
        const double x = k * (double)i;
        switch (type) {
            case WIN_BLACKMAN_HARRIS:
                w[i] = (float)(0.35875 - 0.48829 * cos(x)
                             + 0.14128 * cos(2*x) - 0.01168 * cos(3*x));
                break;
            case WIN_FLATTOP:
                // 5 项 flat-top。负瓣是它的固有特性，不是错误。
                w[i] = (float)(0.21557895 - 0.41663158 * cos(x)
                             + 0.277263158 * cos(2*x) - 0.083578947 * cos(3*x)
                             + 0.006947368 * cos(4*x));
                break;
            case WIN_HANN:
            default:
                w[i] = (float)(0.5 - 0.5 * cos(x));
                break;
        }
    }
}

// 相干增益 mean(w)。**用于单音幅度**（FFT_MajorPeak 那条路）。
// Hann 0.500 · Blackman-Harris 0.359 · Flat-Top 0.216
inline float coherentGain(const float *w, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += w[i];
    return (float)(s / (double)n);
}

// 噪声功率增益 mean(w²)。**用于宽带能量**——频段归一化用这个。
// Hann 0.375 · Blackman-Harris 0.258 · Flat-Top 0.175
//
// 用相干增益去归一化频段能量是错的：Hann 与 BH 之间会差 0.5²/0.359² = 1.94 倍，
// 远超 §3.3.4 定的 5% 跨档连续性验收线。
inline float noisePowerGain(const float *w, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += (double)w[i] * (double)w[i];
    return (float)(s / (double)n);
}

} // namespace lamp
