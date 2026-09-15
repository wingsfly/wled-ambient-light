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
    @AppStorage("consoleSidebar") private var showSidebar = true

    enum Page: String, CaseIterable, Identifiable {
        case control = "控制", presets = "预设", segments = "分区"
        case analyzer = "灯珠调试", settings = "设置"
        var id: String { rawValue }
        var icon: String {
            switch self {
            case .control:  return "slider.horizontal.3"
            case .presets:  return "square.grid.2x2"
            case .segments: return "rectangle.split.3x1"
            case .analyzer: return "lightbulb.led"
            case .settings: return "gearshape"
            }
        }
    }

    var body: some View {
        // 不用 NavigationSplitView：空间不足时它会牺牲侧边栏，把内容压到声明的
        // 最小宽度以下再裁掉 —— 实测窗口稍微拖窄一点，左边那列就只剩半个字。
        // 自己摆的话宽度是硬的，压不动。
        HStack(spacing: 0) {
            if showSidebar {
                sidebar.frame(width: 210)
                Divider()
            }
            NavigationStack { detail }
        }
        .environmentObject(model)
        // LSUIElement 的 App 默认不进 Dock，也就没法用 Cmd+Tab 切回来。
        // 控制台是个正经窗口，开着的时候就该露面；关掉再缩回菜单栏。
        .onAppear { NSApp.setActivationPolicy(.regular) }
        .onDisappear {
            // 只有确实没别的窗口了才缩回去 —— 将来多开一个窗口时这里不用改
            if NSApp.windows.filter({ $0.isVisible && $0.canBecomeMain }).isEmpty {
                NSApp.setActivationPolicy(.accessory)
            }
        }
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
        // 侧边栏 210 固定 + 详情页最窄 500 + 分隔。边栏可以收起，收起后窗口能
        // 拖到更窄。
        .frame(minWidth: 720, minHeight: 500)
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
                                Text(d.name).lineLimit(1).truncationMode(.tail)
                                Text(d.host).font(.caption2).foregroundStyle(.secondary)
                                    .lineLimit(1).truncationMode(.middle)
                            }
                            Spacer()
                        }
                    }
                    .buttonStyle(.plain)
                }
                // 跨隧道时 mDNS 过不去，手填地址是必备入口而不是兜底
                HStack(spacing: 4) {
                    TextField("地址", text: $manualHost)
                        .textFieldStyle(.roundedBorder).font(.caption)
                        .frame(minWidth: 90)
                        .onSubmit(addManual)
                    Button("加", action: addManual)
                        .disabled(manualHost.isEmpty).controlSize(.small)
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
            case .analyzer: ConsoleAnalyzer()
            case .settings: ConsoleSettings()
            }
        }
        .navigationTitle(model.selected?.name ?? "氛围灯")
        .navigationSubtitle(model.selected?.host ?? "未连接")
        .toolbar {
            ToolbarItem(placement: .navigation) {
                Button { withAnimation(.easeInOut(duration: 0.15)) { showSidebar.toggle() } } label: {
                    Image(systemName: "sidebar.leading")
                }
                .help(showSidebar ? "隐藏边栏" : "显示边栏")
            }
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
