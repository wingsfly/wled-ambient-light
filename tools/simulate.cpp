// 离线模拟器：WAV 进，逐帧 JSON 出。
//
// 页面只负责回放这份 JSON —— **所有算法都在这里跑**，用的是 usermods/lamp 下
// 那批头文件本身。用 JS 重写一遍算法的话，页面验证的就是另一套代码了。
//
// 构建：
//   c++ -std=c++17 -O2 -I ../usermods/lamp simulate.cpp -o simulate
// 用法：
//   ./simulate song.wav out.json [effect] [preset]
//   ./simulate --synth out.json 128          合成 128BPM 的测试轨
//   加 --px 额外输出每帧 96 个 RGB（C++ 效果层的渲染结果）。
//   默认不输出：像素占 576 字节/帧，而模拟页面只需要 AudioFrame ——
//   效果层本来就是示意的（见 lamp_fx.h 开头），页面用 JS 复刻反而能实时切换。
//
// 主机上 FFT 用朴素 DFT（目标板是 esp-dsp）。N=2048 时每帧 210 万次运算，
// 一分钟音频约需十几秒 —— 慢，但这是离线工具，换来的是零依赖。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

#include "lamp_fx.h"

using namespace lamp;

// ── WAV ───────────────────────────────────────────────────
// 只认最常见的一种：RIFF/WAVE、PCM 16-bit。其余格式明确报错而不是猜。

static bool readWav(const char *path, std::vector<float> &mono, uint32_t &rate) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return false; }
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) {
        fprintf(stderr, "不是 RIFF/WAVE 文件\n"); fclose(f); return false;
    }
    uint16_t channels = 0, bits = 0; bool got_fmt = false;
    while (true) {
        char id[4]; uint32_t sz;
        if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
        if (!memcmp(id, "fmt ", 4)) {
            uint16_t fmt; uint32_t sr; uint32_t byte_rate; uint16_t align;
            fread(&fmt, 2, 1, f); fread(&channels, 2, 1, f); fread(&sr, 4, 1, f);
            fread(&byte_rate, 4, 1, f); fread(&align, 2, 1, f); fread(&bits, 2, 1, f);
            if (fmt != 1) { fprintf(stderr, "只支持 PCM（fmt=1），这个是 fmt=%u\n", fmt);
                            fclose(f); return false; }
            if (bits != 16) { fprintf(stderr, "只支持 16-bit，这个是 %u-bit\n", bits);
                              fclose(f); return false; }
            rate = sr; got_fmt = true;
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            if (!got_fmt) { fprintf(stderr, "data 块出现在 fmt 之前\n"); fclose(f); return false; }
            const uint32_t frames = sz / (2 * channels);
            mono.resize(frames);
            std::vector<int16_t> buf(channels);
            for (uint32_t i = 0; i < frames; ++i) {
                if (fread(buf.data(), 2, channels, f) != channels) { mono.resize(i); break; }
                int32_t acc = 0;
                for (uint16_t c = 0; c < channels; ++c) acc += buf[c];
                mono[i] = (float)acc / (float)channels / 32768.0f;
            }
            fclose(f); return true;
        } else {
            fseek(f, (sz + 1) & ~1u, SEEK_CUR);
        }
    }
    fprintf(stderr, "没找到 data 块\n"); fclose(f); return false;
}

// 线性重采样到 22050Hz。够用 —— 我们分析的是包络与频段能量，
// 不是要还原音质，而 22.05k 的奈奎斯特已经盖住了顶段 9.26kHz。
static void resample(const std::vector<float> &in, uint32_t from,
                     std::vector<float> &out) {
    if (from == (uint32_t)kSampleRate) { out = in; return; }
    const double ratio = (double)from / (double)kSampleRate;
    const size_t n = (size_t)((double)in.size() / ratio);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double src = (double)i * ratio;
        const size_t j = (size_t)src;
        const double t = src - (double)j;
        out[i] = (j + 1 < in.size()) ? (float)(in[j] * (1.0 - t) + in[j + 1] * t)
                                     : in[j < in.size() ? j : in.size() - 1];
    }
}

// 确定性伪随机，用作瞬态的宽带成分。
static float noiseAt(uint32_t k) {
    uint32_t s = k * 1664525u + 1013904223u;
    s ^= s >> 16; s *= 2246822519u; s ^= s >> 13;
    return (float)(s & 0xFFFFu) / 32768.0f - 1.0f;
}

// 合成测试轨。
//
// 第一版只用纯正弦叠加，结果 16 段里只有两三段有能量 —— 频谱图上孤零零一根柱子。
// 真实的鼓不是这样：底鼓有一记宽带的「拍击」瞬态，军鼓基本就是带通噪声，
// hi-hat 是高频噪声。能量铺得开，16 段才有东西可看，档位判据也才有意义
// （频谱平坦度对纯正弦恒等于 0，那样「电子/打击」这类档位永远判不出来）。
static void synth(float bpm, float seconds, std::vector<float> &out) {
    const size_t n = (size_t)(seconds * kSampleRate);
    const float period = 60000.0f / bpm;
    out.resize(n);
    // 一个二阶带通的状态（用来给噪声塑形），比逐帧算 FFT 便宜得多
    float lp1 = 0, lp2 = 0, hp1 = 0;
    for (size_t i = 0; i < n; ++i) {
        const float ms = (float)i * 1000.0f / kSampleRate;
        const float ph = fmodf(ms, period) / period;

        // 底鼓：60Hz 下滑音 + 一记极短的宽带拍击
        const float ke = (ph < 0.05f) ? (ph / 0.05f) : expf(-(ph - 0.05f) * 10.0f);
        const float sweep = 60.0f + 40.0f * expf(-ph * 25.0f);
        float v = 0.50f * ke * sinf(6.283185307f * sweep * ms / 1000.0f);
        v += 0.30f * expf(-ph * 90.0f) * noiseAt((uint32_t)i);        // 拍击瞬态

        // 反拍军鼓：带通噪声，中频为主
        const float sp = fmodf(ms + period * 0.5f, period) / period;
        const float se = expf(-sp * 22.0f);
        const float nz = noiseAt((uint32_t)i + 777u);
        lp1 += 0.35f * (nz - lp1);            // 单极点低通，压掉最顶端
        hp1  = 0.90f * (hp1 + lp1 - lp2); lp2 = lp1;   // 再高通，留中频
        v += 0.34f * se * hp1;

        // 半拍 hi-hat：高频噪声
        const float hh = fmodf(ms, period * 0.5f) / (period * 0.5f);
        v += 0.10f * expf(-hh * 40.0f) * (nz - lp1);

        // 持续的谐波垫：三度和弦，给中频一点常驻能量
        v += 0.05f * sinf(6.283185307f * 220.0f * ms / 1000.0f);
        v += 0.04f * sinf(6.283185307f * 277.0f * ms / 1000.0f);
        v += 0.03f * sinf(6.283185307f * 330.0f * ms / 1000.0f);
        out[i] = v * 0.8f;
    }
}

// ── 主流程 ────────────────────────────────────────────────

static std::vector<float> g_win, g_mag, g_frame;

static void dft(const float *x, size_t n, WindowType wt) {
    g_win.resize(n); g_mag.resize(n / 2 + 1);
    fillWindow(wt, g_win.data(), n);
    for (size_t k = 0; k <= n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (size_t t = 0; t < n; ++t) {
            const double a = -6.283185307179586 * (double)k * (double)t / (double)n;
            const double v = (double)x[t] * (double)g_win[t];
            re += v * cos(a); im += v * sin(a);
        }
        g_mag[k] = (float)sqrt(re * re + im * im);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <in.wav|--synth BPM> <out.json> [effect 0-2] [preset 0-3]\n", argv[0]);
        return 2;
    }
    std::vector<float> raw, pcm;
    uint32_t rate = (uint32_t)kSampleRate;
    std::string src;
    if (!strcmp(argv[1], "--synth")) {
        const float bpm = (argc > 3) ? atof(argv[3]) : 128.0f;
        synth(bpm, 24.0f, pcm);
        src = "synth " + std::to_string((int)bpm) + "BPM";
    } else {
        if (!readWav(argv[1], raw, rate)) return 1;
        resample(raw, rate, pcm);
        src = argv[1];
    }
    bool want_px = false;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--px")) want_px = true;
    const bool synth_mode = !strcmp(argv[1], "--synth");
    const FxId       fx     = (FxId)((argc > 3 && !synth_mode) ? atoi(argv[3]) : 0);
    const StylePreset start = (StylePreset)((argc > 4 && !synth_mode) ? atoi(argv[4]) : (int)STYLE_GENERAL);

    Pipeline p; PipelineConfig c; Geometry geo;
    if (!pipelineInit(p, c, start)) { fprintf(stderr, "pipelineInit 失败\n"); return 1; }

    FILE *o = fopen(argv[2], "w");
    if (!o) { fprintf(stderr, "写不了 %s\n", argv[2]); return 1; }
    fprintf(o, "{\n  \"source\": \"%s\",\n  \"sampleRate\": %g,\n"
               "  \"effect\": \"%s\",\n  \"leds\": %u,\n  \"frames\": [\n",
            src.c_str(), kSampleRate, fxName(fx), (unsigned)TOTAL_LEDS);

    std::vector<Rgb> px(TOTAL_LEDS);
    size_t pos = 0; uint32_t k = 0; bool first = true;
    while (true) {
        const size_t n = p.an.n, hop = presetParams(p.style.current).hop;
        if (pos + n > pcm.size()) break;
        g_frame.assign(pcm.begin() + pos, pcm.begin() + pos + n);
        dft(g_frame.data(), n, p.an.wt);

        const uint32_t t = (uint32_t)((double)pos * 1000.0 / kSampleRate);
        const AudioFrame f = pipelineProcess(p, c, g_frame.data(), g_mag.data(), t);
        fxRender(fx, f, geo, true, px.data());

        if (!first) fprintf(o, ",\n");
        first = false;
        fprintf(o, "    {\"t\":%u,\"bpm\":%.2f,\"conf\":%.3f,\"lock\":%d,\"phase\":%.4f,"
                   "\"rms\":%.4f,\"peak\":%.4f,\"gain\":%.2f,\"gate\":%d,\"onset\":%d,"
                   "\"rate\":%.2f,\"preset\":%d,\"bands\":[",
                t, f.bpm, f.bpm_conf, (int)f.beat_locked, f.phase,
                f.rms_fast, f.peak, f.gain, (int)f.gated, (int)f.onset,
                f.onset_rate, (int)f.preset);
        for (int i = 0; i < NUM_BANDS; ++i)
            fprintf(o, "%s%.4f", i ? "," : "", f.bands[i]);
        fprintf(o, "]");
        if (want_px) {
            fprintf(o, ",\"px\":\"");
            for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
                fprintf(o, "%02x%02x%02x", px[i].r, px[i].g, px[i].b);
            fprintf(o, "\"");
        }
        fprintf(o, "}");

        pos += hop;
        if (++k % 100 == 0) { fprintf(stderr, "\r%.1fs", (double)pos / kSampleRate); fflush(stderr); }
    }
    fprintf(o, "\n  ]\n}\n");
    fclose(o);
    fprintf(stderr, "\r%u 帧 · %.1fs · 效果 %s · 写入 %s\n",
            k, (double)pos / kSampleRate, fxName(fx), argv[2]);
    return 0;
}
