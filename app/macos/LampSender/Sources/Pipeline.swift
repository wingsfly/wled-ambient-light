import Foundation
import SwiftUI
import Synchronization

/// 跨线程的停止标志与增益旋钮。
///
/// 工作线程不能去读 `@MainActor` 的属性（`MainActor.assumeIsolated` 在别的
/// 线程上是直接断言失败，不是回退），所以这两个值单独装在能跨线程读的盒子里。
final class ControlBox: @unchecked Sendable {
    private let stop = Atomic<Bool>(false)
    private let gainBits = Atomic<UInt32>(Float(1.0).bitPattern)

    var shouldStop: Bool { stop.load(ordering: .relaxed) }
    func requestStop() { stop.store(true, ordering: .relaxed) }

    var gain: Float {
        get { Float(bitPattern: gainBits.load(ordering: .relaxed)) }
        set { gainBits.store(newValue.bitPattern, ordering: .relaxed) }
    }
}

/// CLAP 的最新一次读数，发包线程每帧都要读。
///
/// 带过期时间：CLAP 每 4 秒推一次，超过 12 秒还没有新值就当它没有了 ——
/// 宁可退回纯 liblamp 的 mood，也不要把几分钟前那首歌的情绪一直发下去。
final class MoodBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: MoodReading?
    private var updatedAt = Date.distantPast
    private static let staleAfter: TimeInterval = 12

    var current: MoodReading? {
        lock.lock(); defer { lock.unlock() }
        return Date().timeIntervalSince(updatedAt) < Self.staleAfter ? value : nil
    }
    func set(_ v: MoodReading?) {
        lock.lock(); value = v; updatedAt = Date(); lock.unlock()
    }
}

/// 把采集、分析、发包串起来。
///
/// 四段各在自己该在的线程上：
///   [HAL 实时线程] tap IOProc → 转单声道 → 写无锁环
///   [工作线程]     读环 → 重采样 48k→22.05k → liblamp → 打包 → UDP
///                        └→ 原始 48k 也喂给 CLAP 的 10 秒滑动窗口
///   [CLAP 线程]    每 4 秒取一次窗口快照 → Core ML → 情绪读数
///   [主线程]       每秒收一次统计，刷菜单栏
@MainActor
final class Pipeline: ObservableObject {
    /// AppDelegate 要在启动时拿到同一个实例来自动开跑，所以给它一个单例入口。
    static let shared = Pipeline()

    struct Stats: Sendable {
        var packetsPerSec = 0
        var onsetsPerSec = 0
        var rms: Float = 0
        var mood: Float = 0
        var bpm: Float = 0
        var beatLocked = false
        var silent = false          // liblamp 的静音门；静音时**停发**，让板子回落本地音源
        var targetIP: String?
        var localIP: String?
        var droppedSamples = 0
        var clap: ClapStatus = .loading
        var moodReading: MoodReading?
    }

    enum ClapStatus: Sendable, Equatable {
        case loading                // 模型还在后台加载
        case unavailable(String)    // 模型没打包或加载失败 —— 明示降级，不静默
        case ready
    }

    @Published private(set) var running = false
    @Published private(set) var stats = Stats()
    @Published private(set) var lastError: String?
    @Published private(set) var tapSampleRate: Double = 0

    @Published var targetSpec: String { didSet { defaults.set(targetSpec, forKey: "targetSpec") } }
    @Published var inputGain: Double {
        didSet { defaults.set(inputGain, forKey: "inputGain"); control?.gain = Float(inputGain) }
    }
    /// 常驻工具的默认姿态就该是「起来就干活」—— Python 版由 launchd 拉起即跑，
    /// 换成菜单栏 App 不该反而多一次手点。
    @Published var autoStart: Bool { didSet { defaults.set(autoStart, forKey: "autoStart") } }
    @Published var clapEnabled: Bool {
        didSet { defaults.set(clapEnabled, forKey: "clapEnabled") }
    }
    /// 期望值存在 UserDefaults，实际状态由 SMAppService 说了算 —— 两者可能
    /// 不一致（系统挂起等批准、app 被挪走），启动时对一次账。
    @Published private(set) var launchAtLogin = false
    @Published private(set) var loginItemNeedsApproval = false

    private let defaults = UserDefaults.standard
    private var control: ControlBox?

    init() {
        // 默认值不该指向某一台具体的灯 —— 这是个公开仓库，别人 clone 下来
        // 不该默认往作者的设备发包。wled.local 是 WLED 的通用名；解析不到就
        // 退回组播，同网段照样送得到，跨网段再手填地址。
        targetSpec = defaults.string(forKey: "targetSpec") ?? "wled.local"
        inputGain = defaults.object(forKey: "inputGain") as? Double ?? 1.0
        autoStart = defaults.object(forKey: "autoStart") as? Bool ?? true
        clapEnabled = defaults.object(forKey: "clapEnabled") as? Bool ?? true
    }

    func startIfAutoStart() { if autoStart { start() } }

    /// 启动时调一次：把系统里的真实状态读回来，并补上「想开但还没注册」的情况。
    func syncLoginItem() {
        let want = defaults.object(forKey: "launchAtLogin") as? Bool ?? false
        // 条件不能只看 .notRegistered —— 实测新装的 app 报 .notFound，
        // 那样会被跳过，静默地不注册。任何非 enabled 状态都该重试。
        if want && !LoginItem.isEnabled {
            do {
                try LoginItem.set(true)
                defaults.removeObject(forKey: "launchAtLoginError")
            } catch {
                lastError = "注册开机自启失败：\(error.localizedDescription)"
                defaults.set(error.localizedDescription, forKey: "launchAtLoginError")
            }
        }
        refreshLoginItem()
    }

    func setLaunchAtLogin(_ on: Bool) {
        defaults.set(on, forKey: "launchAtLogin")
        do { try LoginItem.set(on); defaults.removeObject(forKey: "launchAtLoginError") }
        catch {
            lastError = "设置开机自启失败：\(error.localizedDescription)"
            defaults.set(error.localizedDescription, forKey: "launchAtLoginError")
        }
        refreshLoginItem()
    }

    /// CLAP 的状态只在菜单栏里看得到，ssh 下没辙 —— 顺手写进 UserDefaults，
    /// `defaults read com.hjma.lamp.sender clapStatus` 就能查。
    private func setClapStatus(_ st: ClapStatus) {
        stats.clap = st
        let text: String
        switch st {
        case .loading:            text = "loading"
        case .ready:              text = "ready"
        case .unavailable(let w): text = "unavailable: \(w)"
        }
        defaults.set(text, forKey: "clapStatus")
    }

    private func refreshLoginItem() {
        launchAtLogin = LoginItem.isEnabled
        loginItemNeedsApproval = LoginItem.needsApproval
        defaults.set(LoginItem.statusText, forKey: "launchAtLoginStatus")
    }

    func toggle() { running ? stop() : start() }

    func start() {
        guard !running else { return }
        lastError = nil
        let ring = AudioRing()
        let tap = AudioTap(ring: ring)
        do { try tap.start() } catch {
            lastError = "\(error)"
            return
        }
        guard let engine = LampEngine() else {
            tap.stop(); lastError = "liblamp 初始化失败（布局自检不过或内存不足）"; return
        }
        guard let resampler = Resampler(from: tap.format.sampleRate, to: engine.sampleRate) else {
            tap.stop(); lastError = "重采样器建不起来（\(tap.format.sampleRate) → \(engine.sampleRate)）"; return
        }
        let box = ControlBox()
        box.gain = Float(inputGain)
        control = box
        tapSampleRate = tap.format.sampleRate
        running = true
        setClapStatus(clapEnabled ? .loading : .unavailable("已在设置里关闭"))

        let mood = MoodBox()
        let window = SlidingWindow(capacity: ClapEngine.windowSamples)
        if clapEnabled { startClapWorker(window: window, mood: mood, control: box) }

        // 这两个值必须在进线程闭包**之前**取出来：它们是 @MainActor 隔离的，
        // 在 Sendable 闭包里读到 Swift 6 就是编译错误，不只是警告。
        let spec = targetSpec
        let clapWindow: SlidingWindow? = clapEnabled ? window : nil
        let t = Thread { [self] in
            Pipeline.run(ring: ring, tap: tap, engine: engine, resampler: resampler,
                         targetSpec: spec, control: box,
                         clapWindow: clapWindow, mood: mood) { s in
                Task { @MainActor in
                    var merged = s
                    merged.clap = self.stats.clap      // CLAP 的加载状态归 CLAP 线程管
                    self.stats = merged
                }
            }
        }
        t.name = "lamp.sender.worker"
        t.qualityOfService = .userInitiated
        t.start()
    }

    func stop() {
        guard running else { return }
        control?.requestStop()
        control = nil
        running = false
        stats = Stats()
        setClapStatus(.loading)
    }

    /// CLAP 模型 126 MB，加载要时间 —— 放后台，绝不挡住音频链路起步。
    /// 加载不出来就是不出来，退回纯 liblamp 的 mood，并在菜单栏说明白。
    private func startClapWorker(window: SlidingWindow, mood: MoodBox, control: ControlBox) {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let clap = ClapEngine() else {
                Task { @MainActor in self?.setClapStatus(.unavailable("模型没打包或加载失败")) }
                return
            }
            Task { @MainActor in self?.setClapStatus(.ready) }

            let scratch = UnsafeMutablePointer<Float>.allocate(capacity: ClapEngine.windowSamples)
            defer { scratch.deallocate() }
            var nextRun = Date()
            while !control.shouldStop {
                // 4 秒一次，与 Python 版同频。窗口没填满（刚启动不到 10 秒）就先等着。
                if Date() < nextRun || !window.snapshot(into: scratch) {
                    Thread.sleep(forTimeInterval: 0.25); continue
                }
                nextRun = Date().addingTimeInterval(4)
                let reading = clap.analyze(scratch)
                mood.set(reading)
                Task { @MainActor in self?.stats.moodReading = reading }
            }
        }
    }

    // 工作线程主体。静态且 nonisolated —— 不碰任何 @MainActor 状态。
    private nonisolated static func run(ring: AudioRing, tap: AudioTap, engine: LampEngine,
                                        resampler: Resampler, targetSpec: String,
                                        control: ControlBox,
                                        clapWindow: SlidingWindow?, mood: MoodBox,
                                        publish: @escaping @Sendable (Stats) -> Void) {
        let sender = UDPSender(targetSpec: targetSpec)
        let enc = PacketEncoder()
        let chunkMax = 4096
        let scratch = UnsafeMutablePointer<Float>.allocate(capacity: chunkMax)
        defer { scratch.deallocate(); tap.stop() }

        var acc = Stats()
        var packets = 0, onsets = 0
        var lastPublish = Date()
        var appliedGain = Float.nan

        while !control.shouldStop {
            let g = control.gain
            if g != appliedGain { engine.setInputGain(g); appliedGain = g }

            let n = ring.read(into: scratch, max: chunkMax)
            if n == 0 { Thread.sleep(forTimeInterval: 0.004); continue }

            // CLAP 要 48 kHz 原始流，在重采样**之前**分一路出去
            clapWindow?.append(scratch, n)

            guard let resampled = resampler.process(scratch, n),
                  let head = resampled.baseAddress else { continue }

            let reading = mood.current
            engine.feed(head, resampled.count) { f in
                let fr = f.pointee
                acc.rms = fr.rms; acc.bpm = fr.bpm
                acc.beatLocked = fr.lock != 0
                acc.silent = fr.gate != 0
                if fr.onset != 0 { onsets += 1 }

                // 静音时不发包 —— 常驻的 sender 如果一直发静音帧，会永久遮蔽
                // 板子的本地麦克风。停发 500 ms 后固件自己回落（kRemoteFreshMs）。
                guard fr.gate == 0 else { return }

                // CLAP 语义融合：mood = 管线的能量近似与 CLAP 的激活度各半，
                // 与 Python 版同式 —— 慢语义修正快近似。CLAP 没读数时 mood
                // 保持纯管线值，valence/emo_id 退回固件给 v1 包的缺省。
                var moodValue = fr.mood
                if let r = reading { moodValue = 0.5 * fr.mood + 0.5 * r.energy }
                acc.mood = moodValue
                enc.encodeLamp1(f, mood: moodValue,
                                valence: reading?.valence ?? 0,
                                emoID: reading?.emoID ?? 255)
                enc.encodeWledV2(f)
                enc.withLamp1 { p, l in sender.send(p, l, port: PacketEncoder.lamp1Port) }
                enc.withWledV2 { p, l in sender.send(p, l, port: PacketEncoder.wledV2Port) }
                packets += 1
            }

            sender.tick()
            let now = Date()
            if now.timeIntervalSince(lastPublish) >= 1.0 {
                acc.packetsPerSec = packets; acc.onsetsPerSec = onsets
                acc.targetIP = sender.targetIP; acc.localIP = sender.localIP
                acc.droppedSamples = ring.dropped
                acc.moodReading = reading
                publish(acc)
                packets = 0; onsets = 0
                lastPublish = now
            }
        }
    }
}
