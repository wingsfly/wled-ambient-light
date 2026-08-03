#include <unity.h>
#include <math.h>
#include "lamp_fx.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// 这个套件测的是**接线**，不是各模块内部 —— 那些已经在各自的套件里测透了。
// 它要回答的是集成评审提出的那几个问题：换档时五个模块是否同步、
// AGC 增益是否真的施加了、StyleFeatures 的四个判据是否都有值。

static float g_pcm[kMaxFftLen];
static float g_mag[kMaxFftLen / 2 + 1];
static float g_win[kMaxFftLen];
static Pipeline   g_p;
static PipelineConfig g_c;

// 主机端的参考 DFT。目标板上这一步是 esp-dsp。
static void analyze(size_t n, WindowType wt) {
    fillWindow(wt, g_win, n);
    for (size_t k = 0; k <= n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (size_t t = 0; t < n; ++t) {
            const double a = -6.283185307179586 * (double)k * (double)t / (double)n;
            const double v = (double)g_pcm[t] * (double)g_win[t];
            re += v * cos(a); im += v * sin(a);
        }
        g_mag[k] = (float)sqrt(re * re + im * im);
    }
}

// 合成一帧：低频拍点 + 可选的宽带内容。
static void fillPcm(size_t n, uint32_t frame, float bpm, float amp, float bright) {
    const float period = 60000.0f / bpm;
    for (size_t t = 0; t < n; ++t) {
        const float ms = (float)frame * g_p.dt_ms + (float)t * 1000.0f / kSampleRate;
        const float ph = fmodf(ms, period) / period;
        const float env = (ph < 0.08f) ? (ph / 0.08f) : expf(-(ph - 0.08f) * 8.0f);
        float v = amp * env * sinf(6.283185307f * 80.0f * ms / 1000.0f);
        v += bright * amp * env * sinf(6.283185307f * 4200.0f * ms / 1000.0f);
        g_pcm[t] = v;
    }
}

static AudioFrame step(uint32_t frame, float bpm, float amp, float bright = 0.2f) {
    fillPcm(g_p.an.n, frame, bpm, amp, bright);
    analyze(g_p.an.n, g_p.an.wt);
    return pipelineProcess(g_p, g_c, g_pcm, g_mag, (uint32_t)(frame * g_p.dt_ms));
}

// ── 装配 ──────────────────────────────────────────────────

void test_init_sets_every_module_to_the_same_hop(void) {
    TEST_ASSERT_TRUE(pipelineInit(g_p, g_c, STYLE_GENERAL));
    const float dt = presetHopMs(STYLE_GENERAL);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, dt, g_p.dt_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(g_c.env.fast_tau_ms, dt), g_p.env.a_fast);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(g_c.agc.attack_ms,   dt), g_p.agc.a_attack);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(g_c.onset.mean_tau_ms, dt), g_p.onset.a_mean);
    TEST_ASSERT_EQUAL_UINT(presetParams(STYLE_GENERAL).n, (unsigned)g_p.an.n);
}

// 换档必须让**五个**模块同步。这条是集成评审的核心诉求：
// 各模块的 retime 单独测过，但「漏调其中一个」只有在这里才看得见。
void test_retime_updates_every_module_at_once(void) {
    pipelineInit(g_p, g_c, STYLE_GENERAL);
    TEST_ASSERT_TRUE(pipelineRetime(g_p, g_c, STYLE_PERCUSSIVE));

    const PresetParams q = presetParams(STYLE_PERCUSSIVE);
    const float dt = presetHopMs(STYLE_PERCUSSIVE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(q.n, (unsigned)g_p.an.n, "Analysis 没换");
    TEST_ASSERT_EQUAL_INT_MESSAGE(q.wt, g_p.an.wt,          "窗没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(g_c.env.fast_tau_ms, dt),
        g_p.env.a_fast, "Envelope 没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(g_c.env.slow_tau_ms, dt),
        g_p.env.a_slow, "Envelope 慢支路没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(g_c.agc.attack_ms, dt),
        g_p.agc.a_attack, "Agc 没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(g_c.agc.release_ms, dt),
        g_p.agc.a_release, "Agc release 没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(g_c.onset.mean_tau_ms, dt),
        g_p.onset.a_mean, "Onset 没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(g_c.onset.rate_tau_ms, dt),
        g_p.onset.a_rate, "Onset 速率支路没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, dt, g_p.dt_ms, "dt 没记录");
}

// 换档不得清掉 BPM 与相位（§3.3.4 第 2 条）。
void test_retime_preserves_beat_lock(void) {
    pipelineInit(g_p, g_c, STYLE_GENERAL);
    for (uint32_t k = 0; k < 700; ++k) step(k, 128.0f, 0.5f);
    TEST_ASSERT_TRUE_MESSAGE(g_p.beat.locked, "喂了 16 秒还没锁定");
    const float bpm = g_p.beat.bpm;

    pipelineRetime(g_p, g_c, STYLE_EDM);
    TEST_ASSERT_TRUE_MESSAGE(g_p.beat.locked, "换档丢了锁定");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, bpm, g_p.beat.bpm, "换档丢了 BPM");
    TEST_ASSERT_TRUE_MESSAGE(g_p.onset.rate > 0.5f, "换档清了 onset 速率");
}

// ── 接线 ──────────────────────────────────────────────────

// AGC 增益必须真的施加到频段上。评审时这一步是缺的：增益算出来没人用。
void test_agc_gain_is_applied_to_bands(void) {
    pipelineInit(g_p, g_c, STYLE_GENERAL);
    AudioFrame f;
    for (uint32_t k = 0; k < 900; ++k) f = step(k, 120.0f, 0.02f);   // 很轻的信号
    TEST_ASSERT_TRUE_MESSAGE(f.gain > 2.0f, "AGC 没有为轻信号提增益");

    // 关掉增益施加的等价物：手工算一遍未加增益的频段，两者应差 gain 倍
    float raw[NUM_BANDS];
    computeBandEnergy(g_p.an, g_mag, raw);
    int compared = 0;
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (raw[i] < 1e-6f) continue;
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * f.bands[i] + 1e-6f,
            raw[i] * f.gain, f.bands[i], "频段上没有施加 AGC 增益");
        ++compared;
    }
    TEST_ASSERT_TRUE_MESSAGE(compared >= 4, "可比的频段太少，这条断言没意义");
}

// 门限时频段必须归零，否则底噪会被 40 倍增益放大（§4 验收条目）。
void test_gated_silence_zeroes_the_bands(void) {
    pipelineInit(g_p, g_c, STYLE_GENERAL);
    AudioFrame f;
    for (uint32_t k = 0; k < 600; ++k) {
        for (size_t t = 0; t < g_p.an.n; ++t) g_pcm[t] = 0.0f;
        analyze(g_p.an.n, g_p.an.wt);
        f = pipelineProcess(g_p, g_c, g_pcm, g_mag, (uint32_t)(k * g_p.dt_ms));
    }
    TEST_ASSERT_TRUE_MESSAGE(f.gated, "静音没有触发门限");
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, f.bands[i], "门限时频段未归零");
}

// StyleFeatures 的四个判据都必须有非平凡的值。
// 评审发现 centroid 与 flatness 没有生产者时，就是这条测试缺席造成的。
void test_all_four_style_judgements_are_populated(void) {
    pipelineInit(g_p, g_c, STYLE_GENERAL);
    AudioFrame f;
    for (uint32_t k = 0; k < 900; ++k) f = step(k, 150.0f, 0.5f, 0.8f);

    TEST_ASSERT_TRUE_MESSAGE(f.onset_rate > 0.5f, "onset_rate 没有生产者");
    TEST_ASSERT_TRUE_MESSAGE(f.bpm > 30.0f,       "bpm 没有生产者");
    TEST_ASSERT_TRUE_MESSAGE(f.bpm_conf > 0.05f,  "bpm_conf 没有生产者");
    // 后两个不在 AudioFrame 里，直接从频段重算 —— 它们是 pipeline 内部喂给
    // styleUpdate 的，这里验证的是「拿得到非零值」
    TEST_ASSERT_TRUE_MESSAGE(spectralCentroid(f.bands) > 50.0f,  "centroid 恒为 0");
    TEST_ASSERT_TRUE_MESSAGE(spectralFlatness(f.bands) > 0.01f,  "flatness 恒为 0");
}

// 全链跑下来 BPM 要对得上。这是端到端的第一次验证 ——
// 之前 BeatTracker 是拿合成的 16 段能量喂的，没走过真正的 PCM→FFT→频段这条路。
void test_end_to_end_bpm_is_correct(void) {
    const float bpms[3] = {100.0f, 128.0f, 174.0f};
    for (int i = 0; i < 3; ++i) {
        pipelineInit(g_p, g_c, STYLE_GENERAL);
        AudioFrame f;
        for (uint32_t k = 0; k < 800; ++k) f = step(k, bpms[i], 0.5f);
        TEST_ASSERT_TRUE_MESSAGE(f.beat_locked, "端到端未锁定");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(bpms[i] * 0.04f, bpms[i], f.bpm,
            "端到端 BPM 偏差过大");
    }
}

// ── 特效 ──────────────────────────────────────────────────

static Rgb g_px[TOTAL_LEDS];
static FxState g_fxst;
static FxConfig g_fxcfg;
// 跑到状态收敛再看结果。key-wash / chroma-ring 依赖平滑后的色度与底色，
// 只渲染一帧的话它们还在从零往上爬，看着就是「几乎全黑」。
static void render(FxId e,const AudioFrame&f,const Geometry&g,bool wb,Rgb*o){
    g_fxst=FxState{};
    for (int k=0;k<20;++k) fxRender(e,g_fxst,g_fxcfg,f,g,wb,23.22f,o); }

// 一帧「什么都有」的信号：能量、节拍、音高全带上。
// 新增字段时补在这里，免得老测试因为字段是零而误判成效果坏了。
static AudioFrame liveFrame(void){
    AudioFrame f;
    for (int i=0;i<NUM_BANDS;++i) f.bands[i]=0.5f;
    for (int i=0;i<kChroma;++i)  f.chroma[i]=(i%4==0)?0.9f:0.15f;
    f.rms_fast=0.4f; f.peak=0.6f; f.beat_locked=true; f.bpm=128.0f; f.phase=0.1f;
    f.key_root=0; f.key_conf=0.8f; f.centroid_hz=900.0f; f.harmony_move=0.1f;
    f.f0_hz=330.0f; f.f0_conf=0.8f; f.f0_voiced=true;
    f.mood=0.6f; f.energy_trend=0.3f; f.section_novelty=0.1f;
    return f;
}

void test_every_effect_writes_every_pixel(void) {
    Geometry g;
    const AudioFrame f = liveFrame();

    for (int e = 0; e < FX_COUNT; ++e) {
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i) g_px[i] = Rgb{9, 9, 9};
        render((FxId)e, f, g, false, g_px);
        int lit = 0;
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
            TEST_ASSERT_FALSE_MESSAGE(g_px[i].r == 9 && g_px[i].g == 9 && g_px[i].b == 9,
                "有像素没被写过");
            if (g_px[i].r || g_px[i].g || g_px[i].b) ++lit;
        }
        TEST_ASSERT_TRUE_MESSAGE(lit > 10, "效果几乎全黑");
    }
}

void test_effects_go_dark_on_silence(void) {
    Geometry g;
    AudioFrame f;                       // 全零：静音
    for (int e = 0; e < FX_COUNT; ++e) {
        render((FxId)e, f, g, true, g_px);
        int lit = 0;
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
            if (g_px[i].r > 8 || g_px[i].g > 8 || g_px[i].b > 8) ++lit;
        // beat-pulse 未锁定时刻意保留呼吸底光，其余两个应当全黑
        if ((FxId)e == FX_BEAT_PULSE) continue;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, lit, "静音时效果没有熄灭");
    }
}

// 白平衡在 fxRender 里统一施加，各效果自己不碰 —— 否则总有一个会忘。
void test_white_balance_applies_to_all_effects(void) {
    Geometry g;
    const AudioFrame f = liveFrame();

    for (int e = 0; e < FX_COUNT; ++e) {
        static Rgb raw[TOTAL_LEDS];
        render((FxId)e, f, g, false, raw);
        render((FxId)e, f, g, true,  g_px);
        int differs = 0;
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
            if (raw[i].g != g_px[i].g || raw[i].b != g_px[i].b) ++differs;
        TEST_ASSERT_TRUE_MESSAGE(differs > 5, "白平衡没有施加到这个效果上");
    }
}

// 效果不得写出 96 个像素之外 —— 几何映射错了会越界。
void test_effects_stay_within_the_pixel_range(void) {
    Geometry g;
    for (int cfg = 0; cfg < 8; ++cfg) {
        g.s1_is_left  = (cfg & 1) != 0;
        g.s1_reversed = (cfg & 2) != 0;
        g.s2_reversed = (cfg & 4) != 0;
        const AudioFrame f = liveFrame();
        for (int e = 0; e < FX_COUNT; ++e) {
            for (uint16_t i = 0; i < TOTAL_LEDS; ++i) g_px[i] = Rgb{0, 0, 0};
            render((FxId)e, f, g, false, g_px);   // 越界会被 sanitizer 或崩溃抓到
        }
    }
    TEST_ASSERT_TRUE(true);
}

// ── 冲击效果：这一组是「跟得上 BPM」的全部要点 ────────────

// 峰值必须**瞬时**跟上，不做任何平滑 —— 冲击峰的上升沿就是声音的上升沿。
void test_impact_rises_instantly(void) {
    FxState st; FxConfig c;
    AudioFrame f;
    f.beat_locked = true; f.bpm = 120.0f;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.0f;
    f.rms_fast = 0.0f;
    fxAdvance(st, c, f, 23.22f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, st.impact);

    f.rms_fast = 0.35f;                       // ×kFxLevelScale(3) ≈ 1.0
    fxAdvance(st, c, f, 23.22f);
    TEST_ASSERT_TRUE_MESSAGE(st.impact > 0.85f, "冲高没有立刻到位 —— 上升沿被平滑了");
}

// 衰减长度必须**按拍周期缩放**。这是整个需求的核心：
// 固定毫秒的衰减在 174BPM（一拍 345ms）会糊成一片，在 90BPM（667ms）又早早熄灭。
void test_impact_decay_scales_with_bpm(void) {
    FxConfig c;
    const float bpms[2] = {90.0f, 180.0f};
    float halfLife[2];
    for (int b = 0; b < 2; ++b) {
        FxState st; AudioFrame f;
        f.beat_locked = true; f.bpm = bpms[b];
        for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.0f;
        f.rms_fast = 1.0f;
        fxAdvance(st, c, f, 5.0f);            // 冲到顶
        TEST_ASSERT_TRUE(st.impact > 0.9f);
        f.rms_fast = 0.0f;
        int steps = 0;
        while (st.impact > 0.5f && steps < 4000) { fxAdvance(st, c, f, 5.0f); ++steps; }
        halfLife[b] = steps * 5.0f;
    }
    // BPM 翻倍 → 拍周期减半 → 衰减也该减半
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.15f * halfLife[1], halfLife[0] / 2.0f, halfLife[1],
        "衰减长度没有随 BPM 缩放 —— 快歌会糊成一片");
    // 而且必须在一拍之内落下去，否则连续两拍会叠在一起
    for (int b = 0; b < 2; ++b)
        TEST_ASSERT_TRUE_MESSAGE(halfLife[b] < 60000.0f / bpms[b],
            "半衰期超过一个拍周期 —— 相邻两拍会糊在一起");
}

// 没锁上节拍时退回固定衰减，而不是除以零或者不衰减。
void test_impact_falls_back_when_unlocked(void) {
    FxState st; FxConfig c;
    AudioFrame f;
    f.beat_locked = false; f.bpm = 0.0f;      // 未锁定，BPM 无意义
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.0f;
    f.rms_fast = 1.0f;
    fxAdvance(st, c, f, 10.0f);
    TEST_ASSERT_TRUE(st.impact > 0.9f);
    f.rms_fast = 0.0f;
    for (int k = 0; k < 40; ++k) fxAdvance(st, c, f, 10.0f);   // 400ms
    TEST_ASSERT_TRUE_MESSAGE(st.impact < 0.3f, "未锁定时没有衰减");
    TEST_ASSERT_FALSE(isnan(st.impact));
}

// 连续拍点之间要落得下去，否则柱子会一直顶在天花板上。
void test_impact_returns_between_beats(void) {
    FxState st; FxConfig c;
    AudioFrame f;
    f.beat_locked = true; f.bpm = 174.0f;     // 一拍 345ms
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.0f;
    float lowest = 1.0f;
    for (int beat = 0; beat < 8; ++beat)
        for (int k = 0; k < 30; ++k) {        // 30 × 11.5ms ≈ 一拍
            f.rms_fast = (k == 0) ? 1.0f : 0.0f;
            fxAdvance(st, c, f, 11.5f);
            if (beat > 2 && k > 20 && st.impact < lowest) lowest = st.impact;
        }
    TEST_ASSERT_TRUE_MESSAGE(lowest < 0.25f, "拍与拍之间没落下来 —— 亮度一直顶着");
}

// 状态不得被非有限输入污染。
void test_impact_rejects_non_finite(void) {
    FxState st; FxConfig c;
    AudioFrame f;
    f.beat_locked = true; f.bpm = 120.0f;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.3f;
    f.rms_fast = 0.3f;
    fxAdvance(st, c, f, 23.0f);
    f.bands[4] = NAN; f.bands[7] = INFINITY; f.rms_fast = NAN;
    fxAdvance(st, c, f, 23.0f);
    fxAdvance(st, c, f, NAN);                 // dt 也可能坏
    TEST_ASSERT_FALSE_MESSAGE(isnan(st.impact), "NaN 渗进了整管冲击包络");
    TEST_ASSERT_TRUE(st.impact >= 0.0f && st.impact <= 1.0f);
    TEST_ASSERT_FALSE(isnan(st.hue));
    for (int i = 0; i < NUM_BANDS; ++i) {
        TEST_ASSERT_FALSE_MESSAGE(isnan(st.bar[i]), "NaN 渗进了分段包络");
        TEST_ASSERT_TRUE(st.bar[i] >= 0.0f && st.bar[i] <= 1.0f);
    }
}

// 六个效果都要能画满、静音都要熄灭（beat-pulse 的呼吸底光除外）。
void test_all_effects_render(void) {
    Geometry g; FxState st; FxConfig c;
    const AudioFrame f = liveFrame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(10, (int)FX_COUNT, "效果数量变了，测试没跟上");
    for (int e = 0; e < FX_COUNT; ++e) {
        st = FxState{};
        for (int w = 0; w < 12; ++w) fxRender((FxId)e, st, c, f, g, true, 23.22f, g_px);
        int lit = 0;
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
            if (g_px[i].r || g_px[i].g || g_px[i].b) ++lit;
        TEST_ASSERT_TRUE_MESSAGE(lit > 6, "效果几乎全黑");
    }
    AudioFrame q;                              // 全零 = 静音
    for (int e = 0; e < FX_COUNT; ++e) {
        if ((FxId)e == FX_BEAT_PULSE) continue;
        st = FxState{};
        for (int w = 0; w < 12; ++w) fxRender((FxId)e, st, c, q, g, true, 23.22f, g_px);
        int lit = 0;
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
            if (g_px[i].r > 8 || g_px[i].g > 8 || g_px[i].b > 8) ++lit;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, lit, "静音时效果没有熄灭");
    }
}

// 冲击柱**不能**是频段柱加个尾巴。
//
// 第一版就是那样：同样的 u→band 布局、同样的按段号取色，只是数据源换成了
// 带衰减的包络。两个效果放在一起没有分工可言。
// 这条从两个方向钉死区别：频谱形状变了它不该跟着变；整体响度变了它必须变。
void test_impact_is_not_a_spectrum_plot(void) {
    Geometry g; FxConfig c;
    static Rgb a[TOTAL_LEDS], b[TOTAL_LEDS];

    // 两帧总能量相同，但频谱形状完全相反
    AudioFrame lo, hi;
    lo.rms_fast = hi.rms_fast = 0.25f;
    lo.peak = hi.peak = 0.3f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        lo.bands[i] = (i < 8) ? 0.5f : 0.0f;
        hi.bands[i] = (i < 8) ? 0.0f : 0.5f;
    }
    FxState sa, sb;
    for (int k = 0; k < 30; ++k) {            // 让色相收敛
        fxRender(FX_BAR_IMPACT, sa, c, lo, g, false, 23.22f, a);
        fxRender(FX_BAR_IMPACT, sb, c, hi, g, false, 23.22f, b);
    }
    int litA = 0, litB = 0;
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
        if (a[i].r || a[i].g || a[i].b) ++litA;
        if (b[i].r || b[i].g || b[i].b) ++litB;
    }
    // 点亮范围取决于**冲击强度**，两帧强度一样就该一样宽 —— 频谱柱做不到这点
    TEST_ASSERT_INT_WITHIN_MESSAGE(4, litA, litB,
        "点亮范围随频谱形状变了 —— 又画成频谱图了");

    // 反过来：整体响度变了，点亮范围必须跟着变
    AudioFrame weak = lo; weak.rms_fast = 0.05f; weak.peak = 0.06f;
    FxState sw;
    static Rgb w[TOTAL_LEDS];
    for (int k = 0; k < 30; ++k) fxRender(FX_BAR_IMPACT, sw, c, weak, g, false, 23.22f, w);
    int litW = 0;
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) if (w[i].r || w[i].g || w[i].b) ++litW;
    TEST_ASSERT_TRUE_MESSAGE(litW < litA - 8,
        "弱击打和强击打点亮范围一样 —— 冲击强度没有体现出来");
}

// 但它仍然该反映音色：色相跟频谱质心走，低沉偏暖、明亮偏冷。
void test_impact_hue_follows_centroid(void) {
    FxConfig c;
    AudioFrame lo, hi;
    lo.rms_fast = hi.rms_fast = 0.3f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        lo.bands[i] = (i < 4)  ? 0.6f : 0.0f;      // 低频
        hi.bands[i] = (i > 11) ? 0.6f : 0.0f;      // 高频
    }
    FxState sa, sb;
    for (int k = 0; k < 80; ++k) { fxAdvance(sa, c, lo, 23.22f); fxAdvance(sb, c, hi, 23.22f); }
    TEST_ASSERT_TRUE_MESSAGE(sb.hue > sa.hue + 0.1f,
        "高频内容没有让色相变冷 —— 质心没接上");
}

// ── 吃音高的两个效果 ──────────────────────────────────────

static void avgHue(const Rgb *px, float &r, float &g2, float &b) {
    double R=0,G=0,B=0; int n=0;
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        if (px[i].r || px[i].g || px[i].b) { R+=px[i].r; G+=px[i].g; B+=px[i].b; ++n; }
    if (!n) { r=g2=b=0; return; }
    r=(float)(R/n); g2=(float)(G/n); b=(float)(B/n);
}

// key-wash 的底色必须由**调**决定 —— 换个调，颜色就得变。
// 这条钉的是「它真的读了 key_root」，而不是又一个音量表。
void test_key_wash_color_follows_the_key(void) {
    Geometry g; FxConfig c;
    static Rgb pa[TOTAL_LEDS], pb[TOTAL_LEDS];
    AudioFrame a = liveFrame(), b = liveFrame();
    a.key_root = 0;  a.key_is_major = true;      // C 大调
    b.key_root = 6;  b.key_is_major = true;      // 升F 大调，色相环的对面
    FxState sa, sb;
    for (int k = 0; k < 120; ++k) {              // key_hue 是慢挪的，要给够时间
        fxRender(FX_KEY_WASH, sa, c, a, g, false, 23.22f, pa);
        fxRender(FX_KEY_WASH, sb, c, b, g, false, 23.22f, pb);
    }
    float r1,g1,b1,r2,g2,b2;
    avgHue(pa,r1,g1,b1); avgHue(pb,r2,g2,b2);
    const float diff = fabsf(r1-r2) + fabsf(g1-g2) + fabsf(b1-b2);
    TEST_ASSERT_TRUE_MESSAGE(diff > 60.0f, "换了调颜色却没变 —— 没有真的读 key_root");
}

// 大调偏暖、小调偏冷。同一个主音，只换大小调，颜色要分得开。
void test_key_wash_separates_major_from_minor(void) {
    Geometry g; FxConfig c;
    static Rgb pa[TOTAL_LEDS], pb[TOTAL_LEDS];
    AudioFrame a = liveFrame(), b = liveFrame();
    a.key_root = 3; a.key_is_major = true;
    b.key_root = 3; b.key_is_major = false;
    FxState sa, sb;
    for (int k = 0; k < 120; ++k) {
        fxRender(FX_KEY_WASH, sa, c, a, g, false, 23.22f, pa);
        fxRender(FX_KEY_WASH, sb, c, b, g, false, 23.22f, pb);
    }
    float r1,g1,b1,r2,g2,b2;
    avgHue(pa,r1,g1,b1); avgHue(pb,r2,g2,b2);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(r1-r2)+fabsf(b1-b2) > 25.0f,
        "大调与小调的颜色分不开");
}

// 调性不明时降饱和度，别硬凑一个颜色出来。
void test_key_wash_desaturates_when_key_is_unclear(void) {
    Geometry g; FxConfig c;
    static Rgb ps[TOTAL_LEDS], pu[TOTAL_LEDS];
    AudioFrame sure = liveFrame(), vague = liveFrame();
    sure.key_conf = 0.95f; vague.key_conf = 0.0f; vague.key_root = -1;
    FxState s1, s2;
    for (int k = 0; k < 60; ++k) {
        fxRender(FX_KEY_WASH, s1, c, sure,  g, false, 23.22f, ps);
        fxRender(FX_KEY_WASH, s2, c, vague, g, false, 23.22f, pu);
    }
    // 饱和度低 = RGB 三分量更接近
    auto spread=[](const Rgb *p){ int mx=0,mn=255,n=0; float acc=0;
        for (uint16_t i=0;i<TOTAL_LEDS;++i){ if(!(p[i].r||p[i].g||p[i].b)) continue;
            mx=p[i].r; mn=p[i].r;
            if(p[i].g>mx)mx=p[i].g; if(p[i].g<mn)mn=p[i].g;
            if(p[i].b>mx)mx=p[i].b; if(p[i].b<mn)mn=p[i].b;
            acc+=(mx-mn); ++n; }
        return n? acc/n : 0.0f; };
    TEST_ASSERT_TRUE_MESSAGE(spread(ps) > spread(pu) + 10.0f,
        "调性不明时没有降饱和 —— 会硬凑出一个不存在的调");
}

// chroma-ring 的横轴是**音级**不是频率：同一个音在任何八度都点亮同一格。
// 这是它与频段柱的根本区别 —— 频段柱上八度关系是两个相隔很远的格子。
// 同一条规矩在管线侧的另一半：和声变化率的平滑。
//
// 挑 EDM {1024,HANN,256} 与 通用 {1024,HANN,512} 来比，是因为这两档的
// **N 和窗完全相同** —— 同一段 PCM 折出的色度一模一样，唯一的差别就是 hop
// （11.61ms vs 23.22ms，正好 2 倍）。换别的档位组合，N 一变色度就变，
// 测出来的差异分不清是「平滑漂了」还是「频率分辨率不同」。
void test_harmony_smoothing_is_defined_in_physical_time(void) {
    const StylePreset ps[2]  = {STYLE_EDM, STYLE_GENERAL};
    // 首帧没有 prev_chroma、不更新，所以真正参与平滑的是 steps-1 次。
    // 8 : 4 才是等时长，9 : 5 帧。
    const int         stp[2] = {9, 5};
    float got[2] = {0, 0};

    for (int r = 0; r < 2; ++r) {
        g_c = PipelineConfig{};
        g_c.style.manual_lock = ps[r];
        TEST_ASSERT_TRUE(pipelineInit(g_p, g_c, ps[r]));
        for (int k = 0; k < stp[r]; ++k) {
            const float hz = (k % 2) ? 523.25f : 440.0f;   // C5 与 A4 轮换
            for (size_t t = 0; t < g_p.an.n; ++t)
                g_pcm[t] = 0.5f * sinf(6.283185307f * hz * (float)t / kSampleRate);
            analyze(g_p.an.n, g_p.an.wt);
            pipelineProcess(g_p, g_c, g_pcm, g_mag, (uint32_t)(k * g_p.dt_ms));
        }
        got[r] = g_p.harmony;
    }

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f,
        (stp[0]-1) * presetHopMs(ps[0]), (stp[1]-1) * presetHopMs(ps[1]),
        "两条路径参与平滑的物理时长必须相等，否则这条测试没意义");

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, got[0], got[1],
        "同样的物理时长，快档与慢档的和声平滑结果应当一致");

    // 正对照。这条同时保证了上面的容差足够严：写死每帧 0.25 的话两者差 0.216·D，
    // 而 got[0] > 0.15 反推 D > 0.22，于是那个差距 > 0.047，必然超出 0.02。
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 0.15f, "harmony 必须真的爬起来了");
}

// ── 彩色流动 / 旋律线：证明它们真的在消费 mood 与 f0 ──

// 跑 n 帧后取平均色相跨度：管两端的色相差多少
static float hueSpread(FxState &st, const AudioFrame &f) {
    Geometry g; Rgb px[TOTAL_LEDS];
    for (int k = 0; k < 40; ++k) fxRender(FX_COLOR_FLOW, st, g_fxcfg, f, g, false, 23.22f, px);
    const Rgb a = px[mapPixel(g, SIDE_L, 0.0f)];
    const Rgb b = px[mapPixel(g, SIDE_L, 1.0f)];
    // 用 RGB 差当色相差的代理 —— 同色相时三通道比例相同
    const float da = fabsf((float)a.r - b.r) + fabsf((float)a.g - b.g) + fabsf((float)a.b - b.b);
    return da;
}

void test_color_flow_hue_span_follows_the_mood(void) {
    // 静：几乎单色的渐变；躁：整条彩虹。两端色差必须拉开。
    AudioFrame calm = liveFrame(), wild = liveFrame();
    calm.mood = 0.0f; wild.mood = 1.0f;
    FxState a{}, b{};
    const float s_calm = hueSpread(a, calm), s_wild = hueSpread(b, wild);
    TEST_ASSERT_TRUE_MESSAGE(s_wild > s_calm + 60.0f,
        "躁的时候色相跨度必须明显更宽");
}

void test_color_flow_direction_follows_the_energy_trend(void) {
    // 渐强往上流、收尾往下流。相位在两个方向上必须朝相反方向走。
    AudioFrame up = liveFrame(), down = liveFrame();
    up.energy_trend = 1.0f; down.energy_trend = -1.0f;
    Geometry g; Rgb px[TOTAL_LEDS];
    FxState a{}, b{};
    for (int k = 0; k < 30; ++k) {
        fxRender(FX_COLOR_FLOW, a, g_fxcfg, up,   g, false, 23.22f, px);
        fxRender(FX_COLOR_FLOW, b, g_fxcfg, down, g, false, 23.22f, px);
    }
    TEST_ASSERT_TRUE_MESSAGE(a.flow > 0.0f && a.flow < 1.0f, "相位应当在 [0,1)");
    // 渐强时相位前进、收尾时后退（后退会绕到接近 1）
    TEST_ASSERT_TRUE_MESSAGE(a.flow != b.flow, "两个方向的相位不能相同");
    float fwd = a.flow, bwd = b.flow; if (bwd > 0.5f) bwd -= 1.0f;
    TEST_ASSERT_TRUE_MESSAGE(fwd > 0.0f && bwd < 0.0f,
        "渐强应当正向流动、收尾应当反向流动");
}

void test_color_flow_speed_follows_the_bpm(void) {
    AudioFrame slow = liveFrame(), fast = liveFrame();
    slow.bpm = 80.0f; fast.bpm = 160.0f;
    Geometry g; Rgb px[TOTAL_LEDS];
    FxState a{}, b{};
    for (int k = 0; k < 10; ++k) {
        fxRender(FX_COLOR_FLOW, a, g_fxcfg, slow, g, false, 23.22f, px);
        fxRender(FX_COLOR_FLOW, b, g_fxcfg, fast, g, false, 23.22f, px);
    }
    TEST_ASSERT_TRUE_MESSAGE(b.flow > a.flow * 1.8f,
        "BPM 翻倍，流动速度应当接近翻倍");
}

void test_color_flow_washes_out_on_a_section_change(void) {
    AudioFrame f = liveFrame();
    Geometry g; Rgb px[TOTAL_LEDS];
    FxState st{};
    for (int k = 0; k < 20; ++k) fxRender(FX_COLOR_FLOW, st, g_fxcfg, f, g, false, 23.22f, px);
    const float before = st.sect;
    f.section_change = true;
    fxRender(FX_COLOR_FLOW, st, g_fxcfg, f, g, false, 23.22f, px);
    TEST_ASSERT_TRUE_MESSAGE(st.sect > before + 0.5f, "换段应当触发一次泛白");
    f.section_change = false;
    for (int k = 0; k < 200; ++k) fxRender(FX_COLOR_FLOW, st, g_fxcfg, f, g, false, 23.22f, px);
    TEST_ASSERT_TRUE_MESSAGE(st.sect < 0.05f, "泛白应当自己退下去");
}

// 旋律线光斑的中心位置（左管，0=底 1=顶）
static float melodyCentroid(FxState &st, const AudioFrame &f) {
    Geometry g; Rgb px[TOTAL_LEDS];
    for (int k = 0; k < 30; ++k) fxRender(FX_MELODY_LINE, st, g_fxcfg, f, g, false, 23.22f, px);
    float wsum = 0.0f, w = 0.0f;
    for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
        const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
        const Rgb c = px[mapPixel(g, SIDE_L, u)];
        const float e = (float)(c.r + c.g + c.b);
        wsum += e * u; w += e;
    }
    return (w > 0.0f) ? wsum / w : -1.0f;
}

void test_melody_line_height_follows_the_pitch(void) {
    AudioFrame lo = liveFrame(), hi = liveFrame();
    lo.f0_hz = 110.0f; hi.f0_hz = 880.0f;
    FxState a{}, b{};
    const float ul = melodyCentroid(a, lo), uh = melodyCentroid(b, hi);
    TEST_ASSERT_TRUE_MESSAGE(ul >= 0.0f && uh >= 0.0f, "两种音高都应当点亮些什么");
    TEST_ASSERT_TRUE_MESSAGE(uh > ul + 0.3f, "音越高，光点越靠管顶");
}

void test_melody_line_maps_octaves_apart_unlike_chroma_ring(void) {
    // 与音级环的分水岭：C3 和 C5 在音级环里是同一格，在这里相距半根管。
    AudioFrame c3 = liveFrame(), c5 = liveFrame();
    c3.f0_hz = 130.8f; c5.f0_hz = 523.3f;
    FxState a{}, b{};
    const float u3 = melodyCentroid(a, c3), u5 = melodyCentroid(b, c5);
    TEST_ASSERT_TRUE_MESSAGE(u5 - u3 > 0.35f,
        "相差两个八度的同一个音必须落在管上不同的位置");

    // 正对照：同样这两个音，在音级环里应当落在同一格
    FxState r3{}, r5{};
    AudioFrame k3 = liveFrame(), k5 = liveFrame();
    for (int i = 0; i < kChroma; ++i) { k3.chroma[i] = k5.chroma[i] = 0.05f; }
    k3.chroma[0] = k5.chroma[0] = 1.0f;     // 都是 C
    Geometry g; Rgb p3[TOTAL_LEDS], p5[TOTAL_LEDS];
    for (int k = 0; k < 30; ++k) {
        fxRender(FX_CHROMA_RING, r3, g_fxcfg, k3, g, false, 23.22f, p3);
        fxRender(FX_CHROMA_RING, r5, g_fxcfg, k5, g, false, 23.22f, p5);
    }
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(p3[i].r, p5[i].r,
            "音级环把八度折叠掉了，两者应当完全一样");
}

void test_melody_line_leaves_a_trail_that_fades(void) {
    AudioFrame f = liveFrame();
    f.f0_hz = 440.0f;
    FxState st{};
    Geometry g; Rgb px[TOTAL_LEDS];
    for (int k = 0; k < 30; ++k) fxRender(FX_MELODY_LINE, st, g_fxcfg, f, g, false, 23.22f, px);
    const float peak = st.trail[(int)(st.f0_u * (LEDS_PER_TUBE - 1) + 0.5f)];
    TEST_ASSERT_TRUE_MESSAGE(peak > 0.05f, "持续音应当点亮它所在的位置");

    f.f0_voiced = false;
    for (int k = 0; k < 60; ++k) fxRender(FX_MELODY_LINE, st, g_fxcfg, f, g, false, 23.22f, px);
    const float after = st.trail[(int)(st.f0_u * (LEDS_PER_TUBE - 1) + 0.5f)];
    TEST_ASSERT_TRUE_MESSAGE(after < peak * 0.2f, "音停了拖影应当淡下去");
    TEST_ASSERT_TRUE_MESSAGE(after > 0.0f, "但不是一刀切黑");
}

void test_melody_line_is_dark_when_unvoiced_from_the_start(void) {
    AudioFrame f = liveFrame();
    f.f0_voiced = false; f.f0_hz = 0.0f;
    FxState st{};
    Geometry g; Rgb px[TOTAL_LEDS];
    for (int k = 0; k < 30; ++k) fxRender(FX_MELODY_LINE, st, g_fxcfg, f, g, false, 23.22f, px);
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        TEST_ASSERT_TRUE_MESSAGE(px[i].r == 0 && px[i].g == 0 && px[i].b == 0,
            "从没测到过音高时不该有拖影");
}

// 平滑必须按**物理时间**定义。各档 hop 从 5.8ms（打点）到 46.4ms（氛围）
// 差 8 倍，如果系数写死成「每帧 0.25」，同一段音乐在两个档位的反应速度就差 8 倍：
// 慢档还在爬升，快档早已收敛。
//
// 这条测试正是冲着那个写法去的 —— 用固定系数它必红。
void test_fx_smoothing_is_defined_in_physical_time(void) {
    const AudioFrame f = liveFrame();
    // 打点档 hop=128 → 5.805ms，氛围档 hop=1024 → 46.44ms，正好 8 倍。
    // 步数取 24 : 3，两条路径的总时长严格相等（139.32ms）。
    //
    // 139ms ≈ 1.7 个时间常数 —— 停在爬升的中段。如果跑到收敛，两种写法
    // 都会到达同一个终点，这条测试就什么也证明不了。
    struct { float dt; int steps; } run[2] = {{5.805f, 24}, {46.44f, 3}};
    const float kTotalMs = run[0].dt * run[0].steps;
    float got[2][kChroma];
    for (int r=0;r<2;++r) {
        FxState st{}; Geometry g; Rgb px[TOTAL_LEDS];
        for (int k=0;k<run[r].steps;++k)
            fxRender(FX_CHROMA_RING, st, g_fxcfg, f, g, false, run[r].dt, px);
        for (int i=0;i<kChroma;++i) got[r][i]=st.chroma[i];
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, kTotalMs, run[1].dt*run[1].steps,
        "两条路径的总时长必须严格相等，否则这条测试没意义");

    for (int i=0;i<kChroma;++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, got[0][i], got[1][i],
            "同样的物理时长，快档与慢档的平滑结果应当一致");

    // 正对照：必须停在「已经明显动了、但远未收敛」的区间。
    // 写死每帧 0.25 的话，这里快档是 0.90、慢档是 0.52 —— 差 0.38，远超上面的容差。
    TEST_ASSERT_TRUE_MESSAGE(got[0][0] > 0.50f, "色度应已明显上升");
    TEST_ASSERT_TRUE_MESSAGE(got[0][0] < 0.85f, "不能跑到收敛，否则两种写法殊途同归");
}

void test_chroma_ring_maps_pitch_class_not_frequency(void) {
    Geometry g; FxConfig c;
    static Rgb px[TOTAL_LEDS];
    AudioFrame f = liveFrame();
    for (int i = 0; i < kChroma; ++i) f.chroma[i] = 0.0f;
    f.chroma[7] = 1.0f;                          // 只有 G 在响
    f.key_conf = 0.0f; f.key_root = -1;          // 去掉主音白边的干扰
    FxState st;
    for (int k = 0; k < 40; ++k) fxRender(FX_CHROMA_RING, st, c, f, g, false, 23.22f, px);

    // 亮起的应当集中在管长 7/11 处附近
    int first = -1, last = -1;
    for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i)
        if (px[i].r || px[i].g || px[i].b) { if (first < 0) first = i; last = i; }
    TEST_ASSERT_TRUE_MESSAGE(first >= 0, "只有一个音级在响却全黑");
    const float mid = 0.5f * (first + last) / (float)(LEDS_PER_TUBE - 1);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.08f, 7.0f / (kChroma - 1), mid,
        "G 没落在音级环的对应位置");
}

// 音级环随旋律移动：换个音，亮的格子跟着走。
void test_chroma_ring_moves_with_the_note(void) {
    Geometry g; FxConfig c;
    static Rgb px[TOTAL_LEDS];
    int centers[kChroma];
    for (int pc = 0; pc < kChroma; ++pc) {
        AudioFrame f = liveFrame();
        for (int i = 0; i < kChroma; ++i) f.chroma[i] = 0.0f;
        f.chroma[pc] = 1.0f;
        f.key_conf = 0.0f; f.key_root = -1;
        FxState st;
        for (int k = 0; k < 40; ++k) fxRender(FX_CHROMA_RING, st, c, f, g, false, 23.22f, px);
        int first = -1, last = -1;
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i)
            if (px[i].r || px[i].g || px[i].b) { if (first < 0) first = i; last = i; }
        centers[pc] = (first < 0) ? -1 : (first + last) / 2;
    }
    for (int pc = 1; pc < kChroma; ++pc) {
        TEST_ASSERT_TRUE_MESSAGE(centers[pc] >= 0, "某个音级点不亮");
        TEST_ASSERT_TRUE_MESSAGE(centers[pc] > centers[pc - 1],
            "音级升高时亮点没有单调右移");
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_init_sets_every_module_to_the_same_hop);
    RUN_TEST(test_retime_updates_every_module_at_once);
    RUN_TEST(test_retime_preserves_beat_lock);
    RUN_TEST(test_agc_gain_is_applied_to_bands);
    RUN_TEST(test_gated_silence_zeroes_the_bands);
    RUN_TEST(test_all_four_style_judgements_are_populated);
    RUN_TEST(test_end_to_end_bpm_is_correct);
    RUN_TEST(test_every_effect_writes_every_pixel);
    RUN_TEST(test_effects_go_dark_on_silence);
    RUN_TEST(test_white_balance_applies_to_all_effects);
    RUN_TEST(test_effects_stay_within_the_pixel_range);
    RUN_TEST(test_impact_rises_instantly);
    RUN_TEST(test_impact_decay_scales_with_bpm);
    RUN_TEST(test_impact_falls_back_when_unlocked);
    RUN_TEST(test_impact_returns_between_beats);
    RUN_TEST(test_impact_rejects_non_finite);
    RUN_TEST(test_impact_is_not_a_spectrum_plot);
    RUN_TEST(test_impact_hue_follows_centroid);
    RUN_TEST(test_all_effects_render);
    RUN_TEST(test_key_wash_color_follows_the_key);
    RUN_TEST(test_key_wash_separates_major_from_minor);
    RUN_TEST(test_key_wash_desaturates_when_key_is_unclear);
    RUN_TEST(test_color_flow_hue_span_follows_the_mood);
    RUN_TEST(test_color_flow_direction_follows_the_energy_trend);
    RUN_TEST(test_color_flow_speed_follows_the_bpm);
    RUN_TEST(test_color_flow_washes_out_on_a_section_change);
    RUN_TEST(test_melody_line_height_follows_the_pitch);
    RUN_TEST(test_melody_line_maps_octaves_apart_unlike_chroma_ring);
    RUN_TEST(test_melody_line_leaves_a_trail_that_fades);
    RUN_TEST(test_melody_line_is_dark_when_unvoiced_from_the_start);
    RUN_TEST(test_harmony_smoothing_is_defined_in_physical_time);
    RUN_TEST(test_fx_smoothing_is_defined_in_physical_time);
    RUN_TEST(test_chroma_ring_maps_pitch_class_not_frequency);
    RUN_TEST(test_chroma_ring_moves_with_the_note);
    return UNITY_END();
}
