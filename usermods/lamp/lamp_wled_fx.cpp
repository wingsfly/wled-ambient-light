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

  void fill(uint32_t now) {
    if (now == lastMs) return;               // 每 WLED 帧只算一次，多 segment 共用
    dtMs = (lastMs && now > lastMs) ? (float)(now - lastMs) : 20.0f;
    if (dtMs > 100.0f) dtMs = 100.0f;
    lastMs = now;

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
  static const FxConfig cfg;               // 第一批用默认参数
  static const Geometry geo;               // 两管方向按默认（S1 左、首颗管底）
  // 白平衡关掉：WLED 自己有全局色彩处理，别叠两层
  fxRender(id, *st, cfg, bridge.frame, geo, false, bridge.dtMs, fxOut);
  // 96 颗设计幅面 → 当前 segment 长度重采样（seg 恰为 96 时逐颗直映）。
  // 调色板融合：palette 0 (Default) 保持效果原生配色；选了调色板则把像素
  // 色相翻译成调色板索引、亮度原样保留 —— 效果的形状与动态不变，颜色风格
  // 交给调色板。低饱和像素（白闪、灰）不映射，冲击感的白不被染色。
  const bool usePal = SEGMENT.palette != 0;
  unsigned len = SEGLEN;
  for (unsigned i = 0; i < len; i++) {
    const Rgb &c = fxOut[(uint32_t)i * TOTAL_LEDS / len];
    if (usePal) {
      uint8_t h, s, v;
      rgb2hsv8(c, h, s, v);
      if (v == 0) { SEGMENT.setPixelColor(i, 0); continue; }
      if (s < 40) { SEGMENT.setPixelColor(i, RGBW32(c.r, c.g, c.b, 0)); continue; }
      SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(h, false, false, 0, v));
    } else {
      SEGMENT.setPixelColor(i, RGBW32(c.r, c.g, c.b, 0));
    }
  }
}

// 23 个包装函数 + 元数据（名字带 ♪ 前缀，特效列表里聚在一起好找）
#define LAMP_FX(fn, fxid, meta) \
  static void fn() { modeLampCommon(fxid); } \
  static const char fn##_data[] PROGMEM = meta;

LAMP_FX(mLampSpectrum,  FX_SPECTRUM_BARS,  "♪ Spectrum Bars@;;!;1v;si=0")
LAMP_FX(mLampBeatPulse, FX_BEAT_PULSE,     "♪ Beat Pulse@;;!;1v;si=0")
LAMP_FX(mLampLevel,     FX_LEVEL_SWEEP,    "♪ Level Sweep@;;!;1v;si=0")
LAMP_FX(mLampImpact,    FX_BAR_IMPACT,     "♪ Bar Impact@;;!;1v;si=0")
LAMP_FX(mLampRunner,    FX_BEAT_RUNNER,    "♪ Beat Runner@;;!;1v;si=0")
LAMP_FX(mLampSplit,     FX_SPLIT_BANDS,    "♪ Split Bands@;;!;1v;si=0")
LAMP_FX(mLampKeyWash,   FX_KEY_WASH,       "♪ Key Wash@;;!;1v;si=0")
LAMP_FX(mLampChroma,    FX_CHROMA_RING,    "♪ Chroma Ring@;;!;1v;si=0")
LAMP_FX(mLampFlow,      FX_COLOR_FLOW,     "♪ Color Flow@;;!;1v;si=0")
LAMP_FX(mLampMelody,    FX_MELODY_LINE,    "♪ Melody Line@;;!;1v;si=0")
LAMP_FX(mLampBloom,     FX_DOWNBEAT_BLOOM, "♪ Downbeat Bloom@;;!;1v;si=0")
LAMP_FX(mLampLadder,    FX_BAR_LADDER,     "♪ Bar Ladder@;;!;1v;si=0")
LAMP_FX(mLampKickSnare, FX_KICK_SNARE,     "♪ Kick & Snare@;;!;1v;si=0")
LAMP_FX(mLampComet,     FX_PITCH_COMET,    "♪ Pitch Comet@;;!;1v;si=0")
LAMP_FX(mLampHarmony,   FX_HARMONY_SHIFT,  "♪ Harmony Shift@;;!;1v;si=0")
LAMP_FX(mLampVocalHalo, FX_VOCAL_HALO,     "♪ Vocal Halo@;;!;1v;si=0")
LAMP_FX(mLampVocalBr,   FX_VOCAL_BREATH,   "♪ Vocal Breath@;;!;1v;si=0")
LAMP_FX(mLampFormant,   FX_FORMANT_RIBBON, "♪ Formant Ribbon@;;!;1v;si=0")
LAMP_FX(mLampDuet,      FX_DUET_SPLIT,     "♪ Duet Split@;;!;1v;si=0")
LAMP_FX(mLampLyric,     FX_LYRIC_PULSE,    "♪ Lyric Pulse@;;!;1v;si=0")
LAMP_FX(mLampTide,      FX_SECTION_TIDE,   "♪ Section Tide@;;!;1v;si=0")
LAMP_FX(mLampMood,      FX_MOOD_GRADIENT,  "♪ Mood Gradient@;;!;1v;si=0")
LAMP_FX(mLampAurora,    FX_SLOW_AURORA,    "♪ Slow Aurora@;;!;1v;si=0")

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
    void loop() override {}
    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

  private:
    static BeatTracker &bridgeBeatRef() { return bridge.beat; }
};

static LampFxUsermod lamp_fx_usermod;
REGISTER_USERMOD(lamp_fx_usermod);
