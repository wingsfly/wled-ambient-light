// 给模拟服务器用的 C 接口。
//
// 编译成动态库后由 Python 的 ctypes 直接调用 —— 没有子进程、没有管道、
// 没有 IPC 开销。跑的是 usermods/lamp 下那批头文件本身，与固件同一份源码。
//
//   c++ -std=c++17 -O2 -shared -fPIC -I ../usermods/lamp -I . \
//       lamp_capi.cpp -o liblamp.dylib      (Linux 用 .so)
//
// 线程模型：每个 handle 只能被一个线程用。服务器给每条 WebSocket 连接
// 建一个 handle，互不共享。
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <new>

#include "lamp_fx.h"
#include "lamp_auto.h"
#include "lamp_fft.h"

using namespace lamp;

extern "C" {

// 与 server.py 里的 struct 定义逐字段对应。改这里必须同步改那边 ——
// ctypes 不会替你检查布局。
struct LampFrameC {
    float bands[NUM_BANDS];
    float chroma[kChroma];
    float bpm, conf, phase, rms, peak, gain, rate, centroid, flatness;
    float key_conf, harmony;
    float f0, f0_conf, mood, trend, novelty, dynamics, percussive, bar_conf;
    float bands_h[NUM_BANDS], bands_p[NUM_BANDS];
    int32_t lock, gate, onset, preset, key_root, key_major, f0_voiced, section;
    int32_t bpb, bar_pos, bar_index, downbeat, auto_fx;
    float auto_score[5];      // 自动选灯效的五个候选分数，排查用
    float auto_duty, auto_jit, auto_perc, auto_split;
    uint8_t px[TOTAL_LEDS * 3];      // C++ 效果层渲染的 96 个 RGB
};

struct LampHandle {
    Pipeline       p;
    PipelineConfig c;
    Geometry       geo;
    FxId           fx = FX_SPECTRUM_BARS;
    FxState        fxst;
    FxConfig       fxcfg;
    bool           auto_mode = false;
    AutoState      autost;
    AutoConfig     autocfg;
    bool           white_balance = true;
    float          in_gain = 1.0f;   // 送进管线前的手动增益，补偿麦克风远近

    std::vector<float> ring;         // 样本累积区，长度至少一帧
    size_t             filled = 0;
    uint32_t           consumed = 0; // 已消费的样本总数，用来算时间戳

    std::vector<float> re, im, mag, win;
    std::vector<Rgb>   px;
};

void *lamp_create(void) {
    LampHandle *h = new (std::nothrow) LampHandle();
    if (!h) return nullptr;
    if (!pipelineInit(h->p, h->c, STYLE_GENERAL)) { delete h; return nullptr; }
    autoInit(h->autost, h->autocfg, presetHopMs(STYLE_GENERAL));
    h->ring.assign(kMaxFftLen * 2, 0.0f);
    h->re.assign(kMaxFftLen, 0.0f);
    h->im.assign(kMaxFftLen, 0.0f);
    h->mag.assign(kMaxFftLen / 2 + 1, 0.0f);
    h->win.assign(kMaxFftLen, 0.0f);
    h->px.assign(TOTAL_LEDS, Rgb{0, 0, 0});
    return h;
}

void lamp_destroy(void *hv) { delete static_cast<LampHandle *>(hv); }

void lamp_set_effect(void *hv, int32_t fx, int32_t white_balance) {
    LampHandle *h = static_cast<LampHandle *>(hv);
    if (!h) return;
    // fx < 0 → 自动模式。自动选择住在渲染层而不是管线里 ——
    // 管线产出特征，「画哪个」是渲染层的决定（见 lamp_auto.h 的分层说明）。
    if (fx < 0)                   h->auto_mode = true;
    else if (fx < FX_COUNT)     { h->auto_mode = false; h->fx = (FxId)fx; }
    h->white_balance = white_balance != 0;
}

// 送进管线前的手动增益。麦克风离声源的远近能差一两个数量级，
// AGC 的上限再高也不该指望它兜住全部 —— 给现场一个旋钮更实在。
void lamp_set_input_gain(void *hv, float g) {
    LampHandle *h = static_cast<LampHandle *>(hv);
    if (!h || !isfinite(g) || g <= 0.0f) return;
    h->in_gain = (g > 1000.0f) ? 1000.0f : g;
}

// 锁到某个档位；preset < 0 表示放开自动切换。
void lamp_lock_preset(void *hv, int32_t preset) {
    LampHandle *h = static_cast<LampHandle *>(hv);
    if (!h) return;
    h->c.style.manual_lock = (preset >= 0 && preset < STYLE_COUNT)
                           ? (StylePreset)preset : STYLE_COUNT;
}

// 布局自检用。ctypes 那边算出的 sizeof 必须与这里一致，否则读出来是错位的数
// 而不是报错 —— 这种错很难查，宁可启动时就崩。
int32_t lamp_frame_size(void) { return (int32_t)sizeof(LampFrameC); }

float lamp_sample_rate(void) { return kSampleRate; }
int32_t lamp_num_bands(void) { return NUM_BANDS; }
int32_t lamp_num_leds(void)  { return TOTAL_LEDS; }

// 喂一批样本，尽可能多地产出分析帧。
//
// 返回本次产出的帧数，最多 max_out。样本按 hop 步进消费，剩余的留在环里
// 等下一批 —— 调用方不必按帧长对齐，麦克风来多少喂多少即可。
int32_t lamp_feed(void *hv, const float *pcm, int32_t count,
                  LampFrameC *out, int32_t max_out) {
    LampHandle *h = static_cast<LampHandle *>(hv);
    if (!h || !pcm || count <= 0 || !out || max_out <= 0) return 0;

    int32_t produced = 0;
    int32_t src = 0;
    while (src < count && produced < max_out) {
        // 补满到一帧。
        //
        // 这一段有两处坑，都出在**档位刚切过**的那一帧上（帧长会从 2048 变成 512）：
        //
        // 1. 用 assign 扩容会把已经攒进来的样本清零，而 filled 还指着它们 ——
        //    不崩，但那一帧读到的是零。必须用 resize。
        // 2. 档位切小时 filled 可能已经大于新的 n，`n - filled` 是 size_t 减法，
        //    会下溢成天文数字，后面的 memcpy 直接写出边界。实测 SIGBUS，
        //    整个进程被内核杀掉、连 Python traceback 都没有。
        const size_t n = h->p.an.n;
        if (h->ring.size() < n * 2) h->ring.resize(n * 2, 0.0f);
        if (h->filled > n) {          // 只保留最近的 n 个样本
            memmove(h->ring.data(), h->ring.data() + (h->filled - n), n * sizeof(float));
            h->filled = n;
        }
        const size_t want = n - h->filled;
        const size_t take = ((size_t)(count - src) < want) ? (size_t)(count - src) : want;
        if (h->in_gain == 1.0f) {
            memcpy(&h->ring[h->filled], pcm + src, take * sizeof(float));
        } else {
            for (size_t i = 0; i < take; ++i) h->ring[h->filled + i] = pcm[src + i] * h->in_gain;
        }
        h->filled += take; src += (int32_t)take;
        if (h->filled < n) break;

        // 一帧齐了：加窗 → FFT → 管线
        fillWindow(h->p.an.wt, h->win.data(), n);
        magnitudeSpectrum(h->ring.data(), h->win.data(), n,
                          h->re.data(), h->im.data(), h->mag.data());
        const uint32_t t_ms = (uint32_t)((double)h->consumed * 1000.0 / kSampleRate);
        const AudioFrame f = pipelineProcess(h->p, h->c, h->ring.data(),
                                             h->mag.data(), t_ms);
        // 换档时自动选择器的时间常数也要跟着换 —— 它有一条 4 秒的
        // f0 占比观察窗，按帧算的话各档会差 8 倍。
        if (f.preset_changed) autoRetime(h->autost, h->autocfg, presetHopMs(f.preset));
        if (h->auto_mode) {
            autoUpdate(h->autost, h->autocfg, f, t_ms);
            h->fx = h->autost.current;
        }
        fxRender(h->fx, h->fxst, h->fxcfg, f, h->geo, h->white_balance,
                 (float)presetHopMs(h->p.style.current), h->px.data());

        // out 指向调用方（Python ctypes）的缓冲。produced 已由循环条件约束，
        // 这里再挡一道 —— 越界写别人的堆是最难查的一类崩溃，代价只是一次比较。
        if (produced >= max_out) break;
        LampFrameC &o = out[produced++];
        memcpy(o.bands, f.bands, sizeof(o.bands));
        o.bpm = f.bpm; o.conf = f.bpm_conf; o.phase = f.phase;
        o.rms = f.rms_fast; o.peak = f.peak; o.gain = f.gain; o.rate = f.onset_rate;
        memcpy(o.chroma, f.chroma, sizeof(o.chroma));
        o.centroid = f.centroid_hz;
        o.flatness = f.flatness;
        o.key_conf = f.key_conf; o.harmony = f.harmony_move;
        o.key_root = f.key_root; o.key_major = f.key_is_major ? 1 : 0;
        o.f0 = f.f0_hz; o.f0_conf = f.f0_conf; o.f0_voiced = f.f0_voiced ? 1 : 0;
        o.mood = f.mood; o.trend = f.energy_trend;
        o.novelty = f.section_novelty; o.section = f.section_change ? 1 : 0;
        o.dynamics = f.dynamics; o.percussive = f.percussive;
        memcpy(o.bands_h, f.bands_h, sizeof(o.bands_h));
        memcpy(o.bands_p, f.bands_p, sizeof(o.bands_p));
        o.auto_fx = (int32_t)h->fx;
        memcpy(o.auto_score, h->autost.score, sizeof(o.auto_score));
        o.auto_duty = h->autost.f0_duty; o.auto_jit = h->autost.f0_jitter;
        { const float e = h->autost.e_h + h->autost.e_p;
          o.auto_perc = (e > 1e-12f) ? h->autost.e_p / e : 0.0f; }
        o.auto_split = splitContrastOf(h->autost.e_ends, h->autost.e_mid);
        o.bar_conf = f.bar_conf;
        o.bpb = f.beats_per_bar; o.bar_pos = f.bar_pos;
        o.bar_index = f.bar_index; o.downbeat = f.downbeat ? 1 : 0;
        o.lock = f.beat_locked; o.gate = f.gated; o.onset = f.onset;
        o.preset = (int32_t)f.preset;
        for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
            o.px[i * 3 + 0] = h->px[i].r;
            o.px[i * 3 + 1] = h->px[i].g;
            o.px[i * 3 + 2] = h->px[i].b;
        }

        // 按 hop 前移。**hop 取自当前档位** —— 档位可能刚在
        // pipelineProcess 里换过，用旧值会让时间轴慢慢漂。
        const size_t hop = presetParams(h->p.style.current).hop;
        const size_t keep = (h->filled > hop) ? (h->filled - hop) : 0;
        if (keep) memmove(h->ring.data(), h->ring.data() + hop, keep * sizeof(float));
        h->filled = keep;
        h->consumed += (uint32_t)hop;
    }
    return produced;
}

} // extern "C"
