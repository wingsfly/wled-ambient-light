import Foundation
import Synchronization

/// 单生产者单消费者的无锁环。
///
/// 写方是 Core Audio 的实时 IOProc 线程 —— 那里不能加锁、不能分配内存、
/// 不能做系统调用。Python 版的 sender 是在音频回调里直接跑完 FFT 再
/// sendto 的（GIL 下本来就不实时，不算错），原生版没有这个借口：
/// 回调只做「转单声道 + 写环」，重采样、分析、发包全在工作线程。
final class AudioRing: @unchecked Sendable {
    private let mask: Int
    private let buf: UnsafeMutablePointer<Float>
    private let w = Atomic<Int>(0)          // 只有生产者写
    private let r = Atomic<Int>(0)          // 只有消费者写
    private let dropCount = Atomic<Int>(0)

    /// capacity 必须是 2 的幂 —— 取模换成掩码，实时线程里少一次除法。
    init(capacity: Int = 1 << 16) {
        precondition(capacity > 0 && capacity & (capacity - 1) == 0, "容量必须是 2 的幂")
        mask = capacity - 1
        buf = .allocate(capacity: capacity)
        buf.initialize(repeating: 0, count: capacity)
    }
    deinit { buf.deallocate() }

    var dropped: Int { dropCount.load(ordering: .relaxed) }

    /// 实时线程调用。环满时丢弃**新**数据并计数 —— SPSC 下生产者不能动
    /// 读下标，所以丢不了旧的。正常情况下工作线程远快于 48 kHz 的来料，
    /// 满环意味着工作线程卡住了，那时丢什么都一样，重要的是不阻塞 HAL。
    func write(_ src: UnsafePointer<Float>, _ count: Int) {
        let wi = w.load(ordering: .relaxed)
        let ri = r.load(ordering: .acquiring)
        let free = mask - (wi &- ri)          // 留一格区分空与满
        let n = min(count, free)
        if n < count { dropCount.wrappingAdd(count - n, ordering: .relaxed) }
        guard n > 0 else { return }
        let start = wi & mask
        let first = min(n, mask + 1 - start)
        buf.advanced(by: start).update(from: src, count: first)
        if first < n { buf.update(from: src.advanced(by: first), count: n - first) }
        w.store(wi &+ n, ordering: .releasing)
    }

    /// 工作线程调用，返回实际取到的样本数。
    func read(into dst: UnsafeMutablePointer<Float>, max maxCount: Int) -> Int {
        let ri = r.load(ordering: .relaxed)
        let wi = w.load(ordering: .acquiring)
        let n = min(maxCount, wi &- ri)
        guard n > 0 else { return 0 }
        let start = ri & mask
        let first = min(n, mask + 1 - start)
        dst.update(from: buf.advanced(by: start), count: first)
        if first < n { dst.advanced(by: first).update(from: buf, count: n - first) }
        r.store(ri &+ n, ordering: .releasing)
        return n
    }
}
