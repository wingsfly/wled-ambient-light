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

#include "lamp_fx.h"
#include "lamp_beat.h"
#include "lamp_onset.h"

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
};
static_assert(sizeof(LampSyncPacket) == 336, "LAMP1 包布局漂移，必须同步 sender");

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
    frame.gated = r.flags & 0x08; frame.onset = r.flags & 0x01; frame.onset_rate = r.rate;
    frame.bpm = r.bpm; frame.bpm_conf = r.conf;
    frame.beat_locked = r.flags & 0x02; frame.phase = r.phase;
    frame.centroid_hz = r.centroid; frame.flatness = r.flatness; frame.percussive = r.percussive;
    frame.key_root = r.key_root; frame.key_is_major = r.flags & 0x80;
    frame.key_conf = r.key_conf; frame.harmony_move = r.harmony;
    frame.f0_hz = r.f0; frame.f0_conf = r.f0_conf; frame.f0_voiced = r.flags & 0x40;
    frame.energy_trend = r.trend; frame.section_novelty = r.novelty;
    frame.section_change = r.flags & 0x10; frame.mood = r.mood; frame.dynamics = r.dynamics;
    frame.beats_per_bar = r.bpb; frame.bar_pos = r.bar_pos; frame.bar_index = r.bar_index;
    frame.bar_conf = r.bar_conf; frame.downbeat = r.flags & 0x04;
    frame.vocal = r.vocal; frame.vocal_onset = r.flags & 0x20;
    StylePreset p = r.preset < STYLE_COUNT ? (StylePreset)r.preset : STYLE_GENERAL;
    frame.preset_changed = p != prevPreset;
    frame.preset = prevPreset = p;
  }

  void fill(uint32_t now) {
    if (now == lastMs) return;               // 每 WLED 帧只算一次，多 segment 共用
    dtMs = (lastMs && now > lastMs) ? (float)(now - lastMs) : 20.0f;
    if (dtMs > 100.0f) dtMs = 100.0f;
    lastMs = now;

    if (remoteFresh(now)) { fillFromRemote(); return; }

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
  bridge.fill(strip.now);
  if (!SEGENV.data) {
    if (!SEGENV.allocateData(sizeof(FxState))) return;  // 内存不足：本帧空过
    new (SEGENV.data) FxState();           // 默认成员值靠 placement-new 落位
  }
  FxState *st = reinterpret_cast<FxState*>(SEGENV.data);

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
  // 白平衡关掉：WLED 自己有全局色彩处理，别叠两层
  fxRender(id, *st, cfg, f, geo, false, bridge.dtMs, fxOut);
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
  const bool posMap = fxPaletteByPosition(id, bridge.remoteFresh(strip.now));
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
}

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

class LampFxUsermod : public Usermod {
  public:
    void setup() override {
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
      beatInit(bridgeBeatRef(), BeatConfig{});
    }

    void connected() override {
      udp.stop();
      udpOk = udp.begin(kLampSyncPort) != 0;
    }

    void loop() override {
      if (!udpOk) return;
      // 非阻塞 drain：一轮 loop 把积压的包全收掉，只留最新
      int len;
      while ((len = udp.parsePacket()) > 0) {
        if (len != (int)sizeof(LampSyncPacket)) { udp.flush(); continue; }
        LampSyncPacket pkt;
        udp.read((uint8_t*)&pkt, sizeof(pkt));
        if (memcmp(pkt.magic, "LAMP1", 5) != 0 || pkt.version != 1) continue;
        bridge.remote = pkt;
        bridge.remoteMs = millis();
      }
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(F("Lamp FX Data"));
      if (bridge.remoteFresh(millis())) arr.add(F("full (LAMP1)"));
      else arr.add(F("basic (bridge)"));
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

  private:
    WiFiUDP udp;
    bool udpOk = false;
    static BeatTracker &bridgeBeatRef() { return bridge.beat; }
};

static LampFxUsermod lamp_fx_usermod;
REGISTER_USERMOD(lamp_fx_usermod);
