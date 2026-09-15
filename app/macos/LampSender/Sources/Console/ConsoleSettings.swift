import SwiftUI
import LampKit
import UniformTypeIdentifiers

/// 设置页：WLED 的全部配置，按普通人找东西的方式分组。
///
/// 左边一列是分组（这盏灯 / 点亮时 / 声音 / 联网 / 时间与定时 / 联动 / 外设 /
/// 权限），外加设备信息、配网、固件升级这三件不属于配置树的事，最后是「全部
/// 设置」—— 整棵树的浏览器，保证没有任何字段在 App 里找不到。
struct ConsoleSettings: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var section: Section = .info

    enum Section: Hashable {
        case info, wifi, firmware, raw
        case group(String)
    }

    var body: some View {
        if model.selected == nil {
            ContentUnavailableView("未连接", systemImage: "gearshape",
                                   description: Text("在左侧选一台灯。"))
        } else {
            HSplitView {
                sectionList.frame(minWidth: 150, idealWidth: 168, maxWidth: 220)
                detail.frame(minWidth: 330)
            }
            .task { if model.configTree == nil { await model.loadConfigTree() } }
        }
    }

    private var sectionList: some View {
        List(selection: $section) {
            SwiftUI.Section("设备") {
                Label("信息", systemImage: "info.circle").tag(Section.info)
                Label("配网", systemImage: "wifi.router").tag(Section.wifi)
                Label("固件升级", systemImage: "arrow.down.circle").tag(Section.firmware)
            }
            SwiftUI.Section("配置") {
                ForEach(ConfigCatalog.groups) { g in
                    Label(g.title, systemImage: g.icon).tag(Section.group(g.title))
                }
                Label("全部设置", systemImage: "list.bullet.indent").tag(Section.raw)
            }
        }
        .listStyle(.sidebar)
    }

    @ViewBuilder
    private var detail: some View {
        switch section {
        case .info:     DeviceInfoPane()
        case .wifi:     WiFiPane()
        case .firmware: FirmwarePane()
        case .raw:      RawConfigPane()
        case .group(let title):
            if let g = ConfigCatalog.groups.first(where: { $0.title == title }) {
                ConfigGroupPane(group: g)
            }
        }
    }
}

// MARK: - 配置分组

struct ConfigGroupPane: View {
    @EnvironmentObject private var model: LampViewModel
    let group: ConfigGroup

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    Text(group.title).font(.title3.bold())
                    if let n = group.note {
                        Text(n).font(.caption).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    ForEach(group.fields) { f in
                        ConfigRow(field: f)
                    }
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if model.configTree?.isDirty == true { saveBar }
        }
    }

    /// 改动不立即下发 —— 配置项之间有耦合（比如改了灯珠数再改引脚），一次
    /// 一个 POST 会让灯在中间状态上重载。攒够了一起发。
    private var saveBar: some View {
        HStack {
            Text("\(model.configTree?.edits.count ?? 0) 项改动未保存")
                .font(.caption).foregroundStyle(.orange)
            Spacer()
            Button("放弃") { model.revertConfig() }
            Button("保存") { model.saveConfig() }
                .keyboardShortcut("s").disabled(model.busy)
        }
        .padding(10)
        .background(.thinMaterial)
    }
}

struct ConfigRow: View {
    @EnvironmentObject private var model: LampViewModel
    let field: ConfigField

    private var value: JSONValue? { model.configTree?.value(at: field.path) }
    private var isEdited: Bool { model.configTree?.edits[field.path] != nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(field.label)
                    .frame(width: 128, alignment: .leading)
                    .foregroundStyle(isEdited ? Color.orange : .primary)
                control
                Spacer(minLength: 0)
            }
            if let h = field.hint {
                Text(h).font(.caption2).foregroundStyle(.secondary)
                    .padding(.leading, 136)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    @ViewBuilder
    private var control: some View {
        if value == nil {
            // 字段不存在不是错误：不同固件版本、不同 usermod 组合下本来就有增减
            Text("此固件没有这一项").font(.caption).foregroundStyle(.secondary)
        } else {
            switch field.kind {
            case .toggle:
                Toggle("", isOn: Binding(
                    get: { value?.asBool ?? false },
                    set: { model.setConfig(field.path, .bool($0)) }))
                    .labelsHidden().toggleStyle(.switch)
            case .int(let lo, let hi):
                HStack(spacing: 6) {
                    TextField("", value: Binding(
                        get: { value?.asInt ?? 0 },
                        set: { model.setConfig(field.path, .number(Double(min(max($0, lo), hi)))) }),
                        format: .number)
                        .textFieldStyle(.roundedBorder).frame(width: 90)
                    Text("\(lo)…\(hi)").font(.caption2).foregroundStyle(.secondary)
                }
            case .double(let lo, let hi):
                TextField("", value: Binding(
                    get: { value?.asDouble ?? 0 },
                    set: { model.setConfig(field.path, .number(min(max($0, lo), hi))) }),
                    format: .number)
                    .textFieldStyle(.roundedBorder).frame(width: 90)
            case .text:
                TextField("", text: Binding(
                    get: { value?.asString ?? "" },
                    set: { model.setConfig(field.path, .string($0)) }))
                    .textFieldStyle(.roundedBorder).frame(maxWidth: 240)
            case .choice(let options):
                Picker("", selection: Binding(
                    get: { value?.asInt ?? options.first?.value ?? 0 },
                    set: { model.setConfig(field.path, .number(Double($0))) })) {
                        ForEach(options, id: \.value) { Text($0.label).tag($0.value) }
                    }
                    .labelsHidden().frame(maxWidth: 190)
            case .readonly:
                Text(value?.preview ?? "—").font(.callout)
            }
        }
    }
}

// MARK: - 全部设置（整棵树）

/// 目录里没收的字段一个都不会丢。按树展开，标量能直接改。
struct RawConfigPane: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var expanded: Set<String> = ["hw", "if", "um"]
    @State private var filter = ""

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                TextField("按路径筛选，如 led、mqtt", text: $filter)
                    .textFieldStyle(.roundedBorder)
                Button("重读") { Task { await model.loadConfigTree() } }
            }
            .padding(10)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    if let tree = model.configTree {
                        ForEach(tree.keys(under: ""), id: \.self) { k in
                            ConfigNode(path: k, depth: 0, filter: filter, expanded: $expanded)
                        }
                    } else {
                        Text("读取中…").foregroundStyle(.secondary)
                    }
                }
                .padding(10)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if model.configTree?.isDirty == true {
                HStack {
                    Text("\(model.configTree?.edits.count ?? 0) 项改动未保存")
                        .font(.caption).foregroundStyle(.orange)
                    Spacer()
                    Button("放弃") { model.revertConfig() }
                    Button("保存") { model.saveConfig() }.disabled(model.busy)
                }
                .padding(10).background(.thinMaterial)
            }
        }
    }
}

/// 树里的一个节点。**必须是独立的 View struct 而不是方法** —— 递归的
/// @ViewBuilder 方法会让 opaque 返回类型用自己定义自己，编译器直接拒绝。
struct ConfigNode: View {
    @EnvironmentObject private var model: LampViewModel
    let path: String
    let depth: Int
    let filter: String
    @Binding var expanded: Set<String>

    private var name: String { path.split(separator: ".").last.map(String.init) ?? path }

    var body: some View {
        let v = model.configTree?.value(at: path)
        if v?.isContainer == true {
            container(v)
        } else if filter.isEmpty || path.localizedCaseInsensitiveContains(filter) {
            RawConfigRow(path: path, name: name, depth: depth)
        }
    }

    @ViewBuilder
    private func container(_ v: JSONValue?) -> some View {
        // 有筛选时自动展开，否则用户得一层层点进去找
        let open = expanded.contains(path) || !filter.isEmpty
        VStack(alignment: .leading, spacing: 2) {
            Button {
                if expanded.contains(path) { expanded.remove(path) } else { expanded.insert(path) }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: open ? "chevron.down" : "chevron.right")
                        .font(.caption2).frame(width: 10)
                    Text(name).font(.callout.bold())
                    Text(v?.preview ?? "").font(.caption2).foregroundStyle(.secondary)
                }
                .padding(.leading, CGFloat(depth) * 12)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if open { children(v) }
        }
    }

    @ViewBuilder
    private func children(_ v: JSONValue?) -> some View {
        if case .array(let items)? = v {
            ForEach(Array(items.enumerated()), id: \.offset) { i, _ in
                ConfigNode(path: "\(path).\(i)", depth: depth + 1, filter: filter, expanded: $expanded)
            }
        } else if let tree = model.configTree {
            ForEach(tree.keys(under: path), id: \.self) { k in
                ConfigNode(path: path + "." + k, depth: depth + 1, filter: filter, expanded: $expanded)
            }
        }
    }
}

struct RawConfigRow: View {
    @EnvironmentObject private var model: LampViewModel
    let path: String
    let name: String
    let depth: Int

    var body: some View {
        let v = model.configTree?.value(at: path)
        let edited = model.configTree?.edits[path] != nil
        HStack(spacing: 8) {
            Text(name).font(.caption)
                .foregroundStyle(edited ? Color.orange : .secondary)
                .frame(width: max(60, 150 - CGFloat(depth) * 12), alignment: .leading)
            switch v {
            case .bool(let b):
                Toggle("", isOn: Binding(get: { b }, set: { model.setConfig(path, .bool($0)) }))
                    .labelsHidden().toggleStyle(.switch).controlSize(.mini)
            case .number(let n):
                TextField("", value: Binding(
                    get: { n }, set: { model.setConfig(path, .number($0)) }), format: .number)
                    .textFieldStyle(.roundedBorder).frame(width: 110).font(.caption)
            case .string(let s):
                TextField("", text: Binding(
                    get: { s }, set: { model.setConfig(path, .string($0)) }))
                    .textFieldStyle(.roundedBorder).frame(maxWidth: 200).font(.caption)
            default:
                Text(v?.preview ?? "—").font(.caption).foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .padding(.leading, CGFloat(depth) * 12)
    }
}
