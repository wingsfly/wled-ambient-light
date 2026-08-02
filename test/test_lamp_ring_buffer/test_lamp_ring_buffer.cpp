// test/test_lamp_ring_buffer/test_lamp_ring_buffer.cpp
#include <unity.h>
#include <deque>
#include "lamp_ring_buffer.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

void test_empty_buffer_reads_nothing(void) {
    RingBuffer<8> rb;
    int16_t out[4];
    TEST_ASSERT_EQUAL_UINT32(0, rb.available());
    TEST_ASSERT_EQUAL_UINT32(8, rb.freeSpace());
    TEST_ASSERT_EQUAL_UINT32(0, rb.read(out, 4));
}

void test_write_then_read_round_trips(void) {
    RingBuffer<8> rb;
    const int16_t in[3] = {100, -200, 300};
    TEST_ASSERT_EQUAL_UINT32(3, rb.write(in, 3));
    TEST_ASSERT_EQUAL_UINT32(3, rb.available());
    int16_t out[3] = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(3, rb.read(out, 3));
    TEST_ASSERT_EQUAL_INT16(100,  out[0]);
    TEST_ASSERT_EQUAL_INT16(-200, out[1]);
    TEST_ASSERT_EQUAL_INT16(300,  out[2]);
    TEST_ASSERT_EQUAL_UINT32(0, rb.available());
}

// 容量用满 N 个，不是 N-1。
void test_capacity_is_exactly_n(void) {
    RingBuffer<8> rb;
    int16_t in[8];
    for (int i = 0; i < 8; ++i) in[i] = (int16_t)i;
    TEST_ASSERT_EQUAL_UINT32(8, rb.write(in, 8));
    TEST_ASSERT_EQUAL_UINT32(0, rb.freeSpace());
    TEST_ASSERT_EQUAL_UINT32(0, rb.write(in, 1));     // 满了，写不进
}

void test_write_truncates_when_full(void) {
    RingBuffer<4> rb;
    const int16_t in[6] = {1, 2, 3, 4, 5, 6};
    TEST_ASSERT_EQUAL_UINT32(4, rb.write(in, 6));
    int16_t out[4];
    rb.read(out, 4);
    TEST_ASSERT_EQUAL_INT16(4, out[3]);               // 写进去的是前 4 个
}

void test_read_truncates_when_insufficient(void) {
    RingBuffer<8> rb;
    const int16_t in[2] = {7, 8};
    rb.write(in, 2);
    int16_t out[5] = {0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(2, rb.read(out, 5));
    TEST_ASSERT_EQUAL_INT16(0, out[2]);               // 多余的位置不动
}

void test_clear_empties(void) {
    RingBuffer<8> rb;
    const int16_t in[3] = {1, 2, 3};
    rb.write(in, 3);
    rb.clear();
    TEST_ASSERT_EQUAL_UINT32(0, rb.available());
    TEST_ASSERT_EQUAL_UINT32(8, rb.freeSpace());
}

// 模型对拍：拿 std::deque 当参考实现，随机 1000 次读写比对。
// 环绕、边界、available/freeSpace 一致性全在里面，而且能抓住逐点断言
// 看不见的状态腐蚀。
void test_matches_reference_model_under_random_ops(void) {
    RingBuffer<8> rb;
    std::deque<int16_t> ref;
    uint32_t rng = 12345;                             // 固定种子，可复现
    int16_t next_val = 0;

    for (int step = 0; step < 1000; ++step) {
        rng = rng * 1103515245u + 12345u;
        const uint32_t n = (rng >> 16) % 5;           // 0..4

        if ((rng >> 8) & 1) {                         // 写
            int16_t in[4];
            for (uint32_t i = 0; i < n; ++i) in[i] = next_val++;
            const uint32_t wrote = rb.write(in, n);
            const uint32_t expect = (8 - (uint32_t)ref.size()) < n
                                  ? (8 - (uint32_t)ref.size()) : n;
            TEST_ASSERT_EQUAL_UINT32(expect, wrote);
            for (uint32_t i = 0; i < wrote; ++i) ref.push_back(in[i]);
        } else {                                      // 读
            int16_t out[4];
            const uint32_t got = rb.read(out, n);
            const uint32_t expect = (uint32_t)ref.size() < n ? (uint32_t)ref.size() : n;
            TEST_ASSERT_EQUAL_UINT32(expect, got);
            for (uint32_t i = 0; i < got; ++i) {
                TEST_ASSERT_EQUAL_INT16(ref.front(), out[i]);
                ref.pop_front();
            }
        }
        TEST_ASSERT_EQUAL_UINT32((uint32_t)ref.size(), rb.available());
        TEST_ASSERT_EQUAL_UINT32(8 - (uint32_t)ref.size(), rb.freeSpace());
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_buffer_reads_nothing);
    RUN_TEST(test_write_then_read_round_trips);
    RUN_TEST(test_capacity_is_exactly_n);
    RUN_TEST(test_write_truncates_when_full);
    RUN_TEST(test_read_truncates_when_insufficient);
    RUN_TEST(test_clear_empties);
    RUN_TEST(test_matches_reference_model_under_random_ops);
    return UNITY_END();
}
