#include <unity.h>
#include <math.h>
#include <string.h>
#include "lamp_mood.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static MoodState  S;
static MoodConfig C;
static const float DT = 23.22f;      // 通用档 hop

// 把 16 段填成一个「重心在 lo..hi 段之间」的轮廓，总量由 amp 定。
static void shape(float *b, int lo, int hi, float amp) {
    for (int i = 0; i < NUM_BANDS; ++i) b[i] = 0.01f * amp;
    for (int i = lo; i <= hi && i < NUM_BANDS; ++i) b[i] = amp;
}

// 跑 ms 毫秒。每帧都喂同一组输入。
// 判据全部走自复位与滞回，没有一处看墙上时钟 —— 所以夹具也不需要计时。
static void run(float ms, const float *bands, float rms, float peak,
                float rate, float cen) {
    const int n = (int)(ms / DT + 0.5f);
    for (int k = 0; k < n; ++k) moodUpdate(S, C, bands, rms, peak, rate, cen, false);
}

static void reset(void) { C = MoodConfig{}; moodInit(S, C, DT); }

// ── 能量走向 ──────────────────────────────────────────────

void test_steady_loudness_gives_zero_trend(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(40000.0f, b, 0.05f, 0.08f, 2.0f, 800.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, 0.0f, S.trend,
        "响度一直不变，走向应当是零");
}

void test_first_frame_primes_both_averages(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    moodUpdate(S, C, b, 0.05f, 0.08f, 2.0f, 800.0f, false);
    // 不做 priming 的话 e_long 从 0 起步，第一帧就会报出 +1.0 的「渐强」
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f, S.trend,
        "第一帧不能凭空报出渐强");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, S.e_short, S.e_long,
        "第一帧两条均线应当被拉到同一点");
}

void test_getting_louder_gives_positive_trend(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(40000.0f, b, 0.02f, 0.03f, 2.0f, 800.0f);   // 先建立基线
    run(4000.0f,  b, 0.09f, 0.14f, 2.0f, 800.0f);   // 突然变响
    TEST_ASSERT_TRUE_MESSAGE(S.trend > 0.3f, "变响之后走向应当明显为正");
}

void test_getting_quieter_gives_negative_trend(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(40000.0f, b, 0.09f, 0.14f, 2.0f, 800.0f);
    run(4000.0f,  b, 0.02f, 0.03f, 2.0f, 800.0f);
    TEST_ASSERT_TRUE_MESSAGE(S.trend < -0.3f, "变轻之后走向应当明显为负");
}

void test_trend_is_relative_not_absolute(void) {
    // 同样是「响度翻三倍」，在两个相差 20 倍的绝对电平上应当给出同一个走向。
    // 用绝对差的话，大电平那次会顶到 ±1 饱和，小电平那次几乎为零。
    float got[2];
    const float lvl[2] = {0.005f, 0.10f};
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    for (int r = 0; r < 2; ++r) {
        reset();
        run(40000.0f, b, lvl[r],        lvl[r] * 1.5f, 2.0f, 800.0f);
        run(4000.0f,  b, lvl[r] * 3.0f, lvl[r] * 4.5f, 2.0f, 800.0f);
        got[r] = S.trend;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, got[0], got[1],
        "走向必须只看相对变化，与绝对电平无关");
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 0.2f, "正对照：这个变化必须真的被测到");
}

void test_trend_is_clamped(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(40000.0f, b, 0.001f, 0.002f, 2.0f, 800.0f);
    run(20000.0f, b, 5.0f,   7.0f,   2.0f, 800.0f);   // 涨了 5000 倍
    TEST_ASSERT_TRUE_MESSAGE(S.trend <= 1.0f, "走向不能超出 +1");
    TEST_ASSERT_TRUE_MESSAGE(S.trend >= 0.9f, "但也确实应该顶到上限");
}

// ── 段落 ──────────────────────────────────────────────────

void test_same_material_is_not_a_section_change(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    int fired = 0;
    for (int k = 0; k < 4000; ++k) {
        moodUpdate(S, C, b, 0.05f, 0.08f, 2.0f, 800.0f, false);
        if (S.section_change) ++fired;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fired, "同一段素材不该报换段");
    TEST_ASSERT_TRUE_MESSAGE(S.novelty < 0.05f, "同一段的轮廓距离应当很小");
}

void test_spectral_shape_change_fires_a_section(void) {
    reset();
    float lo[NUM_BANDS], hi[NUM_BANDS];
    shape(lo, 1, 4,  0.3f);      // 低频段落
    shape(hi, 10, 14, 0.3f);     // 高频段落
    run(30000.0f, lo, 0.05f, 0.08f, 2.0f, 400.0f);

    int fired = 0;
    for (int k = 0; k < 900; ++k) {
        moodUpdate(S, C, hi, 0.05f, 0.08f, 2.0f, 3000.0f, false);
        if (S.section_change) ++fired;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fired, "换了素材应当恰好报一次换段");
}

void test_getting_louder_alone_is_not_a_section_change(void) {
    // 轮廓做了 L1 归一化，只看形状。不归一化的话「同一段变响」会被误报。
    reset();
    float q[NUM_BANDS], l[NUM_BANDS];
    shape(q, 2, 6, 0.05f);
    shape(l, 2, 6, 0.90f);       // 形状完全相同，只是响了 18 倍
    run(30000.0f, q, 0.02f, 0.03f, 2.0f, 800.0f);
    int fired = 0;
    for (int k = 0; k < 900; ++k) {
        moodUpdate(S, C, l, 0.09f, 0.14f, 2.0f, 800.0f, false);
        if (S.section_change) ++fired;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fired, "只是变响，形状没变，不算换段");
}

void test_hysteresis_blocks_a_second_report_before_rearming(void) {
    reset();
    float a[NUM_BANDS], b2[NUM_BANDS], c3[NUM_BANDS];
    shape(a,  1, 4,   0.3f);
    shape(b2, 10, 14, 0.3f);
    shape(c3, 6, 9,   0.3f);
    run(30000.0f, a, 0.05f, 0.08f, 2.0f, 800.0f);

    int fired = 0;
    for (int k = 0; k < 900; ++k) {      // ~21s：换段 + 不应期内再换一次
        const float *src = (k < 450) ? b2 : c3;
        moodUpdate(S, C, src, 0.05f, 0.08f, 2.0f, 800.0f, false);
        if (S.section_change) ++fired;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fired,
        "第二次变化时 novelty 还没跌回下阈，滞回应当挡掉");
}

void test_novelty_returns_to_zero_once_the_new_material_settles(void) {
    // 自复位是这套判据的核心：novelty 不需要任何簿记就会自己回落，
    // 于是滞回能重新武装、下一次换段还报得出来。
    reset();
    float lo[NUM_BANDS], hi[NUM_BANDS];
    shape(lo, 1, 4,   0.3f);
    shape(hi, 10, 14, 0.3f);
    run(30000.0f, lo, 0.05f, 0.08f, 2.0f, 800.0f);
    TEST_ASSERT_TRUE_MESSAGE(S.novelty < 0.02f, "稳态下 novelty 应当接近零");

    run(6000.0f, hi, 0.05f, 0.08f, 2.0f, 800.0f);
    const float peak_nov = S.novelty;
    TEST_ASSERT_TRUE_MESSAGE(peak_nov > C.section_hi, "换段时 novelty 必须冲高");

    run(90000.0f, hi, 0.05f, 0.08f, 2.0f, 800.0f);
    TEST_ASSERT_TRUE_MESSAGE(S.novelty < 0.02f, "新素材站稳后 novelty 必须自己回落");
    TEST_ASSERT_TRUE_MESSAGE(S.armed, "回落之后滞回应当重新武装");
}

void test_a_second_change_fires_again_after_rearming(void) {
    // 上一条证明了自复位，这一条证明自复位是**有用的** —— 否则一首歌只报一次换段。
    reset();
    float lo[NUM_BANDS], hi[NUM_BANDS];
    shape(lo, 1, 4,   0.3f);
    shape(hi, 10, 14, 0.3f);
    int fired = 0;
    for (int seg = 0; seg < 3; ++seg) {
        const float *src = (seg % 2) ? hi : lo;
        for (int k = 0; k < 4000; ++k) {      // 每段 ~93s，足够站稳并重新武装
            moodUpdate(S, C, src, 0.05f, 0.08f, 2.0f, 800.0f, false);
            if (S.section_change) ++fired;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, fired, "三段素材之间的两次切换都要报到");
}

// ── 氛围标量 ──────────────────────────────────────────────

void test_dense_bright_music_scores_higher_than_sparse_dark(void) {
    float got[2];
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    // 躁：起音密、质心高、波峰因数低（塞满）
    // 静：起音疏、质心低、波峰因数高（有空隙）
    const float rate[2] = {0.3f, 7.0f}, cen[2] = {320.0f, 3200.0f};
    const float pk  [2] = {0.25f, 0.055f};
    for (int r = 0; r < 2; ++r) {
        reset();
        run(30000.0f, b, 0.05f, pk[r], rate[r], cen[r]);
        got[r] = S.mood;
    }
    TEST_ASSERT_TRUE_MESSAGE(got[1] > got[0] + 0.35f,
        "躁的那一档必须明显高于静的那一档");
    TEST_ASSERT_TRUE_MESSAGE(got[0] >= 0.0f && got[1] <= 1.0f, "必须落在 [0,1]");
}

void test_each_mood_component_moves_it_on_its_own(void) {
    // 三个分量各自单独变化时都必须推动 mood。少接一个不会让上面那条测试变红 ——
    // 那条是三个一起动的。
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    struct { float rate, cen, peak; } base = {2.0f, 700.0f, 0.10f};
    reset();
    run(30000.0f, b, 0.05f, base.peak, base.rate, base.cen);
    const float ref = S.mood;

    reset();
    run(30000.0f, b, 0.05f, base.peak, 6.5f, base.cen);
    TEST_ASSERT_TRUE_MESSAGE(S.mood > ref + 0.05f, "起音密度必须推动氛围");

    reset();
    run(30000.0f, b, 0.05f, base.peak, base.rate, 3800.0f);
    TEST_ASSERT_TRUE_MESSAGE(S.mood > ref + 0.05f, "谱重心必须推动氛围");

    reset();
    run(30000.0f, b, 0.05f, 0.051f, base.rate, base.cen);
    TEST_ASSERT_TRUE_MESSAGE(S.mood > ref + 0.05f, "波峰因数（织体密度）必须推动氛围");
}

void test_crest_is_gain_invariant(void) {
    // 波峰因数是比值，整体音量放大 50 倍不应改变它 —— 这正是选它的理由。
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, crestFactor(0.20f, 0.05f),
                                    crestFactor(10.0f, 2.50f));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 4.0f, crestFactor(0.20f, 0.05f),
        "0.20/0.05 = 4");
}

void test_brightness_maps_logarithmically(void) {
    // 200→400 和 400→800 都是一个八度，对 mood 的贡献应当相等。
    // 线性映射的话后者会是前者的两倍。
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    float m[3];
    const float cen[3] = {200.0f, 400.0f, 800.0f};
    for (int r = 0; r < 3; ++r) {
        reset();
        // 只让质心变，另两个分量按权重恒定
        C.w_onset = 0.0f; C.w_sparse = 0.0f; C.w_bright = 1.0f;
        moodInit(S, C, DT);
        run(30000.0f, b, 0.05f, 0.10f, 2.0f, cen[r]);
        m[r] = S.mood;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, m[1] - m[0], m[2] - m[1],
        "两个八度对氛围的贡献应当相等");
    TEST_ASSERT_TRUE_MESSAGE(m[1] - m[0] > 0.1f, "正对照：一个八度确实有可观贡献");
}

// ── 动态（AGC 旁路）────────────────────────────────────────

void test_loudest_passage_reads_full_dynamics(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(5000.0f, b, 0.08f, 0.12f, 2.0f, 800.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f, 1.0f, S.dynamics,
        "一直是同一个电平，那它就是最响处，动态应当满格");
}

void test_quiet_passage_reads_low_dynamics(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(5000.0f, b, 0.10f, 0.15f, 2.0f, 800.0f);   // ff 建立参考电平
    run(3000.0f, b, 0.01f, 0.015f, 2.0f, 800.0f);  // −20dB
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.04f, 0.5f, S.dynamics,
        "−20dB 在 40dB 量程里应当落在 0.5");
    run(3000.0f, b, 0.001f, 0.0015f, 2.0f, 800.0f); // −40dB
    TEST_ASSERT_TRUE_MESSAGE(S.dynamics < 0.05f, "−40dB 应当落到量程底部");
}

void test_dynamics_is_a_ratio_not_an_absolute_level(void) {
    // 麦克风灵敏度和音源电平各不相同。整体放大 50 倍，动态读数不该变 ——
    // 它量的是「这一句相对全曲最响处有多轻」。
    float got[2];
    const float k[2] = {1.0f, 50.0f};
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    for (int r = 0; r < 2; ++r) {
        reset();
        run(5000.0f, b, 0.10f*k[r], 0.15f*k[r], 2.0f, 800.0f);
        run(3000.0f, b, 0.02f*k[r], 0.03f*k[r], 2.0f, 800.0f);
        got[r] = S.dynamics;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, got[0], got[1], "动态必须与绝对电平无关");
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 0.4f && got[0] < 0.9f, "正对照：确实落在中段");
}

void test_reference_level_attacks_instantly(void) {
    // 参考电平必须瞬时跟上上升。慢慢爬的话，一段渐强会把自己当成参考，
    // 「这里是最响的」永远测不出来 —— 整首曲子的动态读数会一直贴着 1。
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(3000.0f, b, 0.01f, 0.015f, 2.0f, 800.0f);
    moodUpdate(S, C, b, 0.20f, 0.30f, 2.0f, 800.0f, false);   // 突然 ff
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.20f, S.loud_ref,
        "参考电平应当一帧就跟到新的峰值");
}

void test_reference_level_releases_slowly(void) {
    // 反过来，回落必须很慢：一个乐句的休止不该把参考电平拉下来，
    // 否则下一句进来就又是「满格」，动态范围整个消失。
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(2000.0f, b, 0.20f, 0.30f, 2.0f, 800.0f);
    const float ref0 = S.loud_ref;
    run(4000.0f, b, 0.02f, 0.03f, 2.0f, 800.0f);   // 4 秒弱奏
    TEST_ASSERT_TRUE_MESSAGE(S.loud_ref > ref0 * 0.8f,
        "4 秒弱奏不该把参考电平拉下来太多（τ=60s）");
    run(180000.0f, b, 0.02f, 0.03f, 2.0f, 800.0f); // 3 分钟
    TEST_ASSERT_TRUE_MESSAGE(S.loud_ref < ref0 * 0.2f,
        "但整首曲子都变轻之后，参考电平应当跟下来重新标定");
}

void test_dynamics_is_defined_in_physical_time(void) {
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    const float dt[2]  = {5.805f, 46.44f};
    const int   stp[2] = {80, 10};        // 同为 464.4ms
    float got[2];
    for (int r = 0; r < 2; ++r) {
        C = MoodConfig{}; moodInit(S, C, dt[r]);
        for (int k = 0; k < stp[r]; ++k)
            moodUpdate(S, C, b, 0.05f, 0.08f, 2.0f, 800.0f, false);
        got[r] = S.dynamics;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, dt[0]*stp[0], dt[1]*stp[1], "总时长必须相等");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f, got[0], got[1],
        "同样的物理时长，快档与慢档的动态应当一致");
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 0.3f && got[0] < 0.95f, "必须停在爬升中段");
}

// ── 稳健性 ────────────────────────────────────────────────

void test_gated_silence_freezes_everything(void) {
    reset();
    float b[NUM_BANDS], z[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    for (int i = 0; i < NUM_BANDS; ++i) z[i] = 0.0f;
    run(30000.0f, b, 0.05f, 0.08f, 3.0f, 900.0f);
    const float m0 = S.mood, e0 = S.e_long, tr0 = S.trend;

    for (int k = 0; k < 2000; ++k) {   // 46s 静音
        moodUpdate(S, C, z, 0.0f, 0.0f, 0.0f, 0.0f, true);
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, m0,  S.mood,  "静音时氛围必须冻结");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, e0,  S.e_long, "静音时基线必须冻结");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, tr0, S.trend, "静音时走向必须冻结");
    TEST_ASSERT_FALSE_MESSAGE(S.section_change, "静音时不该报换段");
}

void test_nan_input_is_dropped_not_absorbed(void) {
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(30000.0f, b, 0.05f, 0.08f, 3.0f, 900.0f);
    const float m0 = S.mood, e0 = S.e_long;

    const float nan_v = NAN;
    for (int k = 0; k < 10; ++k) {
        moodUpdate(S, C, b, nan_v, nan_v, nan_v, nan_v, false);
    }
    TEST_ASSERT_TRUE_MESSAGE(isfinite(S.mood) && isfinite(S.e_long),
        "NaN 一旦渗进 IIR 就永远出不来了");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, m0, S.mood);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, e0, S.e_long);
}

void test_retime_keeps_the_statistics(void) {
    // 换档只换系数。三十秒的基线要是每次换档都清零，那它就不是三十秒的基线。
    reset();
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    run(30000.0f, b, 0.05f, 0.08f, 3.0f, 900.0f);
    const float e0 = S.e_long, m0 = S.mood, a0 = S.p_slow[3];

    moodRetime(S, C, 5.805f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, e0, S.e_long, "换档不能丢基线");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, m0, S.mood,   "换档不能丢氛围");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, a0, S.p_slow[3], "换档不能丢段落轮廓");
    TEST_ASSERT_TRUE_MESSAGE(S.a_short > 0.0f, "系数确实换了");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, envCoeff(C.short_tau_ms, 5.805f), S.a_short,
        "换档后的系数应当对应新的 dt");
}

void test_statistics_are_defined_in_physical_time(void) {
    // 与色度那两条同一条规矩：写死每帧系数的话，各档 hop 差 8 倍，
    // 三十秒基线在打点档会变成不到四秒。
    float b[NUM_BANDS]; shape(b, 2, 6, 0.3f);
    const float dt[2]  = {5.805f, 46.44f};
    const int   stp[2] = {800, 100};        // 同为 4644ms
    float got[2];
    for (int r = 0; r < 2; ++r) {
        C = MoodConfig{}; moodInit(S, C, dt[r]);
        for (int k = 0; k < stp[r]; ++k)
            moodUpdate(S, C, b, 0.05f, 0.08f, 5.0f, 2500.0f, false);
        got[r] = S.mood;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, dt[0]*stp[0], dt[1]*stp[1],
        "两条路径的总时长必须相等，否则这条测试没意义");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, got[0], got[1],
        "同样的物理时长，快档与慢档的氛围应当一致");
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 0.15f && got[0] < 0.85f,
        "必须停在爬升中段，跑到收敛这条测试就什么也证明不了");
}

void test_profile_distance_ignores_scale(void) {
    float a[NUM_BANDS], b[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) { a[i] = 0.1f * (i + 1); b[i] = 7.0f * a[i]; }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.0f, profileDistance(a, b),
        "只差一个比例因子的两条轮廓，余弦距离应当是零");
}

void test_band_profile_sums_to_one(void) {
    float b[NUM_BANDS], p[NUM_BANDS]; shape(b, 3, 9, 0.7f);
    bandProfile(b, p);
    float s = 0.0f; for (int i = 0; i < NUM_BANDS; ++i) s += p[i];
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, s);
}

void test_band_profile_of_silence_is_all_zero(void) {
    float b[NUM_BANDS], p[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) b[i] = 0.0f;
    bandProfile(b, p);
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, p[i],
            "全零输入不能除出 NaN，也不能凭空均分成 1/16");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_steady_loudness_gives_zero_trend);
    RUN_TEST(test_first_frame_primes_both_averages);
    RUN_TEST(test_getting_louder_gives_positive_trend);
    RUN_TEST(test_getting_quieter_gives_negative_trend);
    RUN_TEST(test_trend_is_relative_not_absolute);
    RUN_TEST(test_trend_is_clamped);
    RUN_TEST(test_same_material_is_not_a_section_change);
    RUN_TEST(test_spectral_shape_change_fires_a_section);
    RUN_TEST(test_getting_louder_alone_is_not_a_section_change);
    RUN_TEST(test_hysteresis_blocks_a_second_report_before_rearming);
    RUN_TEST(test_novelty_returns_to_zero_once_the_new_material_settles);
    RUN_TEST(test_a_second_change_fires_again_after_rearming);
    RUN_TEST(test_dense_bright_music_scores_higher_than_sparse_dark);
    RUN_TEST(test_each_mood_component_moves_it_on_its_own);
    RUN_TEST(test_crest_is_gain_invariant);
    RUN_TEST(test_brightness_maps_logarithmically);
    RUN_TEST(test_loudest_passage_reads_full_dynamics);
    RUN_TEST(test_quiet_passage_reads_low_dynamics);
    RUN_TEST(test_dynamics_is_a_ratio_not_an_absolute_level);
    RUN_TEST(test_reference_level_attacks_instantly);
    RUN_TEST(test_reference_level_releases_slowly);
    RUN_TEST(test_dynamics_is_defined_in_physical_time);
    RUN_TEST(test_gated_silence_freezes_everything);
    RUN_TEST(test_nan_input_is_dropped_not_absorbed);
    RUN_TEST(test_retime_keeps_the_statistics);
    RUN_TEST(test_statistics_are_defined_in_physical_time);
    RUN_TEST(test_profile_distance_ignores_scale);
    RUN_TEST(test_band_profile_sums_to_one);
    RUN_TEST(test_band_profile_of_silence_is_all_zero);
    return UNITY_END();
}
