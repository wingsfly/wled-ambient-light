// 长期统计 → 氛围（段落感与能量走向）。
//
// 前面所有模块的时间尺度都在**一拍以内**：包络 30ms、起音一帧、节拍 6 秒的
// 自相关窗。它们回答「此刻发生了什么」，回答不了「这首歌正在往哪走」——
// 副歌进来了没有、是渐强还是收尾、整体是安静还是躁。
//
// 这个模块的时间尺度是**几十秒**，输出三样东西：
//
//   energy_trend    [-1,1]  短期响度相对长期基线的偏离，正=渐强
//   section_novelty [0,1]   当前频谱轮廓与「上一段」的距离
//   mood            [0,1]   静↔躁的合成标量
//
// ⚠️ 一个必须先说清的坑：**不能拿 AGC 之后的能量当强度**。
// AGC 的 attack 是 120ms、release 是 6s，它存在的全部目的就是把响度差异抹平——
// 安静的钢琴和炸裂的 drop 最后都被拉到 target=0.25 附近。拿 `bands` 求和当
// 「有多躁」，量出来的是 AGC 的收敛程度，不是音乐。
//
// 能用的是这几样：
//   - `rms_fast` / `rms_slow` / `peak` —— 管线里它们取自 **AGC 之前**的帧 RMS，
//     只有 `bands` 乘了增益。所以响度走向直接建在它们上面是干净的。
//   - `onset_rate`、`centroid_hz` —— 密度与音色，与增益无关。
//   - **波峰因数** `peak / rms_fast` —— 分子分母同乘增益，天然增益无关。
//     稀疏的鼓点波峰因数高，塞满的音墙低。这是「织体密不密」的直接度量。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_bands.h"
#include "lamp_envelope.h"   // envCoeff

namespace lamp {

struct MoodConfig {
    // 「此刻」与「基线」。3s 能盖住一个乐句，30s 能盖住一个段落。
    float short_tau_ms = 3000.0f;
    float long_tau_ms  = 30000.0f;

    // 段落判据：**两条不同时间常数的频谱轮廓相比**。
    //
    // 第一版是「与上一次换段时的快照比」，跑起来一次切换会连报两三次。
    // 根因是快照取在过渡的中途 —— 它既不代表旧段也不代表新段，而平滑轮廓
    // 还在继续朝新素材漂，于是反复越过阈值。
    //
    // 两条 EMA 没有这个问题：换段时快的先动、慢的滞后，novelty 涨起来；
    // 新素材站稳之后两条重合，novelty **自己回到零**。不需要快照簿记，
    // 也不需要不应期定时器 —— 自复位本身就是速率限制。
    float profile_fast_tau_ms = 4000.0f;
    float profile_slow_tau_ms = 20000.0f;

    // 滞回。上阈触发，必须先跌回下阈才重新武装。
    float section_hi = 0.22f;
    float section_lo = 0.08f;

    float mood_tau_ms = 5000.0f;    // 氛围标量自身的平滑

    // ── 动态（AGC 旁路）──
    //
    // AGC 把响度差异抹平，这对「灯要一直看得见」是必要的，对古典却是灾难：
    // pp 和 ff 被画成一样亮。而古典的信息**主要就在动态里**。
    //
    // 解法不是关掉 AGC，是把两件事分开：**AGC 负责形状（bands 的相对高低），
    // dynamics 负责电平**。灯效用归一化的频段画形状、再整体乘 dynamics，
    // 两者各司其职。
    //
    // 参考电平取长期峰值保持（瞬时攻击、60s 回落），dynamics 是当前响度相对
    // 它的 dB 值映射到 [0,1]。这是个**比值**，所以与麦克风灵敏度无关 ——
    // 它量的是「这一句相对这首曲子最响处有多轻」，不是绝对声压。
    //
    // 压得很死的流行/电子，dynamics 会一直贴近 1，等于没开这个功能；
    // 古典才会真正用上它。自适应，不需要开关。
    float dyn_range_db    = 40.0f;
    float loud_ref_tau_ms = 60000.0f;
    float dyn_tau_ms      = 250.0f;

    // mood 的三个分量各自的归一化上界与权重。
    float onset_full   = 6.0f;      // 每秒 6 个起音就算满
    float bright_lo_hz = 200.0f;
    float bright_hi_hz = 4000.0f;
    float crest_full   = 5.0f;      // 波峰因数 5 视为「最稀疏」
    float w_onset      = 0.40f;
    float w_bright     = 0.30f;
    float w_sparse     = 0.30f;
};

struct MoodState {
    float e_short = 0.0f, e_long = 0.0f;
    bool  primed  = false;

    float p_fast[NUM_BANDS] = {0};     // 「最近在放什么」
    float p_slow[NUM_BANDS] = {0};     // 「中期以来在放什么」
    bool  has_profile = false;

    float novelty        = 0.0f;
    bool  section_change = false;      // 只在发生的那一帧为真
    bool  armed          = true;       // 滞回：跌回下阈才重新武装

    float mood  = 0.0f;
    float trend = 0.0f;

    float loud_ref = 0.0f;
    float dynamics = 0.0f;   // [0,1]，1 = 与全曲最响处相当

    float a_short = 0.0f, a_long = 0.0f, a_pf = 0.0f, a_ps = 0.0f, a_mood = 0.0f;
    float a_ref = 0.0f, a_dyn = 0.0f;
};

// 换档只换系数，**保留统计量** —— 与包络、AGC、节拍同一个道理。
// 三十秒的基线要是每次换档都清零，那它就永远不是三十秒的基线。
inline void moodRetime(MoodState &s, const MoodConfig &c, float dt_ms) {
    s.a_short = envCoeff(c.short_tau_ms,        dt_ms);
    s.a_long  = envCoeff(c.long_tau_ms,         dt_ms);
    s.a_pf    = envCoeff(c.profile_fast_tau_ms, dt_ms);
    s.a_ps    = envCoeff(c.profile_slow_tau_ms, dt_ms);
    s.a_mood  = envCoeff(c.mood_tau_ms,         dt_ms);
    s.a_ref   = envCoeff(c.loud_ref_tau_ms,     dt_ms);
    s.a_dyn   = envCoeff(c.dyn_tau_ms,          dt_ms);
}

inline void moodInit(MoodState &s, const MoodConfig &c, float dt_ms) {
    s = MoodState{};
    moodRetime(s, c, dt_ms);
}

// 把 16 段能量化成**只含形状、不含总量**的轮廓（L1 归一化）。
// 不归一化的话轮廓距离会被总响度带着走，「变响」会被误报成「换段」。
inline void bandProfile(const float *bands, float *out) {
    float sum = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float v = (isfinite(bands[i]) && bands[i] > 0.0f) ? bands[i] : 0.0f;
        out[i] = v; sum += v;
    }
    if (sum > 1e-12f) for (int i = 0; i < NUM_BANDS; ++i) out[i] /= sum;
    else              for (int i = 0; i < NUM_BANDS; ++i) out[i] = 0.0f;
}

// 余弦距离，[0,1]。和色度那里同一个理由：比的是形状不是长度。
inline float profileDistance(const float *a, const float *b) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (!isfinite(a[i]) || !isfinite(b[i])) return 0.0f;
        dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i];
    }
    const float den = sqrtf(na * nb);
    if (!(den > 1e-12f)) return 0.0f;
    float cs = dot / den;
    if (cs > 1.0f) cs = 1.0f; else if (cs < -1.0f) cs = -1.0f;
    return 1.0f - cs;
}

// 波峰因数。稀疏（鼓点之间有空隙）时高，塞满时接近 1。
inline float crestFactor(float peak, float rms_fast) {
    if (!isfinite(peak) || !isfinite(rms_fast)) return 1.0f;
    if (!(rms_fast > 1e-9f)) return 1.0f;
    const float cf = peak / rms_fast;
    return (cf < 1.0f) ? 1.0f : cf;
}

// 喂一帧。bands 是 AGC 之后的（只用它的形状），响度三件套是 AGC 之前的。
inline void moodUpdate(MoodState &s, const MoodConfig &c,
                       const float *bands, float rms_fast, float peak,
                       float onset_rate, float centroid_hz, bool gated) {
    s.section_change = false;

    // 静音时冻结全部统计。放开的话三十秒基线会被一段空白慢慢拉到零，
    // 音乐一回来立刻误报「渐强」和「换段」—— 与 AGC 静音冻结增益同理。
    if (gated || !isfinite(rms_fast) || rms_fast < 0.0f) return;

    // ── 能量走向 ──
    if (!s.primed) { s.e_short = s.e_long = rms_fast; s.primed = true; }
    else {
        s.e_short += s.a_short * (rms_fast - s.e_short);
        s.e_long  += s.a_long  * (rms_fast - s.e_long);
    }
    // 相对偏离，不是绝对差 —— 麦克风灵敏度、音源电平各不相同，绝对值没有可比性。
    const float base = (s.e_long > 1e-9f) ? s.e_long : 1e-9f;
    float tr = (s.e_short - s.e_long) / base;
    if (tr >  1.0f) tr =  1.0f;
    if (tr < -1.0f) tr = -1.0f;
    s.trend = tr;

    // ── 动态 ──
    // 参考电平：瞬时攻击、极慢回落。攻击必须是瞬时的 —— 慢慢爬的话
    // 一段渐强会把自己当成参考，永远测不出「这里是最响的」。
    if (rms_fast > s.loud_ref) s.loud_ref = rms_fast;
    else                       s.loud_ref += s.a_ref * (rms_fast - s.loud_ref);
    float dyn_t = 0.0f;
    if (s.loud_ref > 1e-9f && rms_fast > 1e-12f) {
        const float db = 20.0f * log10f(rms_fast / s.loud_ref);   // ≤ 0
        dyn_t = 1.0f + db / ((c.dyn_range_db > 1.0f) ? c.dyn_range_db : 1.0f);
        if (dyn_t > 1.0f) dyn_t = 1.0f;
        if (dyn_t < 0.0f) dyn_t = 0.0f;
    }
    s.dynamics += s.a_dyn * (dyn_t - s.dynamics);

    // ── 段落 ──
    float prof[NUM_BANDS];
    bandProfile(bands, prof);
    float psum = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) psum += prof[i];
    if (psum > 0.0f) {
        if (!s.has_profile) {
            // 两条都拉到当前轮廓。不这么做的话开机头几秒两条从零各自爬升，
            // 差异纯粹来自初始化，会凭空报一次换段。
            for (int i = 0; i < NUM_BANDS; ++i) s.p_fast[i] = s.p_slow[i] = prof[i];
            s.has_profile = true;
        } else {
            for (int i = 0; i < NUM_BANDS; ++i) {
                s.p_fast[i] += s.a_pf * (prof[i] - s.p_fast[i]);
                s.p_slow[i] += s.a_ps * (prof[i] - s.p_slow[i]);
            }
        }
        s.novelty = profileDistance(s.p_fast, s.p_slow);

        if (s.armed && s.novelty > c.section_hi) {
            s.section_change = true;
            s.armed = false;
        } else if (!s.armed && s.novelty < c.section_lo) {
            s.armed = true;
        }
    }

    // ── 氛围标量 ──
    float d_onset = (isfinite(onset_rate) && onset_rate > 0.0f)
                        ? onset_rate / c.onset_full : 0.0f;
    if (d_onset > 1.0f) d_onset = 1.0f;

    float d_bright = 0.0f;
    if (isfinite(centroid_hz) && centroid_hz > c.bright_lo_hz && c.bright_hi_hz > c.bright_lo_hz) {
        // 对数映射。人耳对频率是对数感知的，线性映射会让 200→400Hz 这一段
        // （听感上整整一个八度）挤成一丁点。
        d_bright = log2f(centroid_hz / c.bright_lo_hz) / log2f(c.bright_hi_hz / c.bright_lo_hz);
        if (d_bright > 1.0f) d_bright = 1.0f;
    }

    const float cf = crestFactor(peak, rms_fast);
    float sparse = (cf - 1.0f) / ((c.crest_full > 1.0f) ? (c.crest_full - 1.0f) : 1.0f);
    if (sparse > 1.0f) sparse = 1.0f;
    if (sparse < 0.0f) sparse = 0.0f;

    const float wsum = c.w_onset + c.w_bright + c.w_sparse;
    float target = (wsum > 1e-6f)
        ? (c.w_onset * d_onset + c.w_bright * d_bright + c.w_sparse * (1.0f - sparse)) / wsum
        : 0.0f;
    if (target > 1.0f) target = 1.0f;
    if (target < 0.0f) target = 0.0f;

    s.mood += s.a_mood * (target - s.mood);
}

} // namespace lamp
