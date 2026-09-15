import Foundation

/// C 数组导入 Swift 会变成 N 元 tuple，没法下标遍历。用字段偏移重绑成
/// Float 缓冲 —— 偏移由 MemoryLayout 算，不是手抄的常数。
@inline(__always)
private func floatArray<T>(_ f: UnsafePointer<LampFrameC>,
                           _ kp: KeyPath<LampFrameC, T>, _ n: Int) -> UnsafeBufferPointer<Float> {
    let off = MemoryLayout<LampFrameC>.offset(of: kp)!
    return UnsafeBufferPointer(
        start: UnsafeRawPointer(f).advanced(by: off).assumingMemoryBound(to: Float.self), count: n)
}

/// 往字节缓冲里按小端、无对齐地写一个值。LAMP1 与 WLED V2 的包都是
/// packed 布局，storeBytes 那条路要求对齐，这里只能逐字节拷。
@inline(__always)
private func put<T>(_ v: T, _ dst: UnsafeMutableRawPointer, _ off: inout Int) {
    withUnsafeBytes(of: v) { src in
        dst.advanced(by: off).copyMemory(from: src.baseAddress!, byteCount: src.count)
    }
    off += MemoryLayout<T>.size
}

/// 两种包的编码器。
///
/// 布局与 `usermods/lamp/lamp_wled_fx.cpp` 的 `LampSyncPacket` 逐字节对应
/// （那边有 `static_assert(sizeof(...) == 341)`）。固件按**包长**分 v1/v2，
/// `version` 字段恒为 1 —— 见 lamp_wled_fx.cpp:772-776，改这里必须同步那边。
final class PacketEncoder: @unchecked Sendable {
    static let lamp1Size = 341
    static let wledV2Size = 44
    static let lamp1Port: UInt16 = 11989
    static let wledV2Port: UInt16 = 11988

    private var lamp1 = [UInt8](repeating: 0, count: PacketEncoder.lamp1Size)
    private var v2 = [UInt8](repeating: 0, count: PacketEncoder.wledV2Size)

    // WLED V2 兼容包的两个有状态量
    private var volEnv: Float = 0.02
    private var smoothed: Float = 0

    func encodeLamp1(_ f: UnsafePointer<LampFrameC>, mood: Float, valence: Float, emoID: UInt8) {
        let fr = f.pointee
        let flags: UInt8 =
            (fr.onset != 0 ? 1 : 0) | (fr.lock != 0 ? 2 : 0) | (fr.downbeat != 0 ? 4 : 0)
            | (fr.gate != 0 ? 8 : 0) | (fr.section != 0 ? 16 : 0) | (fr.vocal_onset != 0 ? 32 : 0)
            | (fr.f0_voiced != 0 ? 64 : 0) | (fr.key_major != 0 ? 128 : 0)

        lamp1.withUnsafeMutableBytes { raw in
            let d = raw.baseAddress!
            var o = 0
            for b in Array("LAMP1".utf8) { put(b, d, &o) }
            put(UInt8(0), d, &o)                       // magic 的结尾 NUL，共 6 字节
            put(UInt8(1), d, &o)                       // version：固件只认 1
            put(flags, d, &o)
            for buf in [floatArray(f, \.bands, 16), floatArray(f, \.bands_h, 16),
                        floatArray(f, \.bands_p, 16), floatArray(f, \.chroma, 12)] {
                for v in buf { put(v, d, &o) }
            }
            for v in [fr.rms, fr.peak, fr.gain, fr.bpm, fr.conf, fr.phase, fr.rate,
                      fr.centroid, fr.flatness, fr.key_conf, fr.harmony, fr.f0, fr.f0_conf,
                      mood, fr.trend, fr.novelty, fr.dynamics, fr.percussive, fr.bar_conf,
                      fr.vocal] { put(v, d, &o) }
            put(Int8(clamping: max(-1, min(11, fr.key_root))), d, &o)
            put(UInt8(clamping: max(0, min(255, fr.preset))), d, &o)
            put(UInt8(truncatingIfNeeded: fr.bpb), d, &o)
            put(UInt8(truncatingIfNeeded: fr.bar_pos), d, &o)
            put(fr.bar_index.littleEndian, d, &o)
            put(valence, d, &o)
            put(emoID, d, &o)
            assert(o == PacketEncoder.lamp1Size, "LAMP1 写了 \(o) 字节，应为 341")
        }
    }

    /// WLED 原生音频灯效（Gravimeter 等）吃的 44 字节包。
    ///
    /// rms→vol 必须按包络归一，不能用固定系数：liblamp 的 AGC 稳态随素材漂
    /// （实测 0.03→0.12），固定系数会恒饱和，原生效果就没有动态了。
    func encodeWledV2(_ f: UnsafePointer<LampFrameC>) {
        let fr = f.pointee
        volEnv = max(volEnv * 0.9995, fr.rms, 1e-4)
        let vol = min(max(fr.rms / volEnv * 220.0, 0), 255)
        smoothed = smoothed * 0.8 + vol * 0.2
        let bands = floatArray(f, \.bands, 16)
        let peakBand = (bands.max() ?? 0) * 1000.0
        // 主频：有基频就用基频，否则退到谱心
        let mp = (fr.f0_voiced != 0 && fr.f0 > 1) ? fr.f0 : min(max(fr.centroid, 1), 11025)

        v2.withUnsafeMutableBytes { raw in
            let d = raw.baseAddress!
            var o = 0
            for b in Array("00002".utf8) { put(b, d, &o) }
            put(UInt8(0), d, &o)                       // header 共 6 字节
            put(UInt16(0), d, &o)                      // 保留 2 字节
            put(vol, d, &o)
            put(smoothed, d, &o)
            put(UInt8(fr.onset != 0 ? 1 : 0), d, &o)
            put(UInt8(0), d, &o)
            // 16 个频段各压成一字节。kFxBandScale=6 是反向补偿，实测恰好满格。
            for v in bands { put(UInt8(min(max(v * 6.0 * 255.0, 0), 255)), d, &o) }
            put(UInt16(0), d, &o)
            put(peakBand, d, &o)
            put(mp, d, &o)
            assert(o == PacketEncoder.wledV2Size, "V2 写了 \(o) 字节，应为 44")
        }
    }

    func withLamp1<R>(_ body: (UnsafeRawPointer, Int) -> R) -> R {
        lamp1.withUnsafeBytes { body($0.baseAddress!, $0.count) }
    }
    func withWledV2<R>(_ body: (UnsafeRawPointer, Int) -> R) -> R {
        v2.withUnsafeBytes { body($0.baseAddress!, $0.count) }
    }
}
