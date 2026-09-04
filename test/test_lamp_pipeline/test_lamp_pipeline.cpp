#include <unity.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
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
// **头一帧带事件标志，后面不带。** 强拍/换段/唱句起音这类是单帧事件，
// 把它们钉成常真会破坏「换段应当触发一次泛白」这类测试（我这么干过一次，
// 两条彩色流动的测试当场红了）。事件驱动的效果要靠它们的包络活着，
// 所以头一帧给一次事件，之后看包络还在不在。
static void render(FxId e,const AudioFrame&f,const Geometry&g,bool wb,Rgb*o){
    g_fxst=FxState{};
    AudioFrame ev=f; ev.downbeat=true; ev.onset=true; ev.vocal_onset=true; ev.section_change=true;
    fxRender(e,g_fxst,g_fxcfg,ev,g,wb,23.22f,o);
    for (int k=0;k<19;++k) fxRender(e,g_fxst,g_fxcfg,f,g,wb,23.22f,o); }

// 一帧「什么都有」的信号：能量、节拍、音高全带上。
// 新增字段时补在这里，免得老测试因为字段是零而误判成效果坏了。
static AudioFrame liveFrame(void){
    AudioFrame f;
    for (int i=0;i<NUM_BANDS;++i) f.bands[i]=0.5f;
    for (int i=0;i<kChroma;++i)  f.chroma[i]=(i%4==0)?0.9f:0.15f;
    f.rms_fast=0.4f; f.rms_slow=0.25f; f.peak=0.6f; f.beat_locked=true; f.bpm=128.0f; f.phase=0.1f;
    f.key_root=0; f.key_conf=0.8f; f.centroid_hz=900.0f; f.harmony_move=0.1f;
    f.f0_hz=330.0f; f.f0_conf=0.8f; f.f0_voiced=true;
    f.mood=0.6f; f.energy_trend=0.3f; f.section_novelty=0.1f;
    // 后加的特征。**夹具的含义是「音乐在放、各特征都在」** ——
    // 漏掉哪一项，吃那一项的效果就会在「每个效果都要画得出东西」里报全黑，
    // 而那是夹具旧了，不是效果坏了。加新特征时这里要一起补。
    f.dynamics=0.8f; f.beats_per_bar=4; f.bar_pos=0; f.bar_index=3;
    f.bar_conf=0.7f;
    f.onset=true;    // 冲击效果改 onset 驱动（2026-09-02）后夹具必须带击打事件
    for (int i=0;i<NUM_BANDS;++i){ f.bands_h[i]=0.45f; f.bands_p[i]=0.25f; }
    f.percussive=0.35f;
    f.vocal=0.8f;
    // **事件标志不放在稳态夹具里**（downbeat / onset / vocal_onset /
    // section_change）—— 它们只该在某一帧为真。见 render() 上面的说明。
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

// 峰值必须**瞬时**跟上，不做任何平滑 —— 冲击峰的上升沿就是击打事件本身。
// （2026-09-02 起冲击由 onset 事件注入：电平包络两条路都被现实击毙 ——
//  绝对 rms 有 AGC 地板、瞬态分量被砖墙压缩抹平，见 lamp_fx.h 注释。）
void test_impact_rises_instantly(void) {
    FxState st; FxConfig c;
    AudioFrame f;
    f.beat_locked = true; f.bpm = 120.0f;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.0f;
    f.onset = false;
    fxAdvance(st, c, f, 23.22f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, st.impact);

    f.onset = true;                           // 击打事件 + 打击通道能量
    for (int i = 0; i < NUM_BANDS; ++i) f.bands_p[i] = 0.02f;
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
        f.onset = true;
        for (int i = 0; i < NUM_BANDS; ++i) f.bands_p[i] = 0.02f;
        fxAdvance(st, c, f, 5.0f);            // 击打事件冲到顶
        TEST_ASSERT_TRUE(st.impact > 0.9f);
        f.onset = false;
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
    f.onset = true;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands_p[i] = 0.02f;
    fxAdvance(st, c, f, 10.0f);
    TEST_ASSERT_TRUE(st.impact > 0.9f);
    f.onset = false;
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

// 每个效果都要能画满、静音都要熄灭（beat-pulse 的呼吸底光除外）。
void test_all_effects_render(void) {
    Geometry g; FxState st; FxConfig c;
    const AudioFrame f = liveFrame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(23, (int)FX_COUNT, "效果数量变了，测试没跟上");
    AudioFrame ev = f;
    ev.downbeat = true; ev.onset = true; ev.vocal_onset = true; ev.section_change = true;
    for (int e = 0; e < FX_COUNT; ++e) {
        st = FxState{};
        fxRender((FxId)e, st, c, ev, g, true, 23.22f, g_px);      // 头一帧带事件
        for (int w = 0; w < 11; ++w) fxRender((FxId)e, st, c, f, g, true, 23.22f, g_px);
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
    lo.onset = hi.onset = true;               // 同强度击打事件
    for (int i = 0; i < NUM_BANDS; ++i) {
        lo.bands[i] = (i < 8) ? 0.5f : 0.0f;  // 频谱形状完全相反
        hi.bands[i] = (i < 8) ? 0.0f : 0.5f;
        lo.bands_p[i] = hi.bands_p[i] = 0.02f;   // 打击能量相同
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
    AudioFrame weak = lo;                     // 弱击打：有事件但打击能量为零（保底峰）
    for (int i = 0; i < NUM_BANDS; ++i) weak.bands_p[i] = 0.0f;
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
        "调明确时应当比不明确时更饱和");
    // 但不能塌到灰白 —— 那是下面两条盯的事
}

void test_key_wash_falls_back_to_timbre_when_the_key_is_unclear(void) {
    // 失真吉他：power chord 没有三度、泛音又密，key_conf 一直很低。
    // 只降饱和的话整首摇滚是一片灰白 —— 判断没错，但什么都没表达。
    // 退路是频谱质心：在失真下依然稳定、依然与音乐相关。
    Geometry g; FxConfig c;
    static Rgb dark[TOTAL_LEDS], bright[TOTAL_LEDS];
    AudioFrame lo = liveFrame(), hi = liveFrame();
    lo.key_conf = 0.0f; lo.key_root = -1;
    hi.key_conf = 0.0f; hi.key_root = -1;
    for (int i = 0; i < NUM_BANDS; ++i) { lo.bands[i] = 0.02f; hi.bands[i] = 0.02f; }
    for (int i = 0; i < 4; ++i)  lo.bands[i] = 0.9f;         // 重心在低频
    for (int i = 11; i < 15; ++i) hi.bands[i] = 0.9f;        // 重心在高频
    FxState a, b;
    for (int k = 0; k < 200; ++k) {   // st.hue 的时间常数较长，要跑到收敛
        fxRender(FX_KEY_WASH, a, c, lo, g, false, 23.22f, dark);
        fxRender(FX_KEY_WASH, b, c, hi, g, false, 23.22f, bright);
    }
    const Rgb x = dark[mapPixel(g, SIDE_L, 0.5f)], y = bright[mapPixel(g, SIDE_L, 0.5f)];
    const int d = abs((int)x.r - y.r) + abs((int)x.g - y.g) + abs((int)x.b - y.b);
    TEST_ASSERT_TRUE_MESSAGE(d > 60,
        "调不明确时颜色应当跟着音色走，而不是两种音色画成同一个颜色");
}

void test_key_wash_stays_colourful_without_a_key(void) {
    // 正对照：上一条只说「两种音色不同色」，这条说它们都还是**有颜色的**。
    // 否则退路退成两种不同的灰，上一条照样能过。
    Geometry g; FxConfig c;
    static Rgb px[TOTAL_LEDS];
    AudioFrame f = liveFrame();
    f.key_conf = 0.0f; f.key_root = -1;
    FxState st;
    for (int k = 0; k < 200; ++k) fxRender(FX_KEY_WASH, st, c, f, g, false, 23.22f, px);
    int best = 0;
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
        int mx = px[i].r, mn = px[i].r;
        if (px[i].g > mx) mx = px[i].g; if (px[i].g < mn) mn = px[i].g;
        if (px[i].b > mx) mx = px[i].b; if (px[i].b < mn) mn = px[i].b;
        if (mx - mn > best) best = mx - mn;
    }
    TEST_ASSERT_TRUE_MESSAGE(best > 40, "没有调也不该退成灰白");
}

void test_spectrum_bars_brightness_follows_the_dynamics(void) {
    // 古典的关键一条：AGC 已经把 bands 的绝对电平抹平，两帧的**形状完全相同**，
    // 只有 AGC 之前的 rms 不同。乘上 dynamics 之后，pp 才比 ff 暗。
    //
    // 注意两帧的 bands 一模一样 —— 不乘 dynamics 的话它们必然画得一样亮，
    // 这条测试是直接冲着那个写法去的。
    Geometry g; FxConfig c;
    static Rgb pf[TOTAL_LEDS], pp[TOTAL_LEDS];
    AudioFrame ff = liveFrame(), quiet = liveFrame();
    ff.dynamics = 1.0f; quiet.dynamics = 0.25f;
    auto sum=[](const Rgb *p){ long a=0; for (uint16_t i=0;i<TOTAL_LEDS;++i) a+=p[i].r+p[i].g+p[i].b; return a; };
    FxState a, b;
    for (int k = 0; k < 20; ++k) {
        fxRender(FX_SPECTRUM_BARS, a, c, ff,    g, false, 23.22f, pf);
        fxRender(FX_SPECTRUM_BARS, b, c, quiet, g, false, 23.22f, pp);
    }
    TEST_ASSERT_TRUE_MESSAGE(sum(pf) > sum(pp) * 2,
        "频段柱必须跟着动态走，否则古典的 pp 和 ff 画出来一样亮");
    TEST_ASSERT_TRUE_MESSAGE(sum(pp) > 0, "弱奏是变暗，不是熄灭");
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

// 跑 n 帧后沿管累计相邻采样点的 RGB 变化 —— 色相跨度的代理。
// 不能只比两端点：0.9 圈的端点绕色环几乎回到原地，端点差和 0.1 圈
// 分不开，旧版实际是靠"静=低饱和"的饱和度差在过关；沿途路程不会被骗。
static float hueSpread(FxState &st, const AudioFrame &f) {
    Geometry g; Rgb px[TOTAL_LEDS];
    for (int k = 0; k < 40; ++k) fxRender(FX_COLOR_FLOW, st, g_fxcfg, f, g, false, 23.22f, px);
    float total = 0.0f;
    Rgb prev = px[mapPixel(g, SIDE_L, 0.0f)];
    for (int k = 1; k <= 16; ++k) {
        const Rgb c = px[mapPixel(g, SIDE_L, (float)k / 16.0f)];
        total += fabsf((float)c.r - prev.r) + fabsf((float)c.g - prev.g) + fabsf((float)c.b - prev.b);
        prev = c;
    }
    return total;
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


// ══ 特征标签与风格推荐表 ══════════════════════════════════

// **需求本身就是契约**：界面按音乐特性筛选，每类至少 5 个效果。
// 写成测试，将来加减效果时立刻知道有没有把某一类掏空。
void test_every_category_has_at_least_five_effects(void) {
    const uint8_t tags[5] = {TAG_BEAT, TAG_MELODY, TAG_VOCAL, TAG_MOOD, TAG_SPECTRUM};
    const char *nm[5] = {"beat", "melody", "vocal", "mood", "spectrum"};
    for (int t = 0; t < 5; ++t) {
        int n = 0;
        for (int e = 0; e < FX_COUNT; ++e) if (fxTags((FxId)e) & tags[t]) ++n;
        char msg[80];
        snprintf(msg, sizeof msg, "%s 类只有 %d 个效果，少于 5 个", nm[t], n);
        TEST_ASSERT_TRUE_MESSAGE(n >= 5, msg);
    }
}

void test_every_effect_has_a_tag_and_a_genre(void) {
    for (int e = 0; e < FX_COUNT; ++e) {
        TEST_ASSERT_TRUE_MESSAGE(fxTags((FxId)e) != 0, "有效果没标特征，界面筛不到它");
        TEST_ASSERT_TRUE_MESSAGE(fxGenres((FxId)e) != 0, "有效果没标风格");
        TEST_ASSERT_TRUE_MESSAGE(fxName((FxId)e)[0] != '\0', "有效果没名字");
        TEST_ASSERT_TRUE_MESSAGE(fxNameCn((FxId)e)[0] != '\0', "有效果没中文名");
    }
}

void test_every_genre_has_at_least_four_effects(void) {
    const uint8_t gs[5] = {GEN_CLASSICAL, GEN_POP, GEN_ROCK, GEN_RAP, GEN_EDM};
    for (int t = 0; t < 5; ++t) {
        int n = 0;
        for (int e = 0; e < FX_COUNT; ++e) if (fxGenres((FxId)e) & gs[t]) ++n;
        TEST_ASSERT_TRUE_MESSAGE(n >= 4, "有风格推荐不足 4 个效果");
    }
}

// 名字不能重复 —— 界面拿名字当键，重了就会选错。
void test_effect_names_are_unique(void) {
    for (int a = 0; a < FX_COUNT; ++a)
        for (int b = a + 1; b < FX_COUNT; ++b) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(fxName((FxId)a), fxName((FxId)b)) != 0, "英文名重复");
            TEST_ASSERT_TRUE_MESSAGE(strcmp(fxNameCn((FxId)a), fxNameCn((FxId)b)) != 0, "中文名重复");
        }
}

// ══ 新效果的区分性 ════════════════════════════════════════
//
// 通用不变量（画满、静音熄灭、不越界）对所有效果自动生效；
// 这里每条钉的是「这个效果凭什么和别的不一样」—— 没有这条，
// 新效果就只是换了个名字的旧效果。

static int litCount(void) {
    int n = 0;
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) if (g_px[i].r || g_px[i].g || g_px[i].b) ++n;
    return n;
}
static long brightness(void) {
    long v = 0;
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) v += g_px[i].r + g_px[i].g + g_px[i].b;
    return v;
}
// 跑一串帧：头一帧用 ev（可带事件标志），其余用 f
static void runFx(FxId e, const AudioFrame &ev, const AudioFrame &f, int n) {
    Geometry g;
    g_fxst = FxState{};
    fxRender(e, g_fxst, g_fxcfg, ev, g, false, 23.22f, g_px);
    for (int k = 1; k < n; ++k) fxRender(e, g_fxst, g_fxcfg, f, g, false, 23.22f, g_px);
}
static void more(FxId e, const AudioFrame &f, int n) {
    Geometry g;
    for (int k = 0; k < n; ++k) fxRender(e, g_fxst, g_fxcfg, f, g, false, 23.22f, g_px);
}

// 强拍绽放：强拍必须比普通起音铺得更开 —— 否则它和冲击柱没区别。
void test_downbeat_bloom_reaches_further_than_a_weak_beat(void) {
    AudioFrame f = liveFrame(), down = f, weak = f;
    down.downbeat = true; down.onset = true;
    weak.downbeat = false; weak.onset = true;
    runFx(FX_DOWNBEAT_BLOOM, down, f, 3);
    const int wide = litCount();
    runFx(FX_DOWNBEAT_BLOOM, weak, f, 3);
    const int narrow = litCount();
    TEST_ASSERT_TRUE_MESSAGE(wide > narrow * 2, "强拍必须明显比弱拍铺得开");
}

// 小节阶梯：拍号看得出来。3/4 的第一段占管长 1/3，4/4 只占 1/4。
void test_bar_ladder_shows_the_time_signature(void) {
    AudioFrame a = liveFrame(), b = liveFrame();
    a.beats_per_bar = 4; a.bar_pos = 0;
    b.beats_per_bar = 3; b.bar_pos = 0;
    runFx(FX_BAR_LADDER, a, a, 2); const long ba = brightness();
    runFx(FX_BAR_LADDER, b, b, 2); const long bb = brightness();
    TEST_ASSERT_TRUE_MESSAGE(bb > ba, "3/4 的第一拍段应当比 4/4 的更长");
}

// 鼓组分离吃的是**打击路**，不是混合谱 —— 这是它与高低分离的分别。
void test_kick_snare_reads_the_percussive_path_only(void) {
    AudioFrame f = liveFrame();
    for (int i = 0; i < NUM_BANDS; ++i) { f.bands_p[i] = 0.0f; f.bands[i] = 0.9f; }
    runFx(FX_KICK_SNARE, f, f, 10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, litCount(), "打击路为零时不该有画面，哪怕混合谱很响");
    for (int i = 0; i < 4; ++i) f.bands_p[i] = 0.9f;      // 只有底鼓
    runFx(FX_KICK_SNARE, f, f, 10);
    TEST_ASSERT_TRUE_MESSAGE(litCount() > 5, "底鼓应当点亮管底");
}

// 音高彗星的余辉必须比旋律线**衰减得慢** —— 否则两个效果是同一个。
//
// ⚠️ 判据是**衰减比例**，不是总亮度。初稿比的是总亮度，红了：
// 旋律线每帧铺 4 颗、彗星只铺 1 颗，那样比的是「铺得宽不宽」，
// 不是「留得久不久」—— 名字断言了一个它并不检验的性质（第 17 条）。
void test_pitch_comet_leaves_a_longer_trail_than_melody_line(void) {
    AudioFrame f = liveFrame(), quiet = f;
    quiet.f0_voiced = false;                       // 停止发声，只剩拖尾
    // **基线要在彗头退掉之后取。** 彗星的头是一团辉光，停止发声那一刻整个
    // 消失，把它算进基线的话量到的是「头占多大比例」，不是拖尾衰减多快 ——
    // 初稿就是这么写的，红了。先空跑 6 帧让头退干净，再开始比。
    runFx(FX_PITCH_COMET, f, f, 6);   more(FX_PITCH_COMET, quiet, 6);
    const double cp = (double)brightness();
    more(FX_PITCH_COMET, quiet, 12);
    const double cr = (double)brightness() / (cp > 0 ? cp : 1.0);
    runFx(FX_MELODY_LINE, f, f, 6);   more(FX_MELODY_LINE, quiet, 6);
    const double mp = (double)brightness();
    more(FX_MELODY_LINE, quiet, 12);
    const double mr = (double)brightness() / (mp > 0 ? mp : 1.0);
    // 实测：彗星 0.94、旋律线 0.79（拖尾数组本身按 τ=900ms/500ms 衰减，
    // 12 帧 279ms 理论保留 0.734/0.574，与实测一致）。加法余量 0.10 有富余。
    TEST_ASSERT_TRUE_MESSAGE(cr > mr + 0.10,
        "同样停止发声，彗星保留的比例应当明显高于旋律线");
}

// 和声推移的色相由调决定 —— 换调画面必须换色。
void test_harmony_shift_hue_follows_the_key(void) {
    AudioFrame c = liveFrame(), fs = liveFrame();
    c.key_root = 0; fs.key_root = 6;               // C 与 升F，色环上正对面
    runFx(FX_HARMONY_SHIFT, c, c, 400);
    const Rgb a = g_px[TOTAL_LEDS / 4];
    runFx(FX_HARMONY_SHIFT, fs, fs, 400);
    const Rgb b = g_px[TOTAL_LEDS / 4];
    const int d = abs((int)a.r - (int)b.r) + abs((int)a.g - (int)b.g) + abs((int)a.b - (int)b.b);
    TEST_ASSERT_TRUE_MESSAGE(d > 60, "换调之后和声推移的颜色必须明显不同");
}

// ── 人声类 ──

void test_vocal_halo_brightness_follows_the_vocal_score(void) {
    AudioFrame hi = liveFrame(), lo = liveFrame();
    hi.vocal = 0.95f; lo.vocal = 0.05f;
    runFx(FX_VOCAL_HALO, hi, hi, 60); const long bh = brightness();
    runFx(FX_VOCAL_HALO, lo, lo, 60); const long bl = brightness();
    TEST_ASSERT_TRUE_MESSAGE(bh > bl * 3, "人声强时光晕必须明显更亮更宽");
}

void test_vocal_breath_dims_in_instrumental_sections(void) {
    AudioFrame sing = liveFrame(), inst = liveFrame();
    sing.vocal = 0.9f; inst.vocal = 0.0f;
    runFx(FX_VOCAL_BREATH, sing, sing, 60); const long bs = brightness();
    runFx(FX_VOCAL_BREATH, inst, inst, 60); const long bi = brightness();
    TEST_ASSERT_TRUE_MESSAGE(bs > bi * 2, "器乐段落必须明显暗下去");
    TEST_ASSERT_TRUE_MESSAGE(bi > 0, "但也不该全黑 —— 那看起来像灯坏了");
}

// 共振峰带只吃谐波路，且只吃 300–3000Hz 那几段。
void test_formant_ribbon_ignores_the_percussive_path(void) {
    AudioFrame f = liveFrame();
    for (int i = 0; i < NUM_BANDS; ++i) { f.bands_h[i] = 0.0f; f.bands_p[i] = 0.9f; }
    runFx(FX_FORMANT_RIBBON, f, f, 4);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, litCount(), "谐波路为零时不该有画面，哪怕打击路很响");
}

void test_formant_ribbon_only_reads_the_vocal_band(void) {
    AudioFrame lowOnly = liveFrame(), inBand = liveFrame();
    for (int i = 0; i < NUM_BANDS; ++i) { lowOnly.bands_h[i] = 0.0f; inBand.bands_h[i] = 0.0f; }
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float cf = bandCenterHz(i);
        if (cf < 300.0f)                   lowOnly.bands_h[i] = 0.9f;
        if (cf >= 300.0f && cf <= 3000.0f) inBand.bands_h[i]  = 0.9f;
    }
    runFx(FX_FORMANT_RIBBON, lowOnly, lowOnly, 4); const long bl = brightness();
    runFx(FX_FORMANT_RIBBON, inBand,  inBand,  4); const long bi = brightness();
    TEST_ASSERT_TRUE_MESSAGE(bi > bl * 4, "只有 300–3000Hz 那几段该点亮这条带子");
}

// 两根管各说一件事：左管谐波、右管打击。这是这台灯有两根管才做得到的。
void test_duet_split_puts_harmonic_left_and_percussive_right(void) {
    Geometry g;
    AudioFrame f = liveFrame();
    for (int i = 0; i < NUM_BANDS; ++i) { f.bands_h[i] = 0.9f; f.bands_p[i] = 0.0f; }
    runFx(FX_DUET_SPLIT, f, f, 4);
    long left = 0, right = 0;
    for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
        const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
        const Rgb l = g_px[mapPixel(g, SIDE_L, u)], r = g_px[mapPixel(g, SIDE_R, u)];
        left  += l.r + l.g + l.b;
        right += r.r + r.g + r.b;
    }
    TEST_ASSERT_TRUE_MESSAGE(left > 0 && right == 0, "只有谐波时应当只有左管亮");
}

// 唱句脉冲画的是**乐句起点**，不是人声的持续 —— 起音后必须退下去。
void test_lyric_pulse_decays_between_phrases(void) {
    AudioFrame f = liveFrame(), ev = f;
    ev.vocal_onset = true;
    runFx(FX_LYRIC_PULSE, ev, f, 2);
    const long peak = brightness();
    more(FX_LYRIC_PULSE, f, 60);
    TEST_ASSERT_TRUE_MESSAGE(peak > 0, "唱句起音时应当亮起来");
    TEST_ASSERT_TRUE_MESSAGE(brightness() < peak / 4, "句与句之间必须退下去");
}

// **人声度低时不能全黑，但真静音必须熄灭。**
//
// 这条是用户报「人声光晕、唱句脉冲完全不显示」之后补的。原来写的是
// voc<=0.01 直接 return / lyric<=0 直接 return —— 在人声度低的素材上
// 整根管全黑，看起来像灯效坏了，而不是「这段没人声」。
// 与 fxBeatRunner 的 25% 底光同一条规矩。
void test_vocal_effects_show_a_standby_glow_but_still_go_dark_on_silence(void) {
    const FxId ids[2] = {FX_VOCAL_HALO, FX_LYRIC_PULSE};
    for (int k = 0; k < 2; ++k) {
        AudioFrame quiet = liveFrame();      // 有声音，但没有人声
        quiet.vocal = 0.0f; quiet.vocal_onset = false;
        runFx(ids[k], quiet, quiet, 30);
        TEST_ASSERT_TRUE_MESSAGE(litCount() > 3,
            "有声音、没人声时应当留一点待机光，不能全黑");

        AudioFrame sing = liveFrame();
        sing.vocal = 0.95f;
        AudioFrame ev = sing; ev.vocal_onset = true;
        runFx(ids[k], ev, sing, 4);
        const long loud = brightness();
        runFx(ids[k], quiet, quiet, 30);
        TEST_ASSERT_TRUE_MESSAGE(loud > brightness() * 3,
            "有人声时必须明显比待机亮 —— 否则待机光把效果本身盖掉了");

        AudioFrame sil;                      // 全零：真静音
        runFx(ids[k], sil, sil, 30);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, litCount(), "真静音必须彻底熄灭");
    }
}

// ── 氛围类 ──

void test_section_tide_sweeps_only_on_a_section_change(void) {
    AudioFrame f = liveFrame(), ev = f;
    ev.section_change = true;
    runFx(FX_SECTION_TIDE, ev, f, 4);
    const long during = brightness();
    more(FX_SECTION_TIDE, f, 200);
    TEST_ASSERT_TRUE_MESSAGE(during > brightness(), "换段那一下应当比段内亮");
}

void test_mood_gradient_hue_follows_the_mood(void) {
    AudioFrame calm = liveFrame(), wild = liveFrame();
    calm.mood = 0.0f; wild.mood = 1.0f;
    runFx(FX_MOOD_GRADIENT, calm, calm, 4);
    const Rgb c = g_px[TOTAL_LEDS / 3];
    runFx(FX_MOOD_GRADIENT, wild, wild, 4);
    const Rgb w = g_px[TOTAL_LEDS / 3];
    TEST_ASSERT_TRUE_MESSAGE(w.r > c.r && c.b > w.b, "静应当偏冷、躁应当偏暖");
}

// 极光是唯一一个**不追随瞬时声音**的效果：响度差 18 倍画面也不该跟着变。
void test_slow_aurora_ignores_the_instantaneous_level(void) {
    AudioFrame soft = liveFrame(), loud = liveFrame();
    soft.rms_fast = 0.05f; loud.rms_fast = 0.9f;
    runFx(FX_SLOW_AURORA, soft, soft, 4); const long bs = brightness();
    runFx(FX_SLOW_AURORA, loud, loud, 4); const long bl = brightness();
    TEST_ASSERT_TRUE_MESSAGE(labs(bs - bl) < bs / 8, "极光不该跟着瞬时响度变");
}


// ── 三段物理布局：主柱 / 底部灯柱 / 底座环（2026-09-05 /leddebug 实测） ──

void test_geometry_maps_u_onto_main_column_only(void) {
    Geometry g;
    TEST_ASSERT_EQUAL_UINT16(18, mapPixel(g, SIDE_L, 0.0f));   // 主柱底
    TEST_ASSERT_EQUAL_UINT16(47, mapPixel(g, SIDE_L, 1.0f));   // 主柱顶
    TEST_ASSERT_EQUAL_UINT16(66, mapPixel(g, SIDE_R, 0.0f));
    TEST_ASSERT_EQUAL_UINT16(95, mapPixel(g, SIDE_R, 1.0f));
    // u 均匀扫过时不得落进环/底柱（索引 0..17 / 48..65）
    for (int k = 0; k <= 100; ++k) {
        const uint16_t a = mapPixel(g, SIDE_L, k / 100.0f), b = mapPixel(g, SIDE_R, k / 100.0f);
        TEST_ASSERT_TRUE(a >= 18 && a <= 47);
        TEST_ASSERT_TRUE(b >= 66 && b <= 95);
    }
    TEST_ASSERT_EQUAL_UINT16(0,  zonePixel(g, SIDE_L, ZONE_RING, 0));
    TEST_ASSERT_EQUAL_UINT16(11, zonePixel(g, SIDE_L, ZONE_RING, 11));
    TEST_ASSERT_EQUAL_UINT16(12, zonePixel(g, SIDE_L, ZONE_BOTTOM, 0));
    TEST_ASSERT_EQUAL_UINT16(17, zonePixel(g, SIDE_L, ZONE_BOTTOM, 5));
    TEST_ASSERT_EQUAL_UINT16(48, zonePixel(g, SIDE_R, ZONE_RING, 0));
    TEST_ASSERT_EQUAL_UINT16(65, zonePixel(g, SIDE_R, ZONE_BOTTOM, 5));
    TEST_ASSERT_EQUAL_UINT16(17, zonePixel(g, SIDE_L, ZONE_BOTTOM, 99));   // 越界钳制
    Geometry rev; rev.s1_reversed = true;
    TEST_ASSERT_EQUAL_UINT16(47, mapPixel(rev, SIDE_L, 0.0f));  // 反转只翻主柱
    TEST_ASSERT_EQUAL_UINT16(0,  zonePixel(rev, SIDE_L, ZONE_RING, 0));
}

void test_zones_derive_from_main_column(void) {
    Geometry g; FxState st{}; static Rgb px[TOTAL_LEDS];
    fxRender(FX_SPECTRUM_BARS, st, g_fxcfg, liveFrame(), g, false, 23.22f, px);
    // 主柱有内容 → 环与底柱各自均匀点亮
    for (int s = 0; s < 2; ++s) {
        const Side side = (Side)s;
        const Rgb r0 = px[zonePixel(g, side, ZONE_RING, 0)], b0 = px[zonePixel(g, side, ZONE_BOTTOM, 0)];
        TEST_ASSERT_TRUE_MESSAGE(r0.r + r0.g + r0.b > 0, "底座环应从主柱派生出亮度");
        TEST_ASSERT_TRUE_MESSAGE(b0.r + b0.g + b0.b > 0, "底部灯柱应从主柱派生出亮度");
        for (uint16_t k = 1; k < RING_LEDS; ++k) {
            const Rgb c = px[zonePixel(g, side, ZONE_RING, k)];
            TEST_ASSERT_TRUE_MESSAGE(c.r == r0.r && c.g == r0.g && c.b == r0.b, "环 12 颗应同色");
        }
        for (uint16_t k = 1; k < BOT_LEDS; ++k) {
            const Rgb c = px[zonePixel(g, side, ZONE_BOTTOM, k)];
            TEST_ASSERT_TRUE_MESSAGE(c.r == b0.r && c.g == b0.g && c.b == b0.b, "底柱 6 颗应同色");
        }
    }
    // 静音：主柱全黑 → 环与底柱同黑（「没声音就熄灯」）
    AudioFrame silent; FxState st2{};
    fxRender(FX_SPECTRUM_BARS, st2, g_fxcfg, silent, g, false, 23.22f, px);
    for (uint16_t i = 0; i < 18; ++i)  TEST_ASSERT_TRUE(px[i].r == 0 && px[i].g == 0 && px[i].b == 0);
    for (uint16_t i = 48; i < 66; ++i) TEST_ASSERT_TRUE(px[i].r == 0 && px[i].g == 0 && px[i].b == 0);
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
    RUN_TEST(test_key_wash_falls_back_to_timbre_when_the_key_is_unclear);
    RUN_TEST(test_key_wash_stays_colourful_without_a_key);
    RUN_TEST(test_spectrum_bars_brightness_follows_the_dynamics);
    RUN_TEST(test_color_flow_hue_span_follows_the_mood);
    RUN_TEST(test_geometry_maps_u_onto_main_column_only);
    RUN_TEST(test_zones_derive_from_main_column);
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
    RUN_TEST(test_every_category_has_at_least_five_effects);
    RUN_TEST(test_every_effect_has_a_tag_and_a_genre);
    RUN_TEST(test_every_genre_has_at_least_four_effects);
    RUN_TEST(test_effect_names_are_unique);
    RUN_TEST(test_downbeat_bloom_reaches_further_than_a_weak_beat);
    RUN_TEST(test_bar_ladder_shows_the_time_signature);
    RUN_TEST(test_kick_snare_reads_the_percussive_path_only);
    RUN_TEST(test_pitch_comet_leaves_a_longer_trail_than_melody_line);
    RUN_TEST(test_harmony_shift_hue_follows_the_key);
    RUN_TEST(test_vocal_halo_brightness_follows_the_vocal_score);
    RUN_TEST(test_vocal_breath_dims_in_instrumental_sections);
    RUN_TEST(test_formant_ribbon_ignores_the_percussive_path);
    RUN_TEST(test_formant_ribbon_only_reads_the_vocal_band);
    RUN_TEST(test_duet_split_puts_harmonic_left_and_percussive_right);
    RUN_TEST(test_lyric_pulse_decays_between_phrases);
    RUN_TEST(test_vocal_effects_show_a_standby_glow_but_still_go_dark_on_silence);
    RUN_TEST(test_section_tide_sweeps_only_on_a_section_change);
    RUN_TEST(test_mood_gradient_hue_follows_the_mood);
    RUN_TEST(test_slow_aurora_ignores_the_instantaneous_level);
    return UNITY_END();
}
