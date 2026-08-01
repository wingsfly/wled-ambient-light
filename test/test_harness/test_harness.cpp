// test/test_harness/test_harness.cpp
// 只验证 native 测试环境本身可用，不测任何业务逻辑。
#include <unity.h>

// 钉住 -std=gnu++17。主机编译器默认是 C++14，若 platformio_override.ini 里的
// -std 标志哪天被删掉，本文件仍会 PASS，故障会推迟到 test_lamp_geometry 表现为
// 一堆莫名的 C++17 语法错 —— 恰恰是本冒烟测试存在的意义所要避免的。
//
// 判据：钉住「坏了也不吭声」的标志。-I usermods/lamp 不在此列，因为
// test_lamp_geometry.cpp 的 #include "lamp_geometry.h" 本身就是它的测试，
// 坏了会大声失败。
static_assert(__cplusplus >= 201703L, "native env must build with -std=gnu++17");

void setUp(void) {}
void tearDown(void) {}

void test_harness_runs(void) {
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_harness_runs);
    return UNITY_END();
}
