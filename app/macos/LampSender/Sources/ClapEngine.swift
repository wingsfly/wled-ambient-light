import Foundation
import CoreML

/// 一次 CLAP 零样本情绪的结果。
struct MoodReading: Sendable {
    let valence: Float       // 愉悦度 -1..1
    let energy: Float        // 激活度 0..1
    let emoID: UInt8         // 八锚点 argmax，0..7
    let label: String        // 锚点提示词的首词，仅供菜单栏显示
}

/// CLAP 零样本情绪：音频 → 512 维 embedding → 与 8 个情绪锚点比相似度。
///
/// 模型和锚点都在 App bundle 里（`clap/export_clap.py` 生成）。文本塔没有
/// 打包 —— 锚点 embedding 是常量，离线算好存 42 KB 就够，见 clap/README.md。
final class ClapEngine: @unchecked Sendable {
    private struct Anchor {
        let label: String, valence: Float, energy: Float, embedding: [Float]
    }

    static let windowSamples = 480000        // 10 秒 @ 48 kHz，模型的固定输入长度
    private static let inputName = "waveform"
    private static let outputName = "embedding"

    private let model: MLModel
    private let anchors: [Anchor]
    private let softmaxScale: Float
    private let input: MLMultiArray          // 复用，省掉每次 1.9 MB 的分配

    init?() {
        guard let modelURL = Bundle.main.url(forResource: "ClapAudioTower", withExtension: "mlmodelc"),
              let anchorsURL = Bundle.main.url(forResource: "anchors", withExtension: "json"),
              let data = try? Data(contentsOf: anchorsURL),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let list = root["anchors"] as? [[String: Any]], !list.isEmpty
        else { return nil }

        softmaxScale = Float(root["softmaxScale"] as? Double ?? 25.0)
        anchors = list.compactMap { item in
            guard let prompt = item["prompt"] as? String,
                  let v = item["valence"] as? Double,
                  let e = item["energy"] as? Double,
                  let emb = item["embedding"] as? [Double] else { return nil }
            return Anchor(label: prompt.split(separator: " ").first.map(String.init) ?? prompt,
                          valence: Float(v), energy: Float(e), embedding: emb.map(Float.init))
        }
        guard anchors.count == list.count else { return nil }

        let cfg = MLModelConfiguration()
        cfg.computeUnits = .all
        guard let m = try? MLModel(contentsOf: modelURL, configuration: cfg),
              let arr = try? MLMultiArray(shape: [1, NSNumber(value: Self.windowSamples)], dataType: .float32)
        else { return nil }
        model = m
        input = arr
    }

    /// 对一窗音频做一次推理。samples 必须正好 windowSamples 个。
    /// 返回 nil 表示这一窗不该用（静音，或推理失败）。
    func analyze(_ samples: UnsafePointer<Float>) -> MoodReading? {
        // 静音不推理。Python 版在这里只把 energy 置空、valence 和 emo_id 留着
        // 上一次的值；这里整个置 nil，让发包侧退回固件给 v1 包的缺省
        // （0 / 255）—— 安静时段挂着上一首歌的情绪没有意义。
        var sum: Float = 0
        for i in stride(from: 0, to: Self.windowSamples, by: 16) {   // 抽样估计，够判静音
            let v = samples[i]; sum += v * v
        }
        guard (sum / Float(Self.windowSamples / 16)).squareRoot() >= 1e-4 else { return nil }

        input.withUnsafeMutableBytes { raw, _ in
            raw.baseAddress!.assumingMemoryBound(to: Float.self)
                .update(from: samples, count: Self.windowSamples)
        }
        guard let provider = try? MLDictionaryFeatureProvider(dictionary: [Self.inputName: input]),
              let out = try? model.prediction(from: provider),
              let emb = out.featureValue(for: Self.outputName)?.multiArrayValue,
              emb.count == anchors[0].embedding.count
        else { return nil }

        // 模型里已经做过 L2 归一化，锚点也是，所以点积即余弦
        var sims = [Float](repeating: 0, count: anchors.count)
        emb.withUnsafeBufferPointer(ofType: Float.self) { e in
            for (i, a) in anchors.enumerated() {
                var s: Float = 0
                for k in 0..<a.embedding.count { s += e[k] * a.embedding[k] }
                sims[i] = s
            }
        }

        // softmax(sim × scale)，与 Python 版同式
        let peak = sims.max() ?? 0
        var weights = sims.map { expf(($0 - peak) * softmaxScale) }
        let total = weights.reduce(0, +)
        guard total > 0, total.isFinite else { return nil }
        for i in weights.indices { weights[i] /= total }

        var valence: Float = 0, energy: Float = 0
        var best = 0
        for (i, a) in anchors.enumerated() {
            valence += weights[i] * a.valence
            energy += weights[i] * a.energy
            if weights[i] > weights[best] { best = i }
        }
        return MoodReading(valence: valence, energy: energy,
                           emoID: UInt8(best), label: anchors[best].label)
    }
}
