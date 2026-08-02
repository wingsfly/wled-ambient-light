// usermods/lamp/lamp_ring_buffer.h
//
// 单生产者单消费者无锁环形缓冲，16 位单声道 PCM（设计文档 §2.3）。
// 生产者是音频驱动的回调，消费者是 AudioFeatures 的 FFT 任务 —— 两者不同线程，
// 所以用 acquire/release 而不是加锁：中断上下文里不能等锁。
//
// 索引用单调递增计数器再按掩码取址，所以能装满 N 个而不是 N-1 个。
// 计数器回绕后无符号差值依然正确，不需要特殊处理。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>

namespace lamp {

template <size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "容量必须是 2 的幂 —— 掩码取址依赖这一点");
    static_assert(N >= 2, "容量至少为 2");

  public:
    // 生产者侧。返回实际写入的样本数，缓冲满时截断。
    size_t write(const int16_t *src, size_t n) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t space = N - (h - t);
        if (n > space) n = space;
        for (size_t i = 0; i < n; ++i) buf_[(h + i) & (N - 1)] = src[i];
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // 消费者侧。返回实际读出的样本数，数据不足时截断。
    size_t read(int16_t *dst, size_t n) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t avail = h - t;
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(t + i) & (N - 1)];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    size_t available() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    size_t freeSpace() const { return N - available(); }

    // 仅在两侧都停止访问时调用（例如切换音频源之后）。并发调用不安全。
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

  private:
    int16_t buf_[N] = {};
    std::atomic<size_t> head_{0};   // 只由生产者写
    std::atomic<size_t> tail_{0};   // 只由消费者写
};

} // namespace lamp
