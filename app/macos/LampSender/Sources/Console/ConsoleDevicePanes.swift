import SwiftUI
import LampKit
import UniformTypeIdentifiers

/// 设备信息。这些是灯当场报出来的运行时状态，不是配置 —— 看得改不得。
struct DeviceInfoPane: View {
    @EnvironmentObject private var model: LampViewModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("设备信息").font(.title3.bold())
                if let i = model.info {
                    grid([
                        ("固件版本", i.ver ?? "?"),
                        ("地址", i.ip ?? model.selected?.host ?? "?"),
                        ("灯珠数", "\(i.leds?.count ?? 0)"),
                        ("当前电流", i.leds?.pwr.map { "\($0) mA" } ?? "—"),
                        ("WiFi 信号", i.wifi?.rssi.map { "\($0) dBm" } ?? "—"),
                        ("灯效数据源", i.fxDataSource ?? "—"),
                        ("上次复位原因", i.resetReason ?? "—"),
                        ("已运行", i.uptime.map { uptimeText($0) } ?? "—"),
                    ])
                } else {
                    Text("未连接").foregroundStyle(.secondary)
                }

                if let t = model.telemetry {
                    Divider()
                    Text("当前音源").font(.headline)
                    grid([
                        ("模式", t.mode.title),
                        ("生效源", ["bridge / 空", "local（板载）", "LAMP1（这台 Mac）"][max(0, min(2, t.src))]),
                        ("语义情绪", t.hasSemanticEmotion ? "可用" : "不可用（只有 LAMP1 源才有）"),
                    ])
                }

                Divider()
                Text("定时规则").font(.headline)
                let timers = model.config?.timers?.ins ?? []
                if timers.isEmpty {
                    Text("没有定时规则。").font(.callout).foregroundStyle(.secondary)
                }
                ForEach(timers) { t in
                    HStack(spacing: 8) {
                        Image(systemName: t.isEnabled ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(t.isEnabled ? .green : .secondary)
                        Text(t.timeText).monospacedDigit()
                        Text("\(t.weekdayText) · 预设 \(t.macro ?? 0)")
                            .font(.caption).foregroundStyle(.secondary)
                        Spacer()
                    }
                }
                // 诚实标注：本版只读，别让人以为点了没反应
                Text("定时规则的增删仍需在网页的 Time & Macros 里做。")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func uptimeText(_ s: Int) -> String {
        let d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60
        if d > 0 { return "\(d) 天 \(h) 小时" }
        if h > 0 { return "\(h) 小时 \(m) 分" }
        return "\(m) 分"
    }

    private func grid(_ rows: [(String, String)]) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 5) {
            ForEach(rows, id: \.0) { k, v in
                GridRow {
                    Text(k).font(.caption).foregroundStyle(.secondary)
                    Text(v).font(.callout)
                }
            }
        }
    }
}

/// 配网。密码只上行，不回显、不存盘。
struct WiFiPane: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var ssid = ""
    @State private var psk = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text("配网").font(.title3.bold())
                Text("灯不会回传已保存的密码，所以下面只列得出名称。改完网络灯会重连，可能要等一会儿。")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                let saved = model.config?.nw?.ins ?? []
                if saved.isEmpty {
                    Text("灯上没有已保存的网络。").font(.callout).foregroundStyle(.secondary)
                } else {
                    ForEach(saved) { e in
                        HStack(spacing: 8) {
                            Image(systemName: "wifi").foregroundStyle(.secondary)
                            Text(e.ssid ?? "?").font(.callout)
                            Spacer()
                        }
                    }
                }

                Divider()
                Text("添加网络").font(.headline)
                HStack {
                    TextField("WiFi 名称", text: $ssid).textFieldStyle(.roundedBorder)
                    SecureField("密码", text: $psk).textFieldStyle(.roundedBorder)
                    Button("添加并连接") {
                        model.addWiFi(ssid: ssid, psk: psk)
                        ssid = ""; psk = ""      // 不留在界面上，也不写进任何存储
                    }
                    .disabled(ssid.isEmpty || psk.isEmpty)
                }
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// 固件升级。
struct FirmwarePane: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var showPicker = false
    @State private var note: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text("固件升级").font(.title3.bold())
                if let i = model.info {
                    Text("当前：\(i.ver ?? "?")").font(.callout)
                }
                HStack {
                    Button("选择固件并升级…") { showPicker = true }
                        .disabled(model.selected == nil || model.busy)
                    if model.busy { ProgressView().controlSize(.small) }
                }
                if let n = note { Text(n).font(.caption).foregroundStyle(.secondary) }
                Text("上传中途断线会被固件丢弃（写的是备用分区，校验通过才切启动），不会变砖。升级后灯会重启。")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .fileImporter(isPresented: $showPicker, allowedContentTypes: [.data],
                      allowsMultipleSelection: false, onCompletion: handlePicked)
    }

    private func handlePicked(_ result: Result<[URL], Error>) {
        switch result {
        case .failure(let e):
            note = "选取失败：\(e.localizedDescription)"
        case .success(let urls):
            guard let url = urls.first else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                let data = try Data(contentsOf: url)
                note = "正在上传 \(data.count / 1024) KB…"
                model.updateFirmware(data, filename: url.lastPathComponent)
            } catch {
                note = "读不出文件：\(error.localizedDescription)"
            }
        }
    }
}
