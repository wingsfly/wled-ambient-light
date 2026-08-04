// 人声存在度。
//
// ⚠️ **先把话说清楚：这不是「唱歌的人」检测器，而且它有一条硬边界。**
//
// 它测的是「此刻有没有一条**处在人声音域、谐波型、且能量集中在共振峰带**
// 的主旋律」。萨克斯独奏、小提琴、失真吉他的长音都会被判成高分 ——
// 单靠 16 段能量 + 单基频，做不到把人声和同音域的独奏乐器分开，
// 那需要音色模型，不是这块板子这一帧 5.8ms 能负担的。
//
// **它只在前景旋律干净的时候可用。** 实测（tools/lamp_calib.cpp，60s）：
//
//   素材            f0conf  共振峰  1-打击   vocal  过0.55占
//   流行             0.93    0.99    0.74    0.57    66%    ← 干净前景旋律，可用
//   古典·柔板       0.88    0.58    0.76    0.13     0%
//   摇滚·断续人声   0.73    0.32    0.56    0.03     0%    ← **抓不到**
//   摇滚·器乐       0.76    0.23    0.58    0.00     0%
//   Rap              0.00    0.00    0.66    0.00     0%
//
// 失真吉他那堵墙把共振峰占比压在 0.32，而密集混音的基线本来就是 0.50 ——
// **不存在既收进摇滚人声、又挡住密集混音的阈值。** 这是 16 段 + 单基频
// 这套特征的天花板，不是阈值没调好。
//
// 试过一条「相对本曲基线抬升」的旁路（快慢两条共振峰包络相减），
// 专为摇滚这一档而建，结果只把它从 0.00 抬到 0.02 —— 因为在密集编曲里
// voiced(0.64) 与 harmonic(0.32) 两个因子自己就已经很低，抬升再准也乘没了。
// **按第 27 条拆掉了**，数留在这里免得下次再造一遍。
//
// 之所以还是值得做：在民谣/流行/人声为主的素材上，它把「有人在唱」与
// 「只有节奏和铺底」分得很干净，而那正是听感上最明显的分界。
// 界面上标成「人声 / 主旋律」，不标成「人声」—— 别夸大它能做什么。
//
// ── 三个因子相乘，缺一不可 ──
//
//   voiced   基频存在且落在人声音域（80–1100Hz，从男低音到女高音）
//   formant  谐波能量集中在 300–3000Hz（共振峰带，人声可懂度所在）
//   harmonic 谐波占比高（1 − 打击占比）
//
// **相乘而不是相加**：底鼓的基频也在 80Hz 附近（voiced 可能过），
// 镲片的能量也在高段（harmonic 不过），只有三样同时成立才是主旋律。
// 相加的话任何一项拉满都能糊弄过去。
//
// ── 时间常数按物理时间定 ──
//
// 各档 hop 从 5.8ms 到 46.4ms 差 8 倍，写死帧数的话同一段音乐在不同档
// 的行为完全不同。这是整个项目的规矩，见 lamp_envelope.h 的 envCoeff()。
//
// 起音快、释放慢（150ms / 600ms）：一句唱腔里换气的空档不该让灯灭掉，
// 但从器乐段进人声段要立刻响应。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_bands.h"
#include "lamp_envelope.h"   // envCoeff

namespace lamp {

struct VocalConfig {
    // 人声基频范围。下界取男低音 ~80Hz，上界取女高音 ~1100Hz。
    // 放宽没好处：底鼓基频 50–70Hz，放到 60 就会把鼓算进来。
    float f0_lo_hz = 80.0f;
    float f0_hi_hz = 1100.0f;
    float f0_conf_min = 0.25f;    // 低于此认为基频不可信

    // 共振峰带：用 Hz 定，不写死段号 —— 段边界是 lamp_bands.h 的事，
    // 那边改了这里应当自动跟上。
    float formant_lo_hz = 300.0f;
    float formant_hi_hz = 3000.0f;
    // 共振峰带占比要过这条线才算「集中」。**这个数是量出来的，不是估的**
    // （初稿写 0.45 并注了「噪声约 0.35」—— 那是猜的，密集混音实测 0.50，
    // 直接从门槛底下穿过去了，是测试逮住的）：
    //
    //   每 Hz 等能量（真白噪声）   0.293
    //   每段等能量（密集混音）     0.500   ← 必须挡住
    //   人声压在全频段底噪上       0.748   ← 必须放过
    //   能量全在共振峰带（纯人声） 0.997
    //
    // 0.62 落在 0.500 与 0.748 中间，两边都有余量。
    float formant_share_min = 0.62f;

    float harmonic_min = 0.35f;   // 1 − 打击占比 要过这条线

    float attack_ms  = 150.0f;
    float release_ms = 600.0f;

    // 起音判定：从低于 off 升到高于 on 才算一次「人声进来了」。
    // 双阈值是为了不在阈值附近抖动 —— 与三路活跃判定同一个道理。
    float onset_on  = 0.55f;
    float onset_off = 0.35f;
};

struct VocalState {
    float vocal = 0.0f;       // [0,1] 平滑后的存在度
    float raw   = 0.0f;       // 未平滑，只为测试与探针
    bool  onset = false;      // 本帧刚进入人声段
    bool  above = false;      // 滞回用：当前在不在「人声段」里
};

// 共振峰带占比：落在 [lo,hi] 内的**谐波**能量 ÷ 全部谐波能量。
//
// 用 bands_h 而不是 bands：底鼓/军鼓在 300–3000Hz 也有大量能量，
// 用混合谱的话一段密集的鼓点也能把这个比例顶上去。
//
// 段与区间部分重叠时按**频率跨度比例**计入，不是非零即一 ——
// 16 段很粗，一刀切会让 300Hz 这条线的位置决定 8% 的结果。
inline float formantShare(const VocalConfig &c, const float *bands_h) {
    if (!bands_h) return 0.0f;
    double inside = 0.0, total = 0.0;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float e = bands_h[i];
        if (!isfinite(e) || e <= 0.0f) continue;
        total += e;
        const float lo = kBandEdgeHz[i], hi = kBandEdgeHz[i + 1];
        const float a = lo > c.formant_lo_hz ? lo : c.formant_lo_hz;
        const float b = hi < c.formant_hi_hz ? hi : c.formant_hi_hz;
        if (b > a) inside += (double)e * (double)((b - a) / (hi - lo));
    }
    return total > 0.0 ? (float)(inside / total) : 0.0f;
}

// 把 x 相对阈值 th 映射到 [0,1]：th 处为 0，1.0 处为 1。
// 与 lamp_auto.h 的 gateAbove 同一个形状 —— 阈值是**起点**不是缩放因子，
// 这一点在 auto 那边踩过坑（用成缩放会让兜底永远选不上）。
inline float vocalGate(float x, float th) {
    if (!(th < 1.0f)) return 0.0f;
    if (!isfinite(x)) return 0.0f;
    const float v = (x - th) / (1.0f - th);
    return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v);
}

inline void vocalUpdate(VocalState &s, const VocalConfig &c,
                        float f0_hz, float f0_conf, bool f0_voiced,
                        const float *bands_h, float percussive, float dt_ms) {
    float voiced = 0.0f;
    if (f0_voiced && isfinite(f0_hz) &&
        f0_hz >= c.f0_lo_hz && f0_hz <= c.f0_hi_hz)
        voiced = vocalGate(isfinite(f0_conf) ? f0_conf : 0.0f, c.f0_conf_min);

    const float share   = formantShare(c, bands_h);
    const float formant = vocalGate(share, c.formant_share_min);
    const float perc    = (isfinite(percussive) && percussive > 0.0f)
                        ? (percussive < 1.0f ? percussive : 1.0f) : 0.0f;
    const float harm    = vocalGate(1.0f - perc, c.harmonic_min);

    s.raw = voiced * formant * harm;

    // 起音快、释放慢：换气的空档不该让灯灭掉。
    const float tau = (s.raw > s.vocal) ? c.attack_ms : c.release_ms;
    s.vocal += envCoeff(tau, dt_ms) * (s.raw - s.vocal);
    if (!isfinite(s.vocal) || s.vocal < 0.0f) s.vocal = 0.0f;
    if (s.vocal > 1.0f) s.vocal = 1.0f;

    const bool was = s.above;
    if (s.above) { if (s.vocal < c.onset_off) s.above = false; }
    else         { if (s.vocal > c.onset_on)  s.above = true;  }
    s.onset = s.above && !was;
}

}  // namespace lamp
