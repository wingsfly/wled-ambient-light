import Foundation
import CoreAudio
import AudioToolbox

/// Core Audio Process Tap：抓系统正在播放的音频。
///
/// 这是原生版相对 Python 版最大的改变 —— Python 版要装 BlackHole 虚拟声卡，
/// 还要把系统输出切到「多输出设备」才能既听见又抓到。Tap 不需要任何一样：
/// 不装驱动、不改默认输出、对正在播放的 App 完全透明。
///
/// 两个非显然的坑（2026-09-15 实测，见 tap 验证记录）：
///  1. `stereoMixdownOfProcesses([])` 的参数是**要包含**的进程，空数组等于
///     「混合零个进程」—— tap 建得成、格式正常、Start 返回 noErr，但 IOProc
///     一次都不回调。全系统必须用 `stereoGlobalTapButExcludeProcesses([])`。
///  2. 进程必须归属图形会话。同一个签名 .app 的同一个二进制，`open` 起来
///     能拿到音频，ssh 里直接执行则回调正常、**数据全是 0**，不报任何错。
final class AudioTap: @unchecked Sendable {
    struct Format { let sampleRate: Double; let channels: Int }

    private(set) var format = Format(sampleRate: 48000, channels: 2)
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggID = AudioObjectID(kAudioObjectUnknown)
    private var procID: AudioDeviceIOProcID?
    private let ring: AudioRing
    private let scratch: UnsafeMutablePointer<Float>
    private let scratchCap = 16384

    init(ring: AudioRing) {
        self.ring = ring
        scratch = .allocate(capacity: scratchCap)
        scratch.initialize(repeating: 0, count: scratchCap)
    }
    deinit { stop(); scratch.deallocate() }

    enum TapError: Error, CustomStringConvertible {
        case osStatus(String, OSStatus)
        var description: String {
            guard case let .osStatus(what, s) = self else { return "" }
            let fourCC = withUnsafeBytes(of: s.bigEndian) { b in String(bytes: b, encoding: .ascii) ?? "" }
            return "\(what) 失败：OSStatus=\(s) '\(fourCC)'"
        }
    }
    private func check(_ s: OSStatus, _ what: String) throws {
        if s != noErr { throw TapError.osStatus(what, s) }
    }

    func start() throws {
        stop()
        let desc = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        desc.uuid = UUID()
        desc.name = "LampSender"
        desc.muteBehavior = .unmuted          // 不影响用户听到的声音
        desc.isPrivate = true                 // 不出现在别的 App 的设备列表里
        try check(AudioHardwareCreateProcessTap(desc, &tapID), "创建音频 tap")

        var asbd = AudioStreamBasicDescription()
        var sz = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        var fa = AudioObjectPropertyAddress(mSelector: kAudioTapPropertyFormat,
                                            mScope: kAudioObjectPropertyScopeGlobal,
                                            mElement: kAudioObjectPropertyElementMain)
        if AudioObjectGetPropertyData(tapID, &fa, 0, nil, &sz, &asbd) == noErr, asbd.mSampleRate > 0 {
            format = Format(sampleRate: asbd.mSampleRate, channels: Int(asbd.mChannelsPerFrame))
        }

        // tap-only aggregate：不挂任何真实子设备，tap 自己提供时钟。
        let agg: [String: Any] = [
            kAudioAggregateDeviceNameKey: "LampSender Tap",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceSubDeviceListKey: [[String: Any]](),
            kAudioAggregateDeviceTapListKey: [[
                kAudioSubTapDriftCompensationKey: true,
                kAudioSubTapUIDKey: desc.uuid.uuidString,
            ]],
        ]
        try check(AudioHardwareCreateAggregateDevice(agg as CFDictionary, &aggID), "创建聚合设备")

        let ring = self.ring, scratch = self.scratch, cap = self.scratchCap
        try check(AudioDeviceCreateIOProcIDWithBlock(&procID, aggID, nil) { _, inData, _, _, _ in
            // ↓↓ 实时线程：不分配、不加锁、不打日志 ↓↓
            let abl = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inData))
            for b in abl {
                guard let d = b.mData else { continue }
                let ch = max(1, Int(b.mNumberChannels))
                let total = Int(b.mDataByteSize) / MemoryLayout<Float>.size
                let frames = min(total / ch, cap)
                guard frames > 0 else { continue }
                let f = d.assumingMemoryBound(to: Float.self)
                if ch == 1 {
                    ring.write(f, frames)
                } else {
                    // 交错多声道 → 单声道均值，与 Python 版的 indata.mean(axis=1) 同口径
                    let inv = 1.0 / Float(ch)
                    for i in 0..<frames {
                        var s: Float = 0
                        for c in 0..<ch { s += f[i * ch + c] }
                        scratch[i] = s * inv
                    }
                    ring.write(scratch, frames)
                }
            }
        }, "安装 IOProc")

        guard let pid = procID else { throw TapError.osStatus("IOProc 为空", -1) }
        try check(AudioDeviceStart(aggID, pid), "启动采集")
    }

    func stop() {
        if let pid = procID, aggID != kAudioObjectUnknown {
            AudioDeviceStop(aggID, pid)
            AudioDeviceDestroyIOProcID(aggID, pid)
        }
        procID = nil
        if aggID != kAudioObjectUnknown { AudioHardwareDestroyAggregateDevice(aggID); aggID = AudioObjectID(kAudioObjectUnknown) }
        if tapID != kAudioObjectUnknown { AudioHardwareDestroyProcessTap(tapID); tapID = AudioObjectID(kAudioObjectUnknown) }
    }
}
