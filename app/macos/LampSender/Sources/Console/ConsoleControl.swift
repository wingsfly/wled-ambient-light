import SwiftUI
import LampKit

/// 日常控制：开关、亮度、音源、夜灯、效果、调色板，外加一块实时仪表。
///
/// 仪表是 Mac 端独有的 —— 窗口够大放得下，而且这台机器往往就是 LAMP1 的
/// 来源，出问题时能当场看出是发端没数据还是灯没收到。
struct ConsoleControl: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var effectFilter = ""

    var body: some View {
        if model.state == nil {
            ContentUnavailableView("未连接", systemImage: "lightbulb.slash",
                                   description: Text("在左侧选一台灯，或手填地址。"))
        } else {
            HSplitView {
                ScrollView { controls.padding(16).frame(maxWidth: .infinity, alignment: .leading) }
                    .frame(minWidth: 290)
                ScrollView { effectPicker.padding(16) }
                    .frame(minWidth: 210)
            }
        }
    }

    // ── 左栏 ─────────────────────────────────────────────

    private var controls: some View {
        VStack(alignment: .leading, spacing: 18) {
            group("开关与亮度") {
                Toggle("电源", isOn: Binding(
                    get: { model.state?.on ?? false },
                    set: { model.setPower($0) }))
                    .toggleStyle(.switch)
                if let bri = model.state?.bri {
                    LabeledSlider(title: "亮度", value: bri, range: 1...255) { model.setBrightness($0) }
                }
            }
            group("音源") {
                Picker("", selection: Binding(
                    get: { model.state?.sourceMode ?? .auto },
                    set: { model.setSource($0) })) {
                        ForEach(AudioSourceMode.allCases, id: \.self) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented).labelsHidden()
                Text("Local 用板载麦克风或 3.5mm 线路输入；LAMP1 用这台 Mac 送来的分析结果，只有它带语义情绪。")
                    .font(.caption2).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let t = model.telemetry {
                    HStack {
                        Text("当前生效").font(.caption).foregroundStyle(.secondary)
                        Text(sourceLabel(t.effectiveSource)).font(.caption.bold())
                    }
                }
            }
            group("夜灯") {
                Toggle("定时关灯", isOn: Binding(
                    get: { model.state?.nl?.on ?? false },
                    set: { model.setNightlight(on: $0) }))
                    .toggleStyle(.switch)
                if let dur = model.state?.nl?.dur {
                    Stepper("\(dur) 分钟后关", value: Binding(
                        get: { dur }, set: { model.setNightlight(minutes: $0) }), in: 1...255, step: 5)
                }
                if let rem = model.state?.nl?.rem, rem > 0 {
                    Text("剩余 \(rem / 60) 分 \(rem % 60) 秒")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            if let t = model.telemetry { meters(t) }
        }
    }

    /// 仪表在界面显示时才拉（10 Hz），切走就停 —— 别让一个后台窗口一直问灯要数据。
    private func meters(_ t: LampTelemetry) -> some View {
        group("实时") {
            spectrum(t.bands)
            HStack(spacing: 16) {
                meter("RMS", String(format: "%.3f", t.rms))
                meter("BPM", t.bpm > 1 ? String(format: "%.0f", t.bpm) : "—")
                // hasSemanticEmotion 连源一起判：有线/本地源拿不到情绪，
                // 那时 emo 是 255、val 是 0，直接显示数字会让人以为是真值
                meter("情绪", t.hasSemanticEmotion ? "#\(t.emo)" : "—")
                meter("愉悦", t.hasSemanticEmotion ? String(format: "%+.2f", t.val) : "—")
            }
        }
        .onAppear { model.startTelemetry() }
        .onDisappear { model.stopTelemetry() }
    }

    private func spectrum(_ bands: [Double]) -> some View {
        GeometryReader { geo in
            let w = max(1, (geo.size.width - CGFloat(bands.count - 1) * 2) / CGFloat(max(bands.count, 1)))
            HStack(alignment: .bottom, spacing: 2) {
                ForEach(Array(bands.enumerated()), id: \.offset) { _, v in
                    RoundedRectangle(cornerRadius: 1)
                        .fill(Color.accentColor.opacity(0.85))
                        .frame(width: w, height: max(1, min(1, v * 3) * geo.size.height))
                }
            }
            .frame(maxHeight: .infinity, alignment: .bottom)
        }
        .frame(height: 48)
    }

    private func meter(_ k: String, _ v: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(k).font(.caption2).foregroundStyle(.secondary)
            Text(v).font(.callout.monospacedDigit())
        }
    }

    private func sourceLabel(_ s: EffectiveSource) -> String {
        switch s {
        case .bridge: return "bridge / 空"
        case .local:  return "local（板载）"
        case .lamp1:  return "LAMP1（这台 Mac）"
        }
    }

    // ── 右栏：效果与调色板 ────────────────────────────────

    private var effectPicker: some View {
        VStack(alignment: .leading, spacing: 12) {
            // 固件里 180 多个效果，没有过滤框就只能靠滚
            TextField("筛选效果", text: $effectFilter)
                .textFieldStyle(.roundedBorder)
            let hits = filtered(model.effects)
            Text("效果（\(hits.count)/\(model.effects.count)）")
                .font(.caption).foregroundStyle(.secondary)
            ForEach(hits, id: \.0) { idx, name in
                Button { model.setEffect(idx) } label: {
                    HStack {
                        Text(name).lineLimit(1)
                        Spacer()
                        if model.state?.primary?.fx == idx {
                            Image(systemName: "checkmark").font(.caption)
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(.vertical, 1)
            }
        }
    }

    private func filtered(_ names: [String]) -> [(Int, String)] {
        let all = Array(names.enumerated()).map { ($0.offset, $0.element) }
        guard !effectFilter.isEmpty else { return all }
        return all.filter { $0.1.localizedCaseInsensitiveContains(effectFilter) }
    }

    @ViewBuilder
    private func group<C: View>(_ title: String, @ViewBuilder content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            content()
        }
    }
}
