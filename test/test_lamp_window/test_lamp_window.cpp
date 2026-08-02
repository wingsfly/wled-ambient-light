#include <unity.h>
#include <math.h>
#include "lamp_window.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static float buf[2048];

// 周期性定义（不是对称定义）：w[0] 必为 0，且**没有**第二个 0 在末尾。
// 用于谱分析必须用周期定义，对称定义会引入半个 bin 的偏差。
void test_hann_is_periodic_not_symmetric(void) {
    fillWindow(WIN_HANN, buf, 8);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, buf[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, buf[4]);          // N/2 处为峰
    TEST_ASSERT_TRUE(buf[7] > 0.1f);                        // 末位不为 0
}

// 周期窗关于 N/2 对称：w[k] == w[N-k]。
void test_windows_are_symmetric_about_half(void) {
    const WindowType all[3] = {WIN_HANN, WIN_BLACKMAN_HARRIS, WIN_FLATTOP};
    for (int t = 0; t < 3; ++t) {
        fillWindow(all[t], buf, 64);
        for (size_t k = 1; k < 32; ++k)
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, buf[k], buf[64 - k]);
    }
}

// 相干增益 = mean(w)。设计 §3.3.2 的表。
void test_coherent_gain_matches_spec(void) {
    fillWindow(WIN_HANN, buf, 1024);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.500f, coherentGain(buf, 1024));
    fillWindow(WIN_BLACKMAN_HARRIS, buf, 1024);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.359f, coherentGain(buf, 1024));
    fillWindow(WIN_FLATTOP, buf, 1024);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.216f, coherentGain(buf, 1024));
}

// 噪声功率增益 = mean(w²)。**频段能量归一化用的是这个，不是相干增益。**
void test_noise_power_gain_matches_spec(void) {
    fillWindow(WIN_HANN, buf, 1024);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.375f, noisePowerGain(buf, 1024));
    fillWindow(WIN_BLACKMAN_HARRIS, buf, 1024);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.258f, noisePowerGain(buf, 1024));
    fillWindow(WIN_FLATTOP, buf, 1024);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.175f, noisePowerGain(buf, 1024));
}

// 两种增益必须不同 —— 若实现把它们写成同一个东西，上面两条会同时挂，
// 但这条把「它们本就该不同」这个意图钉死。
void test_two_gains_are_distinct(void) {
    fillWindow(WIN_HANN, buf, 512);
    const float cg = coherentGain(buf, 512);
    const float ng = noisePowerGain(buf, 512);
    TEST_ASSERT_TRUE(fabsf(cg - ng) > 0.1f);
}

// 增益与 N 无关（都是均值）。
void test_gains_are_independent_of_n(void) {
    fillWindow(WIN_HANN, buf, 512);
    const float ng512 = noisePowerGain(buf, 512);
    fillWindow(WIN_HANN, buf, 2048);
    const float ng2048 = noisePowerGain(buf, 2048);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, ng512, ng2048);
}

// 全部系数落在 [0,1]。Flat-Top 会取负值，是它的固有特性 —— 单独放行。
void test_hann_and_bh_are_bounded_nonnegative(void) {
    const WindowType nn[2] = {WIN_HANN, WIN_BLACKMAN_HARRIS};
    for (int t = 0; t < 2; ++t) {
        fillWindow(nn[t], buf, 256);
        for (size_t k = 0; k < 256; ++k) {
            TEST_ASSERT_TRUE(buf[k] >= -1e-6f);
            TEST_ASSERT_TRUE(buf[k] <= 1.0f + 1e-6f);
        }
    }
}

void test_flattop_goes_negative(void) {
    fillWindow(WIN_FLATTOP, buf, 256);
    bool neg = false;
    for (size_t k = 0; k < 256; ++k) if (buf[k] < -0.001f) neg = true;
    TEST_ASSERT_TRUE_MESSAGE(neg, "Flat-Top 应有负瓣；没有说明系数写错了");
}

// 端点必须平滑归零 —— 这是窗存在的全部意义。端点不为零就是截断不连续，
// 旁瓣立刻劣化（BH 少一项三次项，端点从 6e-5 变 0.012，旁瓣从 −92dB 掉到 −40dB）。
// 比直接测旁瓣简单得多，而且说的是同一件事。
void test_endpoints_taper_to_zero(void) {
    const WindowType all[3] = {WIN_HANN, WIN_BLACKMAN_HARRIS, WIN_FLATTOP};
    for (int t = 0; t < 3; ++t) {
        fillWindow(all[t], buf, 512);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 0.0f, buf[0],
            "端点未归零 —— 系数不满足 Σ(-1)^k a_k = 0");
    }
}

// 峰值恒为 1.0 且落在 N/2。三种窗的系数都满足 Σa_k = 1。
// 若某项符号翻转，峰会跑到 i=0（幅度谱看不出来，但加窗对准的信号段错了）。
void test_peak_is_unity_at_half(void) {
    const WindowType all[3] = {WIN_HANN, WIN_BLACKMAN_HARRIS, WIN_FLATTOP};
    for (int t = 0; t < 3; ++t) {
        fillWindow(all[t], buf, 512);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, buf[256]);
        size_t pk = 0;
        for (size_t k = 1; k < 512; ++k) if (buf[k] > buf[pk]) pk = k;
        TEST_ASSERT_EQUAL_UINT_MESSAGE(256, pk, "峰不在 N/2 —— 多半是某项符号翻了");
    }
}

// 小 N 下增益仍精确。N=8 时 n 与 n-1 差 14%，off-by-one 无处可藏；
// 上面 N=1024 的那两条容差里塞得下 0.1% 的误差，杀不掉除以 n-1。
void test_gains_exact_at_small_n(void) {
    fillWindow(WIN_HANN, buf, 8);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.500f, coherentGain(buf, 8));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.375f, noisePowerGain(buf, 8));
    fillWindow(WIN_BLACKMAN_HARRIS, buf, 8);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.35875f, coherentGain(buf, 8));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.257963f, noisePowerGain(buf, 8));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_hann_is_periodic_not_symmetric);
    RUN_TEST(test_windows_are_symmetric_about_half);
    RUN_TEST(test_coherent_gain_matches_spec);
    RUN_TEST(test_noise_power_gain_matches_spec);
    RUN_TEST(test_two_gains_are_distinct);
    RUN_TEST(test_gains_are_independent_of_n);
    RUN_TEST(test_hann_and_bh_are_bounded_nonnegative);
    RUN_TEST(test_flattop_goes_negative);
    RUN_TEST(test_endpoints_taper_to_zero);
    RUN_TEST(test_peak_is_unity_at_half);
    RUN_TEST(test_gains_exact_at_small_n);
    return UNITY_END();
}
