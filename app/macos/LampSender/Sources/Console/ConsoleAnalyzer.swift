import SwiftUI
import LampKit

/// 灯珠调试页。
///
/// 分析仪表原先也在这页，后来挪到了控制页 —— 调效果时正需要看灯收到了什么，
/// 为此切页就断了。这里只留与硬件核对有关的部分。
struct ConsoleAnalyzer: View {
    @EnvironmentObject private var model: LampViewModel

    var body: some View {
        if model.selected == nil {
            ContentUnavailableView("未连接", systemImage: "lightbulb.led",
                                   description: Text("在左侧选一台灯。"))
        } else {
            ScrollView {
                LedDebugPane()
                    .padding(16)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .task { await model.loadDebug() }
        }
    }
}

/// 灯珠调试：点亮指定索引，核对物理布局。
///
/// 固件那份「底座环 0…11、底柱 12…17、主柱 18…47」就是用它逐颗点出来的。
struct LedDebugPane: View {
    @EnvironmentObject private var model: LampViewModel
    @State private var spec = "0-11"
    @State private var color = "FFFFFF"
    @State private var holdTask: Task<Void, Never>?
    private var holding: Bool { holdTask != nil }

    private static let presets: [(String, String)] = [
        ("左·底座环", "0-11"), ("左·底柱", "12-17"), ("左·主柱", "18-47"),
        ("右·底座环", "48-59"), ("右·底柱", "60-65"), ("右·主柱", "66-95"),
        ("状态灯", "96"),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("灯珠调试").font(.headline)
            // 固件用的是 realtimeLock(3000)，点一次只亮 3 秒就自动过期 —— 按一下
            // 基本看不清。所以这里按住不放：每 2 秒续一次，直到手动停。
            Text("点亮指定索引来核对布局。灯会进入 realtime，正常渲染暂停；固件那边 3 秒自动过期，所以「保持点亮」会每 2 秒续一次。")
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 6) {
                TextField("索引，如 0,5,7-12", text: $spec)
                    .textFieldStyle(.roundedBorder).frame(maxWidth: 190)
                TextField("RRGGBB", text: $color)
                    .textFieldStyle(.roundedBorder).frame(width: 90)
                    .font(.body.monospaced())
                Button(holding ? "停止" : "保持点亮") { holding ? stopHold() : startHold() }
                    .disabled(spec.isEmpty)
                    .tint(holding ? .orange : nil)
                if holding {
                    Text("续命中…").font(.caption2).foregroundStyle(.orange)
                }
                Spacer()
            }

            // 一键点亮某个分区 —— 比手敲索引快，也不会记错边界
            HStack(spacing: 5) {
                ForEach(Self.presets, id: \.1) { name, range in
                    Button(name) { spec = range; startHold() }
                        .buttonStyle(.plain).font(.caption)
                        .padding(.horizontal, 7).padding(.vertical, 2)
                        .background(Color.secondary.opacity(0.15), in: Capsule())
                }
            }

            if let d = model.debug {
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
                    GridRow { label("strip 长度"); Text("\(d.n)").font(.callout.monospacedDigit()) }
                    GridRow {
                        label("TOTAL_LEDS")
                        // 两个数不一样是正常的：固件几何只算灯柱，第 97 颗是扩展板状态灯
                        Text("\(d.t)" + (d.n != d.t ? "　（差 \(d.n - d.t) 颗：扩展板状态灯）" : ""))
                            .font(.callout.monospacedDigit())
                    }
                    GridRow { label("仅主分段"); Text(d.mso != 0 ? "是" : "否").font(.callout) }
                    GridRow {
                        // ovr 是 WLED 的 realtimeOverride 开关，与调试点亮无关 ——
                        // 调试走的是 realtimeLock，不会把这个置 1。别拿它判断调试状态。
                        label("realtime 覆盖")
                        Text(d.ovr != 0 ? "是" : "否（调试点亮不影响此项）").font(.callout)
                    }
                    GridRow { label("板子看到的客户端"); Text(d.src).font(.callout.monospaced()) }
                    GridRow { label("固件标识"); Text(d.fw).font(.callout.monospaced()) }
                }
            }
        }
        // 切走这一页就松手，否则灯会一直被按着不放
        .onDisappear { if holding { stopHold() } }
    }

    private func label(_ s: String) -> some View {
        Text(s).font(.caption).foregroundStyle(.secondary)
    }

    private func startHold() {
        holdTask?.cancel()
        let spec = spec, color = color
        holdTask = Task { @MainActor in
            while !Task.isCancelled {
                model.debugPixels(spec, color: color)
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    private func stopHold() {
        holdTask?.cancel()
        holdTask = nil
        model.debugOff()
    }
}
