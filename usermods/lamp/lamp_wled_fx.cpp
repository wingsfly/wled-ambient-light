#include "wled.h"

/*
 * lamp 灯效 → WLED 特效桥（第一批）。
 *
 * usermods/lamp/ 的 23 个灯效吃 AudioFrame（模拟器里由完整管线从 PCM 算出）。
 * 固件里不重跑那条管线：基础字段从 audioreactive 的 um_data 桥接（fftResult/
 * volumeSmth/samplePeak —— 三个音源在它内部已统一，麦克风/线路/UDP sync 对
 * 本文件透明），拍点由 lamp_beat 的 BeatTracker 在固件内吃谱通量实时跟踪。
 * chroma/f0/vocal 等重分析字段第一批置零 —— 对应灯效走各自的无信号回退分支
 * （设计上都有：如 BeatPulse 未锁拍退呼吸），完整效果等第二批（Mac sender
 * 调 liblamp 算好推送）。
 *
 * 量纲对齐（见 lamp_fx.h 的 kFxBandScale=6 / kFxLevelScale=3，效果内
 * bands*6、rms*3 进 perceptual() 期望落 [0,1]）：
 *   bands[i]  = fftResult[i] / (255*6)
 *   rms_fast  = volumeSmth   / (255*3)
 */

#include <new>
#include <arduinoFFT.h>

#include "lamp_fx.h"
#include "lamp_beat.h"
#include "lamp_onset.h"
#include "lamp_auto.h"
#include "lamp_pipeline.h"
#include "lamp_pcm_tap.h"

namespace {

using namespace lamp;

// WLED GEQ 16 段的近似中心频率（audioreactive 的对数分段），谱重心用
constexpr float kBandHz[NUM_BANDS] = {
  43, 86, 129, 216, 301, 430, 560, 818, 1120, 1421,
  2019, 2618, 3510, 4479, 6236, 9260
};

// ── 第二批 A：完整 AudioFrame 的 UDP 推送（LAMP1 协议）──
// Mac 端 sender 调 liblamp 跑完整管线（色度/音高/人声/HPSS/段落/档位），
// 打包成本结构推到 11989 端口。与 sender 的 struct.pack 逐字节对应 ——
// 改这里必须同步改 tools/wled_sync_sender.py。336 字节，小端。
constexpr uint16_t kLampSyncPort = 11989;
constexpr uint32_t kRemoteFreshMs = 500;   // 超过则回落本地桥接

struct __attribute__((packed)) LampSyncPacket {
  char     magic[6];        // "LAMP1\0"
  uint8_t  version;         // 1
  uint8_t  flags;           // bit0 onset, bit1 beat_locked, bit2 downbeat,
                            // bit3 gated, bit4 section_change, bit5 vocal_onset,
                            // bit6 f0_voiced, bit7 key_is_major
  float    bands[NUM_BANDS];
  float    bands_h[NUM_BANDS];
  float    bands_p[NUM_BANDS];
  float    chroma[kChroma];
  float    rms, peak, gain, bpm, conf, phase, rate, centroid, flatness,
           key_conf, harmony, f0, f0_conf, mood, trend, novelty, dynamics,
           percussive, bar_conf, vocal;
  int8_t   key_root;
  uint8_t  preset;
  uint8_t  bpb;
  uint8_t  bar_pos;
  int32_t  bar_index;
  // ── v2 追加（2026-09-06）：CLAP 语义情绪 ──
  float    valence;         // 愉悦度 -1..1
  uint8_t  emo_id;          // 八锚点 argmax；255 = CLAP 未就绪/旧包
};
static_assert(sizeof(LampSyncPacket) == 341, "LAMP1 包布局漂移，必须同步 sender");
constexpr int kLampSyncV1Len = 336;   // 旧 sender 兼容

// 本地完整管线的输出（定义在下方「第二批 B」块；bridge.fill 按新鲜度取用）
extern AudioFrame localFrame;
extern uint32_t   localFrameMs;

struct LampBridge {
  AudioFrame  frame;
  BeatTracker beat;
  BeatConfig  beatCfg;
  uint8_t  prevFft[NUM_BANDS] = {0};
  float    rmsSlow = 0.0f;
  int      beatCount = 0;      // 小节推算：4 拍一小节
  float    prevPhase = 0.0f;
  uint32_t lastMs = 0;
  float    dtMs = 20.0f;

  LampSyncPacket remote;       // 最近一个 LAMP1 包
  uint8_t  pulseLatch = 0;     // 脉冲位锁存（onset/downbeat/section/vocal_onset）：
                               // 包率 43~86Hz > 渲染 42fps，单帧脉冲按「最新包」
                               // 采样会被后续 false 包覆盖 —— Beat It 实拍鼓点
                               // 闪烁只剩 ~40/min（应 ~139）。OR 累积、消费即清。
  uint32_t remoteMs = 0;       // 收到时刻，0 = 从未收到
  StylePreset prevPreset = STYLE_GENERAL;

  bool remoteFresh(uint32_t now) const {
    return remoteMs && (now - remoteMs) < kRemoteFreshMs;
  }

  // LAMP1 → AudioFrame：远端管线全字段直填，本地合成整段跳过
  void fillFromRemote() {
    const LampSyncPacket &r = remote;
    for (int i = 0; i < NUM_BANDS; i++) {
      frame.bands[i] = r.bands[i]; frame.bands_h[i] = r.bands_h[i]; frame.bands_p[i] = r.bands_p[i];
    }
    for (int i = 0; i < kChroma; i++) frame.chroma[i] = r.chroma[i];
    frame.rms_fast = r.rms;
    rmsSlow += (frame.rms_fast - rmsSlow) * (dtMs / 2000.0f);
    frame.rms_slow = rmsSlow;
    frame.peak = r.peak; frame.gain = r.gain;
    frame.gated = r.flags & 0x08; frame.onset = pulseLatch & 0x01; frame.onset_rate = r.rate;
    frame.bpm = r.bpm; frame.bpm_conf = r.conf;
    frame.beat_locked = r.flags & 0x02; frame.phase = r.phase;
    frame.centroid_hz = r.centroid; frame.flatness = r.flatness; frame.percussive = r.percussive;
    frame.key_root = r.key_root; frame.key_is_major = r.flags & 0x80;
    frame.key_conf = r.key_conf; frame.harmony_move = r.harmony;
    frame.f0_hz = r.f0; frame.f0_conf = r.f0_conf; frame.f0_voiced = r.flags & 0x40;
    frame.energy_trend = r.trend; frame.section_novelty = r.novelty;
    frame.section_change = pulseLatch & 0x10; frame.mood = r.mood; frame.dynamics = r.dynamics;
    frame.beats_per_bar = r.bpb; frame.bar_pos = r.bar_pos; frame.bar_index = r.bar_index;
    frame.bar_conf = r.bar_conf; frame.downbeat = pulseLatch & 0x04;
    frame.vocal = r.vocal; frame.vocal_onset = pulseLatch & 0x20;
    StylePreset p = r.preset < STYLE_COUNT ? (StylePreset)r.preset : STYLE_GENERAL;
    frame.preset_changed = p != prevPreset;
    frame.preset = prevPreset = p;
    // ⚠️ latch 不在这里清 —— fill 由 loop 高频调用（2~5ms），在此消费会让
    // 脉冲只存活一个 loop 周期、23ms 的渲染帧几乎总错过（实测比不锁存更差）。
    // 清零在渲染帧末尾的 consumePulses()：语义 =「两次渲染之间到达过事件」。
  }

  void consumePulses() { pulseLatch = 0; }

  void fill(uint32_t now) {
    if (now == lastMs) return;               // 每 WLED 帧只算一次，多 segment 共用
    dtMs = (lastMs && now > lastMs) ? (float)(now - lastMs) : 20.0f;
    if (dtMs > 100.0f) dtMs = 100.0f;
    lastMs = now;

    if (remoteFresh(now)) { fillFromRemote(); return; }

    // 二级：本地完整管线（麦克风/3.5mm 有 PCM 时；100ms ≈ 4 帧新鲜窗）
    if (localFrameMs && (now - localFrameMs) < 100) {
      StylePreset p = localFrame.preset;
      localFrame.preset_changed = p != prevPreset;
      prevPreset = p;
      frame = localFrame;
      return;
    }

    um_data_t *um = nullptr;
    if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE)) {
      frame = AudioFrame{};                  // 无音频源：全零帧，效果走回退分支
      return;
    }
    float    volumeSmth =  *(float*)  um->u_data[0];
    float    volumeRaw  =  *(float*)  um->u_data[1];
    uint8_t *fftResult  =   (uint8_t*)um->u_data[2];
    uint8_t  samplePeak =  *(uint8_t*)um->u_data[3];

    // ── 频谱与包络 ──
    float flux = 0.0f, esum = 0.0f, fsum = 0.0f, gsum = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) {
      frame.bands[i] = fftResult[i] / (255.0f * kFxBandScale);
      float d = (float)fftResult[i] - (float)prevFft[i];
      if (d > 0) flux += d;
      prevFft[i] = fftResult[i];
      esum += fftResult[i];
      fsum += fftResult[i] * kBandHz[i];
      gsum += logf((float)fftResult[i] + 1.0f);
      // 第一批没有 HPSS：谐波/打击两份都给原频谱，percussive 由通量近似
      frame.bands_h[i] = frame.bands[i];
      frame.bands_p[i] = frame.bands[i];
    }
    flux /= 255.0f;
    frame.rms_fast = volumeSmth / (255.0f * kFxLevelScale);
    rmsSlow += (frame.rms_fast - rmsSlow) * (dtMs / 2000.0f);   // 2s 慢包络
    frame.rms_slow = rmsSlow;
    frame.peak  = volumeRaw / (255.0f * kFxLevelScale);
    frame.gain  = 1.0f;
    frame.gated = volumeSmth < 1.0f;

    // ── 音色近似 ──
    frame.centroid_hz = esum > 1.0f ? fsum / esum : 0.0f;
    // 谱平坦度 = 几何均 / 算术均
    float amean = esum / NUM_BANDS;
    frame.flatness = amean > 1.0f ? expf(gsum / NUM_BANDS) / amean : 0.0f;
    if (frame.flatness > 1.0f) frame.flatness = 1.0f;
    frame.percussive = flux > 0.6f ? 1.0f : flux / 0.6f;

    // ── 拍点 ──
    frame.onset = samplePeak > 0;
    beatUpdate(beat, beatCfg, flux, frame.onset, dtMs, now);
    frame.bpm         = beat.bpm;
    frame.bpm_conf    = beat.conf;
    frame.beat_locked = beat.locked;
    frame.phase       = beat.locked ? beatPhaseAhead(beat, now, 0.0f) : 0.0f;

    // ── 小节（4/4 推算，phase 回绕即一拍）──
    frame.beats_per_bar = 4;
    frame.downbeat = false;
    if (beat.locked) {
      if (frame.phase < prevPhase - 0.5f) {          // 回绕
        beatCount++;
        frame.downbeat = (beatCount % 4) == 0;
      }
      frame.bar_pos   = beatCount % 4;
      frame.bar_index = beatCount / 4;
      frame.bar_conf  = beat.conf;
    } else {
      frame.bar_conf = 0.0f;
    }
    prevPhase = frame.phase;

    // ── 氛围近似 ──
    frame.energy_trend = fminf(1.0f, fmaxf(-1.0f, (frame.rms_fast - rmsSlow) * 4.0f));
    frame.mood = fminf(1.0f, rmsSlow * 2.0f + frame.percussive * 0.3f);
    frame.dynamics = 1.0f;                 // AGC 旁路信息拿不到，恒 1 = 不衰减
    frame.section_novelty = 0.0f;
    frame.section_change  = false;

    // ── 第一批没有的重分析：置“无结论”，效果走回退 ──
    for (int i = 0; i < kChroma; i++) frame.chroma[i] = 0.0f;
    frame.key_root = -1; frame.key_conf = 0.0f; frame.harmony_move = 0.0f;
    frame.f0_hz = 0.0f; frame.f0_conf = 0.0f; frame.f0_voiced = false;
    frame.vocal = 0.0f; frame.vocal_onset = false;
    frame.preset = STYLE_GENERAL; frame.preset_changed = false;
  }
};

LampBridge bridge;
Rgb        fxOut[TOTAL_LEDS];              // 渲染缓冲，各 segment 复用

// 渲染帧距时基。⚠️ 不能把 bridge.dtMs 传给 fxRender/autoRetime：数据桥由
// loop 高频保鲜（2~5ms 间距）后 dtMs 是 loop 间距，而渲染 23ms 一帧才推进
// 一次 —— 所有衰减/包络的实际时间常数被拉慢 5~8 倍（实测：0.35 拍冲击衰减
// 观感 2 拍+，Speed 拉到 204 仍糊）。渲染用自己的帧距。
float    gRenderDt = 23.0f;
uint32_t gLastRenderMs = 0;
inline float renderTick() {
  uint32_t now = millis();
  if (now != gLastRenderMs) {
    gRenderDt = (gLastRenderMs && now > gLastRenderMs) ? (float)(now - gLastRenderMs) : 23.0f;
    if (gRenderDt > 100.0f) gRenderDt = 100.0f;
    gLastRenderMs = now;
  }
  return gRenderDt;   // 同 ms 的第二个 ♪ segment 返回同帧距（状态已被首个推进）
}

// ── 临时崩溃插桩（定位「WS 切 ♪ 效果即 PANIC」）──
// RTC noinit 段在 PANIC 重启后保留：崩溃时停留的最后标记 = 崩溃区间。
// 定位完成后整段移除。
RTC_NOINIT_ATTR uint32_t lampTrace;

// ── 第二批 B：本地完整管线（麦克风/3.5mm 独立场景）──
// 可行性基准（2026-08-27 实测）：GENERAL 档 pipelineProcess = 2928 µs/帧，
// 加 FFT 合计 ~6ms，帧预算 23.2ms 的 26% —— 无需裁剪。
// PCM 由 audioreactive FFT task 分接（lamp_pcm_tap，SPSC ring），本函数在
// WLED loop 消费：攒 hop=512 新样本 → lamp 窗 + arduinoFFT 1024 点 → 幅度谱
// → pipelineProcess → 完整 AudioFrame（含真 HPSS/色度/音高/人声/自动档位）。
// UDP receive 模式下 audioreactive 挂起本地采样 → ring 空 → 管线自然停，
// 与 LAMP1 远端帧无缝互补。
constexpr int      kLocalMaxN = 2048;           // AMBIENT 档帧长（= kMaxFftLen）
Pipeline           localPipe;                    // ~10KB，bss
PipelineConfig     localCfg;
bool               localInited = false;
float              localPcm[kLocalMaxN];         // 滑动帧缓冲（按当前档位取前 n）
int                localFill = 0;
float              localRe[kLocalMaxN], localIm[kLocalMaxN], localMag[kLocalMaxN / 2 + 1];
AudioFrame         localFrame;
uint32_t           localFrameMs = 0;             // 0 = 从未出帧
// 档位动态切换（C 项）：pipelineProcess 内部换档时自动 pipelineRetime——
// FFT 帧长随档位在 512/1024/2048 间变，三个实例绑同一缓冲按需选用。
ArduinoFFT<float>  fft512 (localRe, localIm,  512, kSampleRate, true);
ArduinoFFT<float>  fft1024(localRe, localIm, 1024, kSampleRate, true);
ArduinoFFT<float>  fft2048(localRe, localIm, 2048, kSampleRate, true);

void serviceLocalPipeline() {
  if (!localInited) {
    localInited = pipelineInit(localPipe, localCfg, STYLE_GENERAL);
    if (!localInited) return;
  }
  auto &rb = pcmTap();
  int16_t tmp[128];
  for (;;) {
    // 帧长取管线现值 —— 档位刚在上一帧的 pipelineProcess 里切过就用新值。
    const int n = (int)localPipe.an.n;
    if (localFill > n) {
      // 档位切小时只保留最近 n 个样本 —— capi 同款坑：size_t 减法下溢
      // 会让后面的写越界（liblamp 实测 SIGBUS），这里先收缩再算 want。
      memmove(localPcm, localPcm + (localFill - n), n * sizeof(float));
      localFill = n;
    }
    size_t want = (size_t)(n - localFill);
    if (want > sizeof(tmp) / sizeof(tmp[0])) want = sizeof(tmp) / sizeof(tmp[0]);
    size_t got = want ? rb.read(tmp, want) : 0;
    if (got == 0 && localFill < n) break;
    for (size_t i = 0; i < got; i++)
      localPcm[localFill + i] = (float)tmp[i] / 32768.0f;   // liblamp 同款 ±1 域
    localFill += (int)got;
    if (localFill < n) continue;

    // 帧就绪：lamp 窗（retime 已按档位填好）→ 对应尺寸 FFT → 幅度谱
    for (int i = 0; i < n; i++) {
      localRe[i] = localPcm[i] * localPipe.an.w[i];
      localIm[i] = 0.0f;
    }
    ArduinoFFT<float> &F = (n == 512) ? fft512 : (n == 2048) ? fft2048 : fft1024;
    F.compute(FFTDirection::Forward);
    F.complexToMagnitude();                      // 幅度就地写入 localRe[0..n/2]
    for (int i = 0; i <= n / 2; i++) localMag[i] = localRe[i];
    localFrame   = pipelineProcess(localPipe, localCfg, localPcm, localMag, millis());
    localFrameMs = millis();
    // hop 取**当前**档位 —— 档位可能刚在 pipelineProcess 里换过，
    // 用旧值会让时间轴慢慢漂（capi 注释同款）。
    const int hop  = (int)presetParams(localPipe.style.current).hop;
    const int keep = (localFill > hop) ? (localFill - hop) : 0;
    if (keep) memmove(localPcm, localPcm + hop, (size_t)keep * sizeof(float));
    localFill = keep;
  }
}

// ♪ Auto：lamp_auto 按音乐内容自动挑效果（f0 占比/音高抖动/打击度/频谱
// 分裂度打分 + 滞回防抖）。状态全局一份 —— 多 segment 同跑 Auto 时同步换。
AutoState  autoSt;
AutoConfig autoCfg;

// 亮度动态型效果（整管近单色相）用位置铺调色板；空间多彩型用色相映射。
// 旋律/人声类是「按数据档动态归类」：LAMP1 完整数据在（rich）时它们有真实
// 色相语义（调性/色度/音高/共振峰）→ 色相映射；退化回退分支近单色相 → 位置型。
inline bool fxPaletteByPosition(FxId id, bool rich) {
  switch (id) {
    case FX_SPECTRUM_BARS:      // 频段彩虹沿管
    case FX_SPLIT_BANDS:        // 低/高频两段异色
    case FX_COLOR_FLOW:         // 色相流动本身多彩
    case FX_MOOD_GRADIENT:      // 沿管情绪渐变
    case FX_SLOW_AURORA:        // 三相位极光
      return false;
    case FX_KEY_WASH: case FX_CHROMA_RING: case FX_MELODY_LINE:
    case FX_PITCH_COMET: case FX_HARMONY_SHIFT: case FX_VOCAL_HALO:
    case FX_VOCAL_BREATH: case FX_FORMANT_RIBBON: case FX_DUET_SPLIT:
    case FX_LYRIC_PULSE:
      return !rich;
    default:
      return true;
  }
}

// RGB → (hue, sat, val)，8bit 域。lamp 效果的色相语义（频段位置、调性、
// mood）经 hue 翻译成调色板索引，效果的空间结构与明暗动态原样保留。
inline void rgb2hsv8(const Rgb &c, uint8_t &h, uint8_t &s, uint8_t &v) {
  uint8_t mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
  uint8_t mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
  v = mx;
  uint8_t d = mx - mn;
  if (mx == 0 || d == 0) { h = 0; s = 0; return; }
  s = (uint8_t)((255 * (int)d) / mx);
  int hh;
  if (mx == c.r)      hh =       (43 * ((int)c.g - c.b)) / d;   // 43 ≈ 255/6
  else if (mx == c.g) hh = 85  + (43 * ((int)c.b - c.r)) / d;
  else                hh = 170 + (43 * ((int)c.r - c.g)) / d;
  h = (uint8_t)(hh < 0 ? hh + 256 : hh);
}

void modeLampCommon(FxId id) {
  lampTrace = 1;
  bridge.fill(millis());
  lampTrace = 2;
  if (!SEGENV.data) {
    if (!SEGENV.allocateData(sizeof(FxState))) return;  // 内存不足：本帧空过
    new (SEGENV.data) FxState();           // 默认成员值靠 placement-new 落位
  }
  FxState *st = reinterpret_cast<FxState*>(SEGENV.data);
  lampTrace = 3;

  // ── 滑条接入（对齐原生效果惯例，per-segment）──
  // Speed → 时间缩放：128 = 设计默认，0 = 2 倍慢，255 = 2 倍快。
  // 缩放全部时间类参数（runner_tail 是空间比例，不动）。
  const float tScale = powf(2.0f, (128.0f - SEGMENT.speed) / 128.0f);
  FxConfig cfg;
  cfg.decay_beats *= tScale;  cfg.decay_free_ms *= tScale;
  cfg.flow_beats  *= tScale;  cfg.flow_free_ms  *= tScale;
  cfg.sect_tau_ms *= tScale;  cfg.trail_tau_ms  *= tScale;
  cfg.f0_glide_ms *= tScale;  cfg.bloom_beats   *= tScale;
  cfg.ripple_beats*= tScale;  cfg.drum_tau_ms   *= tScale;
  cfg.comet_tail_ms *= tScale; cfg.harm_tau_ms  *= tScale;
  cfg.voc_tau_ms  *= tScale;  cfg.lyric_tau_ms  *= tScale;
  cfg.tide_ms     *= tScale;  cfg.aurora_ms     *= tScale;
  // Sensitivity → 音频增益：128 = 1.0×。效果内部 perceptual/clamp 自行封顶。
  const float sGain = SEGMENT.intensity / 128.0f;
  AudioFrame f = bridge.frame;             // per-seg 副本（多 seg 不同参数）
  if (sGain != 1.0f) {
    for (int i = 0; i < NUM_BANDS; i++) {
      f.bands[i] *= sGain; f.bands_h[i] *= sGain; f.bands_p[i] *= sGain;
    }
    f.rms_fast *= sGain; f.rms_slow *= sGain; f.peak *= sGain;
  }

  static const Geometry geo;               // 两管方向按默认（S1 左、首颗管底）
  lampTrace = 4;
  // 白平衡关掉：WLED 自己有全局色彩处理，别叠两层
  fxRender(id, *st, cfg, f, geo, false, renderTick(), fxOut);
  lampTrace = 5;
  // 96 颗设计幅面 → 当前 segment 长度重采样（seg 恰为 96 时逐颗直映）。
  // 调色板融合：palette 0 (Default) 保持效果原生配色。选了调色板时分两型：
  //  · 空间多彩型（色相沿管本来就有行程）：hue → 调色板索引，形状与配色
  //    行程都保留；
  //  · 亮度动态型（整管近单色相，靠明暗表达，如 Bar Impact）：hue 映射只会
  //    取到调色板上一个点（用户实测 Fairy Reef 下恒蓝）——改用像素位置铺开
  //    调色板（WLED 原生惯例），效果自身 hue 变为滚动偏移，随音色/小节在
  //    调色板上漂移。
  // 低饱和像素（白闪、灰）两型都不映射，冲击感的白不被染色。
  const bool usePal = SEGMENT.palette != 0;
  const bool posMap = fxPaletteByPosition(id, bridge.remoteFresh(millis()));
  // mirror 兼容：seg 开镜像时 vLength 已折半，只采左管（S1=[0,47]）交引擎
  // 镜像出右半 —— 对称类效果视觉不变，与原生效果的 mirror 语义统一。
  const unsigned span = SEGMENT.mirror ? LEDS_PER_TUBE : TOTAL_LEDS;
  // Palette drift（check1，默认关）：调色板索引叠加极慢时间滚动（≈16s 一圈），
  // 对齐原生效果“颜色自己流动”的观感；关掉则颜色只跟音乐特征走。
  const uint8_t drift = SEGMENT.check1 ? (uint8_t)(strip.now >> 6) : 0;
  const uint32_t bg = SEGCOLOR(1);         // 暗部用背景色，对齐原生惯例
  unsigned len = SEGLEN;
  for (unsigned i = 0; i < len; i++) {
    const Rgb &c = fxOut[(uint32_t)i * span / len];
    if (usePal) {
      uint8_t h, s, v;
      rgb2hsv8(c, h, s, v);
      if (v == 0) { SEGMENT.setPixelColor(i, bg); continue; }
      if (s < 40) { SEGMENT.setPixelColor(i, RGBW32(c.r, c.g, c.b, 0)); continue; }
      uint8_t idx = (posMap ? (uint8_t)((i * 255u) / (len > 1 ? len - 1 : 1) + h)
                            : h) + drift;
      SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(idx, false, false, 0, v));
    } else {
      if (c.r == 0 && c.g == 0 && c.b == 0) { SEGMENT.setPixelColor(i, bg); continue; }
      SEGMENT.setPixelColor(i, RGBW32(c.r, c.g, c.b, 0));
    }
  }
  bridge.consumePulses();                    // 渲染帧消费脉冲（见 fillFromRemote 注释）
  lampTrace = 6;
}

// ♪ Auto：每帧先让选择器投票，再按它选中的效果渲染。
// autoRetime 每帧必须调 —— 它设置特征平滑系数 a_feat；漏掉则 EMA 恒 0、
// 评分全零，Auto 永远停在兜底效果不动（首版实际踩中）。
void modeLampAuto() {
  lampTrace = 0x10;
  static bool autoInited = false;
  bridge.fill(millis());
  if (!autoInited) { autoInit(autoSt, autoCfg, gRenderDt); autoInited = true; }
  autoRetime(autoSt, autoCfg, gRenderDt);
  autoUpdate(autoSt, autoCfg, bridge.frame, millis());
  lampTrace = 0x11;
  modeLampCommon(autoSt.current);
}
static const char mLampAuto_data[] PROGMEM = "♪ Auto@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0";

// 23 个包装函数 + 元数据（名字带 ♪ 前缀，特效列表里聚在一起好找）
#define LAMP_FX(fn, fxid, meta) \
  static void fn() { modeLampCommon(fxid); } \
  static const char fn##_data[] PROGMEM = meta;

LAMP_FX(mLampSpectrum,  FX_SPECTRUM_BARS,  "♪ Spectrum Bars@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampBeatPulse, FX_BEAT_PULSE,     "♪ Beat Pulse@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampLevel,     FX_LEVEL_SWEEP,    "♪ Level Sweep@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampImpact,    FX_BAR_IMPACT,     "♪ Bar Impact@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampRunner,    FX_BEAT_RUNNER,    "♪ Beat Runner@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampSplit,     FX_SPLIT_BANDS,    "♪ Split Bands@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampKeyWash,   FX_KEY_WASH,       "♪ Key Wash@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampChroma,    FX_CHROMA_RING,    "♪ Chroma Ring@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampFlow,      FX_COLOR_FLOW,     "♪ Color Flow@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampMelody,    FX_MELODY_LINE,    "♪ Melody Line@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampBloom,     FX_DOWNBEAT_BLOOM, "♪ Downbeat Bloom@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampLadder,    FX_BAR_LADDER,     "♪ Bar Ladder@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampKickSnare, FX_KICK_SNARE,     "♪ Kick & Snare@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampComet,     FX_PITCH_COMET,    "♪ Pitch Comet@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampHarmony,   FX_HARMONY_SHIFT,  "♪ Harmony Shift@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampVocalHalo, FX_VOCAL_HALO,     "♪ Vocal Halo@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampVocalBr,   FX_VOCAL_BREATH,   "♪ Vocal Breath@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampFormant,   FX_FORMANT_RIBBON, "♪ Formant Ribbon@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampDuet,      FX_DUET_SPLIT,     "♪ Duet Split@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampLyric,     FX_LYRIC_PULSE,    "♪ Lyric Pulse@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampTide,      FX_SECTION_TIDE,   "♪ Section Tide@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampMood,      FX_MOOD_GRADIENT,  "♪ Mood Gradient@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")
LAMP_FX(mLampAurora,    FX_SLOW_AURORA,    "♪ Slow Aurora@Speed,Sensitivity,,,,Palette drift;,Bg;!;1v;si=0")

} // namespace

// ── 灯珠位置调试页（/leddebug）：按物理编号直写像素 ──
// 走 realtime 通道（DDP/E1.31 同款机制）而不是 JSON "i"：后者按分段相对编号
// 并经反转/镜像/分组变换，调试要的恰恰是不经任何变换的物理序。页面每秒
// 续租 3 秒锁；关页 3 秒后自动退出 realtime、灯恢复原效果。
static const char kLedDbgPage[] PROGMEM = R"lamp(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>灯珠位置调试</title>
<style>
body{margin:0;background:#111;color:#eee;font:15px/1.5 -apple-system,Helvetica,Arial,sans-serif;padding:12px}
h1{font-size:18px;margin:0 0 6px}
.st{font-size:13px;opacity:.85;margin-bottom:8px}
.warn{color:#fc6}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}
button{background:#333;color:#eee;border:1px solid #555;border-radius:8px;padding:8px 14px;font-size:15px}
button.on{background:#2a6;border-color:#2a6;color:#000}
.sw{width:32px;height:32px;border-radius:50%;border:2px solid #555;padding:0}
.sw.on{border-color:#fff;box-shadow:0 0 0 2px #2a6}
.big{font-size:56px;font-weight:700;text-align:center;margin:6px 0;line-height:1}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:4px;margin:6px 0 12px}
.c{aspect-ratio:1;display:flex;align-items:center;justify-content:center;background:#222;border:1px solid #444;border-radius:6px;font-size:12px;cursor:pointer;user-select:none}
.c.on{background:#fff;color:#000;font-weight:700}
.c.cur{outline:2px solid #2a6}
label.t{font-size:13px;opacity:.85;display:block;margin-top:8px}
input[type=text]{background:#222;color:#eee;border:1px solid #555;border-radius:6px;padding:6px 8px;font-size:15px;width:90px}
textarea{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #555;border-radius:6px;padding:6px;font-size:13px;height:110px}
table{border-collapse:collapse}td{padding:4px 6px;font-size:14px}
</style></head><body>
<h1>灯珠位置调试</h1>
<div class="st" id="st">连接中…</div>
<div class="row">颜色
 <button class="sw on" data-c="ffffff" style="background:#fff"></button><button class="sw" data-c="ff0000" style="background:#f00"></button><button class="sw" data-c="00ff00" style="background:#0f0"></button><button class="sw" data-c="0000ff" style="background:#00f"></button>
 亮度 <input type="range" id="bri" min="10" max="255" value="120">
 <button id="stop">退出调试</button></div>
<div class="row"><button class="tab on" data-m="step">单步点亮</button><button class="tab" data-m="multi">多选点亮</button></div>
<div id="pStep">
 <div class="big" id="cur">0</div>
 <div class="row" style="justify-content:center"><button id="prev">◀ 上一颗</button><button id="next">下一颗 ▶</button><button id="auto">自动步进</button>
 <select id="iv"><option value="300">0.3s</option><option value="600" selected>0.6s</option><option value="1000">1s</option><option value="2000">2s</option></select></div>
 <div class="st">键盘 ←/→ 单步、空格 自动步进；点下方格子直接跳到该编号。</div>
</div>
<div id="pMulti" style="display:none">
 <div class="row"><button id="clr">清空</button><button data-r="0-47">全选 A 管</button><button data-r="48-95">全选 B 管</button><button id="inv">反选</button>
 范围 <input type="text" id="rng" placeholder="如 3-10"><button id="rgo">点亮范围</button></div>
 <div class="st">点格子切换选中；多颗同时亮用于确认一段的边界。</div>
</div>
<label class="t">灯管 A（bus 1 · 编号 0–47）</label><div class="grid" id="grid0"></div>
<label class="t">灯管 B（bus 2 · 编号 48–95）</label><div class="grid" id="grid1"></div>
<label class="t">记录结果：填编号范围（如 3-40），点“试亮”验证整段</label>
<table id="rec"></table>
<div class="row"><button id="exp">生成记录文本</button></div>
<textarea id="out" placeholder="点“生成记录文本”后整段复制给我"></textarea>
<script>
var N=96,sel=new Set(),cur=0,mode='step',color='ffffff',timer=null,autoT=null,active=false;
function $(i){return document.getElementById(i)}
function hex(){var b=+$('bri').value;return [0,2,4].map(function(k){var v=parseInt(color.substr(k,2),16)*b/255|0;return ('0'+v.toString(16)).slice(-2)}).join('')}
function px(){return mode=='step'?[cur]:Array.from(sel).sort(function(a,b){return a-b})}
function send(){active=true;fetch('/leddbg?px='+px().join(',')+'&c='+hex()).then(function(r){return r.json()}).then(function(j){var w=[];
 if(j.mso)w.push('⚠ 设置里 Use main segment only 已开，编号会经分段变换，请关闭后再调');
 if(j.ovr)w.push('⚠ Live override 已开，实时数据被忽略，调试无效');
 $('st').innerHTML='调试中 · 管灯 '+(j.t||j.n)+' 颗'+(j.n>(j.t||j.n)?'（另 '+(j.n-j.t)+' 颗状态灯不参与）':'')+' · 已点亮 '+px().length+' 颗'+(w.length?'<div class="warn">'+w.join('<br>')+'</div>':'');
 var tn=j.t||j.n;if(tn&&tn!=N){N=tn;build()}}).catch(function(){$('st').textContent='连接失败，检查板子是否在线'});
 if(!timer)timer=setInterval(send,1000)}
function stop(){active=false;clearInterval(timer);timer=null;stopAuto();fetch('/leddbg?off=1').catch(function(){});$('st').textContent='已退出，灯 3 秒内恢复原效果'}
function build(){['grid0','grid1'].forEach(function(id,k){var g=$(id);g.innerHTML='';for(var i=k*48;i<Math.min(N,(k+1)*48);i++){var d=document.createElement('div');d.className='c';d.textContent=i;d.dataset.i=i;g.appendChild(d)}});paint()}
function paint(){document.querySelectorAll('.c').forEach(function(d){var i=+d.dataset.i;d.classList.toggle('on',mode=='step'?i==cur:sel.has(i));d.classList.toggle('cur',mode=='step'&&i==cur)});$('cur').textContent=cur}
function setCur(i){cur=((i%N)+N)%N;paint();send()}
function stopAuto(){if(autoT){clearInterval(autoT);autoT=null;$('auto').classList.remove('on')}}
function setMode(m){mode=m;document.querySelectorAll('.tab').forEach(function(s){s.classList.toggle('on',s.dataset.m==m)});$('pStep').style.display=m=='step'?'':'none';$('pMulti').style.display=m=='multi'?'':'none';stopAuto();paint()}
function addRange(s){var m=/^(\d+)\s*-\s*(\d+)$/.exec((s||'').trim());if(!m)return;var a=+m[1],b=+m[2];if(a>b){var x=a;a=b;b=x}for(var i=a;i<=b&&i<N;i++)sel.add(i);setMode('multi');send()}
document.body.addEventListener('click',function(e){var t=e.target;
 if(t.classList.contains('c')){var i=+t.dataset.i;if(mode=='step')setCur(i);else{sel.has(i)?sel.delete(i):sel.add(i);paint();send()}}
 else if(t.classList.contains('sw')){document.querySelectorAll('.sw').forEach(function(s){s.classList.remove('on')});t.classList.add('on');color=t.dataset.c;if(active)send()}
 else if(t.classList.contains('tab')){setMode(t.dataset.m);send()}
 else if(t.dataset.r){addRange(t.dataset.r)}
 else if(t.dataset.z!==undefined){sel.clear();addRange($('z'+t.dataset.z).value)}});
$('prev').onclick=function(){setCur(cur-1)};$('next').onclick=function(){setCur(cur+1)};
$('auto').onclick=function(){if(autoT){stopAuto();return}$('auto').classList.add('on');autoT=setInterval(function(){setCur(cur+1)},+$('iv').value)};
$('iv').onchange=function(){if(autoT){stopAuto();$('auto').click()}};
$('bri').oninput=function(){if(active)send()};
$('stop').onclick=stop;
$('clr').onclick=function(){sel.clear();paint();send()};
$('inv').onclick=function(){for(var i=0;i<N;i++){sel.has(i)?sel.delete(i):sel.add(i)}paint();send()};
$('rgo').onclick=function(){addRange($('rng').value)};
document.addEventListener('keydown',function(e){if(/INPUT|TEXTAREA|SELECT/.test(e.target.tagName))return;if(e.key=='ArrowRight')setCur(cur+1);else if(e.key=='ArrowLeft')setCur(cur-1);else if(e.key==' '){e.preventDefault();$('auto').click()}});
var Z=[['A','主灯柱'],['A','底部灯柱'],['A','底座环'],['B','主灯柱'],['B','底部灯柱'],['B','底座环']],rec=$('rec');
Z.forEach(function(z,k){var tr=document.createElement('tr');tr.innerHTML='<td>'+z[0]+' 管 · '+z[1]+'</td><td><input type="text" id="z'+k+'" placeholder="如 3-40"></td><td><button data-z="'+k+'">试亮</button></td>';rec.appendChild(tr)});
$('exp').onclick=function(){var s='灯珠位置记录（编号=物理序，0 起）\n';Z.forEach(function(z,k){s+=z[0]+' 管 '+z[1]+'：'+($('z'+k).value||'?')+'\n'});s+='方向备注：';$('out').value=s};
window.addEventListener('beforeunload',function(){if(active&&navigator.sendBeacon)navigator.sendBeacon('/leddbg?off=1')});
build();send();
</script></body></html>
)lamp";

static uint8_t       dbgMask[32];                 // 256 位，够任何灯数
static uint8_t       dbgRgb[3] = {255, 255, 255};
static volatile bool dbgPending = false, dbgOff = false;

// "0,5,7-12" + "RRGGBB"。AsyncTCP 任务写、loop 读——最坏一帧花屏，无害。
static void dbgParse(const String &px, const String &c) {
  memset(dbgMask, 0, sizeof(dbgMask));
  int i = 0, n = px.length();
  while (i < n) {
    int j = i; while (j < n && px[j] != ',') j++;
    String tok = px.substring(i, j); i = j + 1;
    int dash = tok.indexOf('-');
    long a, b;
    if (dash < 0) { a = b = tok.toInt(); }
    else { a = tok.substring(0, dash).toInt(); b = tok.substring(dash + 1).toInt(); }
    if (a > b) { long s = a; a = b; b = s; }
    for (long k = a; k <= b && k < 256; ++k) if (k >= 0) dbgMask[k >> 3] |= 1 << (k & 7);
  }
  if (c.length() == 6) {
    long v = strtol(c.c_str(), nullptr, 16);
    dbgRgb[0] = (v >> 16) & 0xFF; dbgRgb[1] = (v >> 8) & 0xFF; dbgRgb[2] = v & 0xFF;
  }
}

static void dbgApply() {                          // 主循环上下文
  if (dbgOff) {
    dbgOff = false; dbgPending = false;
    if (realtimeMode) realtimeTimeout = millis(); // 下一轮 handleNotifications 退出 realtime
    return;
  }
  dbgPending = false;
  realtimeLock(3000, REALTIME_MODE_GENERIC);
  const uint16_t total = strip.getLengthTotal();
  for (uint16_t i = 0; i < total; ++i) {
    const bool on = i < 256 && (dbgMask[i >> 3] & (1 << (i & 7)));
    setRealtimePixel(i, on ? dbgRgb[0] : 0, on ? dbgRgb[1] : 0, on ? dbgRgb[2] : 0, 0);
  }
  strip.show();
}

class LampFxUsermod : public Usermod {
  public:
    void setup() override {
      if (esp_reset_reason() == ESP_RST_POWERON) lampTrace = 0;  // 上电随机值清零
      // ⚠️ 注册顺序 = ID 分配顺序（255 自动填洞）。新效果只许追加在**最后**，
      // 插前面会把既有 ♪ 效果的 ID 全部后移，用户已存预设集体指错效果。
      strip.addEffect(255, &mLampSpectrum,  mLampSpectrum_data);
      strip.addEffect(255, &mLampBeatPulse, mLampBeatPulse_data);
      strip.addEffect(255, &mLampLevel,     mLampLevel_data);
      strip.addEffect(255, &mLampImpact,    mLampImpact_data);
      strip.addEffect(255, &mLampRunner,    mLampRunner_data);
      strip.addEffect(255, &mLampSplit,     mLampSplit_data);
      strip.addEffect(255, &mLampKeyWash,   mLampKeyWash_data);
      strip.addEffect(255, &mLampChroma,    mLampChroma_data);
      strip.addEffect(255, &mLampFlow,      mLampFlow_data);
      strip.addEffect(255, &mLampMelody,    mLampMelody_data);
      strip.addEffect(255, &mLampBloom,     mLampBloom_data);
      strip.addEffect(255, &mLampLadder,    mLampLadder_data);
      strip.addEffect(255, &mLampKickSnare, mLampKickSnare_data);
      strip.addEffect(255, &mLampComet,     mLampComet_data);
      strip.addEffect(255, &mLampHarmony,   mLampHarmony_data);
      strip.addEffect(255, &mLampVocalHalo, mLampVocalHalo_data);
      strip.addEffect(255, &mLampVocalBr,   mLampVocalBr_data);
      strip.addEffect(255, &mLampFormant,   mLampFormant_data);
      strip.addEffect(255, &mLampDuet,      mLampDuet_data);
      strip.addEffect(255, &mLampLyric,     mLampLyric_data);
      strip.addEffect(255, &mLampTide,      mLampTide_data);
      strip.addEffect(255, &mLampMood,      mLampMood_data);
      strip.addEffect(255, &mLampAurora,    mLampAurora_data);
      strip.addEffect(255, &modeLampAuto,   mLampAuto_data);
      beatInit(bridgeBeatRef(), BeatConfig{});
    }

    void connected() override {
      udp.stop();
      udpOk = udp.begin(kLampSyncPort) != 0;
      // ♪ 分析仪表数据端点（B 项）：主页仪表卡片 10Hz 轮询。~500B 紧凑 JSON。
      // AsyncTCP 任务读 bridge.frame（loop 写）——纯展示用，float 撕裂无害。
      if (!routeOk) {
        server.on("/lampdata", HTTP_GET, [](AsyncWebServerRequest *request) {
          const AudioFrame &f = bridge.frame;
          char buf[640];
          int n = snprintf(buf, sizeof(buf),
            "{\"bands\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
            "\"chroma\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],"
            "\"rms\":%.4f,\"bpm\":%.0f,\"conf\":%.2f,\"phase\":%.2f,\"lock\":%d,"
            "\"key\":%d,\"maj\":%d,\"kconf\":%.2f,\"f0\":%.0f,\"voiced\":%d,"
            "\"vocal\":%.2f,\"mood\":%.2f,\"perc\":%.2f,\"style\":%d,\"src\":%d,"
            "\"val\":%.2f,\"emo\":%d}",
            f.bands[0], f.bands[1], f.bands[2], f.bands[3], f.bands[4], f.bands[5], f.bands[6], f.bands[7],
            f.bands[8], f.bands[9], f.bands[10], f.bands[11], f.bands[12], f.bands[13], f.bands[14], f.bands[15],
            f.chroma[0], f.chroma[1], f.chroma[2], f.chroma[3], f.chroma[4], f.chroma[5],
            f.chroma[6], f.chroma[7], f.chroma[8], f.chroma[9], f.chroma[10], f.chroma[11],
            f.rms_fast, f.bpm, f.bpm_conf, f.phase, f.beat_locked ? 1 : 0,
            f.key_root, f.key_is_major ? 1 : 0, f.key_conf, f.f0_hz, f.f0_voiced ? 1 : 0,
            f.vocal, f.mood, f.percussive, (int)f.preset,
            bridge.remoteFresh(millis()) ? 2 : ((localFrameMs && millis() - localFrameMs < 100) ? 1 : 0),
            bridge.remoteFresh(millis()) ? bridge.remote.valence : 0.0f,
            bridge.remoteFresh(millis()) ? (int)bridge.remote.emo_id : 255);
          if (n > 0 && n < (int)sizeof(buf)) request->send(200, "application/json", buf);
          else request->send(500);
        });
        server.on("/leddebug", HTTP_GET, [](AsyncWebServerRequest *request) {
          request->send_P(200, "text/html; charset=utf-8", kLedDbgPage);
        });
        server.on("/leddbg", HTTP_ANY, [](AsyncWebServerRequest *request) {
          if (request->hasParam("off")) { dbgOff = true; }
          else {
            dbgParse(request->hasParam("px") ? request->getParam("px")->value() : String(),
                     request->hasParam("c")  ? request->getParam("c")->value()  : String());
            dbgPending = true;
          }
          char buf[64];
          snprintf(buf, sizeof(buf), "{\"n\":%u,\"t\":%u,\"mso\":%d,\"ovr\":%d}",
                   (unsigned)strip.getLengthTotal(), (unsigned)TOTAL_LEDS, useMainSegmentOnly ? 1 : 0, realtimeOverride ? 1 : 0);
          request->send(200, "application/json", buf);
        });
        routeOk = true;
      }
    }

    void loop() override {
      if (dbgPending || dbgOff) dbgApply();   // 灯珠调试页的直写请求
      serviceLocalPipeline();          // 本地完整管线：有 PCM 就出帧，无则空转
      // 数据桥每轮保鲜（同 tick 幂等）：bridge.frame 原本只在 ♪ 效果渲染时
      // 更新，跑原生效果时 /lampdata 与分析仪表拿到的是陈旧全零（实测踩中）。
      // loop 与渲染同任务串行，无竞态；顺带让拍点/Auto 在任何效果下保持热身。
      bridge.fill(millis());
      if (!udpOk) return;
      // 非阻塞 drain：一轮 loop 把积压的包全收掉，只留最新
      int len;
      while ((len = udp.parsePacket()) > 0) {
        if (len != (int)sizeof(LampSyncPacket) && len != kLampSyncV1Len) { udp.flush(); continue; }
        LampSyncPacket pkt;
        pkt.valence = 0.0f; pkt.emo_id = 255;      // v1 包缺省
        udp.read((uint8_t*)&pkt, len);
        if (memcmp(pkt.magic, "LAMP1", 5) != 0 || pkt.version != 1) continue;
        bridge.remote = pkt;
        bridge.pulseLatch |= pkt.flags & 0x35;   // onset|downbeat|section|vocal_onset
        bridge.remoteMs = millis();
      }
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(F("Lamp FX Data"));
      uint32_t _n = millis();
      if (bridge.remoteFresh(_n)) arr.add(F("full (LAMP1)"));
      else if (localFrameMs && (_n - localFrameMs) < 100) arr.add(F("full (local)"));
      else arr.add(F("basic (bridge)"));
      JsonArray tr = user.createNestedArray(F("Lamp trace"));
      tr.add((int)lampTrace);
      // ♪ Auto 当前选中的效果 —— 「灯怎么在放这个」的第一排查入口
      JsonArray af = user.createNestedArray(F("Auto FX"));
      af.add(fxName(autoSt.current));
      // 上次复位原因：排查「切换特效即重启」类问题的第一手证据 ——
      // PANIC/WDT 指向固件 bug，BROWNOUT 指向供电跌落（换 J1 适配器供电）。
      JsonArray rr = user.createNestedArray(F("Reset reason"));
      switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   rr.add(F("power-on"));  break;
        case ESP_RST_SW:        rr.add(F("software"));  break;
        case ESP_RST_PANIC:     rr.add(F("PANIC"));     break;
        case ESP_RST_INT_WDT:   rr.add(F("INT WDT"));   break;
        case ESP_RST_TASK_WDT:  rr.add(F("TASK WDT"));  break;
        case ESP_RST_WDT:       rr.add(F("WDT"));       break;
        case ESP_RST_BROWNOUT:  rr.add(F("BROWNOUT"));  break;
        default:                rr.add((int)esp_reset_reason()); break;
      }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

  private:
    WiFiUDP udp;
    bool udpOk = false;
    bool routeOk = false;
    static BeatTracker &bridgeBeatRef() { return bridge.beat; }
};

static LampFxUsermod lamp_fx_usermod;
REGISTER_USERMOD(lamp_fx_usermod);
