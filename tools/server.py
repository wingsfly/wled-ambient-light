#!/usr/bin/env python3
"""模拟服务器：浏览器采音频，这里跑 C++ 算法。

页面把 PCM 通过 WebSocket 送过来，服务端用 ctypes 调 liblamp（就是
usermods/lamp 那批头文件编译出来的），把 AudioFrame 送回去。

这样麦克风、文件、链接三种输入共用同一条代码路径，而且跑的是原生编译的
固件同源代码 —— 不是 JS 重写的仿制品。

    python3 server.py [--port 8080] [--host 0.0.0.0]

WebSocket 用标准库手写（握手 + 帧解析约 80 行），不引第三方依赖 ——
这台机器是 temporary-workloads-only，装东西不合适。
"""
import argparse, base64, hashlib, json, mimetypes, os, socket, struct, sys, threading
from urllib.parse import urlparse, parse_qs
import ctypes as C
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "liblamp.dylib" if sys.platform == "darwin" else "liblamp.so")
NB, NLED = 16, 96

# 与 lamp_capi.cpp 的 LampFrameC 逐字段对应。ctypes 不会替你检查布局 ——
# 改了那边必须同步改这里，否则读出来的是错位的垃圾而不是报错。
class Frame(C.Structure):
    _fields_ = [("bands", C.c_float * NB),
                ("chroma", C.c_float * 12),
                ("bpm", C.c_float), ("conf", C.c_float), ("phase", C.c_float),
                ("rms", C.c_float), ("peak", C.c_float), ("gain", C.c_float),
                ("rate", C.c_float), ("centroid", C.c_float), ("flatness", C.c_float),
                ("key_conf", C.c_float), ("harmony", C.c_float),
                ("f0", C.c_float), ("f0_conf", C.c_float), ("mood", C.c_float),
                ("trend", C.c_float), ("novelty", C.c_float),
                ("dynamics", C.c_float), ("percussive", C.c_float), ("bar_conf", C.c_float),
                ("bands_h", C.c_float * NB), ("bands_p", C.c_float * NB),
                ("lock", C.c_int32), ("gate", C.c_int32), ("onset", C.c_int32),
                ("preset", C.c_int32), ("key_root", C.c_int32), ("key_major", C.c_int32),
                ("f0_voiced", C.c_int32), ("section", C.c_int32),
                ("bpb", C.c_int32), ("bar_pos", C.c_int32),
                ("bar_index", C.c_int32), ("downbeat", C.c_int32), ("auto_fx", C.c_int32),
                ("auto_score", C.c_float * 5),
                ("auto_duty", C.c_float), ("auto_jit", C.c_float),
                ("auto_perc", C.c_float), ("auto_split", C.c_float),
                ("px", C.c_uint8 * (NLED * 3))]

# ── WLED 固定灯效 ─────────────────────────────────────────
# 跑的是 WLED **自己的** FX.cpp（见 tools/hostwled/README.md）。
WLIB = os.path.join(HERE, "libwledfx.dylib" if sys.platform == "darwin" else "libwledfx.so")

def load_wled():
    """加载失败不致命 —— 音乐律动那半边照常可用，只是固定灯效页不可用。"""
    if not os.path.exists(WLIB):
        return None
    w = C.CDLL(WLIB)
    w.wledfx_init.restype = C.c_int32
    w.wledfx_mode_data.restype = C.c_char_p
    w.wledfx_mode_data.argtypes = [C.c_int32]
    w.wledfx_mode_count.restype = C.c_int32
    w.wledfx_mode_blocked.restype = C.c_int32
    w.wledfx_mode_blocked.argtypes = [C.c_int32]
    w.wledfx_palette_count.restype = C.c_int32
    w.wledfx_set.argtypes = [C.c_int32] * 4 + [C.c_uint32] * 3
    w.wledfx_render.argtypes = [C.c_uint32, C.POINTER(C.c_uint8)]
    w.wledfx_init()
    return w

def wled_catalog(w):
    """效果表直接取自 WLED 的 _modeData，界面不用手抄一份。"""
    out = []
    for i in range(w.wledfx_mode_count()):
        raw = w.wledfx_mode_data(i).decode("utf-8", "replace")
        out.append({"i": i, "d": raw, "x": bool(w.wledfx_mode_blocked(i))})
    return {"modes": out, "palettes": w.wledfx_palette_count()}

def load():
    if not os.path.exists(LIB):
        sys.exit(f"找不到 {LIB}\n先构建：\n  cd {HERE} && ./build.sh")
    lib = C.CDLL(LIB)
    lib.lamp_create.restype = C.c_void_p
    lib.lamp_destroy.argtypes = [C.c_void_p]
    lib.lamp_set_effect.argtypes = [C.c_void_p, C.c_int32, C.c_int32]
    lib.lamp_lock_preset.argtypes = [C.c_void_p, C.c_int32]
    lib.lamp_set_input_gain.argtypes = [C.c_void_p, C.c_float]
    lib.lamp_sample_rate.restype = C.c_float
    lib.lamp_feed.argtypes = [C.c_void_p, C.POINTER(C.c_float), C.c_int32,
                              C.POINTER(Frame), C.c_int32]
    lib.lamp_feed.restype = C.c_int32
    lib.lamp_frame_size.restype = C.c_int32
    # 布局自检。ctypes 不会替你核对字段，布局错了读到的是错位的数而不是异常 ——
    # 这种错极难查，宁可启动时就退出。
    got, want = lib.lamp_frame_size(), C.sizeof(Frame)
    if got != want:
        sys.exit(f"LampFrameC 布局不一致：C={got}B Python={want}B。"
                 f"改过结构体就要两边一起改。")
    return lib

# ── WebSocket（标准库手写）────────────────────────────────
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def ws_accept(key):
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()

def ws_send(sock, payload, opcode=0x1):
    n = len(payload)
    hdr = bytearray([0x80 | opcode])
    if n < 126:      hdr.append(n)
    elif n < 65536:  hdr.append(126); hdr += struct.pack(">H", n)
    else:            hdr.append(127); hdr += struct.pack(">Q", n)
    sock.sendall(bytes(hdr) + payload)

def ws_recv(sock):
    """返回 (opcode, payload)；连接关闭返回 (None, None)。"""
    def rd(n):
        b = b""
        while len(b) < n:
            c = sock.recv(n - len(b))
            if not c: return None
            b += c
        return b
    h = rd(2)
    if not h: return None, None
    op, l = h[0] & 0x0F, h[1] & 0x7F
    masked = h[1] & 0x80
    if l == 126:   l = struct.unpack(">H", rd(2))[0]
    elif l == 127: l = struct.unpack(">Q", rd(8))[0]
    if l > 8 << 20: return None, None          # 8MB 上限，防跑飞
    mask = rd(4) if masked else None
    data = rd(l) if l else b""
    if data is None: return None, None
    if mask:
        # 批量异或。逐字节生成器在 8KB 的音频块上是可测量的开销，
        # 而麦克风每秒会送十几块。
        m = (mask * ((l + 3) // 4))[:l]
        data = (int.from_bytes(data, "big") ^ int.from_bytes(m, "big")).to_bytes(l, "big") if l else b""
    return op, data

MAXF = 64      # 一次 feed 最多产出多少帧

def serve_ws(sock, lib):
    # 实时流必须关 Nagle。不关的话小包会被攒着等 ACK，而我们是
    # 「每块音频回一批结果」的一问一答模式 —— 正好撞上延迟 ACK，
    # 实测跨 ZeroTier 时吞吐掉到实时的十分之一。
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass
    h = lib.lamp_create()
    if not h:
        ws_send(sock, b'{"error":"lamp_create failed"}'); return
    buf = (Frame * MAXF)()
    try:
        while True:
            op, data = ws_recv(sock)
            if op is None or op == 0x8: break          # 关闭
            if op == 0x9: ws_send(sock, data, 0xA); continue   # ping
            if op == 0x1:                                       # 文本 = 控制
                try:
                    m = json.loads(data)
                    if "fx" in m or "wb" in m:
                        lib.lamp_set_effect(h, int(m.get("fx", 0)), int(m.get("wb", 1)))
                    if "preset" in m:
                        lib.lamp_lock_preset(h, int(m["preset"]))
                    if "ig" in m:
                        lib.lamp_set_input_gain(h, C.c_float(float(m["ig"])))
                except Exception:
                    pass
                continue
            if op != 0x2: continue                              # 只处理二进制 PCM
            n = len(data) // 4
            if n == 0: continue
            # 一次别喂太多。浏览器正常一块是 2048 样本；远超这个量说明发送侧
            # 出了问题（比如把整个 ArrayBuffer 当成一块发了），截断而不是照单全收。
            if n > 1 << 20:
                sys.stderr.write(f"丢弃异常大的音频块：{n} 样本\n"); continue
            pcm = (C.c_float * n).from_buffer_copy(data)
            got = lib.lamp_feed(h, pcm, n, buf, MAXF)
            if got <= 0: continue
            out = []
            for i in range(got):
                f = buf[i]
                out.append({
                    "b": [round(x, 4) for x in f.bands],
                    "bpm": round(f.bpm, 2), "conf": round(f.conf, 3),
                    "ph": round(f.phase, 4), "rms": round(f.rms, 4),
                    "pk": round(f.peak, 4), "g": round(f.gain, 2),
                    "rate": round(f.rate, 2), "cen": round(f.centroid, 1),
                    "flat": round(f.flatness, 3),
                    "chr": [round(x, 3) for x in f.chroma],
                    "kr": f.key_root, "km": f.key_major,
                    "kc": round(f.key_conf, 3), "hm": round(f.harmony, 3),
                    "f0": round(f.f0, 1), "fc": round(f.f0_conf, 3), "fv": f.f0_voiced,
                    "mo": round(f.mood, 3), "tr": round(f.trend, 3),
                    "nv": round(f.novelty, 3), "sc": f.section,
                    "dy": round(f.dynamics, 3), "pc": round(f.percussive, 3),
                    "bh": [round(x, 4) for x in f.bands_h],
                    "bp": [round(x, 4) for x in f.bands_p],
                    "bpb": f.bpb, "bpos": f.bar_pos, "bidx": f.bar_index, "bc": round(f.bar_conf, 3),
                    "db": f.downbeat, "afx": f.auto_fx,
                    "asc": [round(x, 3) for x in f.auto_score],
                    "ad": round(f.auto_duty, 3), "aj": round(f.auto_jit, 1),
                    "ap": round(f.auto_perc, 3), "as": round(f.auto_split, 3),
                    "lk": f.lock, "gt": f.gate, "on": f.onset, "ps": f.preset,
                    "px": bytes(f.px).hex(),
                })
            ws_send(sock, json.dumps(out, separators=(",", ":")).encode())
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
    except Exception:
        # 单条连接出错不该影响别人。注意：C++ 侧的段错误捕获不到 ——
        # 那会直接杀掉整个进程，连 traceback 都没有（线上遇到过一次 SIGBUS）。
        import traceback; traceback.print_exc()
    finally:
        lib.lamp_destroy(h)
        try: sock.close()
        except Exception: pass

# ── HTTP（静态文件）+ WebSocket 升级 ──────────────────────

class Handler(SimpleHTTPRequestHandler):
    lib = None
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=os.path.join(HERE, "sim"), **kw)
    def guess_type(self, path):
        # SimpleHTTPRequestHandler 对 .html 只返回 "text/html"，不带 charset，
        # 浏览器于是按默认编码解析 —— UTF-8 的中文全成乱码。
        # HTML 里的 <meta charset> 已经能救，但 HTTP 头优先级更高，两边都给。
        t = super().guess_type(path)
        base = t.split(";")[0].strip()
        if base in ("text/html", "text/plain", "text/css",
                    "text/javascript", "application/javascript", "application/json"):
            return base + "; charset=utf-8"
        return t

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % a))
    def do_GET(self):
        # 固定灯效：一问一答的 HTTP，不走 WebSocket。
        # 它不需要实时音频，页面按自己的节奏拉帧就行 —— 少一条长连接少一处状态。
        if self.path.startswith("/wled/"):
            return self.do_wled()
        if self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers.get("Sec-WebSocket-Key")
            if not key: self.send_error(400); return
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", ws_accept(key))
            self.end_headers()
            self.wfile.flush()
            serve_ws(self.connection, Handler.lib)
            self.close_connection = True
            return
        super().do_GET()

    def _json(self, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    wargs = None
    wtime = 0            # WLED 的时钟，**只增不减**
    wlock = threading.Lock()

    def do_wled(self):
        w = Handler.wled
        if w is None:
            return self._json({"err": "libwledfx 未构建：./tools/build_wled.sh"})
        u = urlparse(self.path)
        q = parse_qs(u.query)
        gi = lambda k, d=0: int(q.get(k, [d])[0])
        if u.path == "/wled/catalog":
            return self._json(wled_catalog(w))
        if u.path == "/wled/frame":
            args = (gi("m"), gi("sx", 128), gi("ix", 128), gi("pal", 0),
                    gi("c0", 0xFF0000), gi("c1", 0x00FF00), gi("c2", 0x0000FF))
            # **只在参数真的变了时才 set。**
            # setMode(mode, true) 会重启过渡，每批都调的话画面永远停在
            # 淡入的头几帧 —— 实测 Solid 只有 (1,0,0)，暗得几乎看不见。
            # 真机上也是「设一次、然后一直跑」。
            with Handler.wlock:
                if args != Handler.wargs:
                    w.wledfx_set(*args)
                    Handler.wargs = args
            # **时钟由服务端单调持有，页面不传绝对时刻。**
            #
            # 一开始是页面传 t，换效果时它把 t 归零 —— 而服务端的 WLED 时钟
            # 已经走到很后面了。millis() 倒流会让过渡进度算成负数，
            # 画面全黑（实测 Solid 是 0,0,0，而 Rainbow 恰好看不出来）。
            dt, n = max(1, gi("dt", 33)), max(1, min(gi("n", 1), 240))
            buf = (C.c_uint8 * 288)()
            frames = []
            # WLED 是一份全局状态，多个请求并发进来会互相踩。
            # 服务器是 ThreadingHTTPServer，这把锁不是可选的。
            with Handler.wlock:
                for k in range(n):
                    Handler.wtime += dt
                    w.wledfx_render(Handler.wtime, buf)
                    frames.append(bytes(buf).hex())
            return self._json({"px": frames, "dt": dt})
        self.send_error(404)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    a = ap.parse_args()
    Handler.lib  = load()
    Handler.wled = load_wled()
    if Handler.wled is None:
        sys.stderr.write("提示：libwledfx 未构建，固定灯效页不可用（./tools/build_wled.sh）\n")
    else:
        sys.stderr.write(f"WLED 固定灯效：{Handler.wled.wledfx_mode_count()} 个效果、"
                         f"{Handler.wled.wledfx_palette_count()} 个调色板\n")
    sr = Handler.lib.lamp_sample_rate()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    print(f"liblamp 已加载 · 采样率 {sr:.0f} Hz · {NLED} 灯 · {NB} 段")
    print(f"监听 http://{a.host}:{a.port}")
    print("⚠ 麦克风需要安全上下文：用 localhost 打开，或做 SSH 端口转发")
    try: srv.serve_forever()
    except KeyboardInterrupt: print("\n停止")

if __name__ == "__main__":
    main()
