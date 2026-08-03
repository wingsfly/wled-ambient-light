// 谐波 / 打击分离（HPSS）。
//
// 到这里为止，所有效果拿到的都是**混在一起**的 16 段能量：底鼓、贝斯、人声、
// 镲片全叠在同几个格子里。Rap 想让一根管画 808、另一根画 hi-hat，摇滚想把鼓
// 和吉他分开 —— 都做不到。
//
// 分离的依据是两类声音在时频面上的形状**正交**：
//
//   谐波型（人声、吉他、弦乐）在**时间**上连续 —— 同一段能量持续好几帧
//   打击型（底鼓、军鼓、镲）在**频率**上连续 —— 一瞬间铺满整个频段
//
// 于是：沿时间轴取中值 → 留下谐波；沿频率轴取中值 → 留下打击。
// 两者做软掩膜，把原能量按比例分给两路。这是 Fitzgerald 2010 的做法。
//
// ── 三处与教科书不同的地方 ──
//
// **在 16 个对数频段上做，不在完整频谱上做。** 教科书用线性频率的几百个 bin，
// 内存和算力都不是 ESP32 上一帧 5.8ms 能负担的。降到 16 段之后频率轴的中值
// 只跨 5 格 —— 但判别力还在：打击声会点亮**所有**段，谐波声只点亮一两段，
// 这个差别在 16 段上依然清清楚楚。
//
// **时间窗按物理时间定，不按帧数。** 各档 hop 从 5.8ms 到 46.4ms 差 8 倍，
// 写死 9 帧的话，同一段音乐在打点档只看 52ms、在氛围档看 418ms，
// 「什么算持续」的定义完全不同。
//
// **掩膜用平方而不是一次方。** 一次方的掩膜太软，两路都拿到一半，
// 分离等于没做；平方把差距拉开，弱势的那一路会被压到接近零。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_bands.h"

namespace lamp {

constexpr int kHpssMaxHist = 17;     // 时间窗最多几帧（必须是奇数）

struct HpssConfig {
    float time_win_ms = 200.0f;      // 时间轴中值窗长
    int   freq_win    = 2;           // 频率轴中值半宽（±2 → 5 段）
    // 掩膜下限。两路都接近零时（静音）不要除出 0/0。
    float eps         = 1e-9f;
};

struct HpssState {
    float hist[kHpssMaxHist][NUM_BANDS] = {{0}};
    int   n     = 0;                 // 实际使用的窗长（奇数）
    int   head  = 0;                 // 环形缓冲写指针
    int   fill  = 0;                 // 已填入几帧
};

// 窗长由物理时间与当前 hop 共同决定，必须是**奇数**（中值要有正中间那个）。
inline int hpssWindow(const HpssConfig &c, float dt_ms) {
    if (!(dt_ms > 0.0f)) return 3;
    // 取**最近的奇数**，不是「四舍五入之后再减一」。
    // 后者在 200/46.44=4.31 处会给出 3（139ms），而 5（232ms）离目标近得多。
    const float x = c.time_win_ms / dt_ms;
    int n = 2 * (int)((x - 1.0f) * 0.5f + 0.5f) + 1;
    if (n < 3) n = 3;
    if (n > kHpssMaxHist) n = kHpssMaxHist;   // 上限本身是奇数
    return n;
}

// 换档只换窗长，**历史照留**。丢掉历史等于分离器重新热身，
// 换档瞬间两路会一起塌下来 —— 与包络、AGC、节拍同一个道理。
//
// 窗变短时多出来的旧帧自然被环形缓冲覆盖掉；窗变长时 fill 还不够，
// 中值暂时在较少的样本上算，几帧后自愈。
inline void hpssRetime(HpssState &s, const HpssConfig &c, float dt_ms) {
    s.n = hpssWindow(c, dt_ms);
    if (s.fill > s.n) s.fill = s.n;
}

inline void hpssInit(HpssState &s, const HpssConfig &c, float dt_ms) {
    s = HpssState{};
    s.n = hpssWindow(c, dt_ms);
}

// 小数组中值。插入排序，n ≤ 17，比任何选择算法都快。
inline float medianOf(float *v, int n) {
    if (n < 1) return 0.0f;
    for (int i = 1; i < n; ++i) {
        const float x = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; --j; }
        v[j + 1] = x;
    }
    return v[n / 2];
}

// 喂一帧频段能量，输出分离后的两路。out_h / out_p 各 NUM_BANDS 个。
inline void hpssProcess(HpssState &s, const HpssConfig &c,
                        const float *bands, float *out_h, float *out_p) {
    // 上下界都要挡。**这里不能只信 hpssWindow 的钳位** —— 少了上限，
    // `s.hist[s.head]` 会写到 17 行的数组之外去，直接踩坏堆。
    // 变异测试里去掉 hpssWindow 那道钳位时，进程不是失败而是**挂死**，
    // 正是这个原因。两层各自独立、各自有测试，不是冗余（第 27 条）：
    // 一层保证「算出来的窗长合法」，一层保证「用的时候不越界」。
    if (s.n < 3) s.n = 3;
    if (s.n > kHpssMaxHist) s.n = kHpssMaxHist;

    // 写入历史
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float v = (isfinite(bands[i]) && bands[i] > 0.0f) ? bands[i] : 0.0f;
        s.hist[s.head][i] = v;
    }
    s.head = (s.head + 1) % s.n;
    if (s.fill < s.n) ++s.fill;

    float buf[kHpssMaxHist];
    for (int i = 0; i < NUM_BANDS; ++i) {
        // 时间轴中值 → 谐波候选
        for (int k = 0; k < s.fill; ++k) {
            int idx = s.head - 1 - k;
            while (idx < 0) idx += s.n;
            buf[k] = s.hist[idx][i];
        }
        const float h = medianOf(buf, s.fill);

        // 频率轴中值 → 打击候选。**只看当前帧**，不看历史 ——
        // 打击声的特征就是「此刻横跨整个频段」，掺进历史等于把它抹平。
        int m = 0;
        for (int d = -c.freq_win; d <= c.freq_win; ++d) {
            const int j = i + d;
            if (j < 0 || j >= NUM_BANDS) continue;
            buf[m++] = s.hist[(s.head + s.n - 1) % s.n][j];
        }
        const float p = medianOf(buf, m);

        // 软掩膜。平方是为了把差距拉开，见文件头。
        const float h2 = h * h, p2 = p * p;
        const float den = h2 + p2 + c.eps;
        const float cur = s.hist[(s.head + s.n - 1) % s.n][i];
        out_h[i] = cur * (h2 / den);
        out_p[i] = cur * (p2 / den);
    }
}

// 打击能量占总能量的比例，[0,1]。自动选灯效时用它区分「以人声/旋律为主」
// 和「以鼓为主」—— 这是流派判断里最有分量的一个量。
inline float percussiveRatio(const float *h, const float *p) {
    float sh = 0.0f, sp = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (isfinite(h[i]) && h[i] > 0.0f) sh += h[i];
        if (isfinite(p[i]) && p[i] > 0.0f) sp += p[i];
    }
    const float tot = sh + sp;
    return (tot > 1e-12f) ? (sp / tot) : 0.0f;
}

} // namespace lamp
