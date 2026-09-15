import SwiftUI
import LampKit

/// 控制台窗口：把网页上能做的事搬进 App。
///
/// 与 iOS 端共用 `LampViewModel`（在 LampKit 里），视图各写各的 —— 手机是竖屏
/// 分页，Mac 是侧边栏加窗口，硬凑一套反而两头别扭。共享的是逻辑和接口规范，
/// 不是控件。
struct ConsoleView: View {
    @StateObject private var model = LampViewModel()
    @State private var page: Page = .control
    @State private var manualHost = ""

    enum Page: String, CaseIterable, Identifiable {
        case control = "控制", presets = "预设", segments = "分段", settings = "设置"
        var id: String { rawValue }
        var icon: String {
            switch self {
            case .control:  return "slider.horizontal.3"
            case .presets:  return "square.grid.2x2"
            case .segments: return "rectangle.split.3x1"
            case .settings: return "gearshape"
            }
        }
    }

    var body: some View {
        NavigationSplitView {
            sidebar.navigationSplitViewColumnWidth(min: 200, ideal: 220, max: 300)
        } detail: {
            detail
        }
        .environmentObject(model)
        .task {
            model.startDiscovery()
            // ssh 里查不到窗口（CGWindowList 拿不到别的会话的窗口，实测连
            // Finder 都报零），所以让视图自己留个时间戳，远程才验证得了。
            UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: "consoleShownAt")
        }
        .onChange(of: statusSummary, initial: true) { _, s in
            UserDefaults.standard.set(s, forKey: "consoleStatus")
        }
        .onChange(of: model.error, initial: true) { _, e in
            // 错误只在窗口顶部飘一下，ssh 下看不见 —— 单独留一份
            UserDefaults.standard.set(e ?? "", forKey: "consoleError")
        }
        .frame(minWidth: 720, minHeight: 520)
    }

    private var sidebar: some View {
        List(selection: $page) {
            Section("设备") {
                ForEach(model.book.devices) { d in
                    Button { Task { await model.select(d) } } label: {
                        HStack(spacing: 8) {
                            Circle()
                                .fill(model.selected?.id == d.id ? Color.accentColor : .secondary.opacity(0.3))
                                .frame(width: 7, height: 7)
                            VStack(alignment: .leading, spacing: 1) {
                                Text(d.name).lineLimit(1)
                                Text(d.host).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                            }
                            Spacer()
                        }
                    }
                    .buttonStyle(.plain)
                }
                // 跨隧道时 mDNS 过不去，手填地址是必备入口而不是兜底
                HStack(spacing: 4) {
                    TextField("手填地址", text: $manualHost)
                        .textFieldStyle(.roundedBorder).font(.caption)
                        .onSubmit(addManual)
                    Button("加", action: addManual).disabled(manualHost.isEmpty)
                }
            }
            Section("面板") {
                ForEach(Page.allCases) { p in
                    Label(p.rawValue, systemImage: p.icon).tag(p)
                }
            }
        }
        .listStyle(.sidebar)
    }

    /// 控制台连上了谁、读到了什么。窗口开着时才有意义，ssh 下查：
    ///   defaults read com.hjma.lamp.sender consoleStatus
    private var statusSummary: String {
        guard let d = model.selected else { return "未选设备" }
        guard let i = model.info else { return "\(d.host) 连接中…" }
        // 带上会变的量：别处改了灯，这里跟着动才说明 WebSocket 是活的
        let st = model.state
        let live = "电源 \(st?.on == true ? "开" : "关") · 亮度 \(st?.bri ?? -1) · 效果 \(st?.primary?.fx ?? -1)"
        return "\(d.name) @ \(d.host) · 固件 \(i.ver ?? "?") · "
             + "\(model.effects.count) 效果 / \(model.palettes.count) 调色板 / \(model.presets.count) 预设 · \(live)"
    }

    private func addManual() {
        guard !manualHost.isEmpty else { return }
        model.addManual(host: manualHost)
        manualHost = ""
    }

    @ViewBuilder
    private var detail: some View {
        Group {
            switch page {
            case .control:  ConsoleControl()
            case .presets:  ConsolePresets()
            case .segments: ConsoleSegments()
            case .settings: ConsoleSettings()
            }
        }
        .navigationTitle(model.selected?.name ?? "氛围灯")
        .navigationSubtitle(model.selected?.host ?? "未连接")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { Task { await model.refresh() } } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .help("重新读取灯的状态")
                .disabled(model.selected == nil)
            }
        }
        .overlay(alignment: .top) {
            if let e = model.error {
                Text(e)
                    .font(.caption).padding(8)
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
                    .padding(.top, 6)
                    .onTapGesture { model.error = nil }
            }
        }
    }
}

/// 几个页面都在用的小控件。
struct LabeledSlider: View {
    let title: String
    let value: Int
    let range: ClosedRange<Double>
    let onChange: (Int) -> Void

    var body: some View {
        HStack {
            Text(title).frame(width: 52, alignment: .leading)
            Slider(value: Binding(get: { Double(value) }, set: { onChange(Int($0)) }), in: range)
            Text("\(value)").font(.caption.monospacedDigit())
                .frame(width: 34, alignment: .trailing)
        }
    }
}
