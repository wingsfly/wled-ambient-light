import Foundation
import AVFoundation

/// 48 kHz（tap 的原生率）→ 22.05 kHz（liblamp 的 kSampleRate）。
///
/// 用 AVAudioConverter 而不是自己做线性插值：降采样必须先抗混叠低通，
/// 否则 11 kHz 以上的能量会折回来污染频段分析。Python 版没这个问题 ——
/// 它直接向 CoreAudio 要 22050 Hz 的输入流，重采样是系统在驱动层做的；
/// tap 给的是设备原生率，这一步得自己补上。
final class Resampler: @unchecked Sendable {
    private let converter: AVAudioConverter
    private let inFormat: AVAudioFormat
    private let inBuffer: AVAudioPCMBuffer
    private let outBuffer: AVAudioPCMBuffer
    let ratio: Double

    init?(from inRate: Double, to outRate: Double, maxInputFrames: Int = 8192) {
        guard let fin = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: inRate,
                                      channels: 1, interleaved: false),
              let fout = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: outRate,
                                       channels: 1, interleaved: false),
              let conv = AVAudioConverter(from: fin, to: fout),
              let bin = AVAudioPCMBuffer(pcmFormat: fin, frameCapacity: AVAudioFrameCount(maxInputFrames))
        else { return nil }
        ratio = outRate / inRate
        // 留两帧余量：转换器内部有滤波延迟，输出帧数会在比例附近浮动
        let outCap = Int(Double(maxInputFrames) * ratio) + 32
        guard let bout = AVAudioPCMBuffer(pcmFormat: fout, frameCapacity: AVAudioFrameCount(outCap))
        else { return nil }
        conv.sampleRateConverterQuality = AVAudioQuality.max.rawValue
        converter = conv; inFormat = fin; inBuffer = bin; outBuffer = bout
    }

    /// 返回输出缓冲的只读视图，有效长度是返回的 count。缓冲复用，
    /// 调用方必须在下一次 process 之前用完。
    func process(_ src: UnsafePointer<Float>, _ count: Int) -> UnsafeBufferPointer<Float>? {
        guard count > 0, count <= Int(inBuffer.frameCapacity),
              let dst = inBuffer.floatChannelData?[0] else { return nil }
        dst.update(from: src, count: count)
        inBuffer.frameLength = AVAudioFrameCount(count)
        outBuffer.frameLength = 0

        var err: NSError?
        var delivered = false
        let status = converter.convert(to: outBuffer, error: &err) { [inBuffer] _, outStatus in
            if delivered { outStatus.pointee = .noDataNow; return nil }
            delivered = true
            outStatus.pointee = .haveData
            return inBuffer
        }
        guard status != .error, outBuffer.frameLength > 0,
              let out = outBuffer.floatChannelData?[0] else { return nil }
        return UnsafeBufferPointer(start: out, count: Int(outBuffer.frameLength))
    }
}
