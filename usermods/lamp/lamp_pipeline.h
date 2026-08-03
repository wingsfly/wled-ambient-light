// 音频管线顶层（设计文档 §2.2 的模块装配）。
//
// 八个模块此前各自测透，但**从没一起跑过**。集成评审发现三处接线无人负责：
// 换档要调五个 retime 却没有统一入口、AGC 增益算出来没人用、
// StyleFeatures 的两个判据没有生产者（后者已在 lamp_bands.h 补上）。
// 这个文件就是那个「负责的人」。
//
// 纯逻辑，零 WLED 依赖 —— 它不碰 I2S、不碰 LED 驱动，只做「PCM 进、特征出」。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_bands.h"
#include "lamp_envelope.h"
#include "lamp_onset.h"
#include "lamp_beat.h"
#include "lamp_style.h"
#include "lamp_chroma.h"
#include "lamp_pitch.h"
#include "lamp_mood.h"

namespace lamp {

// 一帧分析的全部输出。特效层只读这个结构，不碰任何内部状态。
struct AudioFrame {
    float       bands[NUM_BANDS] = {0};  // AGC 后的 16 段能量
    float       rms_fast = 0.0f;         // 快包络（30ms）
    float       rms_slow = 0.0f;         // 慢包络（2s）
    float       peak     = 0.0f;
    float       gain     = 1.0f;         // AGC 当前增益
    bool        gated    = false;        // 静音门限
    bool        onset    = false;        // 本帧是起音
    float       onset_rate = 0.0f;       // 每秒起音数
    float       bpm      = 0.0f;
    float       bpm_conf = 0.0f;
    bool        beat_locked = false;
    float       phase    = 0.0f;         // 拍点相位 [0,1)
    StylePreset preset   = STYLE_GENERAL;
    bool        preset_changed = false;

    // ── 音色 ──
    // 这两个此前只喂给档位切换、算完就丢，灯效想用都拿不到。
    float centroid_hz = 0.0f;   // 谱重心，亮/暗
    float flatness    = 0.0f;   // 噪声型 vs 谐波型

    // ── 音高 ──
    // 在这之前整条链只知道「有多响、鼓点在哪」，同一首歌换成节拍器画面
    // 不会有本质区别。色度打开了旋律与和声这一维。
    float chroma[kChroma] = {0};
    int   key_root     = -1;    // 0=C … 11=B，-1 表示没结论
    bool  key_is_major = true;
    float key_conf     = 0.0f;
    float harmony_move = 0.0f;  // 和声变化率，换和弦时会跳

    // ── 旋律线 ──
    // 色度不分八度、和弦与旋律糊在一起；这里是单声部主旋律的绝对音高。
    float f0_hz     = 0.0f;
    float f0_conf   = 0.0f;
    bool  f0_voiced = false;

    // ── 氛围（几十秒尺度）──
    float energy_trend    = 0.0f;   // [-1,1]，正=渐强
    float section_novelty = 0.0f;   // [0,1]
    bool  section_change  = false;  // 只在换段那一帧为真
    float mood            = 0.0f;   // [0,1]，静↔躁
};

// 和声变化率的平滑时间常数。够短能跟上换和弦，够长能压住单帧色度抖动。
constexpr float kHarmonyTauMs = 80.0f;

struct PipelineConfig {
    EnvelopeConfig env;
    AgcConfig      agc;
    OnsetConfig    onset;
    BeatConfig     beat;
    StyleConfig    style;
    PitchConfig    pitch;
    MoodConfig     mood;
    float          latency_comp_ms = 0.0f;   // 相位提前量，抵消流水线延迟（§3.5）
};

struct Pipeline {
    Analysis      an;
    float         prev_chroma[kChroma] = {0};
    bool          has_prev_chroma = false;
    KeyEstimate   key;
    uint32_t      last_key_ms = 0;
    bool          key_dated = false;
    float         harmony = 0.0f;
    Envelope      env;
    Agc           agc;
    OnsetDetector onset;
    BeatTracker   beat;
    StyleSelector style;
    PitchTracker  pitch;
    MoodState     mood;
    float         dt_ms = 0.0f;
};

// 切档位时**必须**走这里。五个模块各有各的 retime，漏调一个不会报错，
// 只会让某个时间常数悄悄错掉 —— 而各模块的 retime 测试都是单独调的，
// 覆盖不到「漏调」这件事。所以入口只留一个。
inline bool pipelineRetime(Pipeline &p, const PipelineConfig &c, StylePreset preset) {
    const PresetParams q  = presetParams(preset);
    const float        dt = presetHopMs(preset);
    if (!analysisInit(p.an, q.n, q.wt)) return false;
    p.dt_ms = dt;
    envelopeRetime(p.env,   c.env,   dt);
    agcRetime    (p.agc,   c.agc,   dt);
    onsetRetime  (p.onset, c.onset, dt);
    beatRetime   (p.beat,  c.beat);          // ODF 固定速率，无需改动
    pitchRetime  (p.pitch, c.pitch, dt);
    moodRetime   (p.mood,  c.mood,  dt);
    return true;
}

inline bool pipelineInit(Pipeline &p, const PipelineConfig &c,
                         StylePreset preset = STYLE_GENERAL) {
    const PresetParams q  = presetParams(preset);
    const float        dt = presetHopMs(preset);
    if (!analysisInit(p.an, q.n, q.wt)) return false;
    p.dt_ms = dt;
    envelopeInit(p.env,   c.env,   dt);
    agcInit     (p.agc,   c.agc,   dt);
    onsetInit   (p.onset, c.onset, dt);
    beatInit    (p.beat,  c.beat);
    pitchInit   (p.pitch, c.pitch, dt);
    moodInit    (p.mood,  c.mood,  dt);
    p.style = StyleSelector{};
    p.style.current = p.style.candidate = preset;
    for (int i = 0; i < kChroma; ++i) p.prev_chroma[i] = 0.0f;
    p.has_prev_chroma = false;
    p.key = KeyEstimate{}; p.last_key_ms = 0; p.key_dated = false; p.harmony = 0.0f;
    return true;
}

// 喂一帧：pcm 是 an.n 个已加窗前的原始样本，mag 是对应的幅度谱（n/2+1 点）。
//
// 调用方负责 FFT —— 目标板上是 esp-dsp，主机上是参考 DFT。这条边界在
// lamp_bands.h 就划定了：我们拥有加窗与频段映射，不拥有 FFT。
inline AudioFrame pipelineProcess(Pipeline &p, const PipelineConfig &c,
                                  const float *pcm, const float *mag,
                                  uint32_t now_ms) {
    AudioFrame f;

    // 1. 响度包络与 AGC
    const float rms = frameRms(pcm, p.an.n);
    envelopeUpdate(p.env, rms);
    f.gain  = agcUpdate(p.agc, c.agc, p.env.slow);
    f.gated = p.agc.gated;
    f.rms_fast = p.env.fast; f.rms_slow = p.env.slow; f.peak = p.env.peak;

    // 2. 频段能量。**AGC 增益在这里施加** —— 评审时这一步是缺的，
    //    增益算出来没人用，特效看到的还是未归一化的能量。
    computeBandEnergy(p.an, mag, f.bands);
    const float g = f.gated ? 0.0f : f.gain;    // 门限时输出归零，别放大底噪
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] *= g;

    // 3. 起音与节拍。flux 用**施加增益后**的频段，这样它与响度无关，
    //    自适应阈值只需要跟踪谱形变化。
    const float flux = spectralFlux(p.onset.prev, f.bands, p.dt_ms);
    f.onset      = onsetUpdate(p.onset, c.onset, f.bands, now_ms);
    f.onset_rate = p.onset.rate;
    beatUpdate(p.beat, c.beat, flux, f.onset, p.dt_ms, now_ms);
    f.bpm = p.beat.bpm; f.bpm_conf = p.beat.conf; f.beat_locked = p.beat.locked;
    f.phase = beatPhaseAhead(p.beat, now_ms, c.latency_comp_ms);

    // 4. 音色与音高。
    //
    // 色度从**已经算好的幅度谱**折叠，不需要第二次 FFT —— 一次 O(bins) 遍历。
    // 调性每秒重算一次就够：调不会一帧一帧地变，而 24 个候选各做一次
    // 12 维相关，每帧都算是白费。
    f.centroid_hz = spectralCentroid(f.bands);
    f.flatness    = spectralFlatness(f.bands);
    computeChroma(mag, p.an.n, f.chroma);

    if (p.has_prev_chroma) {
        // 和声变化率做一次平滑：单帧的色度抖动很大，不平滑的话每一帧都在「换和弦」。
        //
        // 系数按**物理时间**算，不能写死成「每帧 0.25」—— 各档 hop 从 5.8ms 到
        // 46.4ms 差 8 倍，写死的话同一段音乐在氛围档和打点档的反应速度会差 8 倍。
        const float d = chromaDistance(p.prev_chroma, f.chroma);
        p.harmony += envCoeff(kHarmonyTauMs, p.dt_ms) * (d - p.harmony);
    }
    for (int i = 0; i < kChroma; ++i) p.prev_chroma[i] = f.chroma[i];
    p.has_prev_chroma = true;
    f.harmony_move = p.harmony;

    if (!p.key_dated || elapsedAtLeast(now_ms, p.last_key_ms, 1000)) {
        p.key = estimateKey(f.chroma);
        p.last_key_ms = now_ms;
        p.key_dated = true;
    }
    f.key_root = p.key.root; f.key_is_major = p.key.is_major; f.key_conf = p.key.conf;

    // 旋律线。基频跟踪吃的是**原始幅度谱**，不是 16 段 —— 段太粗，
    // 一段就跨了好几个半音。
    const PitchEstimate pe = estimateF0(mag, p.an.n, c.pitch);
    pitchUpdate(p.pitch, c.pitch, pe, p.dt_ms);
    f.f0_hz = p.pitch.hz; f.f0_conf = p.pitch.conf; f.f0_voiced = p.pitch.voiced;

    // 5. 氛围。喂的响度三件套是 **AGC 之前**的（`p.env`），
    //    频段是 AGC 之后的但只取形状 —— 理由见 lamp_mood.h 的头注释。
    moodUpdate(p.mood, c.mood, f.bands, p.env.fast, p.env.peak,
               f.onset_rate, f.centroid_hz, f.gated);
    f.energy_trend    = p.mood.trend;
    f.section_novelty = p.mood.novelty;
    f.section_change  = p.mood.section_change;
    f.mood            = p.mood.mood;

    // 6. 风格档位。四个判据现在齐了。
    StyleFeatures sf;
    sf.onset_rate  = f.onset_rate;
    sf.bpm         = f.bpm;
    sf.bpm_conf    = f.bpm_conf;
    sf.centroid_hz = f.centroid_hz;
    sf.flatness    = f.flatness;
    f.preset_changed = styleUpdate(p.style, c.style, sf, now_ms);
    f.preset = p.style.current;

    // 换档：一处调用，五个模块同步。BPM 与相位在 beatRetime 里刻意保留 ——
    // 全量重新锁定要 6 秒，用户会看到灯「发呆」（§3.3.4 第 2 条）。
    if (f.preset_changed) pipelineRetime(p, c, f.preset);

    return f;
}

} // namespace lamp
