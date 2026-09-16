import SwiftUI
import LampKit

/// 日常控制：开关、亮度、音源、夜灯、效果、调色板，外加一块实时仪表。
///
/// 仪表是 Mac 端独有的 —— 窗口够大放得下，而且这台机器往往就是 LAMP1 的
/// 来源，出问题时能当场看出是发端没数据还是灯没收到。
struct ConsoleControl: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var effectFilter = ""
    @State private var category: EffectCategory?

    var body: some View {
        if model.state == nil {
            ContentUnavailableView("未连接", systemImage: "lightbulb.slash",
                                   description: Text("在左侧选一台灯，或手填地址。"))
        } else {
            HSplitView {
                ScrollView { controls.padding(16).frame(maxWidth: .infinity, alignment: .leading) }
                    .frame(minWidth: 290)
                effectPane.frame(minWidth: 230)
            }
            // 轮询的开关必须挂在整页上。挂在仪表自己身上就成了死循环：
            // 没数据 → 不渲染仪表 → onAppear 不触发 → 不开轮询 → 永远没数据。
            .onAppear { model.startTelemetry() }
            .onDisappear { model.stopTelemetry() }
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
            effectParams
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
            meters
        }
    }

    /// 效果的速度与强度/灵敏度。
    ///
    /// 放在这里而不是「分区 → 选分段」里 —— 那 24 段效果说明每一条都在教怎么
    /// 调这两个值，两跳才够得着不合理。
    @ViewBuilder
    private var effectParams: some View {
        if let seg = model.state?.primary {
            group("效果参数") {
                if let name = model.currentEffectName {
                    Text(name).font(.caption).foregroundStyle(.secondary)
                }
                LabeledSlider(title: "速度", value: seg.sx ?? 128, range: 0...255) {
                    model.setSpeed($0)
                }
                LabeledSlider(title: EffectCatalog.intensityLabel(forEffect: model.currentEffectName),
                              value: seg.ix ?? 128, range: 0...255) {
                    model.setIntensity($0)
                }
                if !EffectCatalog.isMusic(model.currentEffectName ?? "") {
                    // 原生效果里 intensity 各有各的含义，说明白免得当成灵敏度调
                    Text("WLED 原生效果的「强度」含义各不相同（密度、数量、宽度等）。")
                        .font(.caption2).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    /// 实时仪表。10 Hz 轮询由整页的 onAppear 管，切走就停 —— 别让一个后台
    /// 窗口一直问灯要数据。
    @ViewBuilder
    private var meters: some View {
        group("实时") {
            if let t = model.telemetry {
                AnalyzerPanel(telemetry: t, compact: true)
            } else {
                // 说清楚是在等什么，别让空白看起来像坏了
                HStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text("正在读取分析数据…").font(.caption).foregroundStyle(.secondary)
                }
                .frame(height: 60)
            }
        }
    }

    private func sourceLabel(_ s: EffectiveSource) -> String {
        switch s {
        case .bridge: return "bridge / 空"
        case .local:  return "local（板载）"
        case .lamp1:  return "LAMP1（这台 Mac）"
        }
    }

    // ── 右栏：效果、分类、说明 ────────────────────────────

    private var effectPane: some View {
        VStack(alignment: .leading, spacing: 0) {
            filterBar.padding(.horizontal, 14).padding(.top, 14).padding(.bottom, 8)
            Divider()
            ScrollView { effectList.padding(14) }
                .frame(maxHeight: .infinity)
            Divider()
            // 固定高度，否则上面的列表会把它压成一行 —— 五段说明一行装不下
            docArea.frame(height: currentDoc == nil ? 76 : 208)
        }
    }

    private var filterBar: some View {
        VStack(alignment: .leading, spacing: 8) {
            // 固件里两百多个效果，没有过滤框就只能靠滚
            TextField("筛选效果", text: $effectFilter)
                .textFieldStyle(.roundedBorder)
            // 分类只有 ♪ 效果才有（标签取自网页那份 LCAT）；选了分类就等于
            // 把 WLED 原生那两百多个滤掉了
            HStack(spacing: 5) {
                chip("全部", on: category == nil) { category = nil }
                ForEach(EffectCategory.allCases) { c in
                    chip(c.title, on: category == c) { category = (category == c) ? nil : c }
                }
            }
        }
    }

    private func chip(_ title: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.caption)
                .padding(.horizontal, 9).padding(.vertical, 3)
                .background(on ? Color.accentColor : Color.secondary.opacity(0.15),
                            in: Capsule())
                .foregroundStyle(on ? Color.white : Color.primary)
        }
        .buttonStyle(.plain)
    }

    private var effectList: some View {
        let groups = EffectCatalog.grouped(model.effects, category: category, filter: effectFilter)
        return VStack(alignment: .leading, spacing: 10) {
            // ♪ 效果置顶。固件的效果 ID 是注册顺序、不能重排，所以动的是显示
            // 顺序 —— ♪ Auto 原本排在 240 个的最末尾，而它最常用。
            if !groups.music.isEmpty {
                section("♪ 音乐效果", groups.music, "\(groups.music.count) 个，带详细说明")
            }
            if !groups.native.isEmpty {
                section("WLED 原生", groups.native, "\(groups.native.count) 个")
            }
            if groups.isEmpty {
                Text("没有匹配的效果").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private func section(_ title: String, _ entries: [EffectCatalog.Entry],
                         _ subtitle: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: 6) {
                Text(title).font(.caption.bold())
                Text(subtitle).font(.caption2).foregroundStyle(.secondary)
            }
            .padding(.bottom, 3)
            ForEach(entries) { e in
                let isCurrent = model.state?.primary?.fx == e.index
                Button { model.setEffect(e.index) } label: {
                    HStack(spacing: 6) {
                        Text(e.name).lineLimit(1)
                        Spacer(minLength: 4)
                        // 有说明的效果标出来，免得用户以为点了没反应
                        if EffectDocs.doc(for: e.name) != nil {
                            Image(systemName: "info.circle").font(.caption2).foregroundStyle(.secondary)
                        }
                        if isCurrent { Image(systemName: "checkmark").font(.caption) }
                    }
                    .padding(.horizontal, 6).padding(.vertical, 3)
                    .background(isCurrent ? Color.accentColor.opacity(0.18) : .clear,
                                in: RoundedRectangle(cornerRadius: 4))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    /// 当前生效的那个效果的说明。跟着灯走而不是跟着点击走 —— 别处（网页、手机）
    /// 换了效果，这里的说明也该跟着换。
    private var currentDoc: EffectDoc? {
        guard let fx = model.state?.primary?.fx, fx < model.effects.count else { return nil }
        return EffectDocs.doc(for: model.effects[fx])
    }

    /// 说明区常驻。之前是「有文档才显示」，选到 WLED 原生效果时整块消失，
    /// 看起来像说明不见了 —— 其实是那个效果本来就没有说明。
    @ViewBuilder
    private var docArea: some View {
        if let d = currentDoc {
            docCard(d)
        } else {
            VStack(alignment: .leading, spacing: 3) {
                Text(currentEffectName ?? "未选中效果").font(.callout.bold())
                Text("WLED 原生效果，没有额外说明。带 ⓘ 的是本项目的 ♪ 效果，选中后这里会显示它的详细说明。")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var currentEffectName: String? {
        guard let fx = model.state?.primary?.fx, fx < model.effects.count else { return nil }
        return model.effects[fx]
    }

    private func docCard(_ d: EffectDoc) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 6) {
                    Text(d.name).font(.callout.bold())
                    ForEach(EffectDocs.categories(of: d.name)) { c in
                        Text(c.title)
                            .font(.caption2)
                            .padding(.horizontal, 5).padding(.vertical, 1)
                            .background(Color.secondary.opacity(0.15), in: Capsule())
                    }
                }
                ForEach(Array(d.lines.enumerated()), id: \.offset) { i, line in
                    HStack(alignment: .top, spacing: 5) {
                        Text(Self.docLabels[i])
                            .font(.caption2).foregroundStyle(.secondary)
                            .frame(width: 30, alignment: .leading)
                        Text(line).font(.caption)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private static let docLabels = ["是什么", "怎么动", "参数", "配色", "场合"]


    @ViewBuilder
    private func group<C: View>(_ title: String, @ViewBuilder content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            content()
        }
    }
}
