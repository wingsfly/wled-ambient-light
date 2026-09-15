import Foundation

/// liblamp 的 Swift 封装。跑的是 usermods/lamp 那批头文件本身 —— 与固件、
/// 与模拟器、与 Python 版 sender 同一份 C++ 源码，分析结果逐位一致。
final class LampEngine: @unchecked Sendable {
    private let handle: UnsafeMutableRawPointer
    private let frames: UnsafeMutablePointer<LampFrameC>
    private let maxFrames: Int

    init?(maxFrames: Int = 8) {
        // 布局自检：头文件与链接进来的实现必须同尺寸。静态链接下几乎不可能
        // 不一致，但这行的代价是零，而错位读出来的是垃圾数而不是崩溃。
        guard Int(lamp_frame_size()) == MemoryLayout<LampFrameC>.size else { return nil }
        guard let h = lamp_create() else { return nil }
        handle = h
        self.maxFrames = maxFrames
        frames = .allocate(capacity: maxFrames)
    }
    deinit { lamp_destroy(handle); frames.deallocate() }

    var sampleRate: Double { Double(lamp_sample_rate()) }

    /// 送进管线前的手动增益，补偿音源电平差异。
    func setInputGain(_ g: Float) { lamp_set_input_gain(handle, g) }

    /// 喂一批样本，对产出的每一帧调用 body。样本按 hop 步进消费，
    /// 剩余的留在 liblamp 内部的环里 —— 调用方不必按帧长对齐。
    func feed(_ pcm: UnsafePointer<Float>, _ count: Int, _ body: (UnsafePointer<LampFrameC>) -> Void) {
        var consumed = 0
        while consumed < count {
            let chunk = min(count - consumed, 4096)
            let n = Int(lamp_feed(handle, pcm.advanced(by: consumed), Int32(chunk), frames, Int32(maxFrames)))
            for i in 0..<n { body(frames.advanced(by: i)) }
            consumed += chunk
            // 一批喂完可能还攒着够几帧的料，但 lamp_feed 已经在内部循环到
            // max_out 为止；这里按 chunk 推进即可，不会漏帧。
        }
    }
}
