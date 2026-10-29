import SwiftUI
import LampKit

/// 日常控制，照讯飞 App 分三大类：固定灯效 / 照明 / 音乐律动（LampKit 的 LightModes）。
///
/// 左栏：开关亮度、大类切换、当前大类的参数；右栏：当前大类的列表。音源、仪表只在
/// 音乐律动里出现 —— 其余两类用不上，摆着只添乱。
///
/// 大类与「固定灯效」的小类都跟着灯走：别处（网页、手机、讯飞 App）切了模式，这里跟着切。
/// 点大类只换显示，不改灯；真正改灯的是在里面选颜色、拖色温、点效果。
///
/// 讯飞相关的部分（讯飞灯效、原厂音乐效果）来自原厂板固件的 /nova，自制板上没有就不显示。
struct ConsoleControl: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var mode: LightMode = .fixed
    @State private var fixedKind: FixedKind = .solid
    @State private var effectFilter = ""
    @State private var novaCategory: Int?
    @State private var musicFilter: MusicFilter?
    @State private var customColor = Color.white

    var body: some View {
        if model.state == nil {
            ContentUnavailableView("未连接", systemImage: "lightbulb.slash",
                                   description: Text("在左侧选一台灯，或手填地址。"))
        } else {
            HSplitView {
                ScrollView { controls.padding(16).frame(maxWidth: .infinity, alignment: .leading) }
                    .frame(minWidth: 290)
                rightPane.frame(minWidth: 250)
            }
            // 轮询的开关必须挂在整页上。挂在仪表自己身上就成了死循环：
            // 没数据 → 不渲染仪表 → onAppear 不触发 → 不开轮询 → 永远没数据。
            .onAppear {
                model.startTelemetry()
                followLamp()
            }
            .onDisappear { model.stopTelemetry() }
            .onChange(of: model.lightMode) { _, _ in followLamp() }
            .onChange(of: model.fixedKind) { _, _ in followLamp() }
        }
    }

    private func followLamp() {
        mode = model.lightMode
        let kind = model.fixedKind
        if kind != .nova || model.novaCatalog != nil { fixedKind = kind }
    }

    private var fixedKinds: [FixedKind] {
        FixedKind.allCases.filter { $0 != .nova || model.novaCatalog != nil }
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
            Picker("", selection: $mode) {
                ForEach(LightMode.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented).labelsHidden()

            switch mode {
            case .fixed:
                if fixedKind == .wled { effectParams(label: "强度") }
            case .lighting:
                group("色温") {
                    LabeledSlider(title: "暖 ← → 冷", value: model.currentCw, range: 0...1000) {
                        model.setWhite(cw: $0)
                    }
                    Text("按原厂白光曲线取色，与讯飞 App、Matter 调出的白光相同。")
                        .font(.caption2).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            case .music:
                effectParams(label: "灵敏度")
                sourcePicker
                meters
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
        }
    }

    /// 速度、强度/灵敏度与调色板。♪ 效果把 ix 当灵敏度用；WLED 原生效果里它各有各的含义。
    @ViewBuilder
    private func effectParams(label: String) -> some View {
        if let seg = model.state?.primary {
            group("效果参数") {
                if let name = currentEffectTitle {
                    Text(name).font(.caption).foregroundStyle(.secondary)
                }
                LabeledSlider(title: "速度", value: seg.sx ?? 128, range: 0...255) { model.setSpeed($0) }
                LabeledSlider(title: label, value: seg.ix ?? 128, range: 0...255) { model.setIntensity($0) }
                // 讯飞调色板（原厂板固件注册，「讯飞: …」）排在最前；效果不用调色板时不显示
                if model.currentUsesPalette {
                    Picker("调色板", selection: Binding(
                        get: { seg.pal ?? 0 },
                        set: { model.setPalette($0) })) {
                            ForEach(model.paletteChoices) { Text($0.name).tag($0.id) }
                        }
                }
                if mode == .fixed {
                    Text("WLED 原生效果的「强度」含义各不相同（密度、数量、宽度等）。")
                        .font(.caption2).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var sourcePicker: some View {
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

    // ── 右栏 ─────────────────────────────────────────────

    @ViewBuilder
    private var rightPane: some View {
        switch mode {
        case .fixed: fixedPane
        case .lighting: lightingPane
        case .music: musicPane
        }
    }

    private var fixedPane: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 8) {
                Picker("", selection: $fixedKind) {
                    ForEach(fixedKinds) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden()
                switch fixedKind {
                case .solid: EmptyView()
                case .nova: novaCategoryBar
                case .wled:
                    // 一百多个效果，没有过滤框就只能靠滚
                    TextField("筛选效果", text: $effectFilter).textFieldStyle(.roundedBorder)
                }
            }
            .padding(14)
            Divider()
            ScrollView {
                switch fixedKind {
                case .solid: solidPicker.padding(14)
                case .nova: novaSceneList.padding(14)
                case .wled: wledList.padding(14)
                }
            }
            .frame(maxHeight: .infinity)
        }
    }

    // 静态：常用色 + 自选色
    private var solidPicker: some View {
        let current = model.state?.primary?.fx == 0 ? model.state?.primary?.col?.first.map { Array($0.prefix(3)) } : nil
        return VStack(alignment: .leading, spacing: 12) {
            LazyVGrid(columns: Array(repeating: GridItem(.fixed(44), spacing: 10), count: 4),
                      alignment: .leading, spacing: 10) {
                ForEach(LightModes.solidSwatches, id: \.name) { s in
                    Button { model.setSolid(s.rgb) } label: {
                        VStack(spacing: 3) {
                            Circle().fill(Color(rgb: s.rgb)).frame(width: 30, height: 30)
                                .overlay(Circle().stroke(Color.accentColor, lineWidth: current == s.rgb ? 3 : 0))
                            Text(s.name).font(.caption2)
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
            HStack {
                ColorPicker("自选颜色", selection: $customColor, supportsOpacity: false)
                Button("应用") { model.setSolid(customColor.rgb) }
            }
            Text("常用色按原厂取色方式给出：总有一个通道满值，混合色与红绿蓝一样亮。")
                .font(.caption2).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // 讯飞：原厂 7 类 + 场景列表
    private var novaCategoryBar: some View {
        let cats = model.novaCatalog?.scene.categories ?? []
        return ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 5) {
                chip("全部", on: novaCategory == nil) { novaCategory = nil }
                ForEach(Array(cats.enumerated()), id: \.offset) { i, c in
                    chip(LightModes.novaCategoryTitle(c), on: novaCategory == i) {
                        novaCategory = (novaCategory == i) ? nil : i
                    }
                }
            }
        }
    }

    private var novaSceneList: some View {
        let scenes = model.novaCatalog?.scene.names ?? []
        let seg = model.state?.primary
        let current = (seg?.fx == model.novaCatalog?.scene.fx) ? seg?.c1 : nil
        return VStack(alignment: .leading, spacing: 1) {
            ForEach(Array(scenes.enumerated()).filter { novaCategory == nil || $0.element.category == novaCategory },
                    id: \.offset) { i, s in
                row(s.name, current: current == i) { model.selectNovaScene(i) }
            }
        }
    }

    // WLED：不依赖音频的原生效果
    private var wledList: some View {
        let list = LightModes.wledEffects(effects: model.effects, fxData: model.fxData,
                                          catalog: model.novaCatalog, filter: effectFilter)
        return VStack(alignment: .leading, spacing: 1) {
            ForEach(list) { e in
                row(EffectNames.display(e.name), current: model.state?.primary?.fx == e.index) { model.setEffect(e.index) }
            }
            if list.isEmpty { Text("没有匹配的效果").font(.caption).foregroundStyle(.secondary) }
        }
    }

    // 照明：预览 + 三档快选（色温滑块在左栏）
    private var lightingPane: some View {
        VStack(alignment: .leading, spacing: 14) {
            RoundedRectangle(cornerRadius: 8)
                .fill(Color(rgb: LightModes.vendorWhite(cw: model.currentCw)))
                .frame(height: 70)
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.3)))
            HStack(spacing: 8) {
                ForEach([("暖光", 0), ("自然", 500), ("冷光", 1000)], id: \.1) { name, cw in
                    Button(name) { model.setWhite(cw: cw) }
                }
            }
            Text("色温 \(model.currentCw)（0 暖 … 1000 冷）").font(.caption).foregroundStyle(.secondary)
            Spacer()
        }
        .padding(14)
    }

    // 音乐律动：讯飞 / 五个分类 + 列表 + 说明
    private var musicPane: some View {
        VStack(alignment: .leading, spacing: 0) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 5) {
                    chip("全部", on: musicFilter == nil) { musicFilter = nil }
                    ForEach(MusicFilter.all.filter { $0 != .nova || model.novaCatalog != nil }) { f in
                        chip(f.title, on: musicFilter == f) { musicFilter = (musicFilter == f) ? nil : f }
                    }
                }
            }
            .padding(14)
            Divider()
            ScrollView {
                let list = LightModes.musicEntries(effects: model.effects, catalog: model.novaCatalog,
                                                   filter: musicFilter)
                VStack(alignment: .leading, spacing: 1) {
                    ForEach(list) { e in
                        let docMark: Bool = {
                            if case .effect = e.kind { return EffectDocs.doc(for: e.name) != nil }
                            return false
                        }()
                        row(musicTitle(e), current: LightModes.isCurrent(e, state: model.state, catalog: model.novaCatalog),
                            info: docMark) { model.select(e) }
                    }
                    if list.isEmpty { Text("没有匹配的效果").font(.caption).foregroundStyle(.secondary) }
                }
                .padding(14)
            }
            .frame(maxHeight: .infinity)
            Divider()
            // 固定高度，否则上面的列表会把它压成一行 —— 五段说明一行装不下
            docArea.frame(height: currentDoc == nil ? 64 : 208)
        }
    }

    private func musicTitle(_ e: MusicEntry) -> String {
        if case .nova = e.kind { return "讯飞 · \(e.name)" }
        return EffectNames.display(e.name)
    }

    // ── 公用 ─────────────────────────────────────────────

    private func row(_ title: String, current: Bool, info: Bool = false,
                     action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Text(title).lineLimit(1)
                Spacer(minLength: 4)
                // 有说明的效果标出来，免得用户以为点了没反应
                if info { Image(systemName: "info.circle").font(.caption2).foregroundStyle(.secondary) }
                if current { Image(systemName: "checkmark").font(.caption) }
            }
            .padding(.horizontal, 6).padding(.vertical, 3)
            .background(current ? Color.accentColor.opacity(0.18) : .clear,
                        in: RoundedRectangle(cornerRadius: 4))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
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

    /// 当前效果的显示名：讯飞音乐显示档位名，讯飞场景显示场景名。
    private var currentEffectTitle: String? {
        guard let seg = model.state?.primary, let fx = seg.fx else { return nil }
        if let c = model.novaCatalog {
            if fx == c.music.fx {
                let i = LightModes.musicIndex(c1: seg.c1 ?? 0, count: c.music.names.count)
                return "讯飞 · \(c.music.names[i])"
            }
            if fx == c.scene.fx, let i = seg.c1, i < c.scene.names.count { return c.scene.names[i].name }
        }
        return fx < model.effects.count ? EffectNames.display(model.effects[fx]) : nil
    }

    /// 当前生效的那个效果的说明。跟着灯走而不是跟着点击走 —— 别处（网页、手机）
    /// 换了效果，这里的说明也该跟着换。
    private var currentDoc: EffectDoc? {
        guard let fx = model.state?.primary?.fx, fx < model.effects.count else { return nil }
        return EffectDocs.doc(for: model.effects[fx])
    }

    @ViewBuilder
    private var docArea: some View {
        if let d = currentDoc {
            docCard(d)
        } else {
            VStack(alignment: .leading, spacing: 3) {
                Text(currentEffectTitle ?? "未选中效果").font(.callout.bold())
                Text("带 ⓘ 的是本项目的 ♪ 效果，选中后这里显示它的详细说明。")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
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

private extension Color {
    init(rgb: [Int]) {
        let c = rgb + [0, 0, 0]
        self.init(red: Double(c[0]) / 255, green: Double(c[1]) / 255, blue: Double(c[2]) / 255)
    }

    /// 自选色转 0–255 的 RGB（按 sRGB 取分量）。
    var rgb: [Int] {
        let ns = NSColor(self).usingColorSpace(.sRGB) ?? .white
        return [ns.redComponent, ns.greenComponent, ns.blueComponent].map { Int(($0 * 255).rounded()) }
    }
}
