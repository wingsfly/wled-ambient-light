// F0 基频跟踪（单声部旋律线）。
//
// 色度回答的是「现在响着哪些音级」——十二维、不分八度、和弦与旋律糊在一起。
// 这里回答的是另一个问题：**主旋律此刻停在哪个音高上**，一个连续的 Hz 值。
// 有了它才能做「灯柱高度跟着旋律走」这类效果，色度做不到。
//
// 方法是谐波积谱（HPS）：真正的基频 F 在 F、2F、3F… 上都有能量，
// 而 2F 只在 2F、4F… 上有。把 k、2k、3k… 的幅度乘起来，F 处会被
// 强烈放大，非基频处被压下去。
//
// 五处实现上的选择，各有其非做不可的理由（后两条都是被实测逼出来的）：
//
// **对数域求和而不是直接相乘。** 四个 1e-4 量级的幅度相乘是 1e-16，
// float 尾数直接吃光。取对数之后是加法，还顺带把「比值」变成「差值」——
// 于是所有比较都与输入电平无关，AGC 开不开都一样。
//
// **地板取相对值** `floor = 1e-3 × max(mag)`。绝对地板会让安静片段的
// 每一个 bin 都贴地，HPS 分数全部相等；相对地板让「缺一个谐波」的代价
// 是一个有界的固定惩罚，而不是负无穷。
//
// **取「分数够高的最低候选」，不是最高分。** HPS 压得住次谐波（F/2 处
// 那些格子本来就没能量），压不住**倍频**：2F 的谐波全都是 F 的真谐波，
// 基音一弱 2F 就可能反超。而真基频按定义是最低的那个，所以从低往高扫、
// 第一个进入 margin 的就是答案。
//
// **候选必须是原始谱的局部极大、且自身有能量。** 缺了这条，纯正弦会错得
// 离谱：没有谐波时 k₀、k₀/2、k₀/3 的分数几乎相等，甚至 k₀/3 会因为第二谐波
// 蹭到主瓣泄漏而**得分最高**。实测 220Hz 纯音报成 118Hz、110Hz 报成无音高。
//
// **置信度取谐波能量占比**，不是分数的统计显著性。前两版都切不开：固定的
// 对数差值让白噪声拿到 0.45；除以 MAD 更糟，大量 bin 贴在地板上 MAD≈0，
// 纯音显著性冲到 190 万，而乐音 5–14 与鼓点 2.4–5.7 区间重叠。
// 能量占比天然有界，实测 乐音 0.9 / 旋律 0.55 / 鼓点 0.35 / 噪声 0.03。
//
// ⚠️ 已知限制：**缺失基音**（basso profondo、小喇叭放低音）测不准。
// F 处真的没有能量，HPS 只能报 2F。真要处理得换成自相关或 YIN，
// 那是另一套代价。这里如实标出来，不假装支持。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_window.h"     // kSampleRate
#include "lamp_envelope.h"   // envCoeff

namespace lamp {

struct PitchConfig {
    float lo_hz = 80.0f;        // 低男声下限
    float hi_hz = 1000.0f;      // 主旋律通常不会更高；再高就是泛音了
    int   harmonics = 4;        // 再多的话高次谐波常常已经出了搜索范围
    float floor_rel = 1e-3f;    // 相对地板

    // **候选自身那个 bin 必须真的有能量。**
    //
    // 纯正弦（长笛、合成器 lead）身上不加这条会错得很离谱：没有谐波时
    // k₀、k₀/2、k₀/3 的分数几乎相等（都是「一个真峰 + 三个地板」），
    // 「取最低候选」会一路选到低两个八度去。实测 220Hz 纯音报成 118Hz。
    //
    // 更阴的是 k₀/3 这种：它的第三谐波恰好落在主瓣泄漏里，分数反而**最高**。
    //
    // 这条判据也正是「不支持缺失基音」那句话的可执行版本。
    float cand_floor_rel = 0.02f;

    // 候选 bin 还必须是原始谱的**局部极大** —— 光有能量不够，那可能只是
    // 强峰的泄漏裙边。
    //
    // 实测的坑：110Hz 纯音落在 bin 5.11，而候选 k=3 的第二谐波（bin 6）恰好
    // 蹭到主瓣，分数比真正的 k=5 还高；bin 3 自己是 3% 的裙边、过得了能量判据。
    // 于是 110Hz 被判成 65Hz、越界、报无音高。裙边是单调下降的，天然不是局部
    // 极大，这条判据一加就干净了 —— 而真基音无论多弱都是个峰。

    // 基音 bin 不能太靠近 DC：抛物线插值要用 k±1，k=2 时左邻居就贴着 DC 泄漏了。
    // n=512（df=43Hz）下这意味着最低只能测到约 129Hz —— 这是分辨率的物理极限，
    // 不是可以调参绕过的。实测 n=512 下 87Hz 会偏到 96Hz（10%）。
    int min_bin = 3;

    // 倍频判据：分数在最佳值 margin 以内的最低候选胜出。
    // 单位是「每谐波的平均对数幅度」之差，也就是几何均值之比的对数，与电平无关。
    float octave_margin = 0.35f;

    // 置信度 = **谐波能量占比**：f0 及其谐波（各 ±1 bin）的功率占全谱的比例。
    //
    // 前两版都不行。固定的对数域差值：白噪声能拿到 0.45，四十个候选里靠运气
    // 冒出个高分是必然的。改成除以 MAD 的显著性：更糟 —— 大量 bin 贴在对数
    // 地板上，MAD≈0，纯正弦的显著性冲到 190 万，而乐音 5–14 与噪声 2–4、
    // 鼓点 2.4–5.7 **区间重叠**，根本切不开。
    //
    // 能量占比天然有界、物理含义直白，实测区分度也够宽：
    //   乐音 0.88–0.95 · 纯音 0.99 · 多声部里的旋律 0.55 · 鼓点 0.28–0.40 · 噪声 0.02–0.05
    float conf_min = 0.50f;     // 低于此视为无音高（unvoiced）

    // 跟踪
    float glide_tau_ms = 90.0f; // 小音程的滑音时间常数
    float leap_semi    = 3.0f;  // 超过这个音程算「跳进」，直接落位不滑
    float hold_ms      = 250.0f;// 短暂失去音高时保持多久
};

struct PitchEstimate {
    float hz   = 0.0f;
    float conf = 0.0f;          // [0,1]
    bool  voiced = false;
};

struct PitchTracker {
    float semi   = 0.0f;        // 平滑后的音高，单位是「相对 lo_hz 的半音数」
    float hz     = 0.0f;
    float conf   = 0.0f;
    bool  voiced = false;
    bool  has    = false;
    float unvoiced_ms = 0.0f;
    float a_glide = 0.0f;
};

inline void pitchRetime(PitchTracker &t, const PitchConfig &c, float dt_ms) {
    t.a_glide = envCoeff(c.glide_tau_ms, dt_ms);
}

inline void pitchInit(PitchTracker &t, const PitchConfig &c, float dt_ms) {
    t = PitchTracker{};
    pitchRetime(t, c, dt_ms);
}

// Hz ↔ 半音。跟踪必须在**半音域**做：100Hz 上的 1Hz 抖动是一个半音，
// 1000Hz 上的 1Hz 什么都不是。在 Hz 域平滑，低音区会糊、高音区会僵。
inline float hzToSemi(float hz, float ref_hz) {
    if (!(hz > 0.0f) || !(ref_hz > 0.0f)) return 0.0f;
    return 12.0f * log2f(hz / ref_hz);
}
inline float semiToHz(float semi, float ref_hz) {
    return ref_hz * powf(2.0f, semi / 12.0f);
}

// 单个候选的 HPS 分数：k、2k、3k… 处对数幅度的**平均值**。
// 取平均而不是求和，是为了让不同 harmonics 设置下的 margin / conf 阈值含义不变。
inline float hpsScore(const float *mag, size_t bins, int k, int harmonics, float flr) {
    if (k < 1 || harmonics < 1) return -1e30f;
    float acc = 0.0f;
    int   used = 0;
    for (int h = 1; h <= harmonics; ++h) {
        const size_t idx = (size_t)k * (size_t)h;
        if (idx >= bins) break;
        float m = mag[idx];
        if (!isfinite(m) || m < flr) m = flr;
        acc += logf(m);
        ++used;
    }
    if (used < 1) return -1e30f;
    return acc / (float)used;
}

// 抛物线插值，在**对数幅度**上做。加窗后的谱峰在对数域近似抛物线，
// 直接在线性幅度上插值会系统性偏向峰值 bin。
inline float parabolicBin(const float *mag, size_t bins, size_t k) {
    if (k < 1 || k + 1 >= bins) return (float)k;
    const float y0 = logf(mag[k - 1] > 1e-30f ? mag[k - 1] : 1e-30f);
    const float y1 = logf(mag[k]     > 1e-30f ? mag[k]     : 1e-30f);
    const float y2 = logf(mag[k + 1] > 1e-30f ? mag[k + 1] : 1e-30f);
    const float den = y0 - 2.0f * y1 + y2;
    if (!(fabsf(den) > 1e-12f)) return (float)k;
    float d = 0.5f * (y0 - y2) / den;
    if (d >  0.5f) d =  0.5f;
    if (d < -0.5f) d = -0.5f;
    return (float)k + d;
}

// mag 是 n/2+1 点的幅度谱，n 是 FFT 长度。
inline PitchEstimate estimateF0(const float *mag, size_t n, const PitchConfig &c) {
    PitchEstimate e;
    if (!mag || n < 16) return e;
    const size_t bins = n / 2 + 1;

    float mx = 0.0f;
    for (size_t i = 1; i < bins; ++i) if (isfinite(mag[i]) && mag[i] > mx) mx = mag[i];
    if (!(mx > 0.0f)) return e;
    const float flr = c.floor_rel * mx;

    const float df = kSampleRate / (float)n;
    int k_lo = (int)(c.lo_hz / df);
    if (k_lo < c.min_bin) k_lo = c.min_bin;
    if (k_lo < 1) k_lo = 1;
    int k_hi = (int)(c.hi_hz / df + 0.5f);
    if ((size_t)k_hi >= bins) k_hi = (int)bins - 1;
    if (k_hi <= k_lo) return e;

    // 一遍算完所有候选分数。范围最多几十个 bin，存栈上够用。
    constexpr int kMaxCand = 512;
    float sc[kMaxCand];
    bool  ok[kMaxCand];
    const int ncand = (k_hi - k_lo + 1 > kMaxCand) ? kMaxCand : (k_hi - k_lo + 1);
    const float cand_floor = c.cand_floor_rel * mx;
    float best = -1e30f;
    for (int i = 0; i < ncand; ++i) {
        sc[i] = hpsScore(mag, bins, k_lo + i, c.harmonics, flr);
        const size_t kb = (size_t)(k_lo + i);
        const float  mk = mag[kb];
        const bool   peak = (kb < 1 || mag[kb - 1] <= mk) &&
                            (kb + 1 >= bins || mag[kb + 1] <= mk);
        ok[i] = isfinite(mk) && mk >= cand_floor && peak;
        // **best 只在合格候选里取。** 第一版没加这个限制，结果 k₀/3 那个
        // 「靠泄漏蹭出来的」高分把 margin 门槛抬到没人够得着，纯音直接判成无音高。
        if (ok[i] && sc[i] > best) best = sc[i];
    }
    if (best <= -1e29f) return e;

    // 「分数够高的最低合格候选」。
    //
    // 这里**曾经**还要求「分数是局部极大」，用来避开 HPS 峰的左肩。加了
    // 原始谱局部极大判据之后那条不但冗余、而且有害：87Hz 纯音的真峰在 bin 4，
    // 而裙边 bin 3 的第二谐波（bin 6）蹭到主瓣，分数反而比 bin 4 高 ——
    // 于是真峰「不是分数局部极大」，被自己人否掉，整帧报无音高。
    //
    // 左肩问题现在由原始谱判据解决：裙边单调下降，天生不是峰。
    int pick = -1;
    for (int i = 0; i < ncand; ++i) {
        if (!ok[i]) continue;                             // 基音必须真的在，且是个峰
        if (sc[i] < best - c.octave_margin) continue;
        pick = i; break;
    }
    if (pick < 0) return e;

    // 谐波能量占比。±1 bin 是为了收进加窗造成的主瓣展宽 ——
    // 只取中心 bin 的话，频率不在格点上时能量会被算漏掉一半。
    const size_t k_pick = (size_t)(k_lo + pick);
    double tot = 0.0, har = 0.0;
    for (size_t i = 1; i < bins; ++i) {
        const float m = mag[i];
        if (isfinite(m)) tot += (double)m * m;
    }
    for (int h = 1; h <= c.harmonics; ++h) {
        const size_t ctr = k_pick * (size_t)h;
        if (ctr >= bins) break;
        for (size_t d = (ctr > 0 ? ctr - 1 : 0); d <= ctr + 1 && d < bins; ++d) {
            const float m = mag[d];
            if (isfinite(m)) har += (double)m * m;
        }
    }
    float conf = (tot > 0.0) ? (float)(har / tot) : 0.0f;
    if (conf > 1.0f) conf = 1.0f;
    if (conf < 0.0f) conf = 0.0f;

    // 亚 bin 细化用**原始谱**在选中 bin 附近找局部极大再插值。
    // HPS 分数曲线本身很钝，直接在它上面插值精度还不如原始谱峰。
    size_t kk = k_pick;
    for (size_t d = 1; d <= 2; ++d) {
        if (kk >= d && mag[kk - d] > mag[kk]) kk -= d;
        if (kk + d < bins && mag[kk + d] > mag[kk]) kk += d;
    }
    const float kf = parabolicBin(mag, bins, kk);

    e.hz     = kf * df;
    e.conf   = conf;
    e.voiced = (conf >= c.conf_min) && (e.hz >= c.lo_hz * 0.9f) && (e.hz <= c.hi_hz * 1.1f);
    if (!e.voiced) e.hz = 0.0f;
    return e;
}

// 跟踪。小音程滑过去，跳进直接落位；短暂无音高时保持住。
inline void pitchUpdate(PitchTracker &t, const PitchConfig &c,
                        const PitchEstimate &e, float dt_ms) {
    if (!isfinite(dt_ms) || dt_ms <= 0.0f) return;

    if (!e.voiced) {
        t.unvoiced_ms += dt_ms;
        if (t.unvoiced_ms >= c.hold_ms) { t.voiced = false; t.conf = 0.0f; }
        // 保持期内 hz / semi 原样不动 —— 换气、辅音、弱拍都会让基频短暂消失，
        // 一消失就熄灯的话旋律线会闪成虚线。
        return;
    }
    t.unvoiced_ms = 0.0f;
    t.conf   = e.conf;
    t.voiced = true;

    const float target = hzToSemi(e.hz, c.lo_hz);
    if (!t.has) { t.semi = target; t.has = true; }
    else if (fabsf(target - t.semi) > c.leap_semi) {
        // 跳进。平滑过去会变成滑音 —— 那是另一种乐器了。
        t.semi = target;
    } else {
        t.semi += t.a_glide * (target - t.semi);
    }
    t.hz = semiToHz(t.semi, c.lo_hz);
}

} // namespace lamp
