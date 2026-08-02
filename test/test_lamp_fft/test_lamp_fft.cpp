#include <unity.h>
#include <math.h>
#include "lamp_fft.h"
#include "lamp_window.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static float x[2048], re[2048], im[2048], mag[1025], win[2048];

// 朴素 DFT。整条音频链的可信度都压在 FFT 上，所以它必须与一个
// 独立写出来的实现对拍 —— 这也是 test_lamp_bands 一直在用的那个参考。
static void refDft(const float *s, const float *w, size_t n, float *out) {
    for (size_t k = 0; k <= n / 2; ++k) {
        double r = 0.0, i2 = 0.0;
        for (size_t t = 0; t < n; ++t) {
            const double a = -6.283185307179586 * (double)k * (double)t / (double)n;
            const double v = (double)s[t] * (w ? (double)w[t] : 1.0);
            r += v * cos(a); i2 += v * sin(a);
        }
        out[k] = (float)sqrt(r * r + i2 * i2);
    }
}

static void fillNoise(size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t s = (uint32_t)i * 1664525u + 1013904223u;
        s ^= s >> 16; s *= 2246822519u; s ^= s >> 13;
        x[i] = (float)(s & 0xFFFFu) / 32768.0f - 1.0f;
    }
}

// ── 与参考 DFT 对拍 ───────────────────────────────────────

void test_matches_reference_dft_on_noise(void) {
    static float ref[1025];
    const size_t ns[3] = {512, 1024, 2048};
    for (int j = 0; j < 3; ++j) {
        const size_t n = ns[j];
        fillNoise(n);
        fillWindow(WIN_HANN, win, n);
        refDft(x, win, n, ref);
        magnitudeSpectrum(x, win, n, re, im, mag);
        for (size_t k = 0; k <= n / 2; ++k)
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f * ref[k] + 1e-3f, ref[k], mag[k],
                "FFT 与参考 DFT 不符");
    }
}

void test_matches_reference_dft_unwindowed(void) {
    static float ref[1025];
    fillNoise(1024);
    refDft(x, nullptr, 1024, ref);
    magnitudeSpectrum(x, nullptr, 1024, re, im, mag);
    for (size_t k = 0; k <= 512; ++k)
        TEST_ASSERT_FLOAT_WITHIN(1e-3f * ref[k] + 1e-3f, ref[k], mag[k]);
}

// 单音必须落在正确的 bin 上，且幅度符合 A·N/2。
void test_single_tone_lands_on_its_bin(void) {
    const size_t n = 1024;
    const int k0 = 37;
    for (size_t t = 0; t < n; ++t)
        x[t] = sinf(6.283185307f * (float)k0 * (float)t / (float)n);
    magnitudeSpectrum(x, nullptr, n, re, im, mag);

    int peak = 0;
    for (size_t k = 1; k <= n / 2; ++k) if (mag[k] > mag[peak]) peak = (int)k;
    TEST_ASSERT_EQUAL_INT_MESSAGE(k0, peak, "峰不在预期的 bin 上");
    // 未加窗的整周期正弦：|X[k0]| = A·N/2
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f * n / 2, (float)n / 2.0f, mag[k0],
        "单音幅度不等于 A·N/2");
    // 其余 bin 应接近 0（整周期，无泄漏）
    for (size_t k = 0; k <= n / 2; ++k)
        if ((int)k != k0) TEST_ASSERT_TRUE(mag[k] < 0.01f * n);
}

// 直流分量落在 bin 0，幅度 = A·N。
void test_dc_lands_on_bin_zero(void) {
    const size_t n = 512;
    for (size_t t = 0; t < n; ++t) x[t] = 0.25f;
    magnitudeSpectrum(x, nullptr, n, re, im, mag);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * n, 0.25f * n, mag[0]);
    for (size_t k = 1; k <= n / 2; ++k) TEST_ASSERT_TRUE(mag[k] < 0.01f * n);
}

// Parseval：时域能量 = 频域能量 / N。
void test_parseval_holds(void) {
    const size_t n = 1024;
    fillNoise(n);
    double te = 0.0;
    for (size_t t = 0; t < n; ++t) te += (double)x[t] * x[t];

    for (size_t i = 0; i < n; ++i) { re[i] = x[i]; im[i] = 0.0f; }
    fftRadix2(re, im, n);
    double fe = 0.0;
    for (size_t k = 0; k < n; ++k) fe += (double)re[k] * re[k] + (double)im[k] * im[k];
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f * (float)te, (float)te, (float)(fe / (double)n),
        "Parseval 不成立");
}

// 非 2 的幂必须原样返回而不是算出垃圾。
void test_non_power_of_two_is_rejected(void) {
    for (size_t i = 0; i < 100; ++i) { re[i] = (float)i; im[i] = 0.0f; }
    fftRadix2(re, im, 100);
    for (size_t i = 0; i < 100; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, (float)i, re[i],
            "非 2 的幂应原样返回");
    fftRadix2(re, im, 1);   // 不得崩溃
    fftRadix2(re, im, 0);
}

// 旋转因子用递推而非每次 sin/cos，长变换下累积误差不能失控。
void test_recurrence_error_stays_bounded_at_2048(void) {
    static float ref[1025];
    fillNoise(2048);
    refDft(x, nullptr, 2048, ref);
    magnitudeSpectrum(x, nullptr, 2048, re, im, mag);
    float worst = 0.0f;
    for (size_t k = 0; k <= 1024; ++k) {
        const float d = fabsf(mag[k] - ref[k]) / (ref[k] + 1.0f);
        if (d > worst) worst = d;
    }
    TEST_ASSERT_TRUE_MESSAGE(worst < 5e-3f, "N=2048 下旋转因子递推的误差过大");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_matches_reference_dft_on_noise);
    RUN_TEST(test_matches_reference_dft_unwindowed);
    RUN_TEST(test_single_tone_lands_on_its_bin);
    RUN_TEST(test_dc_lands_on_bin_zero);
    RUN_TEST(test_parseval_holds);
    RUN_TEST(test_non_power_of_two_is_rejected);
    RUN_TEST(test_recurrence_error_stays_bounded_at_2048);
    return UNITY_END();
}
