#!/usr/bin/env python3
"""交叉验证 App 里的 CLAP 集成是否与 PyTorch 原版一致。

    python3 crosscheck.py            # 只算 PyTorch 参考值
    python3 crosscheck.py --play     # 同时播出来，好对着板子的 /lampdata 看

做法：合成两段性质相反的音频，先用 PyTorch 原版 CLAP 算出它该给什么分，再
播给 App 听，比对板子收到的 val/emo。

**要比的是 App 与 PyTorch 是否一致，不是模型判得对不对。** 模型的语义能力是
既定的（Python 版一直在用），这里要抓的是集成引入的偏差。实测时合成的「悲伤」
片段被两边一致判成 cheerful —— 纯正弦波的小调琶音确实不像悲伤音乐，这是素材
的局限，不是实现的问题。真要评模型效果，得拿真实音乐。
"""
import argparse, json, os, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np

SR = 44100
HERE = os.path.dirname(os.path.abspath(__file__))


def tone(f, dur, amp=0.25, harm=(1.0, 0.5, 0.25)):
    t = np.arange(int(SR * dur)) / SR
    x = sum(a * np.sin(2 * np.pi * f * (i + 1) * t) for i, a in enumerate(harm))
    env = np.minimum(1.0, np.minimum(t * 40, (dur - t) * 8))      # 快起慢落
    return (amp * x * env).astype(np.float32)


def build(kind, seconds=24):
    if kind == "happy":         # C 大调琶音，140 BPM，明亮
        notes = [523.25, 659.25, 783.99, 1046.50, 783.99, 659.25]
        beat, harm, amp, bass = 60.0 / 140 / 2, (1.0, 0.6, 0.4), 0.3, [130.81, 196.00]
    else:                       # A 小调，60 BPM，低沉缓慢
        notes = [220.00, 261.63, 329.63, 261.63]
        beat, harm, amp, bass = 60.0 / 60, (1.0, 0.3, 0.1), 0.22, [55.00, 82.41]
    out = []
    for i in range(int(seconds / beat)):
        seg = tone(notes[i % len(notes)], beat, amp, harm)
        seg = seg + tone(bass[i % len(bass)], beat, amp * 0.5, (1.0, 0.2))[:len(seg)]
        out.append(seg)
    x = np.concatenate(out)[:int(SR * seconds)]
    return x / (np.abs(x).max() + 1e-9) * 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--play", action="store_true", help="播出来（内建扬声器），好对着板子看")
    ap.add_argument("--anchors", default=os.path.join(HERE, "..", "Resources", "anchors.json"))
    args = ap.parse_args()

    anchors = json.load(open(args.anchors))
    temb = np.array([a["embedding"] for a in anchors["anchors"]], dtype=np.float32)
    AV = np.array([a["valence"] for a in anchors["anchors"]])
    AE = np.array([a["energy"] for a in anchors["anchors"]])
    names = [a["prompt"].split()[0] for a in anchors["anchors"]]
    scale = float(anchors.get("softmaxScale", 25.0))

    import torch, torch.nn as nn, laion_clap
    m = laion_clap.CLAP_Module(enable_fusion=False)
    m.load_ckpt()

    class Tower(nn.Module):
        def __init__(s, c):
            super().__init__(); s.ab = c.audio_branch; s.ap = c.audio_projection
        def forward(s, w):
            o = s.ab({"waveform": w}, None, device=w.device)
            e = s.ap(o["embedding"]); return e / e.norm(dim=-1, keepdim=True)

    tower = Tower(m.model).eval()
    if args.play:
        import sounddevice as sd
        dev = next(i for i, d in enumerate(sd.query_devices())
                   if "扬声器" in d["name"] and d["max_output_channels"] > 0)

    print("%-8s %-22s %9s %9s" % ("素材", "PyTorch argmax", "valence", "energy"))
    for kind in ("happy", "sad"):
        x = build(kind)
        x48 = np.interp(np.linspace(0, len(x) - 1, int(len(x) * 48000 / SR)),
                        np.arange(len(x)), x).astype(np.float32)
        mid = len(x48) // 2
        seg = x48[mid - 240000:mid + 240000]           # 中间 10 秒，避开首尾包络
        with torch.no_grad():
            e = tower(torch.from_numpy(seg)[None, :]).numpy().reshape(-1)
        sims = temb @ e
        w = np.exp((sims - sims.max()) * scale); w /= w.sum()
        i = int(np.argmax(w))
        print("%-8s %-22s %+9.3f %9.3f" % (kind, "%d(%s)" % (i, names[i]), float(w @ AV), float(w @ AE)))
        print("         前三: " + ", ".join("%s %.3f" % (names[j], w[j]) for j in np.argsort(-w)[:3]))
        if args.play:
            print("         播放中，对着 curl http://<灯>/lampdata 看 val/emo …", flush=True)
            sd.play(x.reshape(-1, 1), samplerate=SR, device=dev); sd.wait()


if __name__ == "__main__":
    main()
