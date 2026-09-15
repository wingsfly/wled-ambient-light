#!/usr/bin/env python3
"""把 LAION-CLAP 导出成 App 能用的两个文件。

    ClapAudioTower.mlpackage   audio 塔，Core ML fp32，约 126 MB
    anchors.json               8 个情绪锚点的 text embedding，约 42 KB

跑法（需要一个装了 laion-clap + coremltools 的 Python 环境）：

    python3 export_clap.py --out ../Resources

两个关键决定，都是实测出来的（2026-09-15，M4 Max / macOS 26.5.2）：

**文本塔不上 App。** 我们的用法里 8 个锚点的 embedding 是常量（Python 版
sender 也只在启动时算一次），离线算好存成 42 KB 就够，RoBERTa 那 499 MB
和 checkpoint 里 1228 MB 的 optimizer 状态全都不需要。1.86 GB → 126 MB。

**必须用 fp32。** Core ML 默认的 fp16 在这个模型上不能用：
    fp32  126 MB  85 ms  余弦 1.000000  valence 偏差 0.0000
    fp16   63 MB  23 ms  余弦 0.954071  valence 偏差 0.2469
valence 值域是 -1..1，0.25 的偏差是 12% 满量程，语义情绪会明显跑偏。省下
的 63 MB 不值这个代价。
"""
import argparse, json, os, warnings
warnings.filterwarnings("ignore")
import numpy as np, torch, torch.nn as nn, torch.nn.functional as F
import coremltools as ct
from coremltools.converters.mil.frontend.torch.torch_op_registry import register_torch_op, _TORCH_OPS_REGISTRY
from coremltools.converters.mil.frontend.torch.ops import _get_inputs
from coremltools.converters.mil import Builder as mb

SAMPLES = 480000          # 10 秒 @ 48 kHz，CLAP 的输入长度
ANCHORS = [
    # (提示文本, valence 愉悦度 -1..1, energy 激活度 0..1)
    ("aggressive intense heavy music",          -0.6, 1.0),
    ("energetic happy dance music",              0.8, 0.9),
    ("cheerful upbeat pop song",                 0.9, 0.6),
    ("peaceful calm relaxing ambient music",     0.5, 0.1),
    ("sad melancholic slow emotional music",    -0.8, 0.2),
    ("dark tense ominous suspenseful music",    -0.7, 0.5),
    ("romantic gentle warm acoustic music",      0.6, 0.3),
    ("epic dramatic powerful orchestral music",  0.2, 0.8),
]


def bicubic_matrix(n_in, n_out, align, on_height):
    """bicubic 沿单一维度插值是线性算子 —— 权重只取决于位置，不取决于数据。
    对单位矩阵做一次同样的插值，得到的就是它的矩阵。"""
    if on_height:
        eye = torch.eye(n_in).reshape(n_in, 1, n_in, 1)
        m = F.interpolate(eye, (n_out, 1), mode="bicubic", align_corners=align).reshape(n_in, n_out)
        return m.t().contiguous().numpy().astype(np.float32)      # (n_out, n_in)
    eye = torch.eye(n_in).reshape(n_in, 1, 1, n_in)
    m = F.interpolate(eye, (1, n_out), mode="bicubic", align_corners=align).reshape(n_in, n_out)
    return m.contiguous().numpy().astype(np.float32)              # (n_in, n_out)


def register_exact_bicubic():
    """Core ML 没有 bicubic，coremltools 会直接报 NotImplementedError。

    这里接管转换，把插值矩阵作为常量嵌进 MIL 图用 matmul 算。
    注意**不要**改成 patch PyTorch 的 nn.functional.interpolate：那条路上
    矩阵是懒构造的，trace 期缓存一旦没命中，构造矩阵时调的那次原始 bicubic
    就会被记进图，转换照样失败（实测踩过）。
    """
    _TORCH_OPS_REGISTRY.name_to_func_mapping.pop("upsample_bicubic2d", None)

    @register_torch_op
    def upsample_bicubic2d(context, node):
        ins = _get_inputs(context, node, expected=4)
        x, size, align = ins[0], ins[1], bool(ins[2].val)
        h_in, w_in = int(x.shape[2]), int(x.shape[3])
        sz = np.array(size.val).reshape(-1)
        h_out, w_out = int(sz[0]), int(sz[1])
        y = x
        if h_out != h_in:
            y = mb.matmul(x=bicubic_matrix(h_in, h_out, align, True), y=y)
        if w_out != w_in:
            y = mb.matmul(x=y, y=bicubic_matrix(w_in, w_out, align, False))
        context.add(mb.identity(x=y, name=node.name))


class AudioTower(nn.Module):
    """只要 audio 塔，且把 L2 归一化也做进模型 —— Swift 侧少一步。"""
    def __init__(self, clap):
        super().__init__()
        self.audio_branch = clap.audio_branch
        self.audio_projection = clap.audio_projection

    def forward(self, waveform):                       # (1, 480000) @48 kHz mono
        out = self.audio_branch({"waveform": waveform}, None, device=waveform.device)
        e = self.audio_projection(out["embedding"])
        return e / e.norm(dim=-1, keepdim=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="../Resources", help="输出目录")
    ap.add_argument("--fp16", action="store_true", help="导出 fp16（体积减半但语义情绪会跑偏，别用）")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    import laion_clap
    model = laion_clap.CLAP_Module(enable_fusion=False)
    model.load_ckpt()

    temb = model.get_text_embedding([a[0] for a in ANCHORS], use_tensor=False)
    temb = temb / np.linalg.norm(temb, axis=1, keepdims=True)
    apath = os.path.join(out, "anchors.json")
    json.dump({"dim": int(temb.shape[1]), "softmaxScale": 25.0,
               "anchors": [{"prompt": a[0], "valence": a[1], "energy": a[2],
                            "embedding": temb[i].astype(float).round(6).tolist()}
                           for i, a in enumerate(ANCHORS)]},
              open(apath, "w"), ensure_ascii=False)
    print("锚点常量 → %s (%.1f KB)" % (apath, os.path.getsize(apath) / 1024))

    tower = AudioTower(model.model).eval()
    x = torch.zeros(1, SAMPLES)
    x[0, ::977] = 0.3                      # 稀疏脉冲：只为 trace 走通形状
    with torch.no_grad():
        ref = tower(x).numpy().reshape(-1)
        traced = torch.jit.trace(tower, x, strict=False)

    register_exact_bicubic()
    prec = ct.precision.FLOAT16 if args.fp16 else ct.precision.FLOAT32
    mlmodel = ct.convert(traced,
                         inputs=[ct.TensorType(name="waveform", shape=(1, SAMPLES), dtype=np.float32)],
                         outputs=[ct.TensorType(name="embedding", dtype=np.float32)],
                         minimum_deployment_target=ct.target.macOS15,
                         compute_precision=prec, convert_to="mlprogram")
    mpath = os.path.join(out, "ClapAudioTower.mlpackage")
    mlmodel.save(mpath)
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(mpath) for f in fs)
    print("audio 塔 → %s (%.1f MB, %s)" % (mpath, size / 1e6, "fp16" if args.fp16 else "fp32"))

    got = np.asarray(ct.models.MLModel(mpath).predict({"waveform": x.numpy()})["embedding"]).reshape(-1)
    got = got / np.linalg.norm(got)
    cos = float(got @ ref)
    print("与 PyTorch 的余弦: %.6f %s" % (cos, "✅" if cos > 0.9999 else "⚠️ 偏差偏大"))


if __name__ == "__main__":
    main()
