import SwiftUI
import LampKit

/// 分析仪表：灯此刻"听到"了什么。
///
/// 内容与网页特效页底部那块 ♪ Analyzer 对齐 —— 16 段频谱、12 音级色度、拍点
/// 相位、以及一行读数。放在控制页里和实时状态一起看：调效果的时候正需要知道
/// 灯收到的是什么，切到别的页去看就断了。
struct AnalyzerPanel: View {
    let telemetry: LampTelemetry
    var compact = false

    static let notes = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    static let styles = ["AMBIENT", "GENERAL", "EDM", "PERCUSSIVE"]
    static let sources = ["bridge / 空", "local（板载）", "LAMP1（这台 Mac）"]
    static let emotions = ["激烈", "活力", "欢快", "平静", "悲伤", "暗黑", "浪漫", "史诗"]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            spectrum.frame(height: compact ? 56 : 88)
            chroma.frame(height: compact ? 46 : 62)
            phase.frame(height: 12)
            readout
        }
    }

    /// 16 段频谱。×6 归一与网页同口径（固件的 kFxBandScale）。
    private var spectrum: some View {
        Canvas { ctx, size in
            let n = max(telemetry.bands.count, 1)
            let bw = size.width / CGFloat(n)
            for (i, v) in telemetry.bands.enumerated() {
                let h = min(1, v * 6) * size.height
                let rect = CGRect(x: CGFloat(i) * bw + 1, y: size.height - h, width: bw - 2, height: h)
                ctx.fill(Path(roundedRect: rect, cornerRadius: 1),
                         with: .color(Color(hue: Double(i) * 16 / 360, saturation: 0.85, brightness: 0.85)))
            }
        }
    }

    /// 12 音级色度，带音名 —— 弹什么和弦亮哪几段。
    private var chroma: some View {
        VStack(spacing: 2) {
            Canvas { ctx, size in
                let n = max(telemetry.chroma.count, 1)
                let cw = size.width / CGFloat(n)
                for (i, v) in telemetry.chroma.enumerated() {
                    let h = min(1, v) * size.height
                    let rect = CGRect(x: CGFloat(i) * cw + 2, y: size.height - h, width: cw - 4, height: h)
                    ctx.fill(Path(roundedRect: rect, cornerRadius: 1),
                             with: .color(Color(hue: Double(i) * 30 / 360, saturation: 0.7, brightness: 0.85)))
                }
            }
            HStack(spacing: 0) {
                ForEach(Array(Self.notes.enumerated()), id: \.offset) { _, n in
                    Text(n).font(.system(size: 9)).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity)
                }
            }
        }
    }

    /// 拍点相位轨道。锁不上拍就只剩底色 —— 这条比 BPM 数字更能看出拍跟得稳不稳。
    private var phase: some View {
        Canvas { ctx, size in
            ctx.fill(Path(CGRect(x: 0, y: size.height / 2 - 1.5, width: size.width, height: 3)),
                     with: .color(.secondary.opacity(0.35)))
            if telemetry.lock != 0 {
                let x = CGFloat(telemetry.phase) * size.width
                ctx.fill(Path(ellipseIn: CGRect(x: x - 4, y: size.height / 2 - 4, width: 8, height: 8)),
                         with: .color(Color(red: 1, green: 0.84, blue: 0.31)))
            }
        }
    }

    private var readout: some View {
        let t = telemetry
        let key = t.key >= 0 && t.key < 12 ? Self.notes[t.key] + (t.maj != 0 ? " 大调" : " 小调") : "–"
        return VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 12) {
                item("BPM", t.lock != 0 ? String(format: "%.0f", t.bpm) : "–")
                item("调性", key)
                item("人声", String(format: "%.0f%%", t.vocal * 100))
                item("打击", String(format: "%.0f%%", t.perc * 100))
                item("情绪", String(format: "%.0f%%", t.mood * 100))
            }
            HStack(spacing: 12) {
                item("档位", t.style >= 0 && t.style < Self.styles.count ? Self.styles[t.style] : "\(t.style)")
                item("生效源", t.src >= 0 && t.src < Self.sources.count ? Self.sources[t.src] : "\(t.src)")
                item("RMS", String(format: "%.4f", t.rms))
                // 语义情绪只有 LAMP1 源才有，别的源下 emo 是 255
                item("语义", t.hasSemanticEmotion && t.emo < Self.emotions.count
                     ? "\(Self.emotions[t.emo])  \(t.val > 0 ? "+" : "")\(String(format: "%.1f", t.val))"
                     : "–")
            }
        }
    }

    private func item(_ k: String, _ v: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(k).font(.caption2).foregroundStyle(.secondary)
            Text(v).font(.callout.monospacedDigit())
        }
    }
}
