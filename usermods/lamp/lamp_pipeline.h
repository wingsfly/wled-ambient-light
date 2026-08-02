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
};

struct PipelineConfig {
    EnvelopeConfig env;
    AgcConfig      agc;
    OnsetConfig    onset;
    BeatConfig     beat;
    StyleConfig    style;
    float          latency_comp_ms = 0.0f;   // 相位提前量，抵消流水线延迟（§3.5）
};

struct Pipeline {
    Analysis      an;
    Envelope      env;
    Agc           agc;
    OnsetDetector onset;
    BeatTracker   beat;
    StyleSelector style;
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
    p.style = StyleSelector{};
    p.style.current = p.style.candidate = preset;
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

    // 4. 风格档位。四个判据现在齐了。
    StyleFeatures sf;
    sf.onset_rate  = f.onset_rate;
    sf.bpm         = f.bpm;
    sf.bpm_conf    = f.bpm_conf;
    sf.centroid_hz = spectralCentroid(f.bands);
    sf.flatness    = spectralFlatness(f.bands);
    f.preset_changed = styleUpdate(p.style, c.style, sf, now_ms);
    f.preset = p.style.current;

    // 换档：一处调用，五个模块同步。BPM 与相位在 beatRetime 里刻意保留 ——
    // 全量重新锁定要 6 秒，用户会看到灯「发呆」（§3.3.4 第 2 条）。
    if (f.preset_changed) pipelineRetime(p, c, f.preset);

    return f;
}

} // namespace lamp
