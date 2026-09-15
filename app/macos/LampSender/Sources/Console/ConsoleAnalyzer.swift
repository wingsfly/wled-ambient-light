import SwiftUI
import LampKit

/// 诊断页：分析仪表 + 灯珠调试。
///
/// 内容与网页上那两块对齐（特效页底部的 ♪ Analyzer 和 /leddebug），因为它们
/// 回答的是同两个问题：灯「听到」了什么，以及第 N 颗灯珠在哪。
struct ConsoleAnalyzer: View {
    @EnvironmentObject private var model: LampViewModel

    static let notes = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    static let styles = ["AMBIENT", "GENERAL", "EDM", "PERCUSSIVE"]
    static let sources = ["bridge", "local", "LAMP1"]
    static let emotions = ["激烈", "活力", "欢快", "平静", "悲伤", "暗黑", "浪漫", "史诗"]

    var body: some View {
        if model.selected == nil {
            ContentUnavailableView("未连接", systemImage: "waveform.badge.magnifyingglass",
                                   description: Text("在左侧选一台灯。"))
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    analyzer
                    Divider()
                    LedDebugPane()
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            // 10 Hz 轮询只在这一页开着的时候跑，切走就停
            .onAppear { model.startTelemetry() }
            .onDisappear { model.stopTelemetry() }
            .task { await model.loadDebug() }
        }
    }

    // ── 分析仪表 ─────────────────────────────────────────

    @ViewBuilder
    private var analyzer: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("分析仪表").font(.headline)
            if let t = model.telemetry {
                spectrum(t).frame(height: 96)
                chroma(t).frame(height: 66)
                phase(t).frame(height: 14)
                readout(t)
            } else {
                Text("还没有数据 —— 灯要在音乐模式下才会产出分析结果。")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    /// 16 段频谱。×6 归一与网页同口径（固件的 kFxBandScale）。
    private func spectrum(_ t: LampTelemetry) -> some View {
        Canvas { ctx, size in
            let n = max(t.bands.count, 1)
            let bw = size.width / CGFloat(n)
            for (i, v) in t.bands.enumerated() {
                let h = min(1, v * 6) * size.height
                let rect = CGRect(x: CGFloat(i) * bw + 1, y: size.height - h,
                                  width: bw - 2, height: h)
                ctx.fill(Path(roundedRect: rect, cornerRadius: 1),
                         with: .color(Color(hue: Double(i) * 16 / 360, saturation: 0.85, brightness: 0.85)))
            }
        }
    }

    /// 12 音级色度，带音名 —— 弹什么和弦亮哪几段。
    private func chroma(_ t: LampTelemetry) -> some View {
        VStack(spacing: 2) {
            Canvas { ctx, size in
                let n = max(t.chroma.count, 1)
                let cw = size.width / CGFloat(n)
                for (i, v) in t.chroma.enumerated() {
                    let h = min(1, v) * size.height
                    let rect = CGRect(x: CGFloat(i) * cw + 2, y: size.height - h,
                                      width: cw - 4, height: h)
                    ctx.fill(Path(roundedRect: rect, cornerRadius: 1),
                             with: .color(Color(hue: Double(i) * 30 / 360, saturation: 0.7, brightness: 0.85)))
                }
            }
            HStack(spacing: 0) {
                ForEach(Array(Self.notes.enumerated()), id: \.offset) { _, n in
                    Text(n).font(.system(size: 9)).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity)
                }
            }
        }
    }

    /// 拍点相位轨道：锁拍时一个光点按相位跑。锁不上就只有底色 —— 这条比 BPM
    /// 数字更能看出拍跟得稳不稳。
    private func phase(_ t: LampTelemetry) -> some View {
        Canvas { ctx, size in
            ctx.fill(Path(CGRect(x: 0, y: size.height / 2 - 1.5, width: size.width, height: 3)),
                     with: .color(.secondary.opacity(0.35)))
            if t.lock != 0 {
                let x = CGFloat(t.phase) * size.width
                ctx.fill(Path(ellipseIn: CGRect(x: x - 4, y: size.height / 2 - 4, width: 8, height: 8)),
                         with: .color(Color(red: 1, green: 0.84, blue: 0.31)))
            }
        }
    }

    private func readout(_ t: LampTelemetry) -> some View {
        let key = t.key >= 0 && t.key < 12 ? Self.notes[t.key] + (t.maj != 0 ? " 大调" : " 小调") : "–"
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 14) {
                item("BPM", t.lock != 0 ? String(format: "%.0f", t.bpm) : "–")
                item("调性", key + (t.kconf > 0 ? String(format: " (%.0f%%)", t.kconf * 100) : ""))
                item("人声", String(format: "%.0f%%", t.vocal * 100))
                item("打击", String(format: "%.0f%%", t.perc * 100))
                item("情绪", String(format: "%.0f%%", t.mood * 100))
            }
            HStack(spacing: 14) {
                item("档位", t.style >= 0 && t.style < Self.styles.count ? Self.styles[t.style] : "\(t.style)")
                item("生效源", t.src >= 0 && t.src < Self.sources.count ? Self.sources[t.src] : "\(t.src)")
                item("RMS", String(format: "%.4f", t.rms))
                // 语义情绪只有 LAMP1 源才有；别的源下 emo 是 255
                if t.hasSemanticEmotion, t.emo < Self.emotions.count {
                    item("语义", "\(Self.emotions[t.emo])  愉悦 \(t.val > 0 ? "+" : "")\(String(format: "%.1f", t.val))")
                } else {
                    item("语义", "–")
                }
            }
        }
    }

    private func item(_ k: String, _ v: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(k).font(.caption2).foregroundStyle(.secondary)
            Text(v).font(.callout.monospacedDigit())
        }
    }
}

/// 灯珠调试：点亮指定索引，核对物理布局。
///
/// 固件那份「底座环 0…11、底柱 12…17、主柱 18…47」就是用它逐颗点出来的。
struct LedDebugPane: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var spec = "0-11"
    @State private var color = "FFFFFF"
    @State private var holdTask: Task<Void, Never>?
    private var holding: Bool { holdTask != nil }

    private static let presets: [(String, String)] = [
        ("左·底座环", "0-11"), ("左·底柱", "12-17"), ("左·主柱", "18-47"),
        ("右·底座环", "48-59"), ("右·底柱", "60-65"), ("右·主柱", "66-95"),
        ("状态灯", "96"),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("灯珠调试").font(.headline)
            // 固件用的是 realtimeLock(3000)，点一次只亮 3 秒就自动过期 —— 按一下
            // 基本看不清。所以这里按住不放：每 2 秒续一次，直到手动停。
            Text("点亮指定索引来核对布局。灯会进入 realtime，正常渲染暂停；固件那边 3 秒自动过期，所以「保持点亮」会每 2 秒续一次。")
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 6) {
                TextField("索引，如 0,5,7-12", text: $spec)
                    .textFieldStyle(.roundedBorder).frame(maxWidth: 190)
                TextField("RRGGBB", text: $color)
                    .textFieldStyle(.roundedBorder).frame(width: 90)
                    .font(.body.monospaced())
                Button(holding ? "停止" : "保持点亮") { holding ? stopHold() : startHold() }
                    .disabled(spec.isEmpty)
                    .tint(holding ? .orange : nil)
                if holding {
                    Text("续命中…").font(.caption2).foregroundStyle(.orange)
                }
                Spacer()
            }

            // 一键点亮某个分区 —— 比手敲索引快，也不会记错边界
            HStack(spacing: 5) {
                ForEach(Self.presets, id: \.1) { name, range in
                    Button(name) { spec = range; startHold() }
                        .buttonStyle(.plain).font(.caption)
                        .padding(.horizontal, 7).padding(.vertical, 2)
                        .background(Color.secondary.opacity(0.15), in: Capsule())
                }
            }

            if let d = model.debug {
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
                    GridRow { label("strip 长度"); Text("\(d.n)").font(.callout.monospacedDigit()) }
                    GridRow {
                        label("TOTAL_LEDS")
                        // 两个数不一样是正常的：固件几何只算灯柱，第 97 颗是扩展板状态灯
                        Text("\(d.t)" + (d.n != d.t ? "　（差 \(d.n - d.t) 颗：扩展板状态灯）" : ""))
                            .font(.callout.monospacedDigit())
                    }
                    GridRow { label("仅主分段"); Text(d.mso != 0 ? "是" : "否").font(.callout) }
                    GridRow {
                        // ovr 是 WLED 的 realtimeOverride 开关，与调试点亮无关 ——
                        // 调试走的是 realtimeLock，不会把这个置 1。别拿它判断调试状态。
                        label("realtime 覆盖")
                        Text(d.ovr != 0 ? "是" : "否（调试点亮不影响此项）").font(.callout)
                    }
                    GridRow { label("板子看到的客户端"); Text(d.src).font(.callout.monospaced()) }
                    GridRow { label("固件标识"); Text(d.fw).font(.callout.monospaced()) }
                }
            }
        }
        // 切走这一页就松手，否则灯会一直被按着不放
        .onDisappear { if holding { stopHold() } }
    }

    private func label(_ s: String) -> some View {
        Text(s).font(.caption).foregroundStyle(.secondary)
    }

    private func startHold() {
        holdTask?.cancel()
        let spec = spec, color = color
        holdTask = Task { @MainActor in
            while !Task.isCancelled {
                model.debugPixels(spec, color: color)
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    private func stopHold() {
        holdTask?.cancel()
        holdTask = nil
        model.debugOff()
    }
}
