// test/test_lamp_geometry/test_lamp_geometry.cpp
#include <unity.h>
#include "lamp_geometry.h"
#include <math.h>

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// 参考配置：S1 接左管，两路数据链首颗都在管底。
// 三个字段全部显式赋值 —— 绝不依赖 struct 的默认值，
// 否则 Task 7 把默认值改成实测结果时，这些测试会假失败。
static Geometry refGeom() {
    Geometry g;
    g.s1_is_left  = true;
    g.s1_reversed = false;
    g.s2_reversed = false;
    return g;
}

void test_ref_left_bottom_is_index_0(void) {
    TEST_ASSERT_EQUAL_UINT16(0, mapPixel(refGeom(), SIDE_L, 0.0f));
}

void test_ref_left_top_is_index_47(void) {
    TEST_ASSERT_EQUAL_UINT16(47, mapPixel(refGeom(), SIDE_L, 1.0f));
}

void test_ref_right_occupies_second_bus(void) {
    TEST_ASSERT_EQUAL_UINT16(48, mapPixel(refGeom(), SIDE_R, 0.0f));
    TEST_ASSERT_EQUAL_UINT16(95, mapPixel(refGeom(), SIDE_R, 1.0f));
}

// s1_is_left=false 表示 S1 接右管，于是「左」这一侧要走第二条总线。
void test_swapped_sides_move_left_to_second_bus(void) {
    Geometry g = refGeom(); g.s1_is_left = false;
    TEST_ASSERT_EQUAL_UINT16(48, mapPixel(g, SIDE_L, 0.0f));
    TEST_ASSERT_EQUAL_UINT16(0,  mapPixel(g, SIDE_R, 0.0f));
}

// s1_reversed=true 表示 S1 数据链首颗在管顶，于是 u=1（顶）对应索引 0。
void test_s1_reversed_flips_only_s1(void) {
    Geometry g = refGeom(); g.s1_reversed = true;
    TEST_ASSERT_EQUAL_UINT16(47, mapPixel(g, SIDE_L, 0.0f));
    TEST_ASSERT_EQUAL_UINT16(0,  mapPixel(g, SIDE_L, 1.0f));
    // S2 不受影响
    TEST_ASSERT_EQUAL_UINT16(48, mapPixel(g, SIDE_R, 0.0f));
    TEST_ASSERT_EQUAL_UINT16(95, mapPixel(g, SIDE_R, 1.0f));
}

void test_s2_reversed_flips_only_s2(void) {
    Geometry g = refGeom(); g.s2_reversed = true;
    TEST_ASSERT_EQUAL_UINT16(95, mapPixel(g, SIDE_R, 0.0f));
    TEST_ASSERT_EQUAL_UINT16(48, mapPixel(g, SIDE_R, 1.0f));
    TEST_ASSERT_EQUAL_UINT16(0,  mapPixel(g, SIDE_L, 0.0f));
}

void test_u_out_of_range_is_clamped(void) {
    Geometry g = refGeom();
    // 用字面量而非自反比较。注意：单靠测试**盖不住**「删掉下钳制」这个变异 ——
    // native env 跑在 -O0，负浮点转无符号是 UB，那里恰好得 0 从而掩盖问题。
    // 下钳制真正由实现侧的有符号中间量兜住，见 lamp_geometry.h。
    TEST_ASSERT_EQUAL_UINT16(0,  mapPixel(g, SIDE_L, -5.0f));
    TEST_ASSERT_EQUAL_UINT16(47, mapPixel(g, SIDE_L,  5.0f));
}

void test_nan_maps_to_bottom(void) {
    Geometry g = refGeom();
    // 诚实说明：这条在 arm64 主机上**无法失败** —— 即便删掉 NaN 守卫，
    // fcvtzs 对 NaN 也返回 0。它记录意图、锁住契约，但真正的兜底来自实现里
    // 的有符号中间量与索引钳制（后置条件 [0, TOTAL_LEDS) 恒成立）。
    // xtensa 上的行为未知，这正是守卫存在的理由。
    TEST_ASSERT_EQUAL_UINT16(0, mapPixel(g, SIDE_L, NAN));
}

// 内部锚点。双射测试是顺序无关的，看不见内部排列错误；而 floor(48u) 与
// round(47u) 在**所有** k/47 网格点上完全一致，所以必须用一个**离网格**的
// 值才能把量化约定钉死。变异测试证明：没有这条，min(47,(uint16_t)(u*48))
// 这个错误实现能全绿通过。
void test_interior_anchors_pin_the_quantization(void) {
    Geometry g = refGeom();
    TEST_ASSERT_EQUAL_UINT16(24, mapPixel(g, SIDE_L, 0.5f));
    TEST_ASSERT_EQUAL_UINT16(1,  mapPixel(g, SIDE_L, 0.0200f));  // round(0.94)=1，floor(0.96)=0
}

// 最强的一条不变量：对任意配置，u = k/47（k=0..47）必须恰好打中
// 该管 48 个索引各一次 —— 能同时抓住 off-by-one、别名、越界。
void test_every_config_is_a_bijection_over_its_tube(void) {
    for (int combo = 0; combo < 8; ++combo) {
        Geometry g;
        g.s1_is_left  = (combo & 1) != 0;
        g.s1_reversed = (combo & 2) != 0;
        g.s2_reversed = (combo & 4) != 0;

        for (int s = 0; s < 2; ++s) {
            Side side = (s == 0) ? SIDE_L : SIDE_R;
            bool hit[TOTAL_LEDS] = {false};
            uint16_t lo = TOTAL_LEDS, hi = 0;
            int prev = -1;

            for (int k = 0; k < LEDS_PER_TUBE; ++k) {
                uint16_t idx = mapPixel(g, side, (float)k / (LEDS_PER_TUBE - 1));
                TEST_ASSERT_LESS_THAN_UINT16(TOTAL_LEDS, idx);
                TEST_ASSERT_FALSE_MESSAGE(hit[idx], "同一索引被打中两次");
                hit[idx] = true;
                if (idx < lo) lo = idx;
                if (idx > hi) hi = idx;

                // 单调性：双射证不到这一条 —— 一个「打乱内部顺序但仍是双射」
                // 的实现能通过双射断言。变异测试实测存活，故补此条。
                if (prev >= 0) {
                    const int step = (int)idx - prev;
                    TEST_ASSERT_EQUAL_INT_MESSAGE(1, step < 0 ? -step : step,
                                                  "相邻 u 的索引必须相差 1");
                }
                prev = (int)idx;
            }

            int total = 0;
            for (int i = 0; i < TOTAL_LEDS; ++i) if (hit[i]) total++;
            TEST_ASSERT_EQUAL_INT(LEDS_PER_TUBE, total);

            // 48 颗必须落在同一根管子的连续半区（[0,47] 或 [48,95]），不能跨管。
            // 让本不变量自足，不依赖端点测试。
            TEST_ASSERT_TRUE_MESSAGE(lo == 0 || lo == LEDS_PER_TUBE, "起点不在某根管子开头");
            TEST_ASSERT_EQUAL_UINT16(LEDS_PER_TUBE - 1, hi - lo);
        }
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_ref_left_bottom_is_index_0);
    RUN_TEST(test_ref_left_top_is_index_47);
    RUN_TEST(test_ref_right_occupies_second_bus);
    RUN_TEST(test_swapped_sides_move_left_to_second_bus);
    RUN_TEST(test_s1_reversed_flips_only_s1);
    RUN_TEST(test_s2_reversed_flips_only_s2);
    RUN_TEST(test_u_out_of_range_is_clamped);
    RUN_TEST(test_nan_maps_to_bottom);
    RUN_TEST(test_interior_anchors_pin_the_quantization);
    RUN_TEST(test_every_config_is_a_bijection_over_its_tube);
    return UNITY_END();
}
