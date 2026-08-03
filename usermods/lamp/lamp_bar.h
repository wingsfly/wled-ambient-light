// 小节与强拍（downbeat）。
//
// BeatTracker 回答「下一拍在什么时候」，回答不了「哪一拍是第一拍」。
// 而音乐的结构感几乎全在小节上：电子的 8/16 小节 build、流行的四小节乐句、
// 华尔兹的强弱弱。只有拍点的灯效永远是均匀闪烁，跟不上这些。
//
// 判据是最朴素的那个，也是实践中最稳的：**底鼓落在第一拍**。
// 按拍号候选各开一组累加器，每来一拍就把这一拍的重音记到对应格子里，
// 攒够几十拍之后哪一格最突出，哪一格就是强拍。
//
// ── 两处与本项目其它模块相反的选择，都是有理由的 ──
//
// **遗忘按「拍」而不是按毫秒。** 别处的时间常数一律用物理时间
// （见 lamp_envelope.h 的 envCoeff），这里刻意不是：小节结构的自然单位就是
// 拍。同样是「记住最近 32 拍」，60BPM 下是 32 秒、180BPM 下是 10.7 秒 ——
// 该记住的是「多少个小节」，不是「多少秒」。
//
// **偏向 4/4。** 候选格子越少，随机数据也越容易凑出高对比度 —— 3 格天然
// 比 4 格占便宜。所以 3/4 要**明显**赢过 4/4 才采信，不然绝大多数四拍子的
// 曲子会被判成三拍子。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

namespace lamp {

constexpr int kBarMaxBeats = 4;      // 支持的最大拍号
constexpr int kBarCands    = 2;      // 候选拍号个数

struct BarConfig {
    // 候选拍号。4 在前 —— 它是默认，3 要明显赢才换。
    int   beats[kBarCands] = {4, 3};

    // 累加器的遗忘长度，单位是**拍**。32 拍 = 8 小节，够盖住一个乐句。
    float forget_beats = 32.0f;

    // 3/4 要比 4/4 的对比度高出这么多才采信（见文件头）。
    float three_margin = 0.15f;

    // 置信度满格所需的对比度。(峰 − 均)/峰，四拍里只有一拍有重音时是 0.75。
    float conf_full = 0.45f;

    // 起步保护：攒够这些拍之前不下结论，否则前两拍就会武断地定强拍。
    int   warmup_beats = 8;
};

struct BarState {
    float acc[kBarCands][kBarMaxBeats] = {{0}};
    int   beat_ix   = 0;        // 锁定以来的第几拍
    int   bar_ix    = 0;        // 第几小节，供乐句级效果用
    float prev_phase = 0.0f;
    bool  has_phase = false;

    int   beats_per_bar = 4;
    int   offset        = 0;    // 强拍落在 beat_ix % beats_per_bar == offset
    int   pos           = 0;    // 当前在小节里的第几拍，0 = 强拍
    float conf          = 0.0f;
    bool  beat          = false;  // 本帧跨过了一个拍点
    bool  downbeat      = false;  // 且那是强拍
};

inline void barInit(BarState &s) { s = BarState{}; }

// 一组累加器的对比度与峰位。(峰 − 均)/峰，[0,1)。
// 用峰做分母而不是均值：均值做分母时，某一格接近 0 会让比值炸到无穷。
inline float barContrast(const float *acc, int m, int &peak_ix) {
    peak_ix = 0;
    if (m < 2) return 0.0f;
    float peak = acc[0], sum = 0.0f;
    for (int i = 0; i < m; ++i) {
        sum += acc[i];
        if (acc[i] > peak) { peak = acc[i]; peak_ix = i; }
    }
    if (!(peak > 1e-9f)) return 0.0f;
    const float mean = sum / (float)m;
    float c = (peak - mean) / peak;
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    return c;
}

// 喂一帧。
//   phase   —— BeatTracker 的拍内相位 [0,1)，跨过 1 → 0 就是一个拍点
//   locked  —— 没锁上节拍时整个模块停摆：拍都不准，谈何强拍
//   accent  —— 这一拍的重音强度。调用方给低频段能量（底鼓落在第一拍）
inline void barUpdate(BarState &s, const BarConfig &c,
                      float phase, bool locked, float accent) {
    s.beat = s.downbeat = false;
    if (!locked || !isfinite(phase)) { s.has_phase = false; return; }
    if (!isfinite(accent) || accent < 0.0f) accent = 0.0f;

    if (!s.has_phase) { s.prev_phase = phase; s.has_phase = true; return; }
    const float prev = s.prev_phase;
    s.prev_phase = phase;
    if (!(phase < prev)) return;          // 没跨过拍点
    s.beat = true;
    ++s.beat_ix;

    // 遗忘 + 累加。遗忘因子是「每拍」的，见文件头。
    const float keep = expf(-1.0f / ((c.forget_beats > 0.5f) ? c.forget_beats : 0.5f));
    for (int m = 0; m < kBarCands; ++m) {
        const int bpb = c.beats[m];
        if (bpb < 2 || bpb > kBarMaxBeats) continue;
        for (int i = 0; i < bpb; ++i) s.acc[m][i] *= keep;
        s.acc[m][s.beat_ix % bpb] += accent;
    }

    if (s.beat_ix < c.warmup_beats) return;

    // 挑拍号：4/4 是默认，3/4 要明显赢才换。
    int  ix4 = 0, ix3 = 0;
    const float c4 = barContrast(s.acc[0], c.beats[0], ix4);
    const float c3 = barContrast(s.acc[1], c.beats[1], ix3);
    const bool  three = (c3 > c4 + c.three_margin);
    const int   bpb   = three ? c.beats[1] : c.beats[0];
    const float best  = three ? c3 : c4;

    // 换拍号时小节计数归零 —— 继续沿用旧的计数会让乐句级效果错位。
    if (bpb != s.beats_per_bar) { s.beats_per_bar = bpb; s.bar_ix = 0; }
    s.offset = three ? ix3 : ix4;

    float cf = (c.conf_full > 1e-6f) ? best / c.conf_full : 0.0f;
    if (cf > 1.0f) cf = 1.0f;
    s.conf = cf;

    int p = (s.beat_ix - s.offset) % bpb;
    if (p < 0) p += bpb;
    s.pos = p;
    if (p == 0) { s.downbeat = true; ++s.bar_ix; }
}

} // namespace lamp
