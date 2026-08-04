#include <unity.h>
#include <math.h>
#include "lamp_vocal.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static VocalState  S;
static VocalConfig C;
static const float DT = 23.22f;          // 通用档 hop

static void reset() { S = VocalState{}; C = VocalConfig{}; }

// 谐波谱：把能量集中在 [lo,hi] 内的段上
static void spectrumIn(float *h, float lo, float hi, float amp) {
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float c = bandCenterHz(i);
        h[i] = (c >= lo && c <= hi) ? amp : 0.0f;
    }
}
static void spectrumFlat(float *h, float amp) {
    for (int i = 0; i < NUM_BANDS; ++i) h[i] = amp;
}

// 跑 ms 毫秒，返回结束时的 vocal
static float run(float ms, float f0, float conf, bool voiced,
                 const float *h, float perc) {
    float t = 0.0f;
    while (t < ms) { vocalUpdate(S, C, f0, conf, voiced, h, perc, DT); t += DT; }
    return S.vocal;
}

// ── 三个因子各自都是必要条件 ────────────────────────────────

// 三个门相乘会把「都不错」压低 —— 这是刻意的，但要知道压到哪。
// conf=0.9 / perc=0.1 这组「很好但不完美」的输入实测得 0.73，
// 理想输入（conf=1 / perc=0）得 0.99。判据是**越过起音阈值 0.55**，
// 不是某个我拍脑袋定的 0.8（初稿就是这么写的，红了）。
void test_a_harmonic_melody_in_the_vocal_range_scores_high(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    const float good = run(2000.0f, 220.0f, 0.9f, true, h, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 0.73f, good);
    TEST_ASSERT_TRUE_MESSAGE(good > C.onset_on, "很好的输入必须越过起音阈值");

    reset();
    const float ideal = run(2000.0f, 220.0f, 1.0f, true, h, 0.0f);
    TEST_ASSERT_TRUE_MESSAGE(ideal > 0.95f, "理想输入应当接近满分");
}

void test_an_unvoiced_frame_scores_zero(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    // 其余两项拉满，只是没有基频
    const float v = run(2000.0f, 220.0f, 0.9f, false, h, 0.1f);
    TEST_ASSERT_TRUE_MESSAGE(v < 0.05f, "没有基频就不该判为人声");
}

void test_a_bass_drum_fundamental_is_below_the_vocal_range(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    const float v = run(2000.0f, 55.0f, 0.9f, true, h, 0.1f);
    TEST_ASSERT_TRUE_MESSAGE(v < 0.05f, "55Hz 在人声音域之下，不该算人声");
}

void test_a_whistle_above_the_vocal_range_is_rejected(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    const float v = run(2000.0f, 2500.0f, 0.9f, true, h, 0.1f);
    TEST_ASSERT_TRUE_MESSAGE(v < 0.05f, "2500Hz 在人声音域之上，不该算人声");
}

void test_energy_outside_the_formant_band_scores_low(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 4000.0f, 9000.0f, 1.0f);   // 全在高段
    const float v = run(2000.0f, 220.0f, 0.9f, true, h, 0.1f);
    TEST_ASSERT_TRUE_MESSAGE(v < 0.05f, "能量不在共振峰带就不该算人声");
}

void test_a_percussive_texture_scores_low(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    const float v = run(2000.0f, 220.0f, 0.9f, true, h, 0.95f);
    TEST_ASSERT_TRUE_MESSAGE(v < 0.05f, "打击占比高时不该算人声");
}

// **相乘不是相加。** 单项拉满不能糊弄过去 —— 这条正是「相加」实现的杀手。
void test_one_factor_alone_cannot_carry_the_score(void) {
    float h[NUM_BANDS];
    reset(); spectrumFlat(h, 1.0f);
    const float onlyVoiced = run(2000.0f, 220.0f, 1.0f, true, h, 0.95f);
    reset(); spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    const float onlyFormant = run(2000.0f, 220.0f, 0.0f, false, h, 0.95f);
    TEST_ASSERT_TRUE_MESSAGE(onlyVoiced  < 0.2f, "只有基频不该判为人声");
    TEST_ASSERT_TRUE_MESSAGE(onlyFormant < 0.2f, "只有共振峰带不该判为人声");
}

// ── 共振峰占比 ─────────────────────────────────────────────

void test_formant_share_counts_partial_bands_by_frequency_span(void) {
    C = VocalConfig{};
    float h[NUM_BANDS] = {0};
    // 第 4 段是 301.46–430.66Hz，整段都在 [300,3000] 内 → 全额计入
    h[4] = 1.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, formantShare(C, h));

    // 第 3 段是 215.33–301.46Hz，只有 301.46-300=1.46Hz 落在区间内
    for (int i = 0; i < NUM_BANDS; ++i) h[i] = 0.0f;
    h[3] = 1.0f;
    const float part = formantShare(C, h);
    TEST_ASSERT_TRUE_MESSAGE(part > 0.0f && part < 0.05f,
        "跨界的段应当按频率跨度比例计入，不是非零即一");
}

void test_formant_share_of_silence_is_zero(void) {
    C = VocalConfig{};
    float h[NUM_BANDS] = {0};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, formantShare(C, h));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, formantShare(C, nullptr));
}

void test_formant_share_ignores_nan_and_negative_bands(void) {
    C = VocalConfig{};
    float h[NUM_BANDS] = {0};
    h[4] = 1.0f;                       // 区间内
    h[0] = NAN;                        // 区间外的脏数据
    h[15] = -5.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, formantShare(C, h));
}

// **门槛两侧的四个实测值都钉在这里。**
// 初稿的 0.45 是估的，密集混音（每段等能量）实测 0.500 会直接穿过去 ——
// 这条测试就是为了让下次有人改门槛时立刻知道两边的余量在哪。
void test_formant_threshold_sits_between_a_dense_mix_and_a_voice(void) {
    C = VocalConfig{};
    float h[NUM_BANDS];

    spectrumFlat(h, 1.0f);                                   // 每段等能量：密集混音
    const float dense = formantShare(C, h);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.500f, dense);

    for (int i = 0; i < NUM_BANDS; ++i)                      // 每 Hz 等能量：白噪声
        h[i] = kBandEdgeHz[i + 1] - kBandEdgeHz[i];
    const float white = formantShare(C, h);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.293f, white);

    spectrumIn(h, 300.0f, 3000.0f, 1.0f);                    // 纯人声
    const float voice = formantShare(C, h);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.997f, voice);

    spectrumIn(h, 300.0f, 3000.0f, 1.0f);                    // 人声 + 全频段底噪
    for (int i = 0; i < NUM_BANDS; ++i) h[i] += 0.5f;
    const float mixed = formantShare(C, h);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.748f, mixed);

    TEST_ASSERT_TRUE_MESSAGE(dense < C.formant_share_min, "密集混音必须被挡住");
    TEST_ASSERT_TRUE_MESSAGE(white < C.formant_share_min, "白噪声必须被挡住");
    TEST_ASSERT_TRUE_MESSAGE(mixed > C.formant_share_min, "压着底噪的人声必须放过");
}

// ── 时间常数按物理时间定 ───────────────────────────────────

void test_rise_time_is_defined_in_physical_time(void) {
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    float got[2];
    const float dts[2] = {5.805f, 46.44f};    // 极限打点档 与 氛围档
    for (int k = 0; k < 2; ++k) {
        reset();
        float t = 0.0f;
        while (t < 300.0f) {                  // 固定 300ms 物理时间
            vocalUpdate(S, C, 220.0f, 0.9f, true, h, 0.1f, dts[k]);
            t += dts[k];
        }
        got[k] = S.vocal;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.06f, got[0], got[1],
        "同样 300ms，两档升到的位置应当接近 —— 写死帧数就会差 8 倍");
}

void test_release_is_slower_than_attack(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    run(3000.0f, 220.0f, 0.9f, true, h, 0.1f);
    const float peak = S.vocal;
    // 换气：200ms 无基频
    const float after = run(200.0f, 220.0f, 0.9f, false, h, 0.1f);
    TEST_ASSERT_TRUE_MESSAGE(after > peak * 0.5f,
        "一句里换气 200ms 不该让人声度掉一半 —— 释放要比起音慢");
}

// ── 起音的滞回 ─────────────────────────────────────────────

void test_onset_fires_once_per_vocal_entry(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    int fired = 0;
    for (int k = 0; k < 200; ++k) {           // 人声进来并保持
        vocalUpdate(S, C, 220.0f, 0.9f, true, h, 0.1f, DT);
        if (S.onset) ++fired;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fired, "持续的一段人声只该报一次起音");
}

void test_onset_does_not_chatter_at_the_threshold(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    run(3000.0f, 220.0f, 0.9f, true, h, 0.1f);   // 先进入人声段
    int fired = 0;
    // 在 on/off 之间反复横跳：有滞回就不该再报
    for (int k = 0; k < 400; ++k) {
        const bool voiced = (k % 2) == 0;
        vocalUpdate(S, C, 220.0f, 0.9f, voiced, h, 0.1f, DT);
        if (S.onset) ++fired;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fired, "阈值附近抖动不该反复报起音");
}

// **滞回真正的用武之地是 off 与 on 之间那条带**（0.35–0.55）。
// 上一条测试让 vocal 一直停在 0.55 以上，那里两种实现表现一样 ——
// 它杀不掉「两个阈值合成一个」的变异体，是变异测试逮出来的。
//
// 这条把 vocal 压到带内（f0_conf 0.652 → raw≈0.45）再拉回去：
// 有滞回 above 全程为真、只报一次；单阈值会掉出去再进来，报两次。
void test_onset_does_not_refire_inside_the_hysteresis_band(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    int fired = 0;
    auto phase = [&](float ms, float conf) {
        float t = 0.0f;
        while (t < ms) {
            vocalUpdate(S, C, 220.0f, conf, true, h, 0.1f, DT);
            if (S.onset) ++fired;
            t += DT;
        }
    };
    phase(2000.0f, 0.90f);      // 进入人声段（raw≈0.73）
    TEST_ASSERT_EQUAL_INT(1, fired);
    phase(3000.0f, 0.652f);     // 落进带内：低于 on(0.55)，但高于 off(0.35)
    TEST_ASSERT_TRUE_MESSAGE(S.vocal < C.onset_on && S.vocal > C.onset_off,
        "夹具没把 vocal 送进滞回带，这条测试就什么都没检验");
    phase(2000.0f, 0.90f);      // 再拉回去
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fired,
        "在滞回带里进出不该再报起音 —— 两个阈值合成一个就会报两次");
}

// ── 脏输入 ─────────────────────────────────────────────────

void test_nan_inputs_do_not_poison_the_state(void) {
    reset();
    float h[NUM_BANDS]; spectrumIn(h, 300.0f, 3000.0f, 1.0f);
    run(1000.0f, 220.0f, 0.9f, true, h, 0.1f);
    const float before = S.vocal;
    vocalUpdate(S, C, NAN, NAN, true, h, NAN, DT);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(S.vocal), "NaN 输入把状态污染了");
    TEST_ASSERT_TRUE_MESSAGE(S.vocal <= before + 1e-6f, "NaN 不该把分数抬高");
}

void test_gate_treats_the_threshold_as_a_floor_not_a_scale(void) {
    // 阈值处应当是 0，1.0 处应当是 1 —— auto 那边把阈值当缩放用过，
    // 结果兜底永远选不上。这条把口径钉死。
    TEST_ASSERT_EQUAL_FLOAT(0.0f, vocalGate(0.5f, 0.5f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, vocalGate(1.0f, 0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, vocalGate(0.75f, 0.5f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, vocalGate(0.2f, 0.5f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, vocalGate(0.9f, 1.0f));   // 退化阈值
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_harmonic_melody_in_the_vocal_range_scores_high);
    RUN_TEST(test_an_unvoiced_frame_scores_zero);
    RUN_TEST(test_a_bass_drum_fundamental_is_below_the_vocal_range);
    RUN_TEST(test_a_whistle_above_the_vocal_range_is_rejected);
    RUN_TEST(test_energy_outside_the_formant_band_scores_low);
    RUN_TEST(test_a_percussive_texture_scores_low);
    RUN_TEST(test_one_factor_alone_cannot_carry_the_score);
    RUN_TEST(test_formant_share_counts_partial_bands_by_frequency_span);
    RUN_TEST(test_formant_share_of_silence_is_zero);
    RUN_TEST(test_formant_share_ignores_nan_and_negative_bands);
    RUN_TEST(test_formant_threshold_sits_between_a_dense_mix_and_a_voice);
    RUN_TEST(test_rise_time_is_defined_in_physical_time);
    RUN_TEST(test_release_is_slower_than_attack);
    RUN_TEST(test_onset_fires_once_per_vocal_entry);
    RUN_TEST(test_onset_does_not_chatter_at_the_threshold);
    RUN_TEST(test_onset_does_not_refire_inside_the_hysteresis_band);
    RUN_TEST(test_nan_inputs_do_not_poison_the_state);
    RUN_TEST(test_gate_treats_the_threshold_as_a_floor_not_a_scale);
    return UNITY_END();
}
