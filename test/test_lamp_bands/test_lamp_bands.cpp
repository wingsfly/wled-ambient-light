#include <unity.h>
#include <math.h>
#include "lamp_bands.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// 边界必须严格递增，且覆盖上游那套 43Hz–9259Hz。
void test_edges_are_monotonic_and_cover_spec_range(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 43.07f,  kBandEdgeHz[0]);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 9259.0f, kBandEdgeHz[NUM_BANDS]);
    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_TRUE(kBandEdgeHz[i + 1] > kBandEdgeHz[i]);
}

// N=512 时必须逐 bin 复现上游 WLED 的映射 —— 这样默认档的观感与上游一致，
// 而且给了这套 Hz 规范一个外部锚点。
void test_512_reproduces_upstream_bin_table(void) {
    static const uint16_t up[NUM_BANDS + 1] =
        {1,2,3,5,7,10,13,19,26,33,44,56,70,86,104,165,215};
    for (int i = 0; i <= NUM_BANDS; ++i)
        TEST_ASSERT_EQUAL_UINT16(up[i], binEdge(i, 512));
}

// 三种 N 下都不许出现空段 —— 空段会让那一格恒为 0，看起来像特效坏了。
void test_no_empty_band_at_any_n(void) {
    const size_t ns[3] = {512, 1024, 2048};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < NUM_BANDS; ++i) {
            const uint16_t lo = binEdge(i, ns[j]), hi = binEdge(i + 1, ns[j]);
            TEST_ASSERT_TRUE_MESSAGE(hi > lo, "出现空段");
        }
}

// bin 数应随 N 成比例 —— N 翻两倍，每段的 bin 数大致翻四倍。
void test_bin_count_scales_with_n(void) {
    for (int i = 0; i < NUM_BANDS; ++i) {
        const int c512  = binEdge(i + 1, 512)  - binEdge(i, 512);
        const int c2048 = binEdge(i + 1, 2048) - binEdge(i, 2048);
        TEST_ASSERT_TRUE(c2048 >= 3 * c512);      // 四倍上下，留取整余量
        TEST_ASSERT_TRUE(c2048 <= 5 * c512 + 2);
    }
}

// 边界不得超出奈奎斯特。
void test_top_edge_below_nyquist(void) {
    const size_t ns[3] = {512, 1024, 2048};
    for (int j = 0; j < 3; ++j)
        TEST_ASSERT_TRUE(binEdge(NUM_BANDS, ns[j]) <= ns[j] / 2);
}

// 边界应大致按对数排布 —— 相邻段的倍频程比值落在一个合理区间。
// 这条把「16 段对数」这个意图钉住：若哪条边界被改成线性插值，比值会跑掉。
void test_edges_are_roughly_logarithmic(void) {
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float r = kBandEdgeHz[i + 1] / kBandEdgeHz[i];
        TEST_ASSERT_TRUE_MESSAGE(r > 1.1f, "相邻边界太近，不像对数排布");
        TEST_ASSERT_TRUE_MESSAGE(r < 2.1f, "相邻边界跨度超过一个倍频程");
    }
}

// binEdge 不得纯截断。表里的 Hz 都是从整数 bin 反算的，落点非 x.9999 即 x.0001，
// 截断会把前者整格拉低。
//
// 这条**只**能区分「截断」与「不截断」—— 区分不了四舍五入与其它偏移量，
// 因为这张表上根本没有小数部分接近 0.5 的落点。名字别写成 rounds。
void test_bin_edge_does_not_truncate(void) {
    // 86.13Hz @ N=512 → 1.9999：截断得 1，不截断得 2
    TEST_ASSERT_EQUAL_UINT16(2, binEdge(1, 512));
    // 3703.71Hz @ N=1024 → 171.9997：截断得 171
    TEST_ASSERT_EQUAL_UINT16(172, binEdge(13, 1024));
}

// N=1024 与 N=2048 的完整 bin 表。
//
// 只钉住 N=512 是不够的：把 1119.73Hz 改成 1100Hz，N=512 下仍是 bin 26
// （25.54 与 26.00 同归 26），N=1024 下却从 52 变成 51 —— 只有性质断言
// （无空段、比例、奈奎斯特）全都放行。高 N 必须有自己的数值锚点。
void test_high_n_bin_tables_are_exact(void) {
    static const uint16_t b1024[NUM_BANDS + 1] =
        {2,4,6,10,14,20,26,38,52,66,88,112,140,172,208,330,430};
    static const uint16_t b2048[NUM_BANDS + 1] =
        {4,8,12,20,28,40,52,76,104,132,176,224,280,344,416,660,860};
    for (int i = 0; i <= NUM_BANDS; ++i) {
        TEST_ASSERT_EQUAL_UINT16(b1024[i], binEdge(i, 1024));
        TEST_ASSERT_EQUAL_UINT16(b2048[i], binEdge(i, 2048));
    }
}

// ── 参考 DFT（仅测试用）──────────────────────────────────
//
// 我们的代码不拥有 FFT：目标板上用 esp-dsp，这里用朴素 DFT 生成频谱。
// N=2048 时约 210 万次运算，主机上几十毫秒，可以接受。

static float g_win[2048];
static float g_mag[1025];

static void referenceSpectrum(const float *x, size_t n, WindowType wt) {
    fillWindow(wt, g_win, n);
    for (size_t k = 0; k <= n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (size_t t = 0; t < n; ++t) {
            const double a = -6.283185307179586 * (double)k * (double)t / (double)n;
            const double v = (double)x[t] * (double)g_win[t];
            re += v * cos(a);
            im += v * sin(a);
        }
        g_mag[k] = (float)sqrt(re * re + im * im);
    }
}

static float g_sig[2048];
static Analysis g_an;   // 8KB，放静态而非栈上

// 每段中心放一个单位正弦，频率取 k·fs/512。
//
// 为什么不用随机噪声：N=512 下最低几段只有 1 个 bin，单 bin 估计功率谱的
// 相对标准差是 100% —— 那种测试的「偏差」全是估计方差，跟归一化对不对无关。
//
// 为什么频率取 k·fs/512：这样在 512/1024/2048 三种 N 下都精确落在 bin 中心
// （k, 2k, 4k），既无 scalloping loss 也无因落点不同引入的差异。
// 每个正弦功率 = 0.5，段功率的理论值因此是确定的。
static void fillSignal(size_t n) {
    for (size_t t = 0; t < n; ++t) g_sig[t] = 0.0f;
    for (int i = 0; i < NUM_BANDS; ++i) {
        uint16_t k = (uint16_t)((binEdge(i, 512) + binEdge(i + 1, 512)) / 2);
        if (k == 0) k = 1;
        for (size_t t = 0; t < n; ++t)
            g_sig[t] += sinf(6.283185307f * (float)k * (float)t / 512.0f);
    }
}

// ── 跨档连续性：本计划的验收线 ────────────────────────────

// 单音的段功率必须绝对正确 —— 单位正弦的 RMS 是 1/√2，不管 N 与窗。
// 这比相对一致更强：它把归一化常数本身钉死，任何缺失的因子都会整体偏移。
void test_band_energy_is_absolutely_calibrated(void) {
    struct Cfg { size_t n; WindowType w; };
    const Cfg cs[4] = {{2048, WIN_BLACKMAN_HARRIS}, {1024, WIN_HANN},
                       {2048, WIN_FLATTOP},         { 512, WIN_HANN}};
    for (int c = 0; c < 4; ++c) {
        fillSignal(cs[c].n);
        referenceSpectrum(g_sig, cs[c].n, cs[c].w);
        float out[NUM_BANDS];
        analysisInit(g_an, cs[c].n, cs[c].w);
        computeBandEnergy(g_an, g_mag, out);
        for (int i = 0; i < NUM_BANDS; ++i) {
            if (!bandIsResolved(i, cs[c].n, cs[c].w)) continue;
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * 0.70711f, 0.70711f, out[i],
                "单位正弦的段 RMS 应为 1/√2 —— 归一化常数不对");
        }
    }
}

// 同一段音频，四个档位算出的 fftResult 必须逐段一致（<5%）。
// 这是 §3.3.4 第 1 条：归一化没做对，切档瞬间灯会跳亮度，
// 用户感知到的是「bug」而不是「更适合这首歌」。
//
// 只对 bandIsResolved 的段成立 —— 段宽小于窗主瓣宽时物理上就分不开，见下一条。
void test_band_energy_is_continuous_across_presets(void) {
    struct Preset { size_t n; WindowType w; };
    const Preset ps[4] = {
        {2048, WIN_BLACKMAN_HARRIS},   // 氛围/古典
        {1024, WIN_HANN},              // 通用
        {1024, WIN_HANN},              // 电子（N 同通用，hop 不同，不影响本层）
        { 512, WIN_HANN},              // 极限打点
    };
    float ref[NUM_BANDS];

    for (int p = 0; p < 4; ++p) {
        fillSignal(ps[p].n);
        referenceSpectrum(g_sig, ps[p].n, ps[p].w);
        float out[NUM_BANDS];
        analysisInit(g_an, ps[p].n, ps[p].w);
        computeBandEnergy(g_an, g_mag, out);

        if (p == 0) { for (int i = 0; i < NUM_BANDS; ++i) ref[i] = out[i]; continue; }
        for (int i = 0; i < NUM_BANDS; ++i) {
            if (!bandIsResolved(i, ps[0].n, ps[0].w)) continue;
            if (!bandIsResolved(i, ps[p].n, ps[p].w)) continue;
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * ref[i] + 1e-4f, ref[i], out[i],
                "跨档偏差超过 5% —— 归一化系数不对");
        }
    }
}

// 换窗不换 N，结果也必须一致 —— 这一条单独钉住「用了噪声功率增益」。
// 若误用相干增益，Hann 与 BH 会差约 1.94 倍，且是全段偏移，筛选也挡不住。
void test_band_energy_is_continuous_across_windows(void) {
    fillSignal(2048);
    float hann[NUM_BANDS], bh[NUM_BANDS], ft[NUM_BANDS];
    referenceSpectrum(g_sig, 2048, WIN_HANN);
    analysisInit(g_an, 2048, WIN_HANN);
    computeBandEnergy(g_an, g_mag, hann);
    referenceSpectrum(g_sig, 2048, WIN_BLACKMAN_HARRIS);
    analysisInit(g_an, 2048, WIN_BLACKMAN_HARRIS);
    computeBandEnergy(g_an, g_mag, bh);
    referenceSpectrum(g_sig, 2048, WIN_FLATTOP);
    analysisInit(g_an, 2048, WIN_FLATTOP);
    computeBandEnergy(g_an, g_mag, ft);
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (!bandIsResolved(i, 2048, WIN_FLATTOP)) continue;
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * hann[i] + 1e-4f, hann[i], bh[i],
            "Hann 与 BH 不一致 —— 多半是用了相干增益而非噪声功率增益");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * hann[i] + 1e-4f, hann[i], ft[i],
            "Hann 与 Flat-Top 不一致");
    }
}

// 把「哪些段分得开」写成显式规格，而不是留成一句注释。
//
// 这不是实现缺陷：43Hz 的段宽在 N=512 下就是 1 个 bin，Hann 主瓣是 2 bin。
// 设计 §3.3 让「极限打点」用 N=512，代价就是低频段读数不再跨档可比。
// 这条测试的作用是：谁改了边界表或窗系数导致可分段数变少，立刻会被发现。
void test_resolution_limits_are_as_specified(void) {
    int n512 = 0, n1024 = 0, n2048 = 0, ft2048 = 0;
    for (int i = 0; i < NUM_BANDS; ++i) {
        if (bandIsResolved(i,  512, WIN_HANN))            ++n512;
        if (bandIsResolved(i, 1024, WIN_HANN))            ++n1024;
        if (bandIsResolved(i, 2048, WIN_BLACKMAN_HARRIS)) ++n2048;
        if (bandIsResolved(i, 2048, WIN_FLATTOP))         ++ft2048;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(10, n512,   "N=512  Hann 应有 10 段可分（最低段 6）");
    TEST_ASSERT_EQUAL_INT_MESSAGE(14, n1024,  "N=1024 Hann 应有 14 段可分（最低段 2）");
    TEST_ASSERT_EQUAL_INT_MESSAGE(14, n2048,  "N=2048 BH   应有 14 段可分（最低段 2）");
    TEST_ASSERT_EQUAL_INT_MESSAGE(12, ft2048, "N=2048 FT   应有 12 段可分（最低段 4）");
    // 最低那几段在任何档位下都分不开 —— 这是 43Hz 起点的固有代价
    TEST_ASSERT_FALSE(bandIsResolved(0, 2048, WIN_BLACKMAN_HARRIS));
    // 最高那几段在任何档位下都分得开
    TEST_ASSERT_TRUE(bandIsResolved(15, 512, WIN_HANN));
}

// 幅度加倍 → 能量输出加倍（本实现输出的是幅度量纲，不是功率）。
void test_output_scales_linearly_with_amplitude(void) {
    fillSignal(1024);
    referenceSpectrum(g_sig, 1024, WIN_HANN);
    float a[NUM_BANDS];
    analysisInit(g_an, 1024, WIN_HANN);
    computeBandEnergy(g_an, g_mag, a);

    for (size_t t = 0; t < 1024; ++t) g_sig[t] *= 2.0f;
    referenceSpectrum(g_sig, 1024, WIN_HANN);
    float b[NUM_BANDS];
    analysisInit(g_an, 1024, WIN_HANN);
    computeBandEnergy(g_an, g_mag, b);

    for (int i = 0; i < NUM_BANDS; ++i)
        TEST_ASSERT_FLOAT_WITHIN(0.02f * a[i] * 2.0f + 1e-6f, a[i] * 2.0f, b[i]);
}

// 静音进 → 全零出，且不得出现 NaN。
void test_silence_yields_zero_without_nan(void) {
    for (size_t t = 0; t < 1024; ++t) g_sig[t] = 0.0f;
    referenceSpectrum(g_sig, 1024, WIN_HANN);
    float out[NUM_BANDS];
    analysisInit(g_an, 1024, WIN_HANN);
    computeBandEnergy(g_an, g_mag, out);
    for (int i = 0; i < NUM_BANDS; ++i) {
        TEST_ASSERT_FALSE(isnan(out[i]));
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out[i]);
    }
}

// 单音落在第 7 段（818–1120Hz），能量必须集中在那一段。
void test_single_tone_lands_in_expected_band(void) {
    for (size_t t = 0; t < 1024; ++t)
        g_sig[t] = sinf(6.283185307f * 1000.0f * (float)t / kSampleRate);
    referenceSpectrum(g_sig, 1024, WIN_HANN);
    float out[NUM_BANDS];
    analysisInit(g_an, 1024, WIN_HANN);
    computeBandEnergy(g_an, g_mag, out);

    int peak = 0;
    for (int i = 1; i < NUM_BANDS; ++i) if (out[i] > out[peak]) peak = i;
    TEST_ASSERT_EQUAL_INT_MESSAGE(7, peak, "1kHz 应落在第 7 段（818–1120Hz）");
    // 相邻段泄漏至少低 10dB
    TEST_ASSERT_TRUE(out[6] < out[7] * 0.32f);
    TEST_ASSERT_TRUE(out[8] < out[7] * 0.32f);
}

// 频段之间必须真的隔开：只在第 3 段放一个单音，其余段应显著更小。
// 这一条防的是「所有段读同一片 bin」这类退化 —— 上面的跨档一致性
// 在退化实现下反而更容易通过。
void test_bands_are_independent(void) {
    const float f3 = 0.5f * (kBandEdgeHz[3] + kBandEdgeHz[4]);   // 段 3 中心
    for (size_t t = 0; t < 1024; ++t)
        g_sig[t] = sinf(6.283185307f * f3 * (float)t / kSampleRate);
    referenceSpectrum(g_sig, 1024, WIN_HANN);
    float out[NUM_BANDS];
    analysisInit(g_an, 1024, WIN_HANN);
    computeBandEnergy(g_an, g_mag, out);

    for (int i = 0; i < NUM_BANDS; ++i) {
        if (i == 3) continue;
        TEST_ASSERT_TRUE_MESSAGE(out[i] < out[3] * 0.5f,
            "非目标段能量过高 —— 各段可能读了同一片 bin");
    }
}

// 段区间是左闭右开：边界 bin 归**右**边那一段，既不重叠也不留缝。
//
// 上一轮变异验证里 k<hi 改成 k<=hi、以及 lo 改成 lo+1，两个都活了下来 ——
// 因为当时的测试信号把正弦全放在段中心，边界处几乎没能量，吃错一个 bin 看不出来。
// 这条专门把正弦放在边界 bin 上。
void test_boundary_bin_belongs_to_upper_band(void) {
    const size_t n = 1024;
    for (int i = 3; i < NUM_BANDS - 1; ++i) {
        const uint16_t kb = binEdge(i, n);        // 段 i 的第一个 bin
        for (size_t t = 0; t < n; ++t)
            g_sig[t] = sinf(6.283185307f * (float)kb * (float)t / (float)n);
        referenceSpectrum(g_sig, n, WIN_HANN);
        analysisInit(g_an, n, WIN_HANN);
        float out[NUM_BANDS];
        computeBandEnergy(g_an, g_mag, out);

        // Hann 对准 bin 的正弦，功率按 0.167 / 0.667 / 0.167 分到三个 bin。
        // 段 i 含中心与右泄漏 = 0.833 份，段 i-1 只含左泄漏 = 0.167 份。
        //   √0.833 × 0.7071 = 0.6455    √0.1667 × 0.7071 = 0.2887
        // 两侧都钉住：下界写成 lo+1 会把 out[i] 拉到 0.2887，
        // 上界写成 k<=hi 会把 out[i-1] 顶到 0.6455。
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * 0.6455f, 0.6455f, out[i],
            "边界 bin 未计入右侧段 —— 下界可能写成了 lo+1");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * 0.6455f, 0.2887f, out[i - 1],
            "边界 bin 同时被左侧段吃掉 —— 上界可能写成了 k <= hi");
    }
}

// 非法帧长必须明确失败，而且不能改动已有配置。
// 静默裁剪到 2048 更糟：调用方以为配成了 4096，读到的谱却是按 2048 算的。
void test_analysis_init_rejects_bad_length(void) {
    TEST_ASSERT_TRUE(analysisInit(g_an, 1024, WIN_HANN));
    const float good_ng = g_an.ng;

    TEST_ASSERT_FALSE_MESSAGE(analysisInit(g_an, 4096, WIN_FLATTOP), "超长应被拒");
    TEST_ASSERT_FALSE_MESSAGE(analysisInit(g_an, 1000, WIN_FLATTOP), "非 2 的幂应被拒");
    TEST_ASSERT_FALSE_MESSAGE(analysisInit(g_an,    0, WIN_FLATTOP), "0 应被拒");

    TEST_ASSERT_EQUAL_UINT(1024, (unsigned)g_an.n);
    TEST_ASSERT_EQUAL_INT(WIN_HANN, g_an.wt);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, good_ng, g_an.ng);
}

// 窗缓冲尾部必须清零 —— 否则按错误长度算增益时会读到上一档的残留，
// 结果看着合理，缺陷藏住。
void test_window_tail_is_zeroed(void) {
    analysisInit(g_an, 2048, WIN_BLACKMAN_HARRIS);
    analysisInit(g_an,  512, WIN_HANN);
    for (size_t i = 512; i < kMaxFftLen; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, g_an.w[i], "尾部残留了上一档的窗系数");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_edges_are_monotonic_and_cover_spec_range);
    RUN_TEST(test_512_reproduces_upstream_bin_table);
    RUN_TEST(test_no_empty_band_at_any_n);
    RUN_TEST(test_bin_count_scales_with_n);
    RUN_TEST(test_top_edge_below_nyquist);
    RUN_TEST(test_edges_are_roughly_logarithmic);
    RUN_TEST(test_bin_edge_does_not_truncate);
    RUN_TEST(test_high_n_bin_tables_are_exact);
    RUN_TEST(test_band_energy_is_absolutely_calibrated);
    RUN_TEST(test_band_energy_is_continuous_across_presets);
    RUN_TEST(test_band_energy_is_continuous_across_windows);
    RUN_TEST(test_resolution_limits_are_as_specified);
    RUN_TEST(test_output_scales_linearly_with_amplitude);
    RUN_TEST(test_silence_yields_zero_without_nan);
    RUN_TEST(test_single_tone_lands_in_expected_band);
    RUN_TEST(test_bands_are_independent);
    RUN_TEST(test_boundary_bin_belongs_to_upper_band);
    RUN_TEST(test_analysis_init_rejects_bad_length);
    RUN_TEST(test_window_tail_is_zeroed);
    return UNITY_END();
}
