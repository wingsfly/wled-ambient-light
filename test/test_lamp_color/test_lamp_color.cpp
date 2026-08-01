// test/test_lamp_color/test_lamp_color.cpp
#include <unity.h>
#include "lamp_color.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// docs/10-灯管与驱动.md 实测：原厂满白线上字节 GRB = DD FF C2，
// 即 R=255 G=221 B=194。白平衡必须能从纯白复现出这组值。
void test_pure_white_reproduces_measured_factory_values(void) {
    Rgb out = applyWhiteBalance(Rgb{255, 255, 255}, true);
    TEST_ASSERT_EQUAL_UINT8(255, out.r);
    TEST_ASSERT_EQUAL_UINT8(221, out.g);
    TEST_ASSERT_EQUAL_UINT8(194, out.b);
}

void test_disabled_is_identity(void) {
    Rgb in{123, 45, 200};
    Rgb out = applyWhiteBalance(in, false);
    TEST_ASSERT_EQUAL_UINT8(in.r, out.r);
    TEST_ASSERT_EQUAL_UINT8(in.g, out.g);
    TEST_ASSERT_EQUAL_UINT8(in.b, out.b);
}

void test_black_stays_black(void) {
    Rgb out = applyWhiteBalance(Rgb{0, 0, 0}, true);
    TEST_ASSERT_EQUAL_UINT8(0, out.r);
    TEST_ASSERT_EQUAL_UINT8(0, out.g);
    TEST_ASSERT_EQUAL_UINT8(0, out.b);
}

// 系数全部 ≤ 1，任何输入都不应变大，更不能回绕。
void test_never_increases_and_never_wraps(void) {
    for (int v = 0; v <= 255; ++v) {
        Rgb out = applyWhiteBalance(Rgb{(uint8_t)v, (uint8_t)v, (uint8_t)v}, true);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8((uint8_t)v, out.r);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8((uint8_t)v, out.g);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8((uint8_t)v, out.b);
    }
}

// 内部锚点，钉住四舍五入而非截断。
//
// **这条是必需的，不是锦上添花。** 满白那条锚不住取整方式 ——
//   255 × 0.867 = 221.085 → round=221, floor=221   ← 相同
//   255 × 0.761 = 194.055 → round=194, floor=194   ← 相同
// 也就是说，把 +0.5f 去掉改成截断，上面四条测试**全绿通过**。
// 这与 Task 3 的量化约定变异体是同一类盲区，必须用小数部分 ≥ 0.5 的输入才能区分。
void test_interior_anchors_pin_rounding(void) {
    Rgb a = applyWhiteBalance(Rgb{100, 100, 100}, true);
    TEST_ASSERT_EQUAL_UINT8(87, a.g);   // 86.7  → round 87，floor 86
    Rgb b = applyWhiteBalance(Rgb{2, 2, 2}, true);
    TEST_ASSERT_EQUAL_UINT8(2, b.g);    // 1.734 → round 2，floor 1
    TEST_ASSERT_EQUAL_UINT8(2, b.b);    // 1.522 → round 2，floor 1
}

// WB_R = 1.000，红通道必须原样透传。若哪天有人误改这个系数，这条会立刻响。
void test_red_is_pass_through(void) {
    for (int v = 0; v <= 255; ++v) {
        Rgb out = applyWhiteBalance(Rgb{(uint8_t)v, 0, 0}, true);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)v, out.r);
    }
}

// 单调性。上面几条都是逐点断言，一个「打乱映射但仍不超界」的实现能通过。
// 这与 Task 3 补单调性断言是同一个理由。
void test_monotonic_in_input(void) {
    Rgb prev = applyWhiteBalance(Rgb{0, 0, 0}, true);
    for (int v = 1; v <= 255; ++v) {
        Rgb cur = applyWhiteBalance(Rgb{(uint8_t)v, (uint8_t)v, (uint8_t)v}, true);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(prev.r, cur.r);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(prev.g, cur.g);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(prev.b, cur.b);
        prev = cur;
    }
}

// 通道独立性：上面所有启用路径的输入三通道都相等，
// 「G 从 in.r 取值」这类复制粘贴错误能全绿存活（变异测试实证）。
void test_channels_are_independent(void) {
    Rgb out = applyWhiteBalance(Rgb{123, 45, 200}, true);
    TEST_ASSERT_EQUAL_UINT8(123, out.r);  // 123 × 1.000
    TEST_ASSERT_EQUAL_UINT8(39,  out.g);  // 39.015 → 39
    TEST_ASSERT_EQUAL_UINT8(152, out.b);  // 152.2  → 152
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_pure_white_reproduces_measured_factory_values);
    RUN_TEST(test_disabled_is_identity);
    RUN_TEST(test_black_stays_black);
    RUN_TEST(test_never_increases_and_never_wraps);
    RUN_TEST(test_interior_anchors_pin_rounding);
    RUN_TEST(test_red_is_pass_through);
    RUN_TEST(test_monotonic_in_input);
    RUN_TEST(test_channels_are_independent);
    return UNITY_END();
}
