// test/test_lamp_ring_buffer_tsan/test_lamp_ring_buffer_tsan.cpp
//
// ⚠️ 读之前先看清这个测试**不是**在证明什么。
//
// 下面只有两条断言（总数对得上、序列没错位）。这两条断言证明不了 RingBuffer 的
// 内存序是对的 —— 已经实测过：把 lamp_ring_buffer.h 里的 acquire/release 全换成
// relaxed，在 arm64（真弱内存序）上跑 2000 万样本 × 3 轮，业务断言同样 0 错。
// 结果导向的并发压测对内存序**没有判别力**，加多少条断言都一样。
//
// 这个文件真正的作用是：给 ThreadSanitizer 制造一次真并发。
// TSan 只在两个线程**实际**冲突访问同一地址、且两次访问之间没有 happens-before
// 边时才报 race。项目里其它测试全是单线程，TSan 对正确版和错误版都会保持沉默 ——
// 所以「加个 sanitizer 环境」这半步单独做没有意义，必须配一个真起线程的测试。
//
// 判别力的证据不在断言里，在 TSan 的输出里：
//   原版        + TSan → 干净
//   全 relaxed  + TSan → 报 buf_ 上的 data race（而且业务断言依旧 0 错）
//
// 因此本测试被 platformio_override.ini 的 [env:native] test_ignore 排除。
// 不带 TSan 跑它 = 上面那种没有判别力的压测，只会制造虚假安全感。
//
// 跑法：pio test -e native_tsan -v
//   ⚠️ 那个 -v 是必须的。TSan 的报告不符合 Unity 的行格式，PlatformIO 的
//   unity runner 在非 verbose 下只回显能解析成用例结果的行（runners/unity.py
//   的 on_testing_line_output），报告会被丢掉，只剩一句
//   "Program received signal SIGABRT"。测试状态仍然会正确地变成 ERRORED
//   （TSan 在 macOS 上默认 abort_on_error=1），但想看到底哪一行出问题必须加 -v。
#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <thread>

#include "lamp_ring_buffer.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

namespace {

// 缓冲开得小，是为了让同一批槽位被高频回收：TSan 的影子内存每个 8 字节粒度
// 只保留最近几次访问，槽位回收得越快，两个线程撞上同一地址的机会越密集。
constexpr size_t kCap   = 64;
constexpr size_t kTotal = 200000;   // 总样本数
constexpr size_t kChunk = 16;       // 单次读写上限

RingBuffer<kCap> g_rb;

// 消费者线程的观测结果。join() 提供 happens-before，主线程读它是安全的。
size_t g_consumed   = 0;
size_t g_mismatches = 0;

// 让每次读写的长度在 1..kChunk 之间抖动，使 head/tail 落在各种非对齐偏移上，
// 生产者与消费者更容易同时停在相邻甚至同一个槽位附近。
inline size_t jitter(uint32_t &s, size_t cap) {
    s = s * 1103515245u + 12345u;
    return 1 + (s >> 16) % cap;
}

void producer() {
    int16_t chunk[kChunk];
    uint32_t rng = 0xC0FFEEu;
    size_t sent = 0;
    while (sent < kTotal) {
        size_t want = jitter(rng, kChunk);
        if (want > kTotal - sent) want = kTotal - sent;
        for (size_t i = 0; i < want; ++i) chunk[i] = (int16_t)((sent + i) & 0xFFFF);
        const size_t wrote = g_rb.write(chunk, want);
        if (wrote == 0) {                 // 满了，让出一次再试
            std::this_thread::yield();
            continue;
        }
        sent += wrote;
    }
}

void consumer() {
    int16_t chunk[kChunk];
    uint32_t rng = 0x5EEDu;
    size_t got = 0, bad = 0;
    while (got < kTotal) {
        const size_t n = g_rb.read(chunk, jitter(rng, kChunk));
        if (n == 0) {                     // 空了，让出一次再试
            std::this_thread::yield();
            continue;
        }
        // 无丢失 / 无重复 / 无乱序：第 k 个样本必须等于 (int16_t)(k & 0xFFFF)。
        for (size_t i = 0; i < n; ++i) {
            if (chunk[i] != (int16_t)((got + i) & 0xFFFF)) ++bad;
        }
        got += n;
    }
    g_consumed   = got;
    g_mismatches = bad;
}

} // namespace

// 断言刻意保持极简 —— 见文件头：它们不是这个测试的产出，TSan 的报告才是。
void test_spsc_producer_consumer_under_sanitizer(void) {
    std::thread p(producer);
    std::thread c(consumer);
    p.join();
    c.join();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)kTotal, (uint32_t)g_consumed);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)g_mismatches);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_spsc_producer_consumer_under_sanitizer);
    return UNITY_END();
}
