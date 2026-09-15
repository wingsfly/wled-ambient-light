import Foundation

/// 保留最近 N 秒的滑动窗口，给 CLAP 用。
///
/// 和 `AudioRing` 不是一回事：那个是消费即走的队列，读过就没了；CLAP 要的是
/// 「此刻往前数 10 秒」的完整快照，每 4 秒取一次，取完样本还得留着。
///
/// 写方是工作线程（不是实时 IOProc），读方是 CLAP 的推理队列 —— 都不在实时
/// 线程上，用锁没有代价。
final class SlidingWindow: @unchecked Sendable {
    private let capacity: Int
    private let buf: UnsafeMutablePointer<Float>
    private var writePos = 0
    private var filled = 0
    private let lock = NSLock()

    init(capacity: Int) {
        self.capacity = capacity
        buf = .allocate(capacity: capacity)
        buf.initialize(repeating: 0, count: capacity)
    }
    deinit { buf.deallocate() }

    func append(_ src: UnsafePointer<Float>, _ count: Int) {
        guard count > 0 else { return }
        lock.lock(); defer { lock.unlock() }
        // 来料比窗口还长时只留最后 capacity 个 —— 前面的反正会被覆盖掉
        let n = min(count, capacity)
        let src = src.advanced(by: count - n)
        let first = min(n, capacity - writePos)
        buf.advanced(by: writePos).update(from: src, count: first)
        if first < n { buf.update(from: src.advanced(by: first), count: n - first) }
        writePos = (writePos + n) % capacity
        filled = min(filled + n, capacity)
    }

    /// 按时间顺序拷出整个窗口。窗口还没填满就返回 false —— CLAP 拿半截
    /// 补零的音频会得出偏冷的情绪，不如不推。
    func snapshot(into dst: UnsafeMutablePointer<Float>) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard filled >= capacity else { return false }
        let tail = capacity - writePos
        dst.update(from: buf.advanced(by: writePos), count: tail)
        if writePos > 0 { dst.advanced(by: tail).update(from: buf, count: writePos) }
        return true
    }
}
