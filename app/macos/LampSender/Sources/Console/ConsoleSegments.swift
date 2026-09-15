import SwiftUI
import LampKit

/// 分段：左边列表，右边编辑器。
///
/// iOS 端是 NavigationLink 推进去再退回来；窗口够宽就不必绕这一下，选中即编辑。
struct ConsoleSegments: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var selectedID: Int?

    private var segments: [LampSegment] { model.state?.seg ?? [] }
    private var current: LampSegment? {
        segments.first { ($0.id ?? 0) == selectedID } ?? segments.first
    }

    var body: some View {
        if segments.isEmpty {
            ContentUnavailableView("没有分段", systemImage: "rectangle.split.3x1",
                                   description: Text("在左侧选一台灯。"))
        } else {
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
                .frame(minWidth: 180, idealWidth: 200, maxWidth: 260)

                if let seg = current {
                    ScrollView { SegmentEditor(segment: seg).padding(16) }
                        .frame(minWidth: 320)
                } else {
                    Text("选一个分段").foregroundStyle(.secondary).frame(minWidth: 320)
                }
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
