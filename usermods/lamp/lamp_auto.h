// 自动灯效。
//
// 系统一直有个断层：**分析档位**（FFT 长度、窗、hop）会自动跟着音乐走，
// 而**看到什么**始终得手动按按钮。于是它能听出这是古典还是电子，
// 却仍然用同一个频段柱去画。这个文件把那半条回路接上。
//
// 判据不是「猜流派」——「这是摇滚吗」是个没有客观答案的问题。
// 判据是**「音乐的信息此刻藏在哪一维」**，然后选一个消费那一维的效果：
//
//   信息在旋律   → 旋律线        判据：f0 长期有声、置信度高
//   信息在和声   → 调性染色      判据：调明确、谐波占比高
//   信息在击打   → 冲击柱        判据：打击占比高、节拍锁定
//   信息在低/高分层 → 高低分离   判据：打击占比高、且两端确实分层
//   信息在长期走向 → 彩色流动    判据：节拍锁不上（rubato / 古典）或段落活跃
//   都不突出     → 频段柱        兜底
//
// 这套判据直接对应到常见流派，但**从不需要给流派命名**：
// 古典自然落到彩色流动（锁不上拍）、流行落到旋律线（人声清晰）、
// 摇滚与 rap 落到冲击柱/高低分离（打击占比高、f0 不稳）、
// 电子落到彩色流动或冲击柱（看它有没有旋律 hook）。
//
// 切换纪律与 §3.3.3 的档位切换同构：**候选连续胜出一段时间 + 距上次切换够久**。
// 灯效切换比档位切换更扎眼（画面整个换掉），所以两个时间都取得更长。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_fx.h"
#include "lamp_time.h"

namespace lamp {

// 参与自动选择的效果。不是全部十个 —— 拍点脉冲、电平扫描、音级环
// 各自都有一个「更好的同类」，自动模式里放进去只会让画面来回横跳。
constexpr int kAutoCands = 5;

// 兜底。它**不在候选表里**，分数恒为 0 —— 「什么都不突出」的语义就是
// 「没有任何一维值得专门去画」，那就画最中性的频段柱。
constexpr FxId kAutoFallback = FX_SPECTRUM_BARS;

struct AutoConfig {
    // 判据的门槛。取值都来自各特征自己的量纲，不是凭空的魔数。
    float f0_duty_full   = 0.55f;   // f0 有声占比达到这个数算「旋律清晰」

    // **旋律不只要「在」，还要「稳」。** 单位是半音/秒。
    //
    // 只看有声占比是不够的：失真吉他的 power chord 有声占比 100%，但 f0 在
    // 和弦各分音之间逐帧乱跳 —— 画成旋律线就是一团噪点。真旋律是「按住一个音、
    // 偶尔跨一步」，抖动率低得多。
    //
    // 实测（半音/秒）：古典 4.3 · 流行 6.6 · **失真吉他 21–23** · 噪声 70+。
    // 门槛放在 8–20 之间线性过渡。
    //
    // 用「每秒」而不是「每帧」—— 各档 hop 差 8 倍，按帧算的话同一段音乐
    // 在打点档的抖动率会是氛围档的 1/8。
    float f0_jit_lo      = 8.0f;
    float f0_jit_hi      = 20.0f;
    // 打击占比高于此算「以鼓为主」。**这个值是量出来的。**
    //
    // 量的必须是**代码实际使用的那个统计量** —— 能量加权的时间平均，
    // 不是逐帧瞬时值。我为此栽过一次：照瞬时值（Rap 0.93、摇滚 0.56）
    // 读出的「空隙」在 0.72，改过去之后 Rap 和电子双双掉回兜底 ——
    // 因为它们的**平均值**只有 0.67 / 0.66。原来的 0.55 本来就是对的。
    //
    // 六段配器完整的素材，实测 perc_avg：
    //   古典 0.24 · 流行 0.27 · 摇滚(器乐) 0.43 · 摇滚(人声) 0.45 · 电子 0.66 · Rap 0.67
    // 分布是两簇，空隙在 0.45–0.66，中点 0.55。
    float perc_hi        = 0.55f;
    float split_contrast = 0.25f;   // 低/高两端的能量差达到此值才谈得上「分层」
    float key_hi         = 0.45f;   // 调性置信度
    float lock_lo        = 0.35f;   // BPM 置信度低于此算「锁不上」
    float move_lo        = 0.35f;   // |走向| + 段落新颖度 超过此才算「结构活跃」

    // **所有判据输入都在这个窗上做时间平均。**
    //
    // 逐帧的瞬时值不能用来选灯效。打击占比就是典型：鼓点帧 0.92、间隙 0.01，
    // 逐帧看的话 want 每几帧翻一次，`cand_since` 不断被重置，hold 永远凑不满 ——
    // 实测 rap 与电子在真机上「打击占比 0.93、节拍也锁上了，却一次都不切」。
    //
    // 单元测试没抓到：那些测试每帧喂的都是同一个常数，
    // 瞬时值与平均值恰好是同一个数（docs/17 第 20 条的形状）。
    float feature_tau_ms = 4000.0f;

    float margin   = 0.08f;         // 新候选要赢现任这么多才换（滞回）
    uint32_t hold_ms  = 5000;       // 候选需连续胜出
    uint32_t dwell_ms = 15000;      // 距上次切换
};

struct AutoState {
    FxId     current   = FX_SPECTRUM_BARS;
    FxId     candidate = FX_SPECTRUM_BARS;
    uint32_t cand_since = 0;
    uint32_t last_switch = 0;
    bool     has_switched = false;
    float    f0_duty   = 0.0f;      // f0 有声的时间占比
    float    f0_jitter = 0.0f;      // 音高抖动率，半音/秒
    float    prev_semi = 0.0f;
    bool     has_prev_semi = false;
    float    dt_ms     = 0.0f;
    // **平均能量，不是平均比值** —— 见 autoUpdate
    float    e_h = 0.0f, e_p = 0.0f;        // 谐波 / 打击能量
    float    e_ends = 0.0f, e_mid = 0.0f;   // 两端 / 中段能量
    float    a_feat    = 0.0f;
    float    score[kAutoCands] = {0};
};

inline void autoRetime(AutoState &s, const AutoConfig &c, float dt_ms) {
    s.a_feat = envCoeff(c.feature_tau_ms, dt_ms);
    s.dt_ms  = dt_ms;              // 抖动率要除以它，换算成「每秒」
}

inline void autoInit(AutoState &s, const AutoConfig &c, float dt_ms) {
    s = AutoState{};
    autoRetime(s, c, dt_ms);
}

inline FxId autoCandidate(int i) {
    switch (i) {
        case 0:  return FX_MELODY_LINE;
        case 1:  return FX_KEY_WASH;
        case 2:  return FX_BAR_IMPACT;
        case 3:  return FX_SPLIT_BANDS;
        default: return FX_COLOR_FLOW;
    }
}

// 门槛函数。**阈值以下返回 0**，以上按剩余量程线性升到 1。
//
// 第一版写的是 `clamp01(x / th)` —— 那不是门槛，是缩放：阈值以下照样给分。
// 后果是「什么都不突出」时冲击柱仍能拿 0.33，兜底的频段柱永远轮不上，
// 四条测试一起红。门槛就该是门槛。
inline float gateAbove(float x, float th) {
    if (!(th < 1.0f)) return 0.0f;
    return clamp01((x - th) / (1.0f - th));
}

// 两端与中段的平均能量。拆出来是为了让调用方能先**对能量做时间平均**、
// 再取比值 —— 见 autoUpdate 里的说明。
inline void splitEnergies(const float *bands, float &ends, float &mid) {
    float lo = 0.0f, hi = 0.0f, md = 0.0f;
    for (int i = 0; i < 4; ++i)                       lo += bands[i];
    for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i)   hi += bands[i];
    for (int i = 5; i < NUM_BANDS - 5; ++i)           md += bands[i];
    const int nmid = (NUM_BANDS - 10 > 0) ? (NUM_BANDS - 10) : 1;
    ends = 0.5f * (lo / 4.0f + hi / 4.0f);
    mid  = md / (float)nmid;
}

inline float splitContrastOf(float ends, float mid) {
    if (!isfinite(ends) || !isfinite(mid)) return 0.0f;
    const float tot = ends + mid;
    if (!(tot > 1e-9f)) return 0.0f;
    float v = (ends - mid) / tot;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

// 低频端与高频端的能量落差，[0,1]。「高低分离」只有在两端真的分层时才好看，
// 频谱平坦的时候它画出来就是两根呆立的柱子。
inline float splitContrast(const float *bands) {
    float ends = 0.0f, mid = 0.0f;
    splitEnergies(bands, ends, mid);
    return splitContrastOf(ends, mid);
}

// 给五个候选各打一分，[0,1]。分数只表达「这一维此刻有多突出」，
// 不做任何流派命名。
// 三个时间平均后的量由调用方传入 —— 让这个函数保持纯粹、可单独测。
inline void autoScore(const AudioFrame &f, const AutoConfig &c,
                      float f0_duty, float f0_jitter,
                      float perc_avg, float split_avg, float *out) {
    const float lock = clamp01(f.bpm_conf);
    const float key  = clamp01(f.key_conf);
    const float perc = clamp01(perc_avg);
    const float duty = clamp01(f0_duty);

    // 旋律线：旋律要**持续**存在。单帧测到一个 f0 不算 —— rap 和失真吉他
    // 都会时不时冒出一个，占比才是可靠的判据。
    // 抖得越厉害越不像旋律。门槛以下满分、门槛以上线性退到 0。
    float steady = 1.0f;
    if (c.f0_jit_hi > c.f0_jit_lo)
        steady = 1.0f - clamp01((f0_jitter - c.f0_jit_lo) / (c.f0_jit_hi - c.f0_jit_lo));
    out[0] = gateAbove(duty, c.f0_duty_full) * (1.0f - 0.6f * perc) * steady;

    // 调性染色：调明确，且以谐波为主
    out[1] = gateAbove(key, c.key_hi) * (1.0f - perc);

    // 冲击柱 与 高低分离 都以「鼓为主」起步，靠**频谱有没有分层**分家：
    //   谱平坦 → 冲击柱（一个标量铺满整管）
    //   两端强、中间空 → 高低分离（808 在下、hi-hat 在上）
    // 写成互斥的一对，而不是各打各的分 —— 否则 rap 的谱明明分层得很清楚，
    // 两者仍会因为分数只差 0.005 而随机取一个。
    const float drums = gateAbove(perc, c.perc_hi);
    const float split = gateAbove(clamp01(split_avg), c.split_contrast);
    out[2] = drums * lock * (1.0f - split);
    out[3] = drums * split;

    // 彩色流动：锁不上拍（rubato、古典），或者段落/走向本身就很活跃。
    // 取两者的较大值 —— 它们各自都足以让这个效果成为最佳选择。
    const float rubato = clamp01((c.lock_lo - lock) / ((c.lock_lo > 1e-3f) ? c.lock_lo : 1e-3f));
    const float moving = gateAbove(clamp01(fabsf(f.energy_trend) + f.section_novelty), c.move_lo);
    out[4] = (rubato > moving) ? rubato : moving;

    for (int i = 0; i < kAutoCands; ++i)
        if (!isfinite(out[i]) || out[i] < 0.0f) out[i] = 0.0f;
}

// 返回本次是否切换了效果。
inline bool autoUpdate(AutoState &s, const AutoConfig &c,
                       const AudioFrame &f, uint32_t now_ms) {
    // f0 有声占比。用占比而不是当前帧的 f0_voiced：换气、辅音、乐句间隙
    // 都会让它瞬间落下去，按帧判会让灯效每两秒换一次。
    const float v = f.f0_voiced ? 1.0f : 0.0f;
    s.f0_duty += s.a_feat * (v - s.f0_duty);

    // 音高抖动率。只在有声时更新 —— 无声期间 prev_semi 保持不变，
    // 否则一段空白之后的第一个音会被记成一次巨大的跳变。
    if (f.f0_voiced && f.f0_hz > 0.0f && isfinite(f.f0_hz)) {
        const float semi = hzToSemi(f.f0_hz, 80.0f);
        if (s.has_prev_semi && s.dt_ms > 0.0f) {
            const float rate = fabsf(semi - s.prev_semi) * 1000.0f / s.dt_ms;
            if (isfinite(rate)) s.f0_jitter += s.a_feat * (rate - s.f0_jitter);
        }
        s.prev_semi = semi; s.has_prev_semi = true;
    }

    // **先平均能量，再取比值。** 平均比值是错的：120BPM 下一记底鼓的
    // 打击占比能到 0.92，但它只占 500ms 里的 50ms，其余帧接近 0 ——
    // 把每帧等权平均，结果被大量间隙帧稀释到 0.1，「以鼓为主」永远判不出来。
    // 而按能量加权，那一记鼓的份量本来就该更重。
    float sh = 0.0f, sp = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (isfinite(f.bands_h[i]) && f.bands_h[i] > 0.0f) sh += f.bands_h[i];
        if (isfinite(f.bands_p[i]) && f.bands_p[i] > 0.0f) sp += f.bands_p[i];
    }
    s.e_h += s.a_feat * (sh - s.e_h);
    s.e_p += s.a_feat * (sp - s.e_p);
    const float etot = s.e_h + s.e_p;
    const float perc_avg = (etot > 1e-12f) ? (s.e_p / etot) : 0.0f;

    float ends = 0.0f, mid = 0.0f;
    splitEnergies(f.bands, ends, mid);
    if (!isfinite(ends)) ends = 0.0f;
    if (!isfinite(mid))  mid  = 0.0f;
    s.e_ends += s.a_feat * (ends - s.e_ends);
    s.e_mid  += s.a_feat * (mid  - s.e_mid);
    const float split_avg = splitContrastOf(s.e_ends, s.e_mid);

    autoScore(f, c, s.f0_duty, s.f0_jitter, perc_avg, split_avg, s.score);

    // 现任要被换掉，挑战者得**多赢一个 margin**。没有这道滞回的话，
    // 两个分数接近的候选会在 hold_ms 的边缘反复交替，永远凑不满驻留时间 ——
    // 与 §3.3.3 档位切换那道滞回是同一个理由。
    // cur_ix == -1 表示现在放的是兜底。**它的分数是 0，不是 -1** ——
    // 第一版给了 -1，于是任何一个 0 分候选都能「赢过」兜底，
    // 「什么都不突出」反而会选中旋律线。
    int   cur_ix = -1;
    for (int i = 0; i < kAutoCands; ++i) if (autoCandidate(i) == s.current) cur_ix = i;
    const float cur_score = (cur_ix >= 0) ? s.score[cur_ix] : 0.0f;

    int   best_ix = cur_ix;
    float best    = cur_score;
    for (int i = 0; i < kAutoCands; ++i) {
        if (i == cur_ix) continue;
        if (s.score[i] > best + c.margin) { best = s.score[i]; best_ix = i; }
    }
    // 注意这个循环**顺带给了挑战者之间的滞回**：基准是 `best + margin`，
    // 一旦某个候选坐上去，后面的要多赢一个 margin 才能顶替。
    // 两个咬得很紧的候选因此不会逐帧交替、把 cand_since 反复重置。
    // （由 test_two_close_challengers_still_get_committed 钉住。）
    // 现任自己也掉到门槛以下时退回兜底 —— 否则人声停了、鼓也停了，
    // 灯效还僵在旋律线上不动。
    if (best_ix >= 0 && !(best > 0.0f)) best_ix = -1;

    const FxId want = (best_ix >= 0) ? autoCandidate(best_ix) : kAutoFallback;

    if (want != s.candidate) { s.candidate = want; s.cand_since = now_ms; }
    if (want == s.current) return false;
    if (!elapsedAtLeast(now_ms, s.cand_since, c.hold_ms)) return false;
    if (s.has_switched && !elapsedAtLeast(now_ms, s.last_switch, c.dwell_ms)) return false;

    s.current      = want;
    s.last_switch  = now_ms;
    s.has_switched = true;
    return true;
}

} // namespace lamp
