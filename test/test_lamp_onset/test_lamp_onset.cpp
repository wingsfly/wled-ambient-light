#include <unity.h>
#include <math.h>
#include <string.h>
#include "lamp_onset.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// ── Spectral flux ─────────────────────────────────────────

static void fill(float *b, float v) {
    for (int i = 0; i < NUM_BANDS; ++i) b[i] = v;
}

// flux 只累加**正向**变化。负向是音符衰减，不是起音。
void test_flux_is_half_wave_rectified(void) {
    float a[NUM_BANDS], b[NUM_BANDS];
    fill(a, 0.5f); fill(b, 0.5f);
    b[3] = 0.9f;                                    // 一段升
    b[7] = 0.1f;                                    // 一段降
    const float up = spectralFlux(a, b, 10.0f);
    // 只有 +0.4 被计入；−0.4 必须被丢弃
    TEST_ASSERT_TRUE_MESSAGE(up > 0.0f, "正向变化没被计入");

    float c[NUM_BANDS];
    fill(c, 0.5f); c[3] = 0.9f;                     // 只有升，没有降
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, up, spectralFlux(a, c, 10.0f),
        "负向变化影响了结果 —— 没做半波整流");
}

void test_flux_of_identical_frames_is_zero(void) {
    float a[NUM_BANDS];
    fill(a, 0.37f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, spectralFlux(a, a, 10.0f));
}

void test_flux_of_全面下降_is_zero(void) {
    float a[NUM_BANDS], b[NUM_BANDS];
    fill(a, 0.8f); fill(b, 0.2f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, spectralFlux(a, b, 10.0f),
        "整体衰减被当成了起音");
}

// flux 是**变化率**，要除以帧间隔。
//
// 这一条是 §3.3.4 第 2 条那句「换档后失效的只是 spectral flux 的尺度」的正解：
// 差分的大小天然正比于帧间隔，hop 减半则同一段音乐的每帧差分也减半。
// 除以 Δt 之后尺度就跨档可比了，换档后自适应阈值只需微调而不是重新爬。
void test_flux_is_a_rate_and_scales_with_hop(void) {
    float a[NUM_BANDS], b[NUM_BANDS];
    fill(a, 0.2f); fill(b, 0.6f);
    const float f_slow = spectralFlux(a, b, 40.0f);
    const float f_fast = spectralFlux(a, b, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 4.0f * f_slow, f_fast,
        "flux 没有按 Δt 归一化 —— 换档后尺度会跳变");
}

void test_flux_rejects_non_finite(void) {
    float a[NUM_BANDS], b[NUM_BANDS];
    fill(a, 0.2f); fill(b, 0.6f);
    b[5] = NAN;
    const float f = spectralFlux(a, b, 10.0f);
    TEST_ASSERT_FALSE_MESSAGE(isnan(f), "NaN 渗进了 flux");
    TEST_ASSERT_TRUE(f > 0.0f);          // 其余 15 段仍应贡献

    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, spectralFlux(a, b, 0.0f));   // Δt=0

    // inf 与 NaN 不同：`d > 0` 对 NaN 天然为 false，对 +inf 却为 true，
    // 一次累加就把整个 flux 变成 inf，然后污染均值、阈值、速率。
    // 挡住它的是 isfinite，不是那个 > 0。
    float e[NUM_BANDS];
    fill(e, 0.2f); e[5] = INFINITY;
    const float fi = spectralFlux(a, e, 10.0f);
    TEST_ASSERT_FALSE_MESSAGE(isinf(fi), "+inf 渗进了 flux");
    TEST_ASSERT_FALSE(isnan(fi));
}

// ── 合成节拍轨 ────────────────────────────────────────────

// 生成一帧。真实起音有 40ms 左右的攻击段，**跨多帧**——
// 这一点很要紧：若把起音写成单帧的瞬时跳变，flux 就只有一帧为正，
// 于是上升沿判据、不应期、自适应阈值全都不会被触达，测试看着全绿而实际没测到。
// 第一版就是这么写的，32 个变异体活了 16 个。
static void beatFrame(float *out, float phase, float period_ms, float amp) {
    const float t_ms = phase * period_ms;
    const float attack_ms = 40.0f;
    const float env = (t_ms < attack_ms) ? (t_ms / attack_ms)
                                         : expf(-(t_ms - attack_ms) / 120.0f);
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float w = (i < 4) ? 1.0f : 0.35f;      // 低频段更强
        out[i] = 0.05f + amp * w * env;
    }
}

// 确定性伪随机，用作背景扰动。
static float noiseAt(uint32_t k) {
    uint32_t s = k * 1664525u + 1013904223u;
    s ^= s >> 16; s *= 2246822519u; s ^= s >> 13;
    return (float)(s & 0xFFFFu) / 65535.0f;          // 0..1
}

// 跑一段指定 BPM 的节拍轨，返回检出的 onset 数。
static int runBeatTrack(OnsetDetector &d, const OnsetConfig &c,
                        float bpm, float dt_ms, float seconds, float amp = 0.8f) {
    const float period_ms = 60000.0f / bpm;
    const int   frames    = (int)(seconds * 1000.0f / dt_ms);
    float bands[NUM_BANDS];
    int count = 0;
    for (int k = 0; k < frames; ++k) {
        const float t     = (float)k * dt_ms;
        const float phase = fmodf(t, period_ms) / period_ms;
        beatFrame(bands, phase, period_ms, amp);
        if (onsetUpdate(d, c, bands, (uint32_t)t)) ++count;
    }
    return count;
}

// ── 检测 ──────────────────────────────────────────────────

void test_detects_beats_at_120bpm(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    const int n = runBeatTrack(d, c, 120.0f, 23.22f, 10.0f);
    // 10 秒 × 2 拍/秒 = 20 拍，允许前几拍在阈值爬升期漏掉
    TEST_ASSERT_TRUE_MESSAGE(n >= 17 && n <= 21, "120BPM 的拍数不对");
}

// 四个档位的 Δt 差 8 倍，检出的拍数必须一致 —— 与频段归一化、包络同一个要求。
void test_detection_is_invariant_to_hop(void) {
    const OnsetConfig c;
    const float dts[4] = {46.44f, 23.22f, 11.61f, 5.80f};
    for (int i = 0; i < 4; ++i) {
        OnsetDetector d;
        onsetInit(d, c, dts[i]);
        const int n = runBeatTrack(d, c, 120.0f, dts[i], 10.0f);
        TEST_ASSERT_TRUE_MESSAGE(n >= 17 && n <= 21, "换 hop 后拍数变了");
    }
}

void test_detects_a_range_of_tempos(void) {
    const OnsetConfig c;
    const float bpms[5] = {60.0f, 90.0f, 120.0f, 140.0f, 180.0f};
    for (int i = 0; i < 5; ++i) {
        OnsetDetector d;
        onsetInit(d, c, 23.22f);
        const int n = runBeatTrack(d, c, bpms[i], 23.22f, 12.0f);
        const int want = (int)(bpms[i] / 60.0f * 12.0f);
        TEST_ASSERT_TRUE_MESSAGE(n >= want - 4 && n <= want + 2, "拍数偏离过多");
    }
}

// 每拍**恰好**报一次。40ms 的攻击段在最快档跨 7 帧，flux 连续 7 帧为正 ——
// 没有上升沿判据就会报 7 次，没有不应期也会报多次。只断言上界不够，
// 必须钉死等于拍数。
void test_exactly_one_onset_per_beat(void) {
    const OnsetConfig c;
    const float dts[3] = {23.22f, 11.61f, 5.80f};
    for (int i = 0; i < 3; ++i) {
        OnsetDetector d;
        onsetInit(d, c, dts[i]);
        const int n = runBeatTrack(d, c, 120.0f, dts[i], 10.0f);
        TEST_ASSERT_TRUE_MESSAGE(n >= 18 && n <= 20,
            "拍数不等于 20 —— 上升沿判据或不应期没起作用");
    }
}

// 攻击段本身跨多帧，这是上面那条能测到东西的前提。
// 若哪天有人把合成信号改回单帧跳变，这条会先叫出来。
void test_synthetic_attack_spans_multiple_frames(void) {
    const float dt = 5.80f, period = 500.0f;
    float a[NUM_BANDS], b[NUM_BANDS];
    int positive = 0;
    beatFrame(a, 0.0f, period, 0.8f);
    for (int k = 1; k < 12; ++k) {
        beatFrame(b, (float)k * dt / period, period, 0.8f);
        if (spectralFlux(a, b, dt) > 0.0f) ++positive;
        for (int i = 0; i < NUM_BANDS; ++i) a[i] = b[i];
    }
    TEST_ASSERT_TRUE_MESSAGE(positive >= 5,
        "合成信号的攻击段只有一两帧 —— 上升沿与不应期将测不到");
}

// 在**高 flux 背景**下只报拍点。这才是「自适应」阈值的意义所在。
//
// 固定阈值在这里会全军覆没：背景扰动的 flux 本身就超过任何固定值，
// 于是每帧都报。前一版的测试只有「有拍点」和「全静音」两种输入，
// 中间这一大片完全没覆盖，阈值机制的四个变异体因此全部存活。
void test_adaptive_threshold_rejects_a_noisy_background(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    const float period = 500.0f;
    float bands[NUM_BANDS];
    int n = 0;
    for (int k = 0; k < (int)(20000.0f / 23.22f); ++k) {
        const float t = (float)k * 23.22f;
        beatFrame(bands, fmodf(t, period) / period, period, 0.8f);
        for (int i = 0; i < NUM_BANDS; ++i)          // 每帧 ±0.15 的背景扰动
            bands[i] += 0.15f * noiseAt((uint32_t)(k * NUM_BANDS + i));
        if (onsetUpdate(d, c, bands, (uint32_t)t)) ++n;
    }
    // 20 秒 × 2 拍/秒 = 40 拍。背景扰动会带来一些误报，但不能失控
    TEST_ASSERT_TRUE_MESSAGE(n >= 30, "噪声背景下漏了太多拍");
    TEST_ASSERT_TRUE_MESSAGE(n <= 60,
        "噪声背景下误报失控 —— 阈值没有跟着背景抬起来");
}

// 音量整体抬高 6 倍后，检出的拍数不变。
// 乘性阈值就是干这个的：只有绝对偏置的话，响的时候会把背景也当成拍点。
void test_threshold_tracks_overall_level(void) {
    const OnsetConfig c;
    OnsetDetector quiet, loud;
    onsetInit(quiet, c, 23.22f);
    onsetInit(loud,  c, 23.22f);
    const int nq = runBeatTrack(quiet, c, 120.0f, 23.22f, 15.0f, 0.15f);
    const int nl = runBeatTrack(loud,  c, 120.0f, 23.22f, 15.0f, 0.90f);
    TEST_ASSERT_TRUE_MESSAGE(nq >= 26 && nq <= 30, "轻声段拍数不对");
    TEST_ASSERT_TRUE_MESSAGE(nl >= 26 && nl <= 30, "响声段拍数不对");
}

// 稳态输入不报 onset。
void test_steady_input_reports_nothing(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    float bands[NUM_BANDS];
    fill(bands, 0.4f);
    int n = 0;
    for (uint32_t t = 0; t < 10000; t += 23)
        if (onsetUpdate(d, c, bands, t)) ++n;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, "稳态输入报了 onset");
}

// 静音不报 onset，也不得因为阈值趋零而开始对底噪敏感。
void test_silence_reports_nothing(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    float bands[NUM_BANDS];
    int n = 0;
    // **间歇性**抖动，不是每帧都抖：每帧都抖的话滑动均值会跟上去，
    // 乘性项自己就把它挡了，测不出绝对偏置 delta 有没有用。
    // 大部分时间 flux=0 让均值趋零，此时阈值就只剩 delta 一个人守着。
    for (uint32_t t = 0; t < 20000; t += 23) {
        fill(bands, 0.0f);
        if ((t / 23) % 40 == 0) bands[(t / 23) % NUM_BANDS] = 0.004f;
        if (onsetUpdate(d, c, bands, t)) ++n;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, "静音时对底噪报了 onset —— delta 偏置太小");
}

// 第一帧没有前一帧可比，不得凭空报一个 onset。
void test_first_frame_never_fires(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    float bands[NUM_BANDS];
    fill(bands, 0.9f);                        // 从零跳到 0.9，差分很大
    TEST_ASSERT_FALSE_MESSAGE(onsetUpdate(d, c, bands, 0),
        "第一帧就报了 onset —— 没有前一帧可比");
}

// 上升沿判据与不应期管的是**两件不同的事**，参数一变就分家：
//
//   上升沿：同一个起音的攻击段跨多帧，只认第一帧
//   不应期：两个独立的起音靠得太近（<60ms），物理上不可能是两拍
//
// 默认参数下攻击段 40ms 短于不应期 60ms，不应期顺手把上升沿的活也干了 ——
// 于是两个判据互为冗余，删掉任何一个测试都发现不了。必须分别构造。

// 长攻击段（200ms 渐强，远超不应期）：只有上升沿判据能压住。
void test_long_attack_still_fires_once(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 11.61f);
    float bands[NUM_BANDS];

    // 先跑一段稳态把均值养起来
    fill(bands, 0.05f);
    for (int k = 0; k < 200; ++k) onsetUpdate(d, c, bands, (uint32_t)(k * 11.61f));

    // 200ms 线性渐强，跨 17 帧，每帧 flux 都为正且远超阈值
    int n = 0;
    for (int k = 0; k < 18; ++k) {
        const float lvl = 0.05f + 0.85f * (float)k / 17.0f;
        fill(bands, lvl);
        if (onsetUpdate(d, c, bands, (uint32_t)(2322.0f + k * 11.61f))) ++n;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "200ms 的渐强报了不止一次 —— 上升沿判据没起作用（不应期只有 60ms，压不住）");
}

// 两个独立起音间隔 30ms（短于不应期）：只有不应期能压住。
// 中间插一帧低 flux 让 armed 复位，上升沿判据就失效了。
void test_two_close_onsets_collapse_into_one(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 10.0f);
    float bands[NUM_BANDS];

    fill(bands, 0.05f);
    for (int k = 0; k < 200; ++k) onsetUpdate(d, c, bands, (uint32_t)(k * 10));

    int n = 0;
    fill(bands, 0.60f); if (onsetUpdate(d, c, bands, 2000)) ++n;   // 起音 1
    fill(bands, 0.10f); if (onsetUpdate(d, c, bands, 2010)) ++n;   // 回落，armed 复位
    fill(bands, 0.05f); if (onsetUpdate(d, c, bands, 2020)) ++n;
    fill(bands, 0.65f); if (onsetUpdate(d, c, bands, 2030)) ++n;   // 起音 2，距起音 1 仅 30ms
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "相隔 30ms 的两个起音都报了 —— 不应期没起作用");

    // 而间隔拉到 80ms（超过不应期）就该报两次
    OnsetDetector e;
    onsetInit(e, c, 10.0f);
    fill(bands, 0.05f);
    for (int k = 0; k < 200; ++k) onsetUpdate(e, c, bands, (uint32_t)(k * 10));
    int m = 0;
    fill(bands, 0.60f); if (onsetUpdate(e, c, bands, 2000)) ++m;
    fill(bands, 0.10f); onsetUpdate(e, c, bands, 2010);
    fill(bands, 0.05f); for (int k = 2; k < 8; ++k) onsetUpdate(e, c, bands, (uint32_t)(2000 + k * 10));
    fill(bands, 0.65f); if (onsetUpdate(e, c, bands, 2080)) ++m;
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, m, "间隔 80ms 的两个起音只报了一次");
}

// 阶跃到高位后保持稳定，只该报一次 —— prev 必须每帧更新。
// prev 不更新的话，之后每一帧都在和那个远低的初始帧比，flux 持续为正。
void test_step_up_then_steady_fires_once(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    float bands[NUM_BANDS];

    fill(bands, 0.05f);
    for (int k = 0; k < 100; ++k) onsetUpdate(d, c, bands, (uint32_t)(k * 23));

    int n = 0;
    fill(bands, 0.80f);
    for (int k = 100; k < 300; ++k)                      // 阶跃后稳定 4.6 秒
        if (onsetUpdate(d, c, bands, (uint32_t)(k * 23))) ++n;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "阶跃后持续报 onset —— prev 没有每帧更新");
}

// 均值必须在阈值判定**之后**更新，否则本帧的尖峰会先把自己的阈值抬高。
//
// 要看见这个差别，得同时满足两件事：`mean_tau` 与 Δt 同量级（否则 a_mean 太小，
// 自抬的幅度淹没在余量里），以及尖峰**刚好**越过正常阈值（否则自抬后照样越过）。
// 默认的 400ms / 23ms 下 a_mean 只有 0.057，自抬 9% —— 怎么试都测不出来。
void test_mean_updates_after_the_comparison(void) {
    OnsetConfig c;
    c.mean_tau_ms   = 10.0f;         // 与 Δt 同量级 → a_mean ≈ 0.63
    c.delta         = 0.05f;
    c.refractory_ms = 0;
    OnsetDetector d;
    onsetInit(d, c, 10.0f);
    float bands[NUM_BANDS];

    // 稳定背景：每帧涨 0.01 → flux = 0.01×16×100 = 16，均值收敛到 16，阈值 25.65
    float lvl = 0.0f;
    for (int k = 0; k < 300; ++k) {
        lvl += 0.01f; fill(bands, lvl);
        onsetUpdate(d, c, bands, (uint32_t)(k * 10));
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 16.0f, d.mean, "背景均值没养到预期值");

    // 尖峰涨 0.0165 → flux ≈ 26.4，刚过 25.65。
    // 若均值先更新，阈值会被自己抬到 36 左右，这一帧就报不出来了。
    lvl += 0.0165f; fill(bands, lvl);
    TEST_ASSERT_TRUE_MESSAGE(onsetUpdate(d, c, bands, 3000),
        "刚好够格的尖峰没报 —— 均值在判定前就被本帧抬高了");
}

// ── onset 速率 ────────────────────────────────────────────

// onset_rate 是喂给 StyleFeatures 的主判据，必须是「每秒个数」而非「每帧个数」。
void test_onset_rate_converges_to_beats_per_second(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    runBeatTrack(d, c, 120.0f, 23.22f, 30.0f);     // 2 拍/秒
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, 2.0f, d.rate, "速率没收敛到 2/秒");

    OnsetDetector d2;
    onsetInit(d2, c, 23.22f);
    runBeatTrack(d2, c, 180.0f, 23.22f, 30.0f);    // 3 拍/秒
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.4f, 3.0f, d2.rate, "速率没收敛到 3/秒");
}

// 速率也必须跨 hop 一致 —— 否则换档时 StyleFeatures 的主判据会跳，
// 而那正是决定要不要换档的东西，会自激。
void test_onset_rate_is_invariant_to_hop(void) {
    const OnsetConfig c;
    const float dts[3] = {46.44f, 11.61f, 5.80f};
    for (int i = 0; i < 3; ++i) {
        OnsetDetector d;
        onsetInit(d, c, dts[i]);
        runBeatTrack(d, c, 120.0f, dts[i], 30.0f);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.4f, 2.0f, d.rate, "onset 速率随 hop 漂了");
    }
}

void test_onset_rate_decays_to_zero_in_silence(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    runBeatTrack(d, c, 120.0f, 23.22f, 20.0f);
    TEST_ASSERT_TRUE(d.rate > 1.0f);

    float bands[NUM_BANDS];
    fill(bands, 0.0f);
    for (uint32_t t = 20000; t < 40000; t += 23) onsetUpdate(d, c, bands, t);
    TEST_ASSERT_TRUE_MESSAGE(d.rate < 0.1f, "静音 20 秒后速率仍未回落");
}

// ── 换档 ──────────────────────────────────────────────────

// 换档只重置自适应阈值的**尺度**，不清 onset 速率与前一帧频谱。
// §3.3.4 第 2 条：全量重新锁定要 6 秒，用户会看到灯「发呆」。
void test_retime_keeps_rate_and_history(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    runBeatTrack(d, c, 120.0f, 23.22f, 20.0f);
    const float rate = d.rate;

    onsetRetime(d, c, 5.80f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, rate, d.rate, "换档清掉了 onset 速率");
    TEST_ASSERT_TRUE_MESSAGE(d.has_prev, "换档清掉了前一帧频谱");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(c.rate_tau_ms, 5.80f), d.a_rate);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(c.mean_tau_ms, 5.80f), d.a_mean,
        "retime 没换均值的系数");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 5.80f, d.dt_ms, "retime 没记录新的 dt");
}

// Init 必须清脏对象。测试里都用新声明的 OnsetDetector，成员有默认初始化，
// 「不清」和「清了」看不出区别 —— 必须显式喂一个脏对象。
void test_init_clears_a_dirty_detector(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    runBeatTrack(d, c, 120.0f, 23.22f, 15.0f);
    TEST_ASSERT_TRUE(d.rate > 1.0f);            // 确认真的脏了（正对照）
    TEST_ASSERT_TRUE(d.has_prev);

    onsetInit(d, c, 23.22f);
    TEST_ASSERT_FALSE_MESSAGE(d.has_prev, "Init 没清 has_prev");
    TEST_ASSERT_FALSE_MESSAGE(d.has_onset, "Init 没清 has_onset");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, d.rate, "Init 没清速率");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, d.mean, "Init 没清均值");
    // 清干净之后第一帧仍不得凭空报
    float bands[NUM_BANDS];
    fill(bands, 0.9f);
    TEST_ASSERT_FALSE_MESSAGE(onsetUpdate(d, c, bands, 0), "Init 后第一帧就报了");
}

// last_onset_ms 恰为 0 时不应期照样要生效。
// 0 是合法的 millis 值（回绕后真的会走到），拿它当「从未触发」的哨兵会漏判。
void test_refractory_holds_when_last_onset_is_zero(void) {
    OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    d.has_onset = true;
    d.last_onset_ms = 0;
    TEST_ASSERT_FALSE_MESSAGE(refractoryExpired(d, c, 30),
        "last_onset_ms 为 0 时不应期被跳过 —— 用了哨兵值而非独立标志位");
    TEST_ASSERT_TRUE(refractoryExpired(d, c, c.refractory_ms));
}

// 换档后必须很快恢复检测，不能停摆几秒。
void test_detection_resumes_quickly_after_retime(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);
    runBeatTrack(d, c, 120.0f, 23.22f, 20.0f);

    onsetRetime(d, c, 5.80f);
    // 换档后只跑 3 秒，应至少检出 4 拍（3 秒 × 2 拍/秒，允许一拍的过渡）
    const float period_ms = 500.0f;
    float bands[NUM_BANDS];
    int n = 0;
    for (int k = 0; k < (int)(3000.0f / 5.80f); ++k) {
        const float t = 20000.0f + (float)k * 5.80f;
        beatFrame(bands, fmodf(t, period_ms) / period_ms, period_ms, 0.8f);
        if (onsetUpdate(d, c, bands, (uint32_t)t)) ++n;
    }
    TEST_ASSERT_TRUE_MESSAGE(n >= 4, "换档后检测停摆了 —— 阈值尺度没跟着换");
}

// ── 时钟 ──────────────────────────────────────────────────

// 不应期的时间比较必须回绕安全，且要覆盖 >2^31 的间隔（docs/17 第 18 条）。
void test_refractory_survives_wraparound_and_long_gaps(void) {
    const OnsetConfig c;
    OnsetDetector d;
    onsetInit(d, c, 23.22f);

    const uint32_t t0 = (uint32_t)(0u - 100u);       // 距回绕 100ms
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)(t0 + 500) < t0, "这个 t0 没有真的回绕");
    float bands[NUM_BANDS];
    fill(bands, 0.1f);
    onsetUpdate(d, c, bands, t0);
    for (int k = 1; k < 40; ++k) {
        beatFrame(bands, fmodf((float)k * 23.22f, 500.0f) / 500.0f, 500.0f, 0.8f);
        onsetUpdate(d, c, bands, (uint32_t)(t0 + (uint32_t)(k * 23)));
    }
    TEST_ASSERT_FALSE(isnan(d.rate));

    // 上一次 onset 之后隔了 27.9 天：不应期早该过了
    OnsetDetector e;
    onsetInit(e, c, 23.22f);
    fill(bands, 0.1f);
    onsetUpdate(e, c, bands, 1000);
    e.last_onset_ms = 1000; e.has_onset = true;
    beatFrame(bands, 0.05f, 500.0f, 0.9f);
    const uint32_t huge = 0x90000000u;
    TEST_ASSERT_TRUE_MESSAGE(refractoryExpired(e, c, (uint32_t)(1000 + huge)),
        "间隔 27.9 天后不应期仍判为未过 —— 用了有符号比较");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_flux_is_half_wave_rectified);
    RUN_TEST(test_flux_of_identical_frames_is_zero);
    RUN_TEST(test_flux_of_全面下降_is_zero);
    RUN_TEST(test_flux_is_a_rate_and_scales_with_hop);
    RUN_TEST(test_flux_rejects_non_finite);
    RUN_TEST(test_detects_beats_at_120bpm);
    RUN_TEST(test_detection_is_invariant_to_hop);
    RUN_TEST(test_detects_a_range_of_tempos);
    RUN_TEST(test_exactly_one_onset_per_beat);
    RUN_TEST(test_synthetic_attack_spans_multiple_frames);
    RUN_TEST(test_adaptive_threshold_rejects_a_noisy_background);
    RUN_TEST(test_threshold_tracks_overall_level);
    RUN_TEST(test_long_attack_still_fires_once);
    RUN_TEST(test_two_close_onsets_collapse_into_one);
    RUN_TEST(test_step_up_then_steady_fires_once);
    RUN_TEST(test_mean_updates_after_the_comparison);
    RUN_TEST(test_steady_input_reports_nothing);
    RUN_TEST(test_silence_reports_nothing);
    RUN_TEST(test_first_frame_never_fires);
    RUN_TEST(test_onset_rate_converges_to_beats_per_second);
    RUN_TEST(test_onset_rate_is_invariant_to_hop);
    RUN_TEST(test_onset_rate_decays_to_zero_in_silence);
    RUN_TEST(test_retime_keeps_rate_and_history);
    RUN_TEST(test_init_clears_a_dirty_detector);
    RUN_TEST(test_refractory_holds_when_last_onset_is_zero);
    RUN_TEST(test_detection_resumes_quickly_after_retime);
    RUN_TEST(test_refractory_survives_wraparound_and_long_gaps);
    return UNITY_END();
}
