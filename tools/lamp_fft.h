// 主机端 FFT（radix-2 迭代，原地）。
//
// **这不是固件代码。** 目标板上 FFT 由 esp-dsp 提供 —— lamp_bands.h 开头就划清了
// 这条边界：我们拥有加窗与频段映射，不拥有 FFT。这个文件只是主机侧的替代品，
// 放在 tools/ 而不是 usermods/lamp/ 就是为了让这件事一眼可见。
//
// 为什么不继续用朴素 DFT：离线跑没问题，实时不行。N=2048 时朴素 DFT 是 210 万次
// 运算约 36ms，而帧间隔只有 46ms —— 没有余量。radix-2 是 N·log2(N) = 22528 次
// 蝶形，快约 90 倍。
#pragma once

#include <stddef.h>
#include <math.h>

namespace lamp {

// n 必须是 2 的幂。re/im 原地变换。
inline void fftRadix2(float *re, float *im, size_t n) {
    if (n < 2 || (n & (n - 1)) != 0) return;

    // 位反转置换
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    // 蝶形。旋转因子用递推而非每次调 sin/cos —— 后者在 N=2048 时是
    // 两万次三角函数调用，比蝶形本身还贵。
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -6.283185307179586 / (double)len;
        const double wr = cos(ang), wi = sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k, b = i + k + len / 2;
                const double xr = re[b] * cr - im[b] * ci;
                const double xi = re[b] * ci + im[b] * cr;
                re[b] = (float)(re[a] - xr); im[b] = (float)(im[a] - xi);
                re[a] = (float)(re[a] + xr); im[a] = (float)(im[a] + xi);
                const double nr = cr * wr - ci * wi;   // 旋转因子递推
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

// 实数序列 → 幅度谱（n/2+1 点）。x 会被复制，不改动。
// win 为空时不加窗。
inline void magnitudeSpectrum(const float *x, const float *win, size_t n,
                              float *re, float *im, float *mag) {
    for (size_t i = 0; i < n; ++i) {
        re[i] = win ? x[i] * win[i] : x[i];
        im[i] = 0.0f;
    }
    fftRadix2(re, im, n);
    for (size_t k = 0; k <= n / 2; ++k)
        mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
}

} // namespace lamp
