#include <unity.h>
#include <math.h>
#include "lamp_beat.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// ── 合成节拍轨（与 test_lamp_onset 同一套）─────────────────

static void beatFrame(float *out, float phase, float period_ms, float amp) {
    const float t_ms = phase * period_ms;
    const float attack_ms = 40.0f;
    const float env = (t_ms < attack_ms) ? (t_ms / attack_ms)
                                         : expf(-(t_ms - attack_ms) / 120.0f);
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float w = (i < 4) ? 1.0f : 0.35f;
        out[i] = 0.05f + amp * w * env;
    }
}

static float noiseAt(uint32_t k) {
    uint32_t s = k * 1664525u + 1013904223u;
    s ^= s >> 16; s *= 2246822519u; s ^= s >> 13;
    return (float)(s & 0xFFFFu) / 65535.0f;
}

// 跑一段节拍轨，返回结束时刻。
static uint32_t feedBeats(BeatTracker &b, const BeatConfig &bc,
                          OnsetDetector &d, const OnsetConfig &oc,
                          float bpm, float dt_ms, float seconds,
                          float amp = 0.8f, float noise = 0.0f) {
    const float period_ms = 60000.0f / bpm;
    const int   frames    = (int)(seconds * 1000.0f / dt_ms);
    float bands[NUM_BANDS];
    uint32_t t = 0;
    for (int k = 0; k < frames; ++k) {
        const float tf = (float)k * dt_ms;
        t = (uint32_t)tf;
        beatFrame(bands, fmodf(tf, period_ms) / period_ms, period_ms, amp);
        if (noise > 0.0f)
            for (int i = 0; i < NUM_BANDS; ++i)
                bands[i] += noise * noiseAt((uint32_t)(k * NUM_BANDS + i));
        const float flux = spectralFlux(d.prev, bands, dt_ms);
        const bool  on   = onsetUpdate(d, oc, bands, t);
        beatUpdate(b, bc, flux, on, dt_ms, t);
    }
    return t;
}

// ── ODF 重采样 ────────────────────────────────────────────

// ODF 的速率必须恒为 43.066 Hz，与输入帧率无关 ——
// lag→BPM 的换算全靠它，漂了 BPM 就跟着漂。
void test_odf_rate_is_fixed_regardless_of_input_rate(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 43.066f, kOdfRateHz);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 23.22f, kOdfPeriodMs);
    // 6 秒窗
    TEST_ASSERT_EQUAL_INT(258, kOdfLen);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 6.0f, (float)kOdfLen / kOdfRateHz);
}

// 三种输入帧率跑同样的物理时长，推进的 ODF 样本数必须一致。
void test_odf_sample_count_is_invariant_to_hop(void) {
    const BeatConfig bc;
    const float dts[4] = {46.44f, 23.22f, 11.61f, 5.80f};
    for (int i = 0; i < 4; ++i) {
        BeatTracker b;
        beatInit(b, bc);
        const int frames = (int)(6000.0f / dts[i]);
        for (int k = 0; k < frames; ++k)
            beatUpdate(b, bc, 1.0f, false, dts[i], (uint32_t)(k * dts[i]));
        // 6 秒 × 43.066 = 258 个样本，允许 ±2 的取整误差
        TEST_ASSERT_TRUE_MESSAGE(b.odf_count >= 256 && b.odf_count <= 260,
            "ODF 推进速率随输入帧率变了");
    }
}

// 慢档（46.44ms > 23.22ms）一帧要产出两个 ODF 样本，两个都得有值。
// 补零的话 ODF 里会出现伪造的静音，自相关的峰会被削掉一半。
void test_slow_hop_upsamples_without_inserting_zeros(void) {
    const BeatConfig bc;
    BeatTracker b;
    beatInit(b, bc);
    for (int k = 0; k < 20; ++k)
        beatUpdate(b, bc, 5.0f, false, 46.44f, (uint32_t)(k * 46.44f));
    int zeros = 0;
    for (uint16_t i = 0; i < b.odf_count && i < kOdfLen; ++i)
        if (b.odf[i] < 0.01f) ++zeros;
    TEST_ASSERT_TRUE_MESSAGE(zeros <= 1, "上采样时插了零");
}

// ── BPM 估计 ──────────────────────────────────────────────

// 设计 §4 的验收线：60/90/120/140/180 BPM，误差 < 2%。
void test_bpm_accuracy_across_tempos(void) {
    const BeatConfig bc; const OnsetConfig oc;
    const float bpms[5] = {60.0f, 90.0f, 120.0f, 140.0f, 180.0f};
    for (int i = 0; i < 5; ++i) {
        BeatTracker b; OnsetDetector d;
        beatInit(b, bc); onsetInit(d, oc, 23.22f);
        feedBeats(b, bc, d, oc, bpms[i], 23.22f, 14.0f);
        TEST_ASSERT_TRUE_MESSAGE(b.locked, "未锁定");
        const float err = fabsf(b.bpm - bpms[i]) / bpms[i];
        TEST_ASSERT_TRUE_MESSAGE(err < 0.02f, "BPM 误差超过 2%");
    }
}

// 整数 lag 的分辨率在 43Hz 下是 4.7%（lag=21 → 122.9BPM，lag=22 → 117.3BPM），
// 远超 2% 的验收线。**抛物线插值不是优化，是达标的前提。**
void test_bpm_is_finer_than_integer_lag_resolution(void) {
    const BeatConfig bc; const OnsetConfig oc;
    // 125 BPM 落在 lag 20.67，两个整数 lag 都差 1.6% 以上
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 125.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2.5f, 125.0f, b.bpm,
        "BPM 卡在整数 lag 上 —— 没做抛物线插值");
    // 结果不该恰好落在整数 lag 对应的 BPM 上
    const float lag_int = roundf(60.0f * kOdfRateHz / b.bpm);
    const float bpm_int = 60.0f * kOdfRateHz / lag_int;
    TEST_ASSERT_TRUE_MESSAGE(fabsf(b.bpm - bpm_int) > 0.05f,
        "BPM 恰好等于整数 lag 的值 —— 插值没生效");
}

// 倍频歧义：120BPM 的自相关在 lag=21.5（120）与 lag=43（60）都有峰。
// 对数高斯加权（以 120 为中心）让真值胜出。
void test_resolves_octave_ambiguity(void) {
    const BeatConfig bc; const OnsetConfig oc;
    const float bpms[3] = {100.0f, 120.0f, 150.0f};
    for (int i = 0; i < 3; ++i) {
        BeatTracker b; OnsetDetector d;
        beatInit(b, bc); onsetInit(d, oc, 23.22f);
        feedBeats(b, bc, d, oc, bpms[i], 23.22f, 14.0f);
        // 不得报成一半或两倍
        TEST_ASSERT_TRUE_MESSAGE(fabsf(b.bpm - bpms[i] * 0.5f) > 5.0f, "报成了半速");
        TEST_ASSERT_TRUE_MESSAGE(fabsf(b.bpm - bpms[i] * 2.0f) > 5.0f, "报成了倍速");
        TEST_ASSERT_FLOAT_WITHIN(bpms[i] * 0.02f, bpms[i], b.bpm);
    }
}

// 跨档一致：同一段音乐在四个档位下 BPM 相同。ODF 固定速率就是为了这个。
void test_bpm_is_invariant_to_hop(void) {
    const BeatConfig bc; const OnsetConfig oc;
    const float dts[4] = {46.44f, 23.22f, 11.61f, 5.80f};
    for (int i = 0; i < 4; ++i) {
        BeatTracker b; OnsetDetector d;
        beatInit(b, bc); onsetInit(d, oc, dts[i]);
        feedBeats(b, bc, d, oc, 140.0f, dts[i], 14.0f);
        TEST_ASSERT_TRUE_MESSAGE(b.locked, "某个档位下没锁定");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(140.0f * 0.02f, 140.0f, b.bpm,
            "BPM 随 hop 漂了");
    }
}

// 锁定要在 6 秒内完成（§4 验收线）。
void test_locks_within_six_seconds(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 128.0f, 23.22f, 6.0f);
    TEST_ASSERT_TRUE_MESSAGE(b.locked, "6 秒内没锁定");
    TEST_ASSERT_FLOAT_WITHIN(128.0f * 0.03f, 128.0f, b.bpm);
}

// 八分音符垫底：每半拍也有一次（较弱的）击打，像 hi-hat。
//
// 这是倍频折叠**唯一真正会出事**的场景，也是我第一版完全没测到的。
// 纯节拍的 r(lag/2) 落在半拍低谷上，是负数，折叠阈值怎么调都不会触发；
// 有垫底时 r(lag/2) 变成显著的正数，阈值一松就会把 100BPM 报成 200BPM。
void test_eighth_note_subdivision_does_not_double_the_tempo(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);

    const float bpm = 100.0f, period = 60000.0f / bpm;
    float bands[NUM_BANDS], tmp[NUM_BANDS];
    for (int k = 0; k < (int)(16000.0f / 23.22f); ++k) {
        const float tf = (float)k * 23.22f;
        const uint32_t t = (uint32_t)tf;
        beatFrame(bands, fmodf(tf, period) / period, period, 0.8f);       // 正拍，强
        beatFrame(tmp, fmodf(tf + 0.5f * period, period) / period, period, 0.30f);
        for (int i = 0; i < NUM_BANDS; ++i) bands[i] += tmp[i] - 0.05f;   // 反拍，弱
        beatUpdate(b, bc, spectralFlux(d.prev, bands, 23.22f),
                   onsetUpdate(d, oc, bands, t), 23.22f, t);
    }
    TEST_ASSERT_TRUE_MESSAGE(b.locked, "带八分音符垫底的节奏没锁定");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(bpm * 0.03f, bpm, b.bpm,
        "八分音符垫底把速度翻倍了 —— 倍频折叠的阈值太松");
}

// 高直流背景下仍要锁定。ODF 恒为正，自相关**必须先去均值**，
// 否则 r 处处接近 1，峰淹没在直流里。背景越高这个效应越强。
void test_locks_despite_high_dc_background(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    const float period = 500.0f;
    float bands[NUM_BANDS];
    for (int k = 0; k < (int)(16000.0f / 23.22f); ++k) {
        const float tf = (float)k * 23.22f;
        const uint32_t t = (uint32_t)tf;
        beatFrame(bands, fmodf(tf, period) / period, period, 0.5f);
        // 每帧都在涨的宽带背景 → flux 有很大的直流分量
        for (int i = 0; i < NUM_BANDS; ++i)
            bands[i] += 0.25f * noiseAt((uint32_t)(k * NUM_BANDS + i)) + 0.20f;
        beatUpdate(b, bc, spectralFlux(d.prev, bands, 23.22f),
                   onsetUpdate(d, oc, bands, t), 23.22f, t);
    }
    TEST_ASSERT_TRUE_MESSAGE(b.locked, "高直流背景下没锁定 —— 自相关可能没去均值");
    TEST_ASSERT_FLOAT_WITHIN(120.0f * 0.04f, 120.0f, b.bpm);
}

// ── 置信度 ────────────────────────────────────────────────

// 无节奏输入不得报假 BPM（§4 验收线：喂白噪与纯人声，看置信度）。
void test_no_false_bpm_on_noise(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    float bands[NUM_BANDS];
    for (int k = 0; k < (int)(20000.0f / 23.22f); ++k) {
        for (int i = 0; i < NUM_BANDS; ++i)
            bands[i] = 0.3f * noiseAt((uint32_t)(k * NUM_BANDS + i));
        const uint32_t t = (uint32_t)(k * 23.22f);
        const float flux = spectralFlux(d.prev, bands, 23.22f);
        const bool  on   = onsetUpdate(d, oc, bands, t);
        beatUpdate(b, bc, flux, on, 23.22f, t);
    }
    TEST_ASSERT_FALSE_MESSAGE(b.locked, "白噪声上报了锁定");
    TEST_ASSERT_TRUE_MESSAGE(b.conf < bc.lock_conf, "白噪声的置信度过高");
}

void test_no_false_bpm_on_silence(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    for (int k = 0; k < 900; ++k)
        beatUpdate(b, bc, 0.0f, false, 23.22f, (uint32_t)(k * 23.22f));
    TEST_ASSERT_FALSE_MESSAGE(b.locked, "静音上报了锁定");
    TEST_ASSERT_FALSE(isnan(b.bpm));
    TEST_ASSERT_FALSE(isnan(b.conf));
}

// 干净节拍的置信度必须显著高于噪声。
void test_confidence_separates_rhythm_from_noise(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker clean; OnsetDetector dc;
    beatInit(clean, bc); onsetInit(dc, oc, 23.22f);
    feedBeats(clean, bc, dc, oc, 120.0f, 23.22f, 14.0f);

    BeatTracker noisy; OnsetDetector dn;
    beatInit(noisy, bc); onsetInit(dn, oc, 23.22f);
    float bands[NUM_BANDS];
    for (int k = 0; k < 600; ++k) {
        for (int i = 0; i < NUM_BANDS; ++i)
            bands[i] = 0.3f * noiseAt((uint32_t)(k * NUM_BANDS + i));
        const uint32_t t = (uint32_t)(k * 23.22f);
        const float flux = spectralFlux(dn.prev, bands, 23.22f);
        beatUpdate(noisy, bc, flux, onsetUpdate(dn, oc, bands, t), 23.22f, t);
    }
    TEST_ASSERT_TRUE_MESSAGE(clean.conf > noisy.conf + 0.2f,
        "节拍与噪声的置信度分不开");
}

// 节拍里混入背景噪声仍应锁定 —— 真实音乐不是纯净的脉冲串。
void test_locks_on_beats_buried_in_noise(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 16.0f, 0.8f, 0.12f);
    TEST_ASSERT_TRUE_MESSAGE(b.locked, "带噪的节拍没锁定");
    TEST_ASSERT_FLOAT_WITHIN(120.0f * 0.03f, 120.0f, b.bpm);
}

// 数据不足时不得报 BPM。自相关窗要 6 秒，前几秒的估计只是噪声。
void test_does_not_guess_before_enough_data(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 2.5f);
    TEST_ASSERT_FALSE_MESSAGE(b.locked, "2.5 秒就锁定了 —— 数据还不够");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, b.bpm, "数据不足却报了 BPM");
}

// 锁定要有滞回：置信度掉到 lock_conf 与 unlock_conf 之间时不得解锁，
// 否则节奏稍一模糊灯就失步。
void test_lock_has_hysteresis(void) {
    BeatConfig bc;
    OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);

    // 人为把置信度压到两个阈值之间，再跑一次分析
    b.conf = 0.5f * (bc.lock_conf + bc.unlock_conf);
    BeatConfig raised = bc;
    raised.lock_conf = 0.95f;              // 现在的置信度够不上重新锁定
    beatAnalyze(b, raised);
    TEST_ASSERT_TRUE_MESSAGE(b.locked,
        "置信度落在滞回带内就解锁了 —— 缺滞回");

    // 明确跌破 unlock_conf 才解锁
    BeatConfig strict = bc;
    strict.unlock_conf = 0.99f;
    beatAnalyze(b, strict);
    TEST_ASSERT_FALSE(b.locked);
}

// ── 相位与预测 ────────────────────────────────────────────

// 锁定后相位必须对齐到真实拍点。
void test_phase_aligns_to_beats(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    const uint32_t t_end = feedBeats(b, bc, d, oc, 120.0f, 23.22f, 16.0f);
    TEST_ASSERT_TRUE(b.locked);

    // 拍点在 t = 0, 500, 1000, ... ms。找一个整拍时刻，相位应接近 0
    const uint32_t on_beat = ((t_end / 500) + 2) * 500;
    const float ph = beatPhaseAhead(b, on_beat, 0.0f);
    const float dist = fminf(ph, 1.0f - ph);        // 到最近拍点的距离
    TEST_ASSERT_TRUE_MESSAGE(dist < 0.12f, "整拍时刻的相位没对齐到 0");

    // 半拍时刻，相位应接近 0.5
    const float ph_half = beatPhaseAhead(b, on_beat + 250, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.12f, 0.5f, ph_half, "半拍时刻的相位不对");
}

void test_phase_is_always_in_unit_interval(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 137.0f, 23.22f, 14.0f);
    for (uint32_t t = 14000; t < 16000; t += 7) {
        const float ph = beatPhaseAhead(b, t, 0.0f);
        TEST_ASSERT_TRUE(ph >= 0.0f && ph < 1.0f);
        TEST_ASSERT_FALSE(isnan(ph));
    }
}

// 提前量：beatPhaseAhead(ms) 把相位往前推 ms 毫秒，用来抵消流水线延迟（§3.5）。
void test_phase_ahead_advances_by_the_given_amount(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 14.0f);      // 周期 500ms
    const uint32_t t = 14000;
    const float p0   = beatPhaseAhead(b, t, 0.0f);
    const float p125 = beatPhaseAhead(b, t, 125.0f);     // 1/4 拍
    float adv = p125 - p0; if (adv < 0.0f) adv += 1.0f;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f, 0.25f, adv, "提前量不等于 1/4 拍");
}

// 未锁定时相位无意义，但不得返回 NaN。
void test_phase_is_safe_before_lock(void) {
    const BeatConfig bc;
    BeatTracker b;
    beatInit(b, bc);
    const float ph = beatPhaseAhead(b, 1234, 50.0f);
    TEST_ASSERT_FALSE(isnan(ph));
    TEST_ASSERT_TRUE(ph >= 0.0f && ph < 1.0f);
}

// next_beat 必须始终在未来。
//
// 它是「下一拍的预测时刻」，落到过去就不再是预测。虽然 beatPhaseAhead 用 fmod
// 兜着、短期内相位仍然对，但差值会一直增长，最终把 int32 转换撑爆。
void test_next_beat_stays_in_the_future(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    const float period = 500.0f;
    float bands[NUM_BANDS];
    for (int k = 0; k < (int)(20000.0f / 23.22f); ++k) {
        const float tf = (float)k * 23.22f;
        const uint32_t t = (uint32_t)tf;
        beatFrame(bands, fmodf(tf, period) / period, period, 0.8f);
        beatUpdate(b, bc, spectralFlux(d.prev, bands, 23.22f),
                   onsetUpdate(d, oc, bands, t), 23.22f, t);
        if (b.has_phase)
            TEST_ASSERT_TRUE_MESSAGE((int32_t)(b.next_beat - t) >= 0,
                "next_beat 落到了过去");
    }
}

// PLL 的误差必须折算到 [-T/2, T/2]：落在预测拍点**稍前**的 onset 说明
// 拍点该往前挪一点，不是往后拖将近一整拍。不折算的话修正方向是反的。
void test_pll_error_wraps_to_nearest_beat(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);

    // 把预测拍点放到 now 之后 300ms（周期 500ms）。
    // 原始 err = −300，折算后是 +200 —— 这个 onset 其实属于**上一拍**，
    // 意味着预测点该往后推，不是往前拉。
    const uint32_t now = 14000;
    b.next_beat = now + 300;
    const uint32_t before = b.next_beat;
    beatUpdate(b, bc, 50.0f, true, 23.22f, now);

    const int32_t delta = (int32_t)(b.next_beat - before);
    TEST_ASSERT_TRUE_MESSAGE(delta > 0,
        "PLL 把预测点往前拉了 —— err 没折算到 [-T/2, T/2]，修正方向反了");
}

// ── 换档 ──────────────────────────────────────────────────

// §3.3.4 第 2 条：换档后**保留 BPM 与相位**，只重置自适应阈值。
// 全量重新锁定要 6 秒，用户会看到灯「发呆」。
void test_retime_preserves_bpm_and_phase(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);
    const float bpm = b.bpm;
    const float ph  = beatPhaseAhead(b, 14000, 0.0f);

    beatRetime(b, bc);
    TEST_ASSERT_TRUE_MESSAGE(b.locked, "换档丢了锁定");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, bpm, b.bpm, "换档丢了 BPM");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, ph, beatPhaseAhead(b, 14000, 0.0f),
        "换档丢了相位");
    TEST_ASSERT_TRUE_MESSAGE(b.odf_count > 0, "换档清了 ODF 历史 —— 会白等 6 秒");
}

// ── 与原厂的对照实验 ──────────────────────────────────────

// 原厂在 174 BPM 的节拍器上跑出 490ms 周期（见 docs/10），而实际周期是 345ms。
// 490/345 = 1.42 —— 既不是半速也不是倍速，是彻底锁错了。
// 我们必须锁对。这是整个音频子系统最直接的对照实验。
void test_beats_the_stock_firmware_at_174bpm(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 174.0f, 23.22f, 14.0f);

    TEST_ASSERT_TRUE_MESSAGE(b.locked, "174BPM 没锁定");
    const float period_ms = 60000.0f / b.bpm;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(345.0f * 0.02f, 345.0f, period_ms,
        "周期偏离 345ms —— 没有胜过原厂");
    TEST_ASSERT_TRUE_MESSAGE(fabsf(period_ms - 490.0f) > 100.0f,
        "复现了原厂那个 490ms 的错误");
}

// ── 健壮性 ────────────────────────────────────────────────

// 非有限的 flux 不得污染 ODF。上游 ADC 掉线时会送 NaN。
void test_non_finite_flux_does_not_poison_odf(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);

    for (int k = 0; k < 50; ++k)
        beatUpdate(b, bc, (k % 2) ? NAN : INFINITY, false, 23.22f,
                   (uint32_t)(14000 + k * 23));
    for (int i = 0; i < kOdfLen; ++i) {
        TEST_ASSERT_FALSE_MESSAGE(isnan(b.odf[i]), "NaN 渗进了 ODF");
        TEST_ASSERT_FALSE_MESSAGE(isinf(b.odf[i]), "inf 渗进了 ODF");
    }
    TEST_ASSERT_FALSE(isnan(b.bpm));
    TEST_ASSERT_FALSE(isnan(b.conf));
}

// Init 必须清 ODF。测试里都用新声明的 BeatTracker，成员有默认初始化，
// 「不清」和「清了」看不出区别 —— 必须显式喂一个脏对象。
void test_init_clears_a_dirty_tracker(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 174.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);                       // 确认真的脏了（正对照）
    float sum = 0.0f;
    for (int i = 0; i < kOdfLen; ++i) sum += b.odf[i];
    TEST_ASSERT_TRUE(sum > 1.0f);

    beatInit(b, bc);
    TEST_ASSERT_FALSE(b.locked);
    TEST_ASSERT_EQUAL_UINT16(0, b.odf_count);
    float sum2 = 0.0f;
    for (int i = 0; i < kOdfLen; ++i) sum2 += b.odf[i];
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, sum2, "Init 没清 ODF");

    // 清干净后不得立刻锁定到旧的 BPM
    OnsetDetector d2; onsetInit(d2, oc, 23.22f);
    feedBeats(b, bc, d2, oc, 90.0f, 23.22f, 14.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(90.0f * 0.03f, 90.0f, b.bpm,
        "Init 后仍受旧 ODF 影响");
}

// ── 时钟 ──────────────────────────────────────────────────

void test_survives_wraparound_and_long_gaps(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);

    const uint32_t t0 = (uint32_t)(0u - 4000u);
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)(t0 + 14000) < t0, "这个 t0 没有真的回绕");

    const float period_ms = 500.0f;
    float bands[NUM_BANDS];
    for (int k = 0; k < (int)(14000.0f / 23.22f); ++k) {
        const float tf = (float)k * 23.22f;
        const uint32_t t = (uint32_t)(t0 + (uint32_t)tf);
        beatFrame(bands, fmodf(tf, period_ms) / period_ms, period_ms, 0.8f);
        const float flux = spectralFlux(d.prev, bands, 23.22f);
        beatUpdate(b, bc, flux, onsetUpdate(d, oc, bands, t), 23.22f, t);
    }
    TEST_ASSERT_TRUE_MESSAGE(b.locked, "跨回绕点后没锁定");
    TEST_ASSERT_FLOAT_WITHIN(120.0f * 0.03f, 120.0f, b.bpm);
    const float ph = beatPhaseAhead(b, (uint32_t)(t0 + 14000), 0.0f);
    TEST_ASSERT_TRUE(ph >= 0.0f && ph < 1.0f);
}

// 间隔超过 2^31 ms（24.8 天）时分析仍要触发。有符号比较会让它永久停摆 ——
// 灯开着 25 天后就再也不更新 BPM 了。见 docs/17 第 18 条。
void test_analyze_survives_intervals_beyond_signed_range(void) {
    const BeatConfig bc; const OnsetConfig oc;
    BeatTracker b; OnsetDetector d;
    beatInit(b, bc); onsetInit(d, oc, 23.22f);
    feedBeats(b, bc, d, oc, 120.0f, 23.22f, 14.0f);
    TEST_ASSERT_TRUE(b.locked);
    const uint32_t last = b.last_analyze;

    // 27.9 天之后再喂一帧：分析必须重新触发
    const uint32_t huge = 0x90000000u;
    beatUpdate(b, bc, 1.0f, false, 23.22f, (uint32_t)(last + huge));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(last + huge), b.last_analyze,
        "间隔 27.9 天后分析没触发 —— 用了有符号比较");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_odf_rate_is_fixed_regardless_of_input_rate);
    RUN_TEST(test_odf_sample_count_is_invariant_to_hop);
    RUN_TEST(test_slow_hop_upsamples_without_inserting_zeros);
    RUN_TEST(test_bpm_accuracy_across_tempos);
    RUN_TEST(test_bpm_is_finer_than_integer_lag_resolution);
    RUN_TEST(test_resolves_octave_ambiguity);
    RUN_TEST(test_bpm_is_invariant_to_hop);
    RUN_TEST(test_locks_within_six_seconds);
    RUN_TEST(test_eighth_note_subdivision_does_not_double_the_tempo);
    RUN_TEST(test_locks_despite_high_dc_background);
    RUN_TEST(test_no_false_bpm_on_noise);
    RUN_TEST(test_no_false_bpm_on_silence);
    RUN_TEST(test_confidence_separates_rhythm_from_noise);
    RUN_TEST(test_locks_on_beats_buried_in_noise);
    RUN_TEST(test_does_not_guess_before_enough_data);
    RUN_TEST(test_lock_has_hysteresis);
    RUN_TEST(test_phase_aligns_to_beats);
    RUN_TEST(test_phase_is_always_in_unit_interval);
    RUN_TEST(test_phase_ahead_advances_by_the_given_amount);
    RUN_TEST(test_phase_is_safe_before_lock);
    RUN_TEST(test_next_beat_stays_in_the_future);
    RUN_TEST(test_pll_error_wraps_to_nearest_beat);
    RUN_TEST(test_retime_preserves_bpm_and_phase);
    RUN_TEST(test_beats_the_stock_firmware_at_174bpm);
    RUN_TEST(test_non_finite_flux_does_not_poison_odf);
    RUN_TEST(test_init_clears_a_dirty_tracker);
    RUN_TEST(test_survives_wraparound_and_long_gaps);
    RUN_TEST(test_analyze_survives_intervals_beyond_signed_range);
    return UNITY_END();
}
