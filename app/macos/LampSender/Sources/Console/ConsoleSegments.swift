import SwiftUI
import LampKit
import UniformTypeIdentifiers

/// 分段：左边列表，右边编辑器。
///
/// iOS 端是 NavigationLink 推进去再退回来；窗口够宽就不必绕这一下，选中即编辑。
struct ConsoleSegments: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var selectedID: Int?
    @State private var templates: [SegmentTemplate] = SegmentTemplate.builtIns
    @State private var chosen: SegmentTemplate.ID = SegmentTemplate.ambientTubesDetailed.id
    @State private var confirmApply = false
    @State private var showImporter = false
    @State private var saveName = ""
    @State private var showSave = false
    @State private var note: String?

    private let store = TemplateStore(appName: "LampSender")
    private var template: SegmentTemplate? { templates.first { $0.id == chosen } }

    private var segments: [LampSegment] { model.state?.seg ?? [] }
    private var current: LampSegment? {
        segments.first { ($0.id ?? 0) == selectedID } ?? segments.first
    }

    var body: some View {
        if segments.isEmpty {
            ContentUnavailableView("没有分段", systemImage: "rectangle.split.3x1",
                                   description: Text("在左侧选一台灯。"))
        } else {
            VStack(spacing: 0) {
                templateBar
                Divider()
                editor
            }
            .task { templates = SegmentTemplate.builtIns + store.load() }
            .fileImporter(isPresented: $showImporter, allowedContentTypes: [.json],
                          allowsMultipleSelection: false, onCompletion: importTemplate)
            .alert("应用模板", isPresented: $confirmApply, presenting: template) { t in
                Button("取消", role: .cancel) {}
                Button("应用") { model.applyTemplate(t) }
            } message: { t in
                // 说清楚这是破坏性的：旧分段会被删掉，不是叠加
                Text("会把灯上现有的 \(model.state?.seg?.count ?? 0) 个分段整个换成「\(t.name)」的 \(t.zones.count) 个区，原来的分段设置（效果、颜色、速度）会丢。")
            }
            .alert("存为模板", isPresented: $showSave) {
                TextField("模板名字", text: $saveName)
                Button("取消", role: .cancel) {}
                Button("保存") { saveCurrent() }
            } message: {
                Text("把灯上现在的 \(model.state?.seg?.count ?? 0) 个分段存成模板文件，以后可以直接套用。")
            }
        }
    }

    // ── 模板栏 ───────────────────────────────────────────

    private var templateBar: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text("模板").font(.caption).foregroundStyle(.secondary)
                Picker("", selection: $chosen) {
                    ForEach(templates) { t in Text(t.name).tag(t.id) }
                }
                .labelsHidden().frame(maxWidth: 220)
                Button("应用") { confirmApply = true }
                    .disabled(template == nil || model.busy)
                Spacer()
                Button("载入…") { showImporter = true }
                Button("存当前") { saveName = ""; showSave = true }
                    .disabled((model.state?.seg?.isEmpty ?? true))
            }
            if let t = template { templateSummary(t) }
            if let n = note {
                Text(n).font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(12)
    }

    private func templateSummary(_ t: SegmentTemplate) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                ForEach(Array(t.zones.enumerated()), id: \.offset) { _, z in
                    Text("\(z.name) \(z.count)")
                        .font(.caption2)
                        .padding(.horizontal, 5).padding(.vertical, 1)
                        .background(Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 3))
                }
            }
            // 模板的颗数与灯的配置对不上时，多出来的那些不会被任何区覆盖
            let actual = model.info?.leds?.count
            if let actual, actual != t.ledCount {
                Text("⚠︎ 模板按 \(t.ledCount) 颗写的，这台灯配置了 \(actual) 颗 —— 差额不会被任何区覆盖。")
                    .font(.caption2).foregroundStyle(.orange)
            }
            ForEach(t.problems, id: \.self) { p in
                Text("⚠︎ \(p)").font(.caption2).foregroundStyle(.orange)
            }
            if let n = t.note {
                Text(n).font(.caption2).foregroundStyle(.secondary)
            }
        }
    }

    private func importTemplate(_ result: Result<[URL], Error>) {
        guard case .success(let urls) = result, let url = urls.first else {
            if case .failure(let e) = result { note = "载入失败：\(e.localizedDescription)" }
            return
        }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let t = try TemplateStore.read(contentsOf: url)
            try store.save(t)                       // 存一份，下次不用再找文件
            templates = SegmentTemplate.builtIns + store.load()
            chosen = t.id
            note = "已载入「\(t.name)」"
        } catch {
            note = "这个文件不是模板：\(error.localizedDescription)"
        }
    }

    private func saveCurrent() {
        guard let t = model.templateFromCurrent(named: saveName.isEmpty ? "我的布局" : saveName) else {
            note = "当前没有可导出的分段"; return
        }
        do {
            let url = try store.save(t)
            templates = SegmentTemplate.builtIns + store.load()
            chosen = t.id
            note = "已存到 \(url.path)"
        } catch {
            note = "存不下来：\(error.localizedDescription)"
        }
    }

    // ── 列表与编辑器 ─────────────────────────────────────

    private var editor: some View {
        HSplitView {
                List(selection: $selectedID) {
                    ForEach(segments, id: \.id) { seg in
                        VStack(alignment: .leading, spacing: 1) {
                            Text(seg.name)
                            Text("灯 \(seg.start ?? 0)–\(seg.stop ?? 0) · 效果 \(seg.fx ?? 0)")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        .tag(seg.id ?? 0)
                    }
                }
                .frame(minWidth: 160, idealWidth: 190, maxWidth: 260)

                if let seg = current {
                    ScrollView { SegmentEditor(segment: seg).padding(16) }
                        .frame(minWidth: 280)
                } else {
                    Text("选一个分段").foregroundStyle(.secondary).frame(minWidth: 280)
                }
        }
    }
}

struct SegmentEditor: View {
    @EnvironmentObject private var model: LampViewModel
    let segment: LampSegment

    private var id: Int { segment.id ?? 0 }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            group("范围") {
                HStack(spacing: 18) {
                    labeled("起始", "\(segment.start ?? 0)")
                    labeled("结束", "\(segment.stop ?? 0)")
                    labeled("长度", "\(segment.len ?? 0)")
                }
            }
            group("开关与亮度") {
                Toggle("启用", isOn: Binding(
                    get: { segment.on ?? true },
                    set: { model.updateSegment(.init(id: id, on: $0)) }))
                    .toggleStyle(.switch)
                LabeledSlider(title: "亮度", value: segment.bri ?? 255, range: 0...255) {
                    model.updateSegment(.init(id: id, bri: $0))
                }
            }
            group("效果") {
                Picker("效果", selection: Binding(
                    get: { segment.fx ?? 0 },
                    set: { model.updateSegment(.init(id: id, fx: $0)) })) {
                        ForEach(Array(model.effects.enumerated()), id: \.offset) { i, n in
                            Text(n).tag(i)
                        }
                    }
                Picker("调色板", selection: Binding(
                    get: { segment.pal ?? 0 },
                    set: { model.updateSegment(.init(id: id, pal: $0)) })) {
                        ForEach(Array(model.palettes.enumerated()), id: \.offset) { i, n in
                            Text(n).tag(i)
                        }
                    }
                LabeledSlider(title: "速度", value: segment.sx ?? 128, range: 0...255) {
                    model.updateSegment(.init(id: id, sx: $0))
                }
                LabeledSlider(title: "强度", value: segment.ix ?? 128, range: 0...255) {
                    model.updateSegment(.init(id: id, ix: $0))
                }
            }
            group("方向") {
                Toggle("反向", isOn: Binding(
                    get: { segment.rev ?? false },
                    set: { model.updateSegment(.init(id: id, rev: $0)) }))
                Toggle("镜像", isOn: Binding(
                    get: { segment.mi ?? false },
                    set: { model.updateSegment(.init(id: id, mi: $0)) }))
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func labeled(_ k: String, _ v: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(k).font(.caption2).foregroundStyle(.secondary)
            Text(v).font(.callout.monospacedDigit())
        }
    }

    @ViewBuilder
    private func group<C: View>(_ title: String, @ViewBuilder content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            content()
        }
    }
}
