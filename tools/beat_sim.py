#!/usr/bin/env python3
"""合成 81BPM boom-boom-clap-rest 测试节奏（We Will Rock You 型，4/4）。

受控验收标准素材：拍点明确、小节结构规整，用于 Bar Impact / Bar Ladder /
Beat 系效果的肉眼对账。拍 0/1 低频鼓、拍 2 宽带 clap、拍 3 休止——
Bar Ladder 下应看到阶梯 1-2-3 格推进后清空换色。

用法: python3 beat_sim.py [秒数=24] [输出.wav=~/.local/wled-sync/beat_sim.wav]
"""
import math
import os
import random
import struct
import sys
import wave

BPM = 81.0
SR = 44100
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 24.0
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.local/wled-sync/beat_sim.wav")

spb = 60.0 / BPM
n = int(DUR * SR)
buf = [0.0] * n
random.seed(7)


def add_kick(t0):
    # 低频鼓：80→45Hz 扫频衰减正弦
    for i in range(int(0.18 * SR)):
        t = i / SR
        f = max(45.0, 80.0 - 150.0 * t)
        j = int(t0 * SR) + i
        if 0 <= j < n:
            buf[j] += 0.9 * math.exp(-t * 22.0) * math.sin(2 * math.pi * f * t)


def add_clap(t0):
    # 宽带噪声脉冲
    for i in range(int(0.12 * SR)):
        t = i / SR
        j = int(t0 * SR) + i
        if 0 <= j < n:
            buf[j] += 0.55 * math.exp(-t * 30.0) * (random.random() * 2 - 1)


beat, t = 0, 0.1
while t < DUR - 0.3:
    b = beat % 4
    if b in (0, 1):
        add_kick(t)
    elif b == 2:
        add_clap(t)
    beat += 1
    t += spb

peak = max(abs(x) for x in buf) or 1.0
with wave.open(OUT, "w") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(b"".join(struct.pack("<h", int(x / peak * 32000)) for x in buf))
print("写出 %s：%.0fs @ %s BPM 4/4" % (OUT, DUR, BPM))
