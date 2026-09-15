import SwiftUI
import LampKit
import UniformTypeIdentifiers

/// 设备信息、定时、配网、OTA。
struct ConsoleSettings: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var ssid = ""
    @State private var psk = ""
    @State private var showPicker = false
    @State private var otaNote: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                infoGroup
                timerGroup
                wifiGroup
                otaGroup
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .fileImporter(isPresented: $showPicker, allowedContentTypes: [.data],
                      allowsMultipleSelection: false, onCompletion: handlePicked)
    }

    private var infoGroup: some View {
        group("设备") {
            if let i = model.info {
                grid([
                    ("固件", i.ver ?? "?"),
                    ("地址", i.ip ?? model.selected?.host ?? "?"),
                    ("灯珠", "\(i.leds?.count ?? 0)"),
                    ("当前电流", i.leds?.pwr.map { "\($0) mA" } ?? "—"),
                    ("信号", i.wifi?.rssi.map { "\($0) dBm" } ?? "—"),
                    ("灯效数据源", i.fxDataSource ?? "—"),
                    ("上次复位", i.resetReason ?? "—"),
                    ("运行", i.uptime.map { "\($0 / 60) 分钟" } ?? "—"),
                ])
            } else {
                Text("未连接").foregroundStyle(.secondary)
            }
        }
    }

    private var timerGroup: some View {
        group("定时") {
            let timers = model.config?.timers?.ins ?? []
            if timers.isEmpty {
                Text("没有定时规则。").font(.callout).foregroundStyle(.secondary)
            }
            ForEach(timers) { t in
                HStack {
                    Image(systemName: t.isEnabled ? "checkmark.circle.fill" : "circle")
                        .foregroundStyle(t.isEnabled ? .green : .secondary)
                    Text(t.timeText).monospacedDigit()
                    Text("\(t.weekdayText) · 预设 \(t.macro ?? 0)")
                        .font(.caption).foregroundStyle(.secondary)
                    Spacer()
                }
            }
            // 诚实标注：本版只读，别让人以为点了没反应
            Text("本版只显示灯上已有的定时规则；新增与编辑仍需在网页的 Time & Macros 里做。")
                .font(.caption2).foregroundStyle(.secondary)
        }
    }

    private var wifiGroup: some View {
        group("网络") {
            ForEach(model.config?.nw?.ins ?? []) { e in
                Text(e.ssid ?? "?").font(.callout)
            }
            HStack {
                TextField("WiFi 名称", text: $ssid).textFieldStyle(.roundedBorder)
                SecureField("密码", text: $psk).textFieldStyle(.roundedBorder)
                Button("添加并连接") {
                    model.addWiFi(ssid: ssid, psk: psk)
                    ssid = ""; psk = ""      // 密码不留在界面上，也不写进任何存储
                }
                .disabled(ssid.isEmpty || psk.isEmpty)
            }
            Text("灯不会回传已保存的密码，所以上面只列得出名称。改完网络灯会重连，可能要等一会儿。")
                .font(.caption2).foregroundStyle(.secondary)
        }
    }

    private var otaGroup: some View {
        group("固件升级") {
            HStack {
                Button("选择固件并升级…") { showPicker = true }
                    .disabled(model.selected == nil || model.busy)
                if model.busy { ProgressView().controlSize(.small) }
                if let n = otaNote { Text(n).font(.caption).foregroundStyle(.secondary) }
            }
            Text("上传中途断线会被固件丢弃（写的是备用分区，校验通过才切启动），不会变砖。升级后灯会重启。")
                .font(.caption2).foregroundStyle(.secondary)
        }
    }

    private func handlePicked(_ result: Result<[URL], Error>) {
        switch result {
        case .failure(let e):
            otaNote = "选取失败：\(e.localizedDescription)"
        case .success(let urls):
            guard let url = urls.first else { return }
            // 沙盒外也照样走一遍取用权限：App 以后要是加了 App Sandbox，这行是必需的
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                let data = try Data(contentsOf: url)
                otaNote = "正在上传 \(data.count / 1024) KB…"
                model.updateFirmware(data, filename: url.lastPathComponent)
            } catch {
                otaNote = "读不出文件：\(error.localizedDescription)"
            }
        }
    }

    private func grid(_ rows: [(String, String)]) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 5) {
            ForEach(rows, id: \.0) { k, v in
                GridRow {
                    Text(k).font(.caption).foregroundStyle(.secondary)
                    Text(v).font(.callout)
                }
            }
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
