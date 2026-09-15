import SwiftUI
import LampKit

/// 预设：加载、存当前状态、删除。
struct ConsolePresets: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var showSave = false
    @State private var newName = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if model.state == nil {
                ContentUnavailableView("未连接", systemImage: "square.grid.2x2",
                                       description: Text("在左侧选一台灯。"))
            } else {
                header
                Divider()
                list
            }
        }
    }

    private var header: some View {
        HStack {
            Text("\(model.presets.count) 个预设").font(.caption).foregroundStyle(.secondary)
            Spacer()
            Button {
                newName = ""
                showSave = true
            } label: { Label("存为预设", systemImage: "plus") }
                .disabled(model.state == nil)
        }
        .padding(12)
        .alert("存为预设", isPresented: $showSave) {
            TextField("名字", text: $newName)
            Button("取消", role: .cancel) {}
            Button("保存") {
                let slot = model.nextFreeSlot
                model.savePreset(slot, name: newName.isEmpty ? "预设 \(slot)" : newName)
            }
        } message: {
            // 说清语义：固件存的是「现在的样子」，不是任意状态
            Text("会把灯当前的状态存进槽位 \(model.nextFreeSlot)。")
        }
    }

    @ViewBuilder
    private var list: some View {
        if model.presets.isEmpty {
            ContentUnavailableView("还没有预设", systemImage: "tray",
                                   description: Text("调好当前效果后，点上面「存为预设」。"))
        } else {
            List {
                ForEach(model.presets) { p in
                    HStack(spacing: 10) {
                        Image(systemName: p.isPlaylist ? "list.number" : "lightbulb")
                            .foregroundStyle(.secondary).frame(width: 18)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(p.name)
                            Text("槽位 \(p.id)" + (p.quickLabel.map { " · \($0)" } ?? ""))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if model.state?.ps == p.id {
                            Image(systemName: "checkmark.circle.fill").foregroundStyle(.tint)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { model.loadPreset(p.id) }
                    .contextMenu {
                        Button("加载") { model.loadPreset(p.id) }
                        Divider()
                        Button("删除", role: .destructive) { model.deletePreset(p.id) }
                    }
                }
            }
        }
    }
}
