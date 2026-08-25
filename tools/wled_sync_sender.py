#!/usr/bin/env python3
"""WLED UDP Sound Sync V2 发送端（macOS）。

从 BlackHole 2ch 读系统音频 → FFT/音量分析 → 44 字节 V2 包发往
组播 239.0.0.1:11988 + 单播板子。--monitor 时同时把音频转发到真实
扬声器（未建多输出设备时的过渡方案）。

包结构抄自 fork usermods/audioreactive/audio_reactive.cpp（44B packed）：
  header[6]="00002", pad[2], f32 sampleRaw, f32 sampleSmth, u8 samplePeak,
  u8 pad, u8 fftResult[16], u16 pad, f32 FFT_Magnitude, f32 FFT_MajorPeak
接收端 decodeAudioData 直接取值：音量域 0~255，MajorPeak 1~11025Hz。
"""
import argparse, socket, struct, sys, time
import numpy as np
import sounddevice as sd

RATE = 48000
BLOCK = 1024                       # ~21ms/块 → ~47 包/秒
NUM_GEQ = 16
# WLED GEQ 频段近似：43Hz~10kHz 对数分布
EDGES = np.logspace(np.log10(43), np.log10(10000), NUM_GEQ + 1)

def find_device(sub, kind):
    for i, d in enumerate(sd.query_devices()):
        if sub.lower() in d["name"].lower() and d[f"max_{kind}_channels"] > 0:
            return i, d["name"]
    return None, None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="wled-c4d3a4.local", help="板子地址（单播）")
    ap.add_argument("--input", default="BlackHole", help="采样输入设备名子串")
    ap.add_argument("--monitor", default="", help="转发输出设备名子串（过渡：软件回放）")
    ap.add_argument("--gain", type=float, default=1.0)
    args = ap.parse_args()

    in_idx, in_name = find_device(args.input, "input")
    if in_idx is None:
        print("找不到输入设备:", args.input); sys.exit(1)
    print("输入:", in_name)

    out_idx = None
    if args.monitor:
        out_idx, out_name = find_device(args.monitor, "output")
        if out_idx is None:
            print("找不到监听输出:", args.monitor); sys.exit(1)
        print("监听转发:", out_name)

    try:
        target_ip = socket.gethostbyname(args.target)
    except OSError:
        target_ip = None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    # 多网卡机器：组播必须显式绑定与板子同网段的出口接口，
    # 否则走默认路由抛 errno 65 (No route to host)
    if target_ip:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((target_ip, 11988))
        local_ip = probe.getsockname()[0]
        probe.close()
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                        socket.inet_aton(local_ip))
        print("出口接口:", local_ip)
    print("发送: 239.0.0.1:11988", "+ %s:11988" % target_ip if target_ip else "(单播不可用)")

    window = np.hanning(BLOCK)
    freqs = np.fft.rfftfreq(BLOCK, 1 / RATE)
    band_idx = [(freqs >= EDGES[i]) & (freqs < EDGES[i + 1]) for i in range(NUM_GEQ)]
    smth = 0.0
    env = 1000.0                    # 自动标度包络
    last_stat = time.time()
    stat = {"pk": 0, "n": 0}

    def process(mono):
        nonlocal smth, env
        spec = np.abs(np.fft.rfft(mono * window))
        bands = np.array([spec[m].max() if m.any() else 0.0 for m in band_idx])
        env = max(env * 0.999, bands.max(), 200.0)          # 慢衰减自动增益
        fft8 = np.clip(bands / env * 255 * args.gain, 0, 255).astype(np.uint8)
        vol = float(np.clip(np.sqrt((mono ** 2).mean()) * 2500 * args.gain, 0, 255))
        peak = 1 if vol > smth * 1.6 and vol > 30 else 0
        smth = smth * 0.82 + vol * 0.18
        mp = float(np.clip(freqs[np.argmax(spec[1:]) + 1], 1, 11025))
        pkt = struct.pack("<6s2sffBB16s2sff", b"00002\0", b"\0\0",
                          vol, smth, peak, 0, fft8.tobytes(), b"\0\0",
                          float(bands.max()), mp)
        stat["pk"] = max(stat["pk"], vol); stat["n"] += 1
        # 网络瞬断绝不能杀回调——音频流一停就再也起不来
        try: sock.sendto(pkt, ("239.0.0.1", 11988))
        except OSError: pass
        if target_ip:
            try: sock.sendto(pkt, (target_ip, 11988))
            except OSError: pass

    def cb_duplex(indata, outdata, frames, t, status):
        outdata[:] = indata
        process(indata.mean(axis=1))

    def cb_in(indata, frames, t, status):
        process(indata.mean(axis=1))

    if out_idx is not None:
        stream = sd.Stream(device=(in_idx, out_idx), samplerate=RATE,
                           blocksize=BLOCK, channels=2, callback=cb_duplex)
    else:
        stream = sd.InputStream(device=in_idx, samplerate=RATE,
                                blocksize=BLOCK, channels=2, callback=cb_in)
    with stream:
        print("运行中，Ctrl-C 退出")
        while True:
            time.sleep(2)
            now = time.time()
            print("pkts=%d vol-peak=%.0f" % (stat["n"], stat["pk"]), flush=True)
            stat["pk"] = 0; stat["n"] = 0; last_stat = now

if __name__ == "__main__":
    main()
