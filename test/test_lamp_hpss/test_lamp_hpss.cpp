#include <unity.h>
#include <math.h>
#include "lamp_hpss.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static HpssState  S;
static HpssConfig C;
static float H[NUM_BANDS], P[NUM_BANDS];
static const float DT = 23.22f;      // 通用档 hop

static void reset(float dt = DT) { C = HpssConfig{}; hpssInit(S, C, dt); }

// 持续音：只点亮 band[b]，帧帧如此
static void tone(float *v, int b, float amp) {
    for (int i = 0; i < NUM_BANDS; ++i) v[i] = 0.0f;
    v[b] = amp;
}
// 击打：一瞬间铺满整个频段
static void hit(float *v, float amp) {
    for (int i = 0; i < NUM_BANDS; ++i) v[i] = amp;
}

static float sum(const float *v) {
    float a = 0.0f; for (int i = 0; i < NUM_BANDS; ++i) a += v[i]; return a;
}

// ── 分离本身 ──────────────────────────────────────────────

void test_a_sustained_tone_goes_to_the_harmonic_side(void) {
    reset();
    float b[NUM_BANDS]; tone(b, 6, 1.0f);
    for (int k = 0; k < 30; ++k) hpssProcess(S, C, b, H, P);
    TEST_ASSERT_TRUE_MESSAGE(sum(H) > sum(P) * 4.0f,
        "帧帧不变的单频音应当几乎全归谐波路");
}

void test_a_broadband_hit_goes_to_the_percussive_side(void) {
    reset();
    float quiet[NUM_BANDS], bang[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) quiet[i] = 0.001f;
    hit(bang, 1.0f);
    for (int k = 0; k < 20; ++k) hpssProcess(S, C, quiet, H, P);
    hpssProcess(S, C, bang, H, P);           // 一瞬间的宽带击打
    TEST_ASSERT_TRUE_MESSAGE(sum(P) > sum(H) * 4.0f,
        "单帧的宽带击打应当几乎全归打击路");
}

void test_a_mixture_is_split_by_where_the_energy_sits(void) {
    // 持续音在 band 6，同时来一记宽带击打。两路应当各拿各的。
    reset();
    float mix[NUM_BANDS];
    float steady[NUM_BANDS]; tone(steady, 6, 1.0f);
    for (int k = 0; k < 30; ++k) hpssProcess(S, C, steady, H, P);

    for (int i = 0; i < NUM_BANDS; ++i) mix[i] = 0.6f;   // 击打
    mix[6] = 1.0f;                                        // 持续音还在
    hpssProcess(S, C, mix, H, P);

    TEST_ASSERT_TRUE_MESSAGE(H[6] > P[6],
        "持续音那一段仍应主要归谐波路");
    TEST_ASSERT_TRUE_MESSAGE(P[1] > H[1] && P[13] > H[13],
        "只有击打能量的那些段应当归打击路");
}

void test_separation_conserves_energy(void) {
    // 软掩膜是把原能量**分配**给两路，不是凭空造能量。两路之和不能超过原值。
    reset();
    float b[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) b[i] = 0.1f + 0.05f * (float)i;
    for (int k = 0; k < 20; ++k) hpssProcess(S, C, b, H, P);
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, b[i], H[i] + P[i],
            "两路之和应当等于原能量");
}

void test_silence_produces_no_nan(void) {
    reset();
    float z[NUM_BANDS]; for (int i = 0; i < NUM_BANDS; ++i) z[i] = 0.0f;
    for (int k = 0; k < 10; ++k) hpssProcess(S, C, z, H, P);
    for (int i = 0; i < NUM_BANDS; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(isfinite(H[i]) && isfinite(P[i]), "静音不能除出 NaN");
        TEST_ASSERT_FLOAT_WITHIN(1e-9f, 0.0f, H[i] + P[i]);
    }
}

void test_nan_and_negative_input_are_dropped(void) {
    reset();
    float bad[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) bad[i] = (i % 2) ? NAN : -3.0f;
    for (int k = 0; k < 10; ++k) hpssProcess(S, C, bad, H, P);
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_TRUE_MESSAGE(isfinite(H[i]) && isfinite(P[i]),
            "NaN 一旦进了历史缓冲就再也出不来了");
}

// ── 打击占比 ──────────────────────────────────────────────

void test_percussive_ratio_separates_a_drum_loop_from_a_pad(void) {
    // 这个量是自动选灯效的主判据，区分度必须够宽。
    reset();
    float pad[NUM_BANDS]; tone(pad, 5, 1.0f);
    for (int k = 0; k < 40; ++k) hpssProcess(S, C, pad, H, P);
    const float r_pad = percussiveRatio(H, P);

    reset();
    float quiet[NUM_BANDS], bang[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) quiet[i] = 0.02f;
    hit(bang, 1.0f);
    float r_drum = 0.0f;
    for (int k = 0; k < 40; ++k) {        // 每 8 帧一记鼓
        hpssProcess(S, C, (k % 8 == 0) ? bang : quiet, H, P);
        if (k % 8 == 0) r_drum = percussiveRatio(H, P);
    }
    TEST_ASSERT_TRUE_MESSAGE(r_pad < 0.25f, "长音垫的打击占比应当很低");
    TEST_ASSERT_TRUE_MESSAGE(r_drum > 0.70f, "鼓点帧的打击占比应当很高");
}

void test_percussive_ratio_of_silence_is_zero(void) {
    float z[NUM_BANDS]; for (int i = 0; i < NUM_BANDS; ++i) z[i] = 0.0f;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, percussiveRatio(z, z),
        "全零不能除出 NaN");
}

// ── 窗长 ──────────────────────────────────────────────────

void test_window_is_odd_and_scaled_by_physical_time(void) {
    C = HpssConfig{};
    // 200ms / 各档 hop
    TEST_ASSERT_EQUAL_INT_MESSAGE(17, hpssWindow(C, 5.805f),  "打点档：200/5.8≈34，截到上限 17");
    TEST_ASSERT_EQUAL_INT_MESSAGE(17, hpssWindow(C, 11.61f),  "电子档：200/11.6≈17");
    TEST_ASSERT_EQUAL_INT_MESSAGE(9,  hpssWindow(C, 23.22f),  "通用档：200/23.2≈9");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5,  hpssWindow(C, 46.44f),  "氛围档：200/46.4≈4，取奇数 5");
    for (float dt = 1.0f; dt < 120.0f; dt += 0.7f) {
        const int n = hpssWindow(C, dt);
        TEST_ASSERT_TRUE_MESSAGE((n & 1) == 1, "窗长必须是奇数，否则中值的位置有偏");
        TEST_ASSERT_TRUE_MESSAGE(n >= 3 && n <= kHpssMaxHist, "窗长必须落在合法范围");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, hpssWindow(C, 0.0f),  "非法 dt 要有退路");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, hpssWindow(C, -5.0f), "负 dt 要有退路");
}

void test_separation_is_defined_in_physical_time(void) {
    // 同一段音乐、同样的物理时长，快档与慢档应当给出一致的分离结论。
    //
    // 第一版这条测试是错的：它按**帧**平均打击占比，而两档的击打帧占空比是
    // 1/34 与 1/4 —— 量到的是占空比，不是分离质量，两档必然差很远。
    //
    // 而且合成信号本身也不诚实：真实鼓声持续的是固定的**物理时长**（约 40ms），
    // 不是固定帧数。写成「一帧一记鼓」的话，快档的鼓只有 5.8ms，
    // 那不是鼓，是一个采样点。
    //
    // 改成量两个有意义的点：击打期间的打击占比、以及两记之间的打击占比。
    const float dt[2] = {5.805f, 46.44f};
    const float kHitMs = 46.0f, kPeriodMs = 400.0f, kRunMs = 4000.0f;
    float on[2], off[2];
    for (int r = 0; r < 2; ++r) {
        reset(dt[r]);
        float pad[NUM_BANDS]; tone(pad, 5, 1.0f);
        float bang[NUM_BANDS]; for (int i = 0; i < NUM_BANDS; ++i) bang[i] = 0.8f;
        bang[5] = 1.0f;
        float acc_on = 0.0f, acc_off = 0.0f; int n_on = 0, n_off = 0;
        const int frames = (int)(kRunMs / dt[r] + 0.5f);
        for (int k = 0; k < frames; ++k) {
            const float t_ms = (float)k * dt[r];
            const float ph   = fmodf(t_ms, kPeriodMs);
            const bool  hitting = (ph < kHitMs);
            hpssProcess(S, C, hitting ? bang : pad, H, P);
            const float ratio = percussiveRatio(H, P);
            if (t_ms < 1000.0f) continue;               // 热身
            if (hitting) { acc_on += ratio; ++n_on; }
            // 只取两记之间的正中间，避开击打的余韵
            else if (ph > kPeriodMs * 0.6f) { acc_off += ratio; ++n_off; }
        }
        on[r]  = n_on  ? acc_on  / (float)n_on  : -1.0f;
        off[r] = n_off ? acc_off / (float)n_off : -1.0f;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.10f, on[0], on[1],
        "击打期间的打击占比，两档应当接近");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.10f, off[0], off[1],
        "两记之间的打击占比，两档应当接近");
    // 正对照：这两个点本身必须分得开，否则「一致」只是因为都一样平
    TEST_ASSERT_TRUE_MESSAGE(on[0] > off[0] + 0.3f,
        "正对照：击打期间与间隙之间必须有明显区别");
}

void test_retime_keeps_the_history(void) {
    // 丢历史等于分离器重新热身，换档瞬间两路会一起塌下来。
    reset();
    float b[NUM_BANDS]; tone(b, 6, 1.0f);
    for (int k = 0; k < 30; ++k) hpssProcess(S, C, b, H, P);
    const float h0 = H[6];
    hpssRetime(S, C, 46.44f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, S.n, "窗长应当换成新 dt 对应的值");
    TEST_ASSERT_TRUE_MESSAGE(S.fill > 0, "历史不能被清掉");
    hpssProcess(S, C, b, H, P);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, h0, H[6],
        "换档后第一帧的分离结果不该突变");
}

void test_retime_clamps_fill_when_the_window_shrinks(void) {
    // 窗从 17 缩到 5，fill 若还留着 17，中值会去读环形缓冲里越界的槽位。
    reset(5.805f);
    float b[NUM_BANDS]; tone(b, 6, 1.0f);
    for (int k = 0; k < 40; ++k) hpssProcess(S, C, b, H, P);
    TEST_ASSERT_EQUAL_INT(17, S.fill);
    hpssRetime(S, C, 46.44f);
    TEST_ASSERT_TRUE_MESSAGE(S.fill <= S.n, "窗缩小后 fill 必须跟着收");
    for (int k = 0; k < 5; ++k) hpssProcess(S, C, b, H, P);
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_TRUE_MESSAGE(isfinite(H[i]) && isfinite(P[i]), "不能读到越界的槽位");
}

// ── 组件 ──────────────────────────────────────────────────

void test_median_of_odd_and_even_counts(void) {
    float a[5] = {5.0f, 1.0f, 4.0f, 2.0f, 3.0f};
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, medianOf(a, 5));
    float b[1] = {7.0f};
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.0f, medianOf(b, 1));
    float c[4] = {4.0f, 1.0f, 3.0f, 2.0f};
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 3.0f, medianOf(c, 4), "偶数取上中位");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, medianOf(a, 0), "空数组要有退路");
}

void test_median_is_not_the_mean(void) {
    // 中值的全部价值在于抗离群点 —— 用均值的话一记鼓就会污染整个时间窗。
    float a[5] = {1.0f, 1.0f, 1.0f, 1.0f, 100.0f};
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 1.0f, medianOf(a, 5),
        "一个离群点不该动摇中值（均值会是 20.8）");
}

void test_process_defends_against_an_illegal_window(void) {
    // 直接把 s.n 拧成非法值，模拟「上游钳位被改坏了」。
    // 没有这道防线的话这里会越界写 hist[17][16]，踩坏堆 —— 变异测试中
    // 表现为整个进程挂死，而不是干净地失败。
    reset();
    float b[NUM_BANDS]; tone(b, 6, 1.0f);
    for (int k = 0; k < 10; ++k) hpssProcess(S, C, b, H, P);
    S.n = 999;                       // 上游坏了
    for (int k = 0; k < 10; ++k) hpssProcess(S, C, b, H, P);
    TEST_ASSERT_TRUE_MESSAGE(S.n <= kHpssMaxHist, "用之前必须把窗长收回合法范围");
    TEST_ASSERT_TRUE_MESSAGE(S.head >= 0 && S.head < S.n, "写指针不能跑出数组");
    S.n = 0;
    hpssProcess(S, C, b, H, P);
    TEST_ASSERT_TRUE_MESSAGE(S.n >= 3, "下界同样要挡");
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_TRUE_MESSAGE(isfinite(H[i]) && isfinite(P[i]), "输出仍应有效");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_a_sustained_tone_goes_to_the_harmonic_side);
    RUN_TEST(test_a_broadband_hit_goes_to_the_percussive_side);
    RUN_TEST(test_a_mixture_is_split_by_where_the_energy_sits);
    RUN_TEST(test_separation_conserves_energy);
    RUN_TEST(test_silence_produces_no_nan);
    RUN_TEST(test_nan_and_negative_input_are_dropped);
    RUN_TEST(test_percussive_ratio_separates_a_drum_loop_from_a_pad);
    RUN_TEST(test_percussive_ratio_of_silence_is_zero);
    RUN_TEST(test_window_is_odd_and_scaled_by_physical_time);
    RUN_TEST(test_separation_is_defined_in_physical_time);
    RUN_TEST(test_retime_keeps_the_history);
    RUN_TEST(test_retime_clamps_fill_when_the_window_shrinks);
    RUN_TEST(test_process_defends_against_an_illegal_window);
    RUN_TEST(test_median_of_odd_and_even_counts);
    RUN_TEST(test_median_is_not_the_mean);
    return UNITY_END();
}
