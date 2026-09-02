#!/usr/bin/env python3
"""WLED 音频同步发送端 v2（macOS）—— liblamp 完整管线版。

BlackHole 采样（22050Hz，CoreAudio 自动重采样）→ liblamp（与固件/模拟器
同一份 C++：FFT/HPSS/色度/音高/人声/节拍/段落/档位）→ 双协议发送：
  · LAMP1 (11989)：完整 AudioFrame，336B —— ♪ lamp 灯效的旋律/人声类吃它
  · WLED V2 (11988)：44B 兼容包 —— WLED 原生音频灯效（Gravimeter 等）吃它
包布局与 usermods/lamp/lamp_wled_fx.cpp 的 LampSyncPacket 逐字节对应，
改一边必须同步另一边。AGC/静音门由 lamp 管线内建（gain/gated），不再自做。
"""
import argparse, ctypes as C, socket, struct, sys, time
import numpy as np
import sounddevice as sd

RATE = 22050            # kSampleRate（lamp_window.h），CoreAudio 自动重采样
BLOCK = 1024
NB, NCH, NLED = 16, 12, 96

class LampFrameC(C.Structure):
    _fields_ = [("bands", C.c_float * NB),
                ("chroma", C.c_float * NCH),
                ("bpm", C.c_float), ("conf", C.c_float), ("phase", C.c_float),
                ("rms", C.c_float), ("peak", C.c_float), ("gain", C.c_float),
                ("rate", C.c_float), ("centroid", C.c_float), ("flatness", C.c_float),
                ("key_conf", C.c_float), ("harmony", C.c_float),
                ("f0", C.c_float), ("f0_conf", C.c_float), ("mood", C.c_float),
                ("trend", C.c_float), ("novelty", C.c_float),
                ("dynamics", C.c_float), ("percussive", C.c_float), ("bar_conf", C.c_float),
                ("vocal", C.c_float),
                ("bands_h", C.c_float * NB), ("bands_p", C.c_float * NB),
                ("lock", C.c_int32), ("gate", C.c_int32), ("onset", C.c_int32),
                ("preset", C.c_int32), ("key_root", C.c_int32), ("key_major", C.c_int32),
                ("f0_voiced", C.c_int32), ("section", C.c_int32),
                ("bpb", C.c_int32), ("bar_pos", C.c_int32),
                ("bar_index", C.c_int32), ("downbeat", C.c_int32), ("auto_fx", C.c_int32),
                ("vocal_onset", C.c_int32),
                ("auto_score", C.c_float * 5),
                ("auto_duty", C.c_float), ("auto_jit", C.c_float),
                ("auto_perc", C.c_float), ("auto_split", C.c_float),
                ("px", C.c_uint8 * (NLED * 3))]

LAMP1_FMT = "<6sBB48f12f20fbBBBi"       # 336B，与 LampSyncPacket 对应
assert struct.calcsize(LAMP1_FMT) == 336

def find_device(sub, kind):
    for i, d in enumerate(sd.query_devices()):
        if sub.lower() in d["name"].lower() and d[f"max_{kind}_channels"] > 0:
            return i, d["name"]
    return None, None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="wled-c4d3a4.local")
    ap.add_argument("--input", default="BlackHole")
    ap.add_argument("--lib", default="/Users/hjma/workspace/iflytek/numira/wled-ambient-light/tools/liblamp.dylib")
    ap.add_argument("--gain", type=float, default=1.0, help="进管线前的手动增益")
    args = ap.parse_args()

    lib = C.CDLL(args.lib)
    lib.lamp_create.restype = C.c_void_p
    lib.lamp_destroy.argtypes = [C.c_void_p]
    lib.lamp_feed.argtypes = [C.c_void_p, C.POINTER(C.c_float), C.c_int32,
                              C.POINTER(LampFrameC), C.c_int32]
    lib.lamp_feed.restype = C.c_int32
    lib.lamp_set_input_gain.argtypes = [C.c_void_p, C.c_float]
    h = lib.lamp_create()
    lib.lamp_set_input_gain(h, args.gain)
    print("liblamp 就绪:", args.lib)

    in_idx, in_name = find_device(args.input, "input")
    if in_idx is None:
        print("找不到输入设备:", args.input); sys.exit(1)
    print("输入:", in_name)

    try:
        target_ip = socket.gethostbyname(args.target)
    except OSError:
        target_ip = None
    def probe_local(tip):
        try:
            pr = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            pr.connect((tip, 11988)); lip = pr.getsockname()[0]; pr.close()
            return lip
        except OSError:
            return None
    def make_sock(tip):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        lip = probe_local(tip) if tip else None
        if lip:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(lip))
        return s, lip
    sock, local_ip = make_sock(target_ip)
    print("出口接口:", local_ip, "| 发送: LAMP1→11989 + V2→11988，目标", target_ip or "组播")

    frames = (LampFrameC * 8)()
    smth = 0.0
    stat = {"n": 0, "vol": 0.0, "rich": ""}

    vol_env = 0.02

    def send_frame(f):
        nonlocal smth
        # 静音（管线 gate）不发包：500ms 后板子自动回落本地麦克风 ——
        # 否则常驻 sender 的静音帧会永久遮蔽本地音源。
        if f.gate:
            stat["rich"] = "静音（已让位本地音源）"
            return
        flags = ((1 if f.onset else 0) | (2 if f.lock else 0) | (4 if f.downbeat else 0)
                 | (8 if f.gate else 0) | (16 if f.section else 0)
                 | (32 if f.vocal_onset else 0) | (64 if f.f0_voiced else 0)
                 | (128 if f.key_major else 0))
        floats48 = list(f.bands) + list(f.bands_h) + list(f.bands_p)
        pkt1 = struct.pack(LAMP1_FMT, b"LAMP1\0", 1, flags,
                           *floats48, *list(f.chroma),
                           f.rms, f.peak, f.gain, f.bpm, f.conf, f.phase, f.rate,
                           f.centroid, f.flatness, f.key_conf, f.harmony,
                           f.f0, f.f0_conf, f.mood, f.trend, f.novelty, f.dynamics,
                           f.percussive, f.bar_conf, f.vocal,
                           max(-1, min(11, f.key_root)), max(0, min(255, f.preset)),
                           f.bpb & 0xFF, f.bar_pos & 0xFF, f.bar_index)
        # V2 兼容包：bands 按 kFxBandScale=6 反向（实测恰好满格）。
        # rms→vol 必须包络归一而不是固定系数：AGC 稳态随素材漂（实测
        # 0.03→0.12），固定系数会恒饱和 → 原生效果失去动态（实测踩中）。
        nonlocal vol_env
        vol_env = max(vol_env * 0.9995, f.rms, 1e-4)
        vol = float(np.clip(f.rms / vol_env * 220.0, 0, 255))
        smth = smth * 0.8 + vol * 0.2
        fft8 = bytes(int(np.clip(b * 6.0 * 255.0, 0, 255)) for b in f.bands)
        mp = f.f0 if f.f0_voiced and f.f0 > 1 else max(1.0, min(11025.0, f.centroid))
        pkt2 = struct.pack("<6s2sffBB16s2sff", b"00002\0", b"\0\0",
                           vol, smth, 1 if f.onset else 0, 0, fft8, b"\0\0",
                           float(max(f.bands) * 1000.0), mp)
        for pkt, port in ((pkt1, 11989), (pkt2, 11988)):
            try: sock.sendto(pkt, ("239.0.0.1", port))
            except OSError: pass
            if target_ip:
                try: sock.sendto(pkt, (target_ip, port))
                except OSError: pass
        stat["n"] += 1; stat["vol"] = max(stat["vol"], vol)
        stat["on"] = stat.get("on", 0) + (1 if f.onset else 0)
        stat["rich"] = "rms=%.4f dyn=%.2f perc=%.2f onset/2s=%d phase=%.2f bmax=%.4f" % (
            f.rms, f.dynamics, f.percussive, stat["on"], f.phase, max(f.bands))

    def cb(indata, nframes, t, status):
        mono = np.ascontiguousarray(indata.mean(axis=1), dtype=np.float32)
        n = lib.lamp_feed(h, mono.ctypes.data_as(C.POINTER(C.c_float)),
                          len(mono), frames, 8)
        for k in range(n): send_frame(frames[k])

    with sd.InputStream(device=in_idx, samplerate=RATE, blocksize=BLOCK,
                        channels=2, callback=cb):
        print("运行中，Ctrl-C 退出")
        last_resolve = time.time()
        while True:
            time.sleep(2)
            print("pkts=%d vol-peak=%.0f %s" % (stat["n"], stat["vol"], stat["rich"]), flush=True)
            stat["n"] = 0; stat["vol"] = 0.0; stat["on"] = 0
            # 自愈：网络路径变化（ZT/WiFi 切换）后老 socket 会「活着但断路」——
            # 照常 sendto、板子颗粒无收（实测踩中，需手动重启才恢复）。
            # 每 2s 探测出口 IP，变化即重建；mDNS 每 60s 重解析防板子换 IP。
            if target_ip:
                cur = probe_local(target_ip)
                if cur and cur != local_ip:
                    print("出口变化 %s → %s，重建 socket" % (local_ip, cur), flush=True)
                    try: sock.close()
                    except OSError: pass
                    sock, local_ip = make_sock(target_ip)
            if time.time() - last_resolve > 60:
                last_resolve = time.time()
                try:
                    nip = socket.gethostbyname(args.target)
                    if nip != target_ip:
                        print("目标变化 %s → %s" % (target_ip, nip), flush=True)
                        target_ip = nip
                        try: sock.close()
                        except OSError: pass
                        sock, local_ip = make_sock(target_ip)
                except OSError: pass

if __name__ == "__main__":
    main()
