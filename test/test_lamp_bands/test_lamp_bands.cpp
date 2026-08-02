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
    return UNITY_END();
}
