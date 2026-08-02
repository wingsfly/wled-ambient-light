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

constexpr int   NUM_BANDS   = 16;
constexpr float kSampleRate = 22050.0f;

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

} // namespace lamp
