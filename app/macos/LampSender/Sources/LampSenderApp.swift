import SwiftUI
import LampKit

/// 菜单栏 App 没有窗口，`onAppear` 要等用户点开菜单才触发 —— 自动开跑得挂在
/// 应用启动上。
final class AppDelegate: NSObject, NSApplicationDelegate {
    /// 同一个 bundle ID 但装在不同路径下（比如 build/ 里的和 ~/Applications 里
    /// 的），LaunchServices 不会拦，会实打实跑起两个进程 —— 两份都在往
    /// 11989/11988 发 LAMP1，板子收到交错的帧。实测踩过一次。
    private func terminateIfAlreadyRunning() -> Bool {
        let me = ProcessInfo.processInfo.processIdentifier
        let others = NSRunningApplication
            .runningApplications(withBundleIdentifier: Bundle.main.bundleIdentifier ?? "")
            .filter { $0.processIdentifier != me }
        guard let existing = others.first else { return false }
        existing.activate()
        NSApp.terminate(nil)
        return true
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if terminateIfAlreadyRunning() { return }
        MainActor.assumeIsolated {
            Pipeline.shared.syncLoginItem()
            Pipeline.shared.startIfAutoStart()
        }

    }
}

@main
struct LampSenderApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var pipeline = Pipeline.shared

    var body: some Scene {
        // 控制台窗口。菜单栏管的是音频面（这台 Mac 往灯发什么），窗口管的是
        // 控制面（灯本身怎么亮）—— 两件事，两个入口。
        Window("氛围灯控制台", id: ConsoleWindow.id) {
            ConsoleView()
        }
        .defaultSize(width: 900, height: 600)
        // 菜单栏 App 平时不该弹窗口，但「开机就要看到控制台」是个合理诉求，
        // 远程验证也够不着菜单。留一个开关：
        //   defaults write com.hjma.lamp.sender openConsoleOnLaunch -bool true
        .defaultLaunchBehavior(
            UserDefaults.standard.bool(forKey: "openConsoleOnLaunch") ? .presented : .suppressed)

        MenuBarExtra {
            MenuContent(pipeline: pipeline)
        } label: {
            Image(systemName: pipeline.running
                  ? (pipeline.stats.silent ? "waveform.badge.exclamationmark" : "waveform")
                  : "waveform.slash")
        }
        .menuBarExtraStyle(.window)
    }
}

enum ConsoleWindow { static let id = "console" }

struct MenuContent: View {
    @ObservedObject var pipeline: Pipeline
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            Divider()
            if let err = pipeline.lastError { errorRow(err) }
            if pipeline.running { liveStats } else { idleHint }
            Divider()
            settings
            Divider()
            footer
        }
        .padding(12)
        .frame(width: 300)
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("台灯音频同步").font(.headline)
                Text(statusText).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Button(pipeline.running ? "停止" : "启动") { pipeline.toggle() }
                .keyboardShortcut(pipeline.running ? "s" : "r")
        }
    }

    private var statusText: String {
        guard pipeline.running else { return "已停止" }
        if pipeline.stats.silent { return "静音 — 已让位板子的本地音源" }
        return "运行中 · 抓系统播放音频"
    }

    private func errorRow(_ err: String) -> some View {
        Text(err)
            .font(.caption).foregroundStyle(.red)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var idleHint: some View {
        Text("不需要 BlackHole，也不会改动系统输出设备 —— 用 Core Audio 的进程 tap 直接抓当前播放的声音。")
            .font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var liveStats: some View {
        let s = pipeline.stats
        return VStack(alignment: .leading, spacing: 4) {
            row("发包", "\(s.packetsPerSec)/秒" + (s.silent ? "（静音停发）" : ""))
            row("音量", String(format: "RMS %.4f", s.rms))
            row("节拍", s.bpm > 1 ? String(format: "%.1f BPM%@", s.bpm, s.beatLocked ? " · 已锁定" : "") : "—")
            row("情绪", moodText)
            row("目标", s.targetIP ?? "解析不到")
            row("出口", s.localIP ?? "—")
            if pipeline.tapSampleRate > 0 {
                row("采集", String(format: "%.0f Hz → 22050 Hz", pipeline.tapSampleRate))
            }
            if s.droppedSamples > 0 {
                row("丢样本", "\(s.droppedSamples)").foregroundStyle(.orange)
            }
        }
    }

    /// CLAP 的状态必须看得出来，不能让「没在工作」长得像「音乐很平淡」。
    private var moodText: String {
        switch pipeline.stats.clap {
        case .loading:            return "模型加载中…"
        case .unavailable(let w): return "未启用（\(w)）"
        case .ready:
            guard let m = pipeline.stats.moodReading else { return "静音，暂不推理" }
            return String(format: "%@ · 愉悦 %+.2f 激活 %.2f", m.label, m.valence, m.energy)
        }
    }

    private func row(_ k: String, _ v: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(k).font(.caption).foregroundStyle(.secondary).frame(width: 48, alignment: .leading)
            Text(v).font(.caption.monospacedDigit())
            Spacer()
        }
    }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("板子地址").font(.caption).foregroundStyle(.secondary)
            TextField("mDNS 名或 IP，逗号分隔多个候选", text: $pipeline.targetSpec)
                .textFieldStyle(.roundedBorder).font(.caption)
                .disabled(pipeline.running)
            Text(pipeline.running ? "停止后才能改地址" : "按序解析，取第一个能解析的")
                .font(.caption2).foregroundStyle(.secondary)

            HStack {
                Text("输入增益").font(.caption).foregroundStyle(.secondary)
                Slider(value: $pipeline.inputGain, in: 0.1...10)
                Text(String(format: "%.1f×", pipeline.inputGain))
                    .font(.caption.monospacedDigit()).frame(width: 40, alignment: .trailing)
            }
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 6) {
            Button {
                openWindow(id: ConsoleWindow.id)
                NSApp.activate(ignoringOtherApps: true)   // 菜单栏 App 不会自动前台
            } label: {
                Label("打开控制台…", systemImage: "slider.horizontal.3")
            }
            if case .unavailable(let why) = pipeline.stats.clap, pipeline.running {
                // 降级要看得见 —— 一个静默降级比没有这个功能更糟
                Label("语义情绪未启用：\(why)。mood 走 liblamp 的能量近似。",
                      systemImage: "exclamationmark.triangle")
                    .font(.caption2).foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Spacer()
                Button("退出") { NSApplication.shared.terminate(nil) }
                    .keyboardShortcut("q")
            }
        }
    }
}
