#include <unity.h>
#include <math.h>
#include <stdio.h>
#include "lamp_pitch.h"
#include "lamp_fft.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static float x[2048], re[2048], im[2048], mag[1025], win[2048];
static PitchConfig C;

// 合成一个带谐波的音，走**真实的加窗 FFT**。
//
// 不喂手工构造的谱：那样测不到窗主瓣宽度、频率泄漏、亚 bin 插值这些
// 真正决定精度的东西 —— 这正是频段那轮踩过的坑（伪随机噪声太干净）。
//
// amp[h] 是第 h+1 次谐波的幅度，0 表示这个谐波缺席。
static void synth(float f0, const float *amp, int nh, size_t n) {
    for (size_t t = 0; t < n; ++t) x[t] = 0.0f;
    for (int h = 1; h <= nh; ++h) {
        if (!(amp[h - 1] > 0.0f)) continue;
        const float f = f0 * (float)h;
        if (f >= kSampleRate * 0.5f) break;
        for (size_t t = 0; t < n; ++t)
            x[t] += amp[h - 1] * sinf(6.283185307f * f * (float)t / kSampleRate);
    }
    fillWindow(WIN_HANN, win, n);
    magnitudeSpectrum(x, win, n, re, im, mag);
}

// 典型乐音：谐波按 1/h 衰减。
static void synthTone(float f0, size_t n, int nh = 6) {
    float a[8];
    for (int h = 1; h <= nh && h <= 8; ++h) a[h - 1] = 1.0f / (float)h;
    synth(f0, a, (nh <= 8 ? nh : 8), n);
}

static void noise(size_t n) {
    uint32_t s = 12345;
    for (size_t t = 0; t < n; ++t) {
        s = s * 1664525u + 1013904223u; s ^= s >> 16;
        x[t] = ((float)(s & 0xFFFF) / 32768.0f) - 1.0f;
    }
    fillWindow(WIN_HANN, win, n);
    magnitudeSpectrum(x, win, n, re, im, mag);
}

static void reset(void) { C = PitchConfig{}; }

// 相对误差，百分号
static float errPct(float got, float want) { return 100.0f * fabsf(got - want) / want; }

// ── 基本估计 ──────────────────────────────────────────────

void test_pure_sine_is_found(void) {
    reset();
    float a[1] = {1.0f};
    synth(220.0f, a, 1, 1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE_MESSAGE(e.voiced, "220Hz 纯音应当判为有音高");
    TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, 220.0f) < 1.0f, "220Hz 纯音误差应小于 1%");
}

void test_harmonic_tone_reports_the_fundamental_not_a_harmonic(void) {
    reset();
    synthTone(147.0f, 1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE(e.voiced);
    TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, 147.0f) < 1.5f,
        "带谐波的乐音应当报基频 147，不是 294 或 441");
}

void test_weak_fundamental_still_reports_the_fundamental(void) {
    // 基音只有二次谐波的 1/6。取最高分的话 2F 会赢 —— 这正是
    // 「分数够高的最低候选」那条规则存在的理由。
    reset();
    float a[6] = {0.16f, 1.0f, 0.7f, 0.5f, 0.35f, 0.25f};
    synth(165.0f, a, 6, 1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE(e.voiced);
    TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, 165.0f) < 2.0f,
        "基音弱也要报 165，报成 330 就是倍频错误");
}

void test_sub_bin_accuracy_on_an_off_grid_frequency(void) {
    // 237.3Hz 在 n=1024（df=21.53Hz）下落在 bin 11.02 —— 故意不在格点上。
    // 不做抛物线插值的话误差会有半个 bin ≈ 4.5%。
    reset();
    synthTone(237.3f, 1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE(e.voiced);
    TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, 237.3f) < 1.5f,
        "非格点频率靠亚 bin 插值也要压进 1.5%");
}

void test_works_at_every_preset_fft_length(void) {
    // 512 / 1024 / 2048 三档都要能用。512 下 df=43Hz，
    // 一个 bin 就跨了一个半音多 —— 插值不生效的话这条必红。
    reset();
    const size_t ns[3] = {512, 1024, 2048};
    for (int i = 0; i < 3; ++i) {
        synthTone(196.0f, ns[i]);
        PitchEstimate e = estimateF0(mag, ns[i], C);
        char msg[64]; snprintf(msg, sizeof msg, "n=%d 下 196Hz 没测准", (int)ns[i]);
        TEST_ASSERT_TRUE_MESSAGE(e.voiced, msg);
        TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, 196.0f) < 3.0f, msg);
    }
}

void test_a_melodic_scale_is_tracked_monotonically(void) {
    // 单调上行的音阶必须测出单调上行的 f0。单点测准不代表映射没有翻转，
    // 这条是冲着「某个区间算错」去的。
    reset();
    const float semis[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    float prev = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const float f = 196.0f * powf(2.0f, semis[i] / 12.0f);
        synthTone(f, 1024);
        PitchEstimate e = estimateF0(mag, 1024, C);
        TEST_ASSERT_TRUE(e.voiced);
        TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, f) < 3.0f, "音阶某一级测偏了");
        TEST_ASSERT_TRUE_MESSAGE(e.hz > prev, "上行音阶的 f0 必须单调上行");
        prev = e.hz;
    }
}

void test_pure_low_sine_is_not_swallowed_by_its_own_skirt(void) {
    // 回归：110Hz 纯音落在 bin 5.11，候选 k=3 的第二谐波（bin 6）蹭到主瓣，
    // 分数比真峰还高，而 bin 3 只是 3% 的裙边。
    //
    // 第一版靠「分数是局部极大」挡这类，结果连真峰一起挡掉了（真峰的分数
    // 确实低于裙边）。正解是要求候选 bin 是**原始谱**的局部极大 ——
    // 裙边单调下降，天生不是峰。
    reset();
    const float f[3] = {87.3f, 110.0f, 130.8f};
    for (int i = 0; i < 3; ++i) {
        float a[1] = {1.0f};
        synth(f[i], a, 1, 1024);
        PitchEstimate e = estimateF0(mag, 1024, C);
        char msg[64]; snprintf(msg, sizeof msg, "%.0fHz 纯音被裙边吃掉了", f[i]);
        TEST_ASSERT_TRUE_MESSAGE(e.voiced, msg);
        TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, f[i]) < 1.5f, msg);
    }
}

void test_a_leakage_skirt_is_never_picked_as_the_fundamental(void) {
    // 直接盯住判据本身：把真峰左边那个裙边 bin 拿出来看，它必须不是局部极大。
    reset();
    synthTone(220.0f, 1024);
    const size_t kpk = (size_t)(220.0f / (kSampleRate / 1024.0f) + 0.5f);
    TEST_ASSERT_TRUE_MESSAGE(mag[kpk - 1] < mag[kpk],
        "裙边必须低于真峰，否则这条测试的前提就不成立");
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE_MESSAGE(e.hz > 220.0f * 0.97f,
        "报出来的频率不能落在真峰左边的裙边上");
}

void test_below_the_resolution_limit_it_says_nothing_rather_than_guessing(void) {
    // n=512（df=43Hz）下 87Hz 在 bin 2.02，抛物线插值的左邻居贴着 DC 泄漏。
    // 实测会偏到 96Hz（10%）—— 与其报一个错的，不如不报。
    //
    // 同一个音在 n=1024 下必须能测准，否则这条就变成「什么都测不出」也能过。
    reset();
    synthTone(87.3f, 512);
    TEST_ASSERT_FALSE_MESSAGE(estimateF0(mag, 512, C).voiced,
        "分辨率不够时应当报无音高，而不是给个偏 10% 的值");
    synthTone(87.3f, 1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE_MESSAGE(e.voiced && errPct(e.hz, 87.3f) < 1.5f,
        "正对照：同一个音在 n=1024 下必须测得准");
}

void test_confidence_separates_melody_from_drums(void) {
    // 置信度用的是谐波能量占比。实测区间：乐音 0.88–0.95、多声部里的旋律
    // 约 0.55、鼓点 0.28–0.40、噪声 0.02–0.05。阈值 0.50 卡在旋律与鼓点之间。
    reset();
    // 鼓：宽带噪声脉冲 + 60Hz 低频
    uint32_t sd = 11;
    for (size_t t = 0; t < 1024; ++t) {
        sd = sd * 1664525u + 1013904223u; sd ^= sd >> 16;
        const float env = expf(-(float)t / 300.0f);
        x[t] = env * (((float)(sd & 0xFFFF) / 32768.0f) - 1.0f)
             + 0.6f * env * sinf(6.283185307f * 60.0f * (float)t / kSampleRate);
    }
    fillWindow(WIN_HANN, win, 1024);
    magnitudeSpectrum(x, win, 1024, re, im, mag);
    const float c_drum = estimateF0(mag, 1024, C).conf;

    // 多声部：旋律 + 贝斯 + 底鼓噪声
    sd = 77;
    for (size_t t = 0; t < 1024; ++t) {
        sd = sd * 1664525u + 1013904223u; sd ^= sd >> 16;
        float v = 0.0f;
        for (int h = 1; h <= 5; ++h)
            v += 0.8f * sinf(6.283185307f * 330.0f * h * (float)t / kSampleRate) / h;
        v += 0.7f * sinf(6.283185307f * 82.4f * (float)t / kSampleRate);
        v += 0.35f * (((float)(sd & 0xFFFF) / 32768.0f) - 1.0f);
        x[t] = v;
    }
    fillWindow(WIN_HANN, win, 1024);
    magnitudeSpectrum(x, win, 1024, re, im, mag);
    const PitchEstimate e = estimateF0(mag, 1024, C);

    TEST_ASSERT_TRUE_MESSAGE(c_drum < C.conf_min, "鼓点不该被判成有旋律");
    TEST_ASSERT_TRUE_MESSAGE(e.voiced, "多声部里的旋律必须被判为有音高");
    TEST_ASSERT_TRUE_MESSAGE(errPct(e.hz, 330.0f) < 3.0f,
        "有贝斯和鼓垫着也要报旋律的 330，不能报贝斯的 82");
    TEST_ASSERT_TRUE_MESSAGE(e.conf > c_drum + 0.1f, "两者的置信度必须拉开");
}

// ── 无音高的判定 ──────────────────────────────────────────

void test_white_noise_is_unvoiced(void) {
    reset();
    noise(1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_FALSE_MESSAGE(e.voiced, "白噪声不该报出音高");
    TEST_ASSERT_TRUE_MESSAGE(e.conf < C.conf_min, "白噪声的置信度必须低");
}

void test_noise_scores_lower_than_a_tone(void) {
    // 正对照：上一条只说噪声低，这条说乐音确实高得多 —— 否则阈值定在
    // 「什么都判 unvoiced」的位置也能让上一条过。
    reset();
    noise(1024);
    const float c_noise = estimateF0(mag, 1024, C).conf;
    synthTone(220.0f, 1024);
    const float c_tone = estimateF0(mag, 1024, C).conf;
    TEST_ASSERT_TRUE_MESSAGE(c_tone > c_noise + 0.3f, "乐音的置信度必须明显高于噪声");
}

void test_silence_is_unvoiced(void) {
    reset();
    for (size_t i = 0; i < 1025; ++i) mag[i] = 0.0f;
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_FALSE(e.voiced);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, e.hz);
}

void test_out_of_range_tone_is_rejected(void) {
    reset();
    synthTone(45.0f, 2048);          // 低于 lo_hz=80
    PitchEstimate e = estimateF0(mag, 2048, C);
    TEST_ASSERT_FALSE_MESSAGE(e.voiced && e.hz < 80.0f,
        "低于搜索下限的音不能被报出来");
}

void test_degenerate_input_does_not_crash(void) {
    reset();
    TEST_ASSERT_FALSE(estimateF0(nullptr, 1024, C).voiced);
    TEST_ASSERT_FALSE(estimateF0(mag, 8, C).voiced);
    for (size_t i = 0; i < 1025; ++i) mag[i] = NAN;
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(e.hz) && isfinite(e.conf), "NaN 谱不能漏出 NaN");
}

// ── 已知限制 ──────────────────────────────────────────────

void test_missing_fundamental_is_a_documented_limitation(void) {
    // HPS 对缺失基音无能为力：F 处真的没有能量，乘出来就是地板。
    // 这条测试不是「验证它能工作」，而是**把已知的失败钉住** ——
    // 哪天换成自相关/YIN 修好了，这条会红，提醒同步改文档。
    reset();
    float a[6] = {0.0f, 1.0f, 0.7f, 0.5f, 0.35f, 0.25f};   // 只有 2F..6F
    synth(150.0f, a, 6, 1024);
    PitchEstimate e = estimateF0(mag, 1024, C);
    TEST_ASSERT_TRUE_MESSAGE(!e.voiced || errPct(e.hz, 150.0f) > 10.0f,
        "缺失基音目前测不准 —— 如果这条红了，说明已经修好，请更新头注释");
}

// ── 跟踪 ──────────────────────────────────────────────────

static PitchEstimate voiced(float hz) {
    PitchEstimate e; e.hz = hz; e.conf = 0.8f; e.voiced = true; return e;
}
static PitchEstimate unvoiced(void) { return PitchEstimate{}; }

void test_tracker_glides_through_a_small_interval(void) {
    reset();
    PitchTracker T; pitchInit(T, C, 23.22f);
    pitchUpdate(T, C, voiced(220.0f), 23.22f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 220.0f, T.hz, "第一帧应当直接落位");

    pitchUpdate(T, C, voiced(233.08f), 23.22f);   // 上行一个半音
    TEST_ASSERT_TRUE_MESSAGE(T.hz > 220.0f && T.hz < 233.0f,
        "小音程应当滑过去，不是一步到位");
}

void test_tracker_snaps_on_a_leap(void) {
    reset();
    PitchTracker T; pitchInit(T, C, 23.22f);
    pitchUpdate(T, C, voiced(220.0f), 23.22f);
    pitchUpdate(T, C, voiced(330.0f), 23.22f);    // 上行纯五度 = 7 个半音
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 330.0f, T.hz,
        "跳进必须直接落位，滑过去就成了滑音");
}

void test_glide_is_measured_in_semitones_not_hz(void) {
    // 同样是「上行一个半音」，在 110Hz 和 880Hz 上滑音应当走过同样的比例。
    // 在 Hz 域平滑的话，高八度的绝对差是低八度的 8 倍，收敛快慢完全不同。
    reset();
    float frac[2];
    const float base[2] = {110.0f, 880.0f};
    for (int r = 0; r < 2; ++r) {
        PitchTracker T; pitchInit(T, C, 23.22f);
        const float from = base[r], to = base[r] * powf(2.0f, 1.0f / 12.0f);
        pitchUpdate(T, C, voiced(from), 23.22f);
        for (int k = 0; k < 3; ++k) pitchUpdate(T, C, voiced(to), 23.22f);
        // 走过的比例，用半音度量
        frac[r] = hzToSemi(T.hz, from) / hzToSemi(to, from);
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, frac[0], frac[1],
        "两个八度上的同一音程，滑音进度应当一致");
    TEST_ASSERT_TRUE_MESSAGE(frac[0] > 0.3f && frac[0] < 0.95f,
        "必须停在滑行中段，跑到收敛这条测试就什么也证明不了");
}

void test_glide_is_defined_in_physical_time(void) {
    // 与色度、氛围那几条同一条规矩。
    reset();
    const float dt[2]  = {5.805f, 46.44f};
    const int   stp[2] = {8, 1};                 // 同为 46.44ms
    float got[2];
    for (int r = 0; r < 2; ++r) {
        PitchTracker T; pitchInit(T, C, dt[r]);
        pitchUpdate(T, C, voiced(220.0f), dt[r]);
        for (int k = 0; k < stp[r]; ++k) pitchUpdate(T, C, voiced(233.08f), dt[r]);
        got[r] = T.hz;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, dt[0]*stp[0], dt[1]*stp[1],
        "两条路径的物理时长必须相等，否则这条测试没意义");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, got[0], got[1],
        "同样的物理时长，快档与慢档的滑音进度应当一致");
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 221.0f && got[0] < 232.0f, "必须停在滑行中段");
}

void test_tracker_holds_through_a_short_gap(void) {
    // 换气、辅音、弱拍都会让基频短暂消失。一消失就熄灯，旋律线会闪成虚线。
    reset();
    PitchTracker T; pitchInit(T, C, 23.22f);
    for (int k = 0; k < 10; ++k) pitchUpdate(T, C, voiced(220.0f), 23.22f);
    for (int k = 0; k < 4; ++k) pitchUpdate(T, C, unvoiced(), 23.22f);   // 93ms
    TEST_ASSERT_TRUE_MESSAGE(T.voiced, "93ms 的空档应当被 hold_ms=250 兜住");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 220.0f, T.hz, "保持期内音高不该动");
}

void test_tracker_gives_up_after_a_long_gap(void) {
    reset();
    PitchTracker T; pitchInit(T, C, 23.22f);
    for (int k = 0; k < 10; ++k) pitchUpdate(T, C, voiced(220.0f), 23.22f);
    for (int k = 0; k < 20; ++k) pitchUpdate(T, C, unvoiced(), 23.22f);  // 464ms
    TEST_ASSERT_FALSE_MESSAGE(T.voiced, "超过 hold_ms 就该判为无音高");
}

void test_hold_is_measured_in_physical_time(void) {
    // 250ms 的保持在打点档是 43 帧、在氛围档是 5 帧。按帧数写死的话
    // 两档的实际保持时长会差 8 倍。
    reset();
    const float dt[2] = {5.805f, 46.44f};
    for (int r = 0; r < 2; ++r) {
        PitchTracker T; pitchInit(T, C, dt[r]);
        pitchUpdate(T, C, voiced(220.0f), dt[r]);
        float acc = 0.0f;
        while (T.voiced && acc < 2000.0f) { pitchUpdate(T, C, unvoiced(), dt[r]); acc += dt[r]; }
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2.0f * dt[r], C.hold_ms, acc,
            "放弃音高的时刻应当由物理时长决定，与帧率无关");
    }
}

// ── 组件 ──────────────────────────────────────────────────

void test_semitone_conversion_round_trips(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.0f, hzToSemi(440.0f, 220.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -12.0f, hzToSemi(110.0f, 220.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 330.0f, semiToHz(hzToSemi(330.0f, 80.0f), 80.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, hzToSemi(0.0f, 220.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, hzToSemi(-5.0f, 220.0f));
}

void test_parabolic_interpolation_finds_the_true_peak(void) {
    // 手工造一个对数域的对称抛物线，顶点故意放在 k=10.3
    float m[21];
    for (int i = 0; i < 21; ++i) {
        const float d = (float)i - 10.3f;
        m[i] = expf(-d * d);
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, 10.3f, parabolicBin(m, 21, 10),
        "对数域抛物线的顶点应当被精确还原");
}

void test_parabolic_interpolation_is_bounded(void) {
    // 平顶或病态输入时不能把峰甩到隔壁 bin 之外
    float m[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, parabolicBin(m, 5, 2));
    float g[5] = {1.0f, 100.0f, 1.0f, 100.0f, 1.0f};
    const float b = parabolicBin(g, 5, 2);
    TEST_ASSERT_TRUE_MESSAGE(b >= 1.5f && b <= 2.5f, "插值结果不能超出 ±0.5 bin");
}

void test_hps_score_is_level_invariant(void) {
    // 分数是对数域的**平均值**，整体放大 100 倍只会给每个候选加同一个常数，
    // 候选之间的差、也就是所有判据，完全不变。
    reset();
    synthTone(220.0f, 1024);
    float m2[1025];
    for (size_t i = 0; i < 1025; ++i) m2[i] = mag[i] * 100.0f;
    const float a = hpsScore(mag, 513, 10, 4, 1e-9f);
    const float b = hpsScore(m2,  513, 10, 4, 1e-7f);
    const float a2 = hpsScore(mag, 513, 20, 4, 1e-9f);
    const float b2 = hpsScore(m2,  513, 20, 4, 1e-7f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, a - a2, b - b2,
        "候选之间的分数差必须与输入电平无关");
}

void test_estimate_is_level_invariant(void) {
    reset();
    synthTone(261.6f, 1024);
    const PitchEstimate e1 = estimateF0(mag, 1024, C);
    for (size_t i = 0; i < 1025; ++i) mag[i] *= 500.0f;
    const PitchEstimate e2 = estimateF0(mag, 1024, C);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, e1.hz, e2.hz, "音高与电平无关");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, e1.conf, e2.conf, "置信度也与电平无关");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_pure_sine_is_found);
    RUN_TEST(test_harmonic_tone_reports_the_fundamental_not_a_harmonic);
    RUN_TEST(test_weak_fundamental_still_reports_the_fundamental);
    RUN_TEST(test_sub_bin_accuracy_on_an_off_grid_frequency);
    RUN_TEST(test_works_at_every_preset_fft_length);
    RUN_TEST(test_a_melodic_scale_is_tracked_monotonically);
    RUN_TEST(test_pure_low_sine_is_not_swallowed_by_its_own_skirt);
    RUN_TEST(test_a_leakage_skirt_is_never_picked_as_the_fundamental);
    RUN_TEST(test_below_the_resolution_limit_it_says_nothing_rather_than_guessing);
    RUN_TEST(test_confidence_separates_melody_from_drums);
    RUN_TEST(test_white_noise_is_unvoiced);
    RUN_TEST(test_noise_scores_lower_than_a_tone);
    RUN_TEST(test_silence_is_unvoiced);
    RUN_TEST(test_out_of_range_tone_is_rejected);
    RUN_TEST(test_degenerate_input_does_not_crash);
    RUN_TEST(test_missing_fundamental_is_a_documented_limitation);
    RUN_TEST(test_tracker_glides_through_a_small_interval);
    RUN_TEST(test_tracker_snaps_on_a_leap);
    RUN_TEST(test_glide_is_measured_in_semitones_not_hz);
    RUN_TEST(test_glide_is_defined_in_physical_time);
    RUN_TEST(test_tracker_holds_through_a_short_gap);
    RUN_TEST(test_tracker_gives_up_after_a_long_gap);
    RUN_TEST(test_hold_is_measured_in_physical_time);
    RUN_TEST(test_semitone_conversion_round_trips);
    RUN_TEST(test_parabolic_interpolation_finds_the_true_peak);
    RUN_TEST(test_parabolic_interpolation_is_bounded);
    RUN_TEST(test_hps_score_is_level_invariant);
    RUN_TEST(test_estimate_is_level_invariant);
    return UNITY_END();
}
