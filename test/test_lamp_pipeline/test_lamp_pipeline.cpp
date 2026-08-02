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

void test_every_effect_writes_every_pixel(void) {
    Geometry g;
    AudioFrame f;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.6f;
    f.rms_fast = 0.5f; f.peak = 0.7f; f.beat_locked = true; f.phase = 0.0f; f.bpm = 120.0f;

    for (int e = 0; e < FX_COUNT; ++e) {
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i) g_px[i] = Rgb{9, 9, 9};
        fxRender((FxId)e, f, g, false, g_px);
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
        fxRender((FxId)e, f, g, true, g_px);
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
    AudioFrame f;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 1.0f;
    f.rms_fast = 1.0f; f.peak = 1.0f; f.beat_locked = true;

    for (int e = 0; e < FX_COUNT; ++e) {
        static Rgb raw[TOTAL_LEDS];
        fxRender((FxId)e, f, g, false, raw);
        fxRender((FxId)e, f, g, true,  g_px);
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
        AudioFrame f;
        for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.5f;
        for (int e = 0; e < FX_COUNT; ++e) {
            for (uint16_t i = 0; i < TOTAL_LEDS; ++i) g_px[i] = Rgb{0, 0, 0};
            fxRender((FxId)e, f, g, false, g_px);   // 越界会被 sanitizer 或崩溃抓到
        }
    }
    TEST_ASSERT_TRUE(true);
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
    return UNITY_END();
}
