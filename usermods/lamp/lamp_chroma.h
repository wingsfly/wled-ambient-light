// 色度与调性（音高维度）。
//
// 在这个文件之前，整条链只回答一个问题：「现在有多响、鼓点在哪」。
// 同一首歌换成纯节拍器，画面不会有本质区别 —— 因为它不知道旋律。
// Chroma 是打开音高维度的钥匙，而且几乎免费：把已经算好的幅度谱按半音
// 折叠成 12 个音级，一次 O(bins) 遍历，不需要第二次 FFT。
//
// 有了它才谈得上「跟着和弦走」而不是「跟着鼓点闪」。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_window.h"
#include "lamp_bands.h"

namespace lamp {

constexpr int kChroma = 12;

// 折叠的频率范围。
// 下限 65Hz（C2）：再低的话一个 bin 跨好几个半音，折出来是糊的。
// 上限 2100Hz（约 C7）：再高谐波密度太大，基音被自己的泛音淹掉。
constexpr float kChromaLoHz = 65.0f;
constexpr float kChromaHiHz = 2100.0f;

// 频率 → 音级（0=C, 1=C#, …, 11=B）。440Hz 是 A4，MIDI 69，音级 9。
inline int pitchClassOf(float hz) {
    if (!(hz > 0.0f)) return -1;
    const float midi = 69.0f + 12.0f * log2f(hz / 440.0f);
    int pc = (int)lrintf(midi) % 12;
    if (pc < 0) pc += 12;
    return pc;
}

// 幅度谱 → 12 维色度，L∞ 归一化到 [0,1]。
//
// 用**功率**（幅度平方）累加而不是幅度：谱峰在功率域更突出，
// 折叠后主音与噪声的对比更清楚。
//
// 归一化是必须的：不归一化的话 chroma 跟着音量整体缩放，
// 后面的调性相关虽然对缩放不敏感，但灯效直接用就会变成又一个音量表。
inline void computeChroma(const float *mag, size_t n, float *out) {
    for (int i = 0; i < kChroma; ++i) out[i] = 0.0f;
    if (!mag || n < 4) return;

    const float df = kSampleRate / (float)n;
    const size_t k0 = (size_t)(kChromaLoHz / df + 0.5f);
    size_t k1 = (size_t)(kChromaHiHz / df + 0.5f);
    if (k1 > n / 2) k1 = n / 2;

    for (size_t k = (k0 < 1 ? 1 : k0); k <= k1; ++k) {
        const float m = mag[k];
        if (!isfinite(m) || m <= 0.0f) continue;
        const int pc = pitchClassOf((float)k * df);
        if (pc >= 0) out[pc] += m * m;
    }
    float mx = 0.0f;
    for (int i = 0; i < kChroma; ++i) if (out[i] > mx) mx = out[i];
    if (mx > 0.0f) for (int i = 0; i < kChroma; ++i) out[i] /= mx;
}

// ── 调性 ──────────────────────────────────────────────────

// Krumhansl-Schmuckler 的调性感知权重（1982 年那组实验测出来的）。
// 索引是相对主音的音级：0=主音, 4=大三度, 7=五度…
constexpr float kKsMajor[kChroma] = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
constexpr float kKsMinor[kChroma] = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

struct KeyEstimate {
    int   root     = -1;      // 0=C … 11=B；-1 表示没结论
    bool  is_major = true;
    float conf     = 0.0f;    // [0,1]，最佳与次佳的差距
};

// 皮尔逊相关。模板旋转 root 个半音后与 chroma 比对。
inline float ksCorrelate(const float *chroma, const float *tmpl, int root) {
    float mx = 0.0f, my = 0.0f;
    for (int i = 0; i < kChroma; ++i) { mx += chroma[(i + root) % kChroma]; my += tmpl[i]; }
    mx /= kChroma; my /= kChroma;
    float num = 0.0f, dx = 0.0f, dy = 0.0f;
    for (int i = 0; i < kChroma; ++i) {
        const float a = chroma[(i + root) % kChroma] - mx;
        const float b = tmpl[i] - my;
        num += a * b; dx += a * a; dy += b * b;
    }
    const float den = sqrtf(dx * dy);
    return (den > 1e-9f) ? (num / den) : 0.0f;
}

// 24 个候选（12 大调 + 12 小调）里挑最像的。
//
// 置信度取**最佳与次佳之差**，不是最佳的绝对值。绝对相关值对任何有调性的
// 音乐都偏高，区分不出「确实是 C 大调」和「C 大调与 a 小调难分伯仲」；
// 而白噪声的 24 个候选彼此接近，差值自然就低。
inline KeyEstimate estimateKey(const float *chroma) {
    KeyEstimate e;
    float sum = 0.0f;
    for (int i = 0; i < kChroma; ++i) {
        if (!isfinite(chroma[i])) return e;
        sum += chroma[i];
    }
    if (!(sum > 0.05f)) return e;              // 近乎静音，不猜

    float best = -2.0f, second = -2.0f;
    for (int r = 0; r < kChroma; ++r) {
        for (int m = 0; m < 2; ++m) {
            const float c = ksCorrelate(chroma, m ? kKsMinor : kKsMajor, r);
            if (c > best) { second = best; best = c; e.root = r; e.is_major = (m == 0); }
            else if (c > second) second = c;
        }
    }
    if (e.root < 0) return e;
    float d = best - second;
    if (!(d > 0.0f)) d = 0.0f;
    e.conf = (d > 0.25f) ? 1.0f : d / 0.25f;   // 0.25 的差距就算很确定了
    return e;
}

// 相邻两帧色度的距离，[0,1]。和弦一换就会跳。
//
// 用余弦距离而不是欧氏：两帧整体音量不同（渐强渐弱）时欧氏会误报，
// 而我们要的是「音级构成变了没有」，那是方向而不是长度。
inline float chromaDistance(const float *a, const float *b) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < kChroma; ++i) {
        if (!isfinite(a[i]) || !isfinite(b[i])) return 0.0f;
        dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i];
    }
    const float den = sqrtf(na * nb);
    if (!(den > 1e-9f)) return 0.0f;
    float cs = dot / den;
    if (cs > 1.0f) cs = 1.0f; else if (cs < -1.0f) cs = -1.0f;
    return 1.0f - cs;
}

} // namespace lamp
