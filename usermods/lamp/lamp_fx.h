// 灯效（设计文档 §5 的最小起点）。
//
// ⚠️ 这只是**三个示例效果**，不是最终的特效集。它们存在的理由是让模拟页面
// 有东西可画、让 AudioFrame 的每个字段都有一个消费者 —— 真正的效果设计是
// 阶段 4 的事。
//
// 三个效果各自只吃 AudioFrame 的一部分，合起来把主要字段都覆盖了：
//   spectrumBars  ← bands[16]           频段是否正确分层
//   beatPulse     ← phase / beat_locked 拍点是否对齐
//   levelSweep    ← rms_fast / peak     包络与峰值的动态
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>
#include <math.h>

#include "lamp_geometry.h"
#include "lamp_color.h"
#include "lamp_pipeline.h"
#include "lamp_envelope.h"   // envCoeff

namespace lamp {

enum FxId : uint8_t {
    FX_SPECTRUM_BARS = 0,
    FX_BEAT_PULSE    = 1,
    FX_LEVEL_SWEEP   = 2,
    FX_BAR_IMPACT    = 3,
    FX_BEAT_RUNNER   = 4,
    FX_SPLIT_BANDS   = 5,
    FX_KEY_WASH      = 6,
    FX_CHROMA_RING   = 7,
    FX_COLOR_FLOW    = 8,
    FX_MELODY_LINE   = 9,
    // ── 节拍 ──
    FX_DOWNBEAT_BLOOM = 10,
    FX_BAR_LADDER     = 11,
    FX_KICK_SNARE     = 12,
    // ── 旋律 ──
    FX_PITCH_COMET    = 13,
    FX_HARMONY_SHIFT  = 14,
    // ── 人声 / 主旋律 ──
    FX_VOCAL_HALO     = 15,
    FX_VOCAL_BREATH   = 16,
    FX_FORMANT_RIBBON = 17,
    FX_DUET_SPLIT     = 18,
    FX_LYRIC_PULSE    = 19,
    // ── 氛围 ──
    FX_SECTION_TIDE   = 20,
    FX_MOOD_GRADIENT  = 21,
    FX_SLOW_AURORA    = 22,
    FX_COUNT          = 23,
};

// 效果吃哪些音乐特征。**按它真正读了 AudioFrame 的哪几个字段标**，
// 不按「听起来像什么」标 —— 界面上的筛选是拿这个当依据的，标虚了就是骗人。
enum FxTag : uint8_t {
    TAG_BEAT     = 1 << 0,   // 拍点 / 小节 / 起音
    TAG_MELODY   = 1 << 1,   // 基频 / 色度 / 调性 / 和声
    TAG_VOCAL    = 1 << 2,   // 人声（主旋律）存在度
    TAG_MOOD     = 1 << 3,   // 氛围 / 段落 / 能量走向
    TAG_SPECTRUM = 1 << 4,   // 频段能量 / 谐波打击分离
};

// 适合的音乐风格。**这是策展推荐，不是自动识别** —— 这套管线不做流派分类，
// 这张表是人按「这个流派突出什么特征、这个效果表达什么」填的。
enum FxGenre : uint8_t {
    GEN_CLASSICAL = 1 << 0,
    GEN_POP       = 1 << 1,
    GEN_ROCK      = 1 << 2,
    GEN_RAP       = 1 << 3,
    GEN_EDM       = 1 << 4,
};

inline const char *fxName(FxId f) {
    switch (f) {
        case FX_BEAT_PULSE:  return "beat-pulse";
        case FX_LEVEL_SWEEP: return "level-sweep";
        case FX_BAR_IMPACT:  return "bar-impact";
        case FX_BEAT_RUNNER: return "beat-runner";
        case FX_SPLIT_BANDS: return "split-bands";
        case FX_KEY_WASH:    return "key-wash";
        case FX_CHROMA_RING: return "chroma-ring";
        case FX_COLOR_FLOW:  return "color-flow";
        case FX_MELODY_LINE: return "melody-line";
        case FX_DOWNBEAT_BLOOM: return "downbeat-bloom";
        case FX_BAR_LADDER:     return "bar-ladder";
        case FX_KICK_SNARE:     return "kick-snare";
        case FX_PITCH_COMET:    return "pitch-comet";
        case FX_HARMONY_SHIFT:  return "harmony-shift";
        case FX_VOCAL_HALO:     return "vocal-halo";
        case FX_VOCAL_BREATH:   return "vocal-breath";
        case FX_FORMANT_RIBBON: return "formant-ribbon";
        case FX_DUET_SPLIT:     return "duet-split";
        case FX_LYRIC_PULSE:    return "lyric-pulse";
        case FX_SECTION_TIDE:   return "section-tide";
        case FX_MOOD_GRADIENT:  return "mood-gradient";
        case FX_SLOW_AURORA:    return "slow-aurora";
        case FX_SPECTRUM_BARS:
        default:             return "spectrum-bars";
    }
}

// 中文名，界面直接用。放在这里而不是页面里 —— 效果表只有一份真相。
inline const char *fxNameCn(FxId f) {
    switch (f) {
        case FX_BEAT_PULSE:     return "拍点脉冲";
        case FX_LEVEL_SWEEP:    return "电平扫描";
        case FX_BAR_IMPACT:     return "冲击柱";
        case FX_BEAT_RUNNER:    return "拍点光点";
        case FX_SPLIT_BANDS:    return "高低分离";
        case FX_KEY_WASH:       return "调性染色";
        case FX_CHROMA_RING:    return "音级环";
        case FX_COLOR_FLOW:     return "彩色流动";
        case FX_MELODY_LINE:    return "旋律线";
        case FX_DOWNBEAT_BLOOM: return "强拍绽放";
        case FX_BAR_LADDER:     return "小节阶梯";
        case FX_KICK_SNARE:     return "鼓组分离";
        case FX_PITCH_COMET:    return "音高彗星";
        case FX_HARMONY_SHIFT:  return "和声推移";
        case FX_VOCAL_HALO:     return "人声光晕";
        case FX_VOCAL_BREATH:   return "气息呼吸";
        case FX_FORMANT_RIBBON: return "共振峰带";
        case FX_DUET_SPLIT:     return "人声伴奏分管";
        case FX_LYRIC_PULSE:    return "唱句脉冲";
        case FX_SECTION_TIDE:   return "段落潮汐";
        case FX_MOOD_GRADIENT:  return "情绪渐变";
        case FX_SLOW_AURORA:    return "极光";
        case FX_SPECTRUM_BARS:
        default:                return "频段柱";
    }
}

inline uint8_t fxTags(FxId f) {
    switch (f) {
        case FX_SPECTRUM_BARS:  return TAG_SPECTRUM;
        case FX_BEAT_PULSE:     return TAG_BEAT;
        case FX_LEVEL_SWEEP:    return TAG_MOOD;
        case FX_BAR_IMPACT:     return TAG_BEAT;
        case FX_BEAT_RUNNER:    return TAG_BEAT;
        case FX_SPLIT_BANDS:    return TAG_SPECTRUM;
        case FX_KEY_WASH:       return TAG_MELODY;
        case FX_CHROMA_RING:    return TAG_MELODY;
        case FX_COLOR_FLOW:     return TAG_MOOD | TAG_MELODY;   // 色相由调性定，速度由 BPM
        case FX_MELODY_LINE:    return TAG_MELODY;
        case FX_DOWNBEAT_BLOOM: return TAG_BEAT;
        case FX_BAR_LADDER:     return TAG_BEAT;
        case FX_KICK_SNARE:     return TAG_BEAT | TAG_SPECTRUM;
        case FX_PITCH_COMET:    return TAG_MELODY;
        case FX_HARMONY_SHIFT:  return TAG_MELODY;
        case FX_VOCAL_HALO:     return TAG_VOCAL | TAG_MELODY;
        case FX_VOCAL_BREATH:   return TAG_VOCAL;
        case FX_FORMANT_RIBBON: return TAG_VOCAL | TAG_SPECTRUM;
        case FX_DUET_SPLIT:     return TAG_VOCAL | TAG_SPECTRUM;
        case FX_LYRIC_PULSE:    return TAG_VOCAL | TAG_BEAT;
        case FX_SECTION_TIDE:   return TAG_MOOD;
        case FX_MOOD_GRADIENT:  return TAG_MOOD;
        case FX_SLOW_AURORA:    return TAG_MOOD;
        default:                return 0;
    }
}

// 推荐给哪些风格。依据是「这个流派突出什么音乐特征」：
//   古典 —— 力度与旋律线，几乎没有稳定鼓点 → 调性/旋律/氛围类
//   流行 —— 人声为主，四拍规整         → 人声/彩色流动/拍点类
//   摇滚 —— 鼓组重、失真吉他压掉调性     → 冲击/鼓组/频段类
//   Rap  —— 808 与 hi-hat 的对话，旋律少 → 鼓组/高低分离/小节类
//   电子 —— 强四拍与段落构建           → 冲击/光点/段落类
inline uint8_t fxGenres(FxId f) {
    switch (f) {
        case FX_SPECTRUM_BARS:  return GEN_EDM | GEN_RAP;
        case FX_BEAT_PULSE:     return GEN_ROCK | GEN_RAP | GEN_EDM;
        case FX_LEVEL_SWEEP:    return GEN_CLASSICAL | GEN_POP;
        case FX_BAR_IMPACT:     return GEN_ROCK | GEN_EDM | GEN_RAP;
        case FX_BEAT_RUNNER:    return GEN_POP | GEN_EDM;
        case FX_SPLIT_BANDS:    return GEN_ROCK | GEN_RAP;
        case FX_KEY_WASH:       return GEN_CLASSICAL | GEN_POP;
        case FX_CHROMA_RING:    return GEN_CLASSICAL | GEN_POP;
        case FX_COLOR_FLOW:     return GEN_POP | GEN_EDM;
        case FX_MELODY_LINE:    return GEN_CLASSICAL | GEN_POP;
        case FX_DOWNBEAT_BLOOM: return GEN_ROCK | GEN_EDM;
        case FX_BAR_LADDER:     return GEN_RAP | GEN_EDM;
        case FX_KICK_SNARE:     return GEN_ROCK | GEN_RAP;
        case FX_PITCH_COMET:    return GEN_CLASSICAL | GEN_POP;
        case FX_HARMONY_SHIFT:  return GEN_CLASSICAL | GEN_POP;
        case FX_VOCAL_HALO:     return GEN_POP;
        case FX_VOCAL_BREATH:   return GEN_POP | GEN_CLASSICAL;
        case FX_FORMANT_RIBBON: return GEN_POP;
        case FX_DUET_SPLIT:     return GEN_POP | GEN_ROCK;
        case FX_LYRIC_PULSE:    return GEN_POP | GEN_RAP;
        case FX_SECTION_TIDE:   return GEN_EDM | GEN_CLASSICAL;
        case FX_MOOD_GRADIENT:  return GEN_CLASSICAL;
        case FX_SLOW_AURORA:    return GEN_CLASSICAL;
        default:                return 0;
    }
}

// 有时间演化的效果需要状态。前三个效果是无状态的纯函数（同一帧输入永远画出
// 同一幅图），冲击/拖尾这类做不到 —— 它们的当前亮度取决于之前发生过什么。
struct FxState {
    float bar[NUM_BANDS] = {0};   // 每段的包络（split-bands 用）
    float impact = 0.0f;          // **整管**的冲击包络
    float impact_cool = 0.0f;     // 注入冷却（ms）：同一击打的「回声 onset」只闪一次
    float hue    = 0.5f;          // 跟随频谱质心平滑移动的色相
    float chroma[kChroma] = {0};  // 平滑后的色度，直接画会闪
    float key_hue = 0.5f;         // 由调性决定的底色，换调时慢慢挪过去
    float flash = 0.0f;           // 和声变化的余波
    float runner = 0.0f;          // 光点位置 [0,1)
    float prev_phase = 0.0f;
    bool  has_phase = false;

    float flow = 0.0f;            // 色相流动相位 [0,1)
    float sect = 0.0f;            // 换段的余波
    float f0_u = 0.5f;            // 旋律线在管上的位置，平滑后
    bool  has_f0 = false;
    float trail[LEDS_PER_TUBE] = {0};   // 旋律线的拖影

    // ── 新增效果的状态 ──
    // 各效果用各自的变量，不共用 —— fxAdvance 每帧对所有效果推进状态，
    // 共用会让「换个效果画面不一样」变成「换个效果把另一个的状态搅了」。
    float bloom  = 0.0f;          // 强拍绽放的包络
    float ripple = 0.0f;          // 弱拍的小涟漪
    int   last_bar = -1;          // 小节阶梯：换小节时换色
    float ladder_hue = 0.0f;
    float kick = 0.0f, snare = 0.0f;      // 鼓组分离的两条包络
    float comet_u = 0.5f;                 // 音高彗星的位置
    bool  has_comet = false;
    float comet_trail[LEDS_PER_TUBE] = {0};
    float harm_hue = 0.0f, harm_wave = 0.0f;   // 和声推移
    float voc = 0.0f;             // 人声存在度的再平滑（画面别跟着抖）
    float voc_u = 0.5f;           // 人声光晕的位置（跟 f0 高低）
    float lyric = 0.0f;           // 唱句脉冲的包络
    float tide = 0.0f;            // 段落潮汐的扫过进度，1→0
    float tide_hue = 0.0f;
    float aur[3] = {0.11f, 0.47f, 0.79f};      // 极光三条相位，互质起点免得同步
};

struct FxConfig {
    // 冲击的衰减长度 = 拍周期 × 这个系数。**不能写成固定毫秒**：
    // 174BPM 一拍只有 345ms，而 90BPM 有 667ms —— 固定值要么在快歌里糊成一片，
    // 要么在慢歌里早早熄灭。0.55 拍意味着落到 1/e 时下一拍还没到。
    // 0.35 拍：视觉暗（衰到 ~0.3，perceptual 后近黑）约需 1.2τ ≈ 0.42 拍，
    // 击打间隙有近一半时间是暗的，「清脆感」成立。初版 0.55 拍衰到视觉暗
    // 要 ~1.3 拍 —— 下一击来时余辉未尽，慢歌里粘连成慢波动（实测观感）。
    float decay_beats   = 0.35f;
    float decay_free_ms = 180.0f;   // 没锁上节拍时的退路
    float runner_tail   = 0.28f;    // 光点拖尾长度，占管长的比例

    // 彩色流动：一个完整色相环走过多少拍。四拍一圈，慢歌快歌都不至于晕。
    float flow_beats    = 4.0f;
    float flow_free_ms  = 2400.0f;  // 没锁上节拍时的退路
    float sect_tau_ms   = 1200.0f;  // 换段余波的衰减
    float trail_tau_ms  = 500.0f;   // 旋律线拖影的衰减
    float f0_glide_ms   = 70.0f;    // 旋律线位置的平滑

    // ── 新增效果 ──
    float bloom_beats   = 1.6f;     // 强拍绽放的衰减长度（拍）——比冲击柱长，才有「绽放」感
    float ripple_beats  = 0.4f;     // 弱拍涟漪，短促
    float drum_tau_ms   = 90.0f;    // 鼓组包络：快到能分出底鼓与军鼓
    float comet_tail_ms = 900.0f;   // 彗星拖尾，比旋律线长得多
    float harm_tau_ms   = 2500.0f;  // 和声色相推移，一个乐句的尺度
    float voc_tau_ms    = 300.0f;   // 人声画面的再平滑
    float lyric_tau_ms  = 450.0f;   // 唱句脉冲的衰减
    float tide_ms       = 3500.0f;  // 段落潮汐扫过整管要多久
    float aurora_ms     = 26000.0f; // 极光一圈。慢到「看不出在动」才对
};

// HSV→RGB，色相 [0,1)。特效常用色相环，写一次省得三处重复。
inline Rgb hsv(float h, float s, float v) {
    h -= floorf(h);
    if (!(s >= 0.0f)) s = 0.0f; if (s > 1.0f) s = 1.0f;
    if (!(v >= 0.0f)) v = 0.0f; if (v > 1.0f) v = 1.0f;
    const float c = v * s, x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0, g = 0, b = 0;
    const int seg = (int)(h * 6.0f) % 6;
    switch (seg) {
        case 0: r = c; g = x; break;   case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;   case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;   default: r = c; b = x; break;
    }
    return Rgb{(uint8_t)((r + m) * 255.0f + 0.5f),
               (uint8_t)((g + m) * 255.0f + 0.5f),
               (uint8_t)((b + m) * 255.0f + 0.5f)};
}

// 显示缩放。AGC 把**总** RMS 拉到 target=0.25，而能量分散在 16 段里，
// 单段落在 0.02–0.3 量级 —— 直接送进 perceptual() 会几乎全黑。
// 这个系数是经验值，真机点亮后要重新标定。
constexpr float kFxBandScale  = 6.0f;
constexpr float kFxLevelScale = 3.0f;

// 把 [0,1] 的能量压成亮度。感知亮度近似平方关系，直接线性映射会让
// 中低电平段全挤在一起看不出层次。
inline float perceptual(float e) {
    if (!(e > 0.0f)) return 0.0f;
    if (e > 1.0f) e = 1.0f;
    return e * e;
}

// ── 效果 ──────────────────────────────────────────────────

// 频段柱：把 16 段铺满一根管，色相按段号走色轮，亮度按该段能量。
// 两根管镜像，这样从正面看是对称的。
// 亮度里的 dynamics 因子是**给古典用的**。AGC 已经把 bands 的绝对电平抹平，
// 只剩形状；乘回 dynamics，pp 与 ff 的差别才回得来。
// 压得死的流行/电子上 dynamics 恒接近 1，等于没这一项。
inline void fxSpectrumBars(const AudioFrame &f, const Geometry &g, Rgb *out) {
    const float dyn = isfinite(f.dynamics) ? clamp01(f.dynamics) : 1.0f;
    for (int s = 0; s < 2; ++s) {
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u   = (float)i / (float)(LEDS_PER_TUBE - 1);
            const int   band = (int)(u * (NUM_BANDS - 1) + 0.5f);
            const float e    = perceptual(f.bands[band] * kFxBandScale) * dyn;
            out[mapPixel(g, (Side)s, u)] = hsv((float)band / NUM_BANDS, 0.9f, e);
        }
    }
}

// 拍点脉冲：整管在拍点亮起，随相位衰减。未锁定时退回呼吸，
// 免得没节奏的音乐下灯完全不动 —— 那看起来像坏了。
inline void fxBeatPulse(const AudioFrame &f, const Geometry &g, Rgb *out) {
    const float env = f.beat_locked ? expf(-f.phase * 4.0f)
                                    : 0.35f + 0.25f * f.rms_fast;
    const float hue = f.beat_locked ? fmodf(f.bpm / 240.0f, 1.0f) : 0.55f;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.75f, perceptual(env));
        }
}

// 电平扫描：快包络决定填充高度，峰值留一条保持线。
// 这是最直观的「音量表」，用来眼看 AGC 是否把动态压在合理范围内。
inline void fxLevelSweep(const AudioFrame &f, const Geometry &g, Rgb *out) {
    const float lvl = perceptual(f.rms_fast * kFxLevelScale);
    const float pk  = perceptual(f.peak * kFxLevelScale);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            Rgb c{0, 0, 0};
            // 峰值保持线。pk 太小就不画 —— 否则静音时管底会留一段白，
            // 那看起来像「灯没关干净」而不是「没有信号」。
            if (pk > 0.02f && fabsf(u - pk) < 0.02f) c = Rgb{255, 255, 255};
            // 填充区间取左闭右开：写成 u <= lvl 的话，lvl=0（静音）时 u=0
            // 那一颗仍会亮，两根管各留一点绿。
            else if (u < lvl)                        c = hsv(0.33f - 0.33f * u, 0.95f, 1.0f);
            out[mapPixel(g, (Side)s, u)] = c;
        }
}

// ── 有状态的效果 ──────────────────────────────────────────

// 推进冲击包络。**上升沿瞬时跟随、衰减按拍周期缩放** —— 这两条合起来
// 才是「冲击峰的宽度和声音一致，同时跟得上 BPM」：
// 峰出现的时刻与高度完全由声音决定，而它落下去的快慢由节拍决定。
// 色度抖动压制要快（否则跟不上旋律），调性换色要慢（否则一犹豫就闪）。
constexpr float kFxChromaTauMs = 80.0f;
constexpr float kFxKeyHueTauMs = 600.0f;

inline void fxAdvance(FxState &st, const FxConfig &c, const AudioFrame &f, float dt_ms) {
    if (!isfinite(dt_ms) || dt_ms <= 0.0f) return;

    const float decay_ms = (f.beat_locked && f.bpm > 1.0f)
                         ? (60000.0f / f.bpm) * c.decay_beats
                         : c.decay_free_ms;
    const float a = envCoeff(decay_ms, dt_ms);

    for (int i = 0; i < NUM_BANDS; ++i) {
        float e = f.bands[i] * kFxBandScale;
        if (!isfinite(e) || e < 0.0f) e = 0.0f;
        if (e > 1.0f) e = 1.0f;
        if (e > st.bar[i]) st.bar[i] = e;              // 冲高：立刻，不做平滑
        else               st.bar[i] += a * (e - st.bar[i]);   // 掉落：按拍
    }

    // 整管冲击。**这是与频段柱的分界**：那个画的是频谱形状（16 根柱子），
    // 这个画的是「一次击打有多重」—— 一个标量，铺满整根管。
    //
    // 峰值高度由声音决定（冲高瞬时），落下去的快慢由节拍决定（衰减跟拍周期）。
    //
    // 冲击由 **onset 事件**注入，不是电平包络 —— 两条路都试死过：
    //   绝对 rms：AGC 归一后地板 ~0.5，衰不到暗（常亮微波动）；
    //   瞬态分量 (fast-slow)：现代流行的砖墙压缩让快慢包络几乎重合，
    //   趋零（Beat It 实拍：整管恒亮 ±0.3%、只剩底部 reach 微亮）。
    // onset 是谱通量突变检测，与绝对电平解耦，压缩免疫。峰高由打击
    // 通道能量定、保底 0.55 让每次击打可见；无击打帧按拍衰向零。
    {
        // 注入冷却 120ms：检测器的不应期只有 60ms（通用语义，节拍统计需要），
        // 而击打包络常拖到 150ms —— 余音再次超阈会发出同击的「回声 onset」，
        // 衰减调快后表现为一个鼓点抖闪 3~4 次（实测）。冷却窗内回声走衰减。
        st.impact_cool -= dt_ms;
        if (st.impact_cool < 0.0f) st.impact_cool = 0.0f;
        if (f.onset && st.impact_cool <= 0.0f) {
            float pe = 0.0f;
            for (int i = 0; i < NUM_BANDS; ++i) {
                if (isfinite(f.bands_p[i]) && f.bands_p[i] > 0.0f) pe += f.bands_p[i];
            }
            float e = pe * kFxBandScale * 0.6f + 0.55f;
            if (e > 1.0f) e = 1.0f;
            if (e > st.impact) st.impact = e;
            st.impact_cool = 120.0f;
        } else {
            st.impact += a * (0.0f - st.impact);
        }
    }

    // 色相跟频谱质心走：低沉的击打偏暖、明亮的偏冷。
    // 慢慢挪，不然每帧跳色会很吵。
    {
        const float cen = spectralCentroid(f.bands);
        if (cen > 40.0f) {
            // 100Hz→0.05（红橙） … 6kHz→0.62（蓝）
            float h = 0.05f + 0.57f * clamp01(log2f(cen / 100.0f) / 5.9f);
            st.hue += 0.06f * (h - st.hue);
        }
    }

    // 色度与调性色相的平滑。系数同样按物理时间算 —— dt_ms 就是 presetHopMs，
    // 各档差 8 倍，写死成每帧固定值会让换档时观感突变。
    const float ac = envCoeff(kFxChromaTauMs, dt_ms);
    const float ah = envCoeff(kFxKeyHueTauMs, dt_ms);

    // ── 彩色流动的相位 ──
    //
    // 速度不是常数，也不是简单地跟 BPM 成正比：
    //   基础周期  = flow_beats 拍（锁不上节拍时退回固定毫秒）
    //   氛围      越躁越快
    //   能量走向  **可以为负** —— 渐强时往上流、收尾时往下流。
    //
    // 走向这一项刻意写成连续的（0.35 + trend），不是按符号切换方向：
    // trend 在零附近来回时，按符号切会让整条光带反复抽搐。
    const float beat_ms = (f.beat_locked && f.bpm > 1.0f) ? (60000.0f / f.bpm) : 0.0f;
    const float cycle_ms = (beat_ms > 0.0f) ? beat_ms * c.flow_beats : c.flow_free_ms;
    float mo = isfinite(f.mood) ? f.mood : 0.0f;
    if (mo < 0.0f) mo = 0.0f; if (mo > 1.0f) mo = 1.0f;
    float tr = isfinite(f.energy_trend) ? f.energy_trend : 0.0f;
    if (tr < -1.0f) tr = -1.0f; if (tr > 1.0f) tr = 1.0f;
    if (cycle_ms > 1.0f) {
        st.flow += (dt_ms / cycle_ms) * (0.6f + 0.8f * mo) * (0.35f + tr);
        st.flow -= floorf(st.flow);
    }

    // 换段的余波。section_change 只有一帧为真，直接画的话眨眼就没了。
    const float as = envCoeff(c.sect_tau_ms, dt_ms);
    // **换段泛白要带电平门。** 没有它的话，音乐停下那一刻若正好判出换段，
    // 灯会在静音里泛一层白 —— 而「静音必须熄灭」是硬不变量。
    // 这是把夹具补成「头一帧带事件标志」之后才暴露的，之前一直藏着。
    if (f.section_change && f.rms_fast > 0.003f) st.sect = 1.0f;
    else                  st.sect += as * (0.0f - st.sect);

    // ── 旋律线 ──
    // 位置按**半音**映射，不是按 Hz：Hz 线性映射会把低八度挤成一小段。
    const float at = envCoeff(c.trail_tau_ms,  dt_ms);
    const float ag = envCoeff(c.f0_glide_ms,   dt_ms);
    if (f.f0_voiced && f.f0_hz > 0.0f) {
        const float span = hzToSemi(1000.0f, 80.0f);            // 搜索范围的总跨度
        float u = hzToSemi(f.f0_hz, 80.0f) / (span > 1.0f ? span : 1.0f);
        if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
        if (!st.has_f0) { st.f0_u = u; st.has_f0 = true; }
        else            { st.f0_u += ag * (u - st.f0_u); }
    }
    for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i)
        st.trail[i] += at * (0.0f - st.trail[i]);
    if (f.f0_voiced) {
        const int c0 = (int)(st.f0_u * (float)(LEDS_PER_TUBE - 1) + 0.5f);
        float lvl = f.rms_fast * kFxLevelScale;
        if (!isfinite(lvl) || lvl < 0.0f) lvl = 0.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        const float v = lvl * (0.4f + 0.6f * f.f0_conf);
        // 升余弦光斑而不是高斯：高斯的尾巴掉得太快，±4 处只剩 1.8%，
        // 量化到 8 位就是黑的 —— 一个持续音在 48 颗管子上只亮 5 颗，
        // 看着像坏了。升余弦在 ±4 处还有 9.5%，halo 是看得见的。
        constexpr int kSpread = 4;
        for (int d = -kSpread; d <= kSpread; ++d) {
            const int i = c0 + d;
            if (i < 0 || i >= (int)LEDS_PER_TUBE) continue;
            const float w = v * 0.5f * (1.0f + cosf(3.14159265f * (float)d / (float)(kSpread + 1)));
            if (w > st.trail[i]) st.trail[i] = w;
        }
    }

    // 色度平滑。单帧色度抖得厉害，直接画会闪成一片。
    for (int i = 0; i < kChroma; ++i) {
        float v = f.chroma[i];
        if (!isfinite(v) || v < 0.0f) v = 0.0f;
        st.chroma[i] += ac * (v - st.chroma[i]);
    }

    // 调性底色。十二个音级铺满色相环 —— 音乐上相邻的调（五度圈）在这里
    // 未必相邻，但对眼睛来说「换调了」这件事看得出来就够了。
    // 大调偏暖（往红黄挪）、小调偏冷（往蓝紫挪），这是最直白的明暗对应。
    if (f.key_root >= 0 && f.key_conf > 0.15f) {
        float h = (float)f.key_root / (float)kChroma;
        h += f.key_is_major ? -0.06f : 0.10f;
        h -= floorf(h);
        // 走最短的一段弧，否则从 0.95 挪到 0.05 会绕整整一圈
        float d = h - st.key_hue;
        if (d > 0.5f) d -= 1.0f; else if (d < -0.5f) d += 1.0f;
        st.key_hue += ah * d;
        st.key_hue -= floorf(st.key_hue);
    }

    // 和声变化的余波：换和弦时冲一下，然后按拍衰减
    {
        float hm = f.harmony_move * 4.0f;
        if (!isfinite(hm) || hm < 0.0f) hm = 0.0f;
        if (hm > 1.0f) hm = 1.0f;
        if (hm > st.flash) st.flash = hm;
        else               st.flash += a * (hm - st.flash);
    }

    // ── 以下是后加的十三个效果的状态 ──────────────────────
    // 上面已经算过 beat_ms，但那份在没锁上时是 0（彩色流动另有退路）；
    // 这里要一个永远可用的拍长。
    const float beat_any = (f.beat_locked && f.bpm > 1.0f) ? (60000.0f / f.bpm) : 500.0f;
    auto clamp01 = [](float v) {
        if (!isfinite(v) || v < 0.0f) return 0.0f;
        return v > 1.0f ? 1.0f : v;
    };
    const float lvl = clamp01(f.rms_fast * kFxLevelScale);

    // 强拍绽放 / 弱拍涟漪。**强拍的包络比弱拍长四倍**，画面上才有主次；
    // 都用同一条包络的话，小节结构就丢了 —— 那正是冲击柱已经在做的事。
    {
        const float ab = envCoeff(beat_any * c.bloom_beats, dt_ms);
        const float ar = envCoeff(beat_any * c.ripple_beats, dt_ms);
        if (f.downbeat && lvl > 0.0f) st.bloom = lvl;
        else st.bloom += ab * (0.0f - st.bloom);
        if (f.onset && !f.downbeat && lvl > 0.0f) { if (lvl > st.ripple) st.ripple = lvl; }
        else st.ripple += ar * (0.0f - st.ripple);
    }

    // 小节阶梯：换小节就换一次色。用 bar_index 的变化沿，不是 bar_pos==0 ——
    // 后者在没锁上小节时会乱跳。
    if (f.bar_index != st.last_bar) {
        st.last_bar = f.bar_index;
        st.ladder_hue += 0.137f;                  // 黄金比附近，连着几小节不会撞色
        st.ladder_hue -= floorf(st.ladder_hue);
    }

    // 鼓组：打击路的低四段是底鼓，高六段是军鼓/镲。
    // 用 bands_p 而不是 bands —— 混合谱里贝斯与人声会盖过鼓。
    {
        const float ad = envCoeff(c.drum_tau_ms, dt_ms);
        float k = 0.0f, sn = 0.0f;
        for (int i = 0; i < 4; ++i) k += f.bands_p[i];
        for (int i = NUM_BANDS - 6; i < NUM_BANDS; ++i) sn += f.bands_p[i];
        k = clamp01(k * kFxBandScale / 4.0f);
        sn = clamp01(sn * kFxBandScale / 6.0f);
        if (k > st.kick)   st.kick = k;   else st.kick  += ad * (k - st.kick);
        if (sn > st.snare) st.snare = sn; else st.snare += ad * (sn - st.snare);
    }

    // 音高彗星：位置跟 f0，拖尾比旋律线长得多，所以滑音能看出轨迹。
    {
        const float at = envCoeff(c.comet_tail_ms, dt_ms);
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i)
            st.comet_trail[i] += at * (0.0f - st.comet_trail[i]);
        if (f.f0_voiced && f.f0_hz > 20.0f) {
            // 与旋律线同一套映射：log2 音高线性铺在管上，两个八度
            const float u = clamp01((log2f(f.f0_hz / 110.0f)) / 3.0f);
            if (!st.has_comet) { st.comet_u = u; st.has_comet = true; }
            st.comet_u += envCoeff(c.f0_glide_ms, dt_ms) * (u - st.comet_u);
            const int n = (int)(st.comet_u * (LEDS_PER_TUBE - 1) + 0.5f);
            if (n >= 0 && n < LEDS_PER_TUBE) st.comet_trail[n] = 1.0f;
        }
    }

    // 和声推移：色相在一个乐句的尺度上跟着和声走，换和弦时泛一层波。
    {
        const float ahm = envCoeff(c.harm_tau_ms, dt_ms);
        const float target = (f.key_root >= 0) ? ((float)f.key_root / 12.0f) : st.harm_hue;
        float d = target - st.harm_hue;
        if (d > 0.5f) d -= 1.0f; else if (d < -0.5f) d += 1.0f;
        st.harm_hue += ahm * d;
        st.harm_hue -= floorf(st.harm_hue);
        const float hm = clamp01(f.harmony_move * 5.0f);
        if (hm > st.harm_wave) st.harm_wave = hm;
        else st.harm_wave += envCoeff(1200.0f, dt_ms) * (0.0f - st.harm_wave);
    }

    // 人声：再平滑一层。lamp_vocal 已经做过时间常数，但那是**判据**用的；
    // 画面还要更稳一点，否则一句里的换气会让光晕一抖一抖。
    {
        st.voc += envCoeff(c.voc_tau_ms, dt_ms) * (clamp01(f.vocal) - st.voc);
        if (f.f0_voiced && f.f0_hz > 20.0f) {
            const float u = clamp01(log2f(f.f0_hz / 110.0f) / 3.0f);
            st.voc_u += envCoeff(240.0f, dt_ms) * (u - st.voc_u);
        }
        // 与强拍绽放同一条规矩：没有声音就不起包络。
        // 真实管线里静音时 vocal_onset 不可能为真，但夹具会喂这种组合，
        // 而「静音要熄灭」是硬不变量 —— 在这里挡住比在渲染里挡干净。
        if (f.vocal_onset && lvl > 0.0f) st.lyric = 1.0f;
        else st.lyric += envCoeff(c.lyric_tau_ms, dt_ms) * (0.0f - st.lyric);
    }

    // 段落潮汐：换段时起一次扫过，扫完停住。
    if (f.section_change) { st.tide = 1.0f; st.tide_hue += 0.31f; st.tide_hue -= floorf(st.tide_hue); }
    else if (st.tide > 0.0f) {
        st.tide -= dt_ms / c.tide_ms;
        if (st.tide < 0.0f) st.tide = 0.0f;
    }

    // 极光：三条极慢的相位。速度只被 energy_trend 轻微推动 ——
    // 这是唯一一个**不追随瞬时声音**的效果，它画的是几十秒的走向。
    {
        const float sp = 1.0f + 0.6f * (isfinite(f.energy_trend) ? f.energy_trend : 0.0f);
        const float step = dt_ms / c.aurora_ms * sp;
        const float mul[3] = {1.0f, 0.61f, 1.37f};
        for (int i = 0; i < 3; ++i) {
            st.aur[i] += step * mul[i];
            st.aur[i] -= floorf(st.aur[i]);
        }
    }

    // 光点：锁上节拍时直接跟相位走，一拍跑完一趟；没锁上就匀速漂
    if (f.beat_locked) { st.runner = f.phase; st.has_phase = true; }
    else {
        st.runner += dt_ms / 1400.0f;
        if (st.runner >= 1.0f) st.runner -= 1.0f;
    }
    st.prev_phase = f.phase;
}

// 整管冲击：一次击打照亮整根管子，然后按拍周期掉落。
//
// **与「频段柱」的区别不只是加了拖尾** —— 那个画的是频谱形状（16 根柱子
// 各有各的高度），这个画的是「这一下打得有多重」：一个标量铺满整管。
// 第一版做成了 16 段各带拖尾，布局和配色都跟频段柱一样，等于同一个东西
// 加了个尾巴，没有存在的理由。
//
// **与「拍点脉冲」的区别**：那个纯由相位驱动，每拍形状一模一样、跟音量无关；
// 这个的峰值高度由实际击打强度决定 —— 弱拍就弱、重拍就亮。
//
// 冲击的强弱同时体现在两处：亮度，以及**点亮的长度**（从管中央向两端扩散）。
// 只用亮度的话弱击打几乎看不见，加上长度才有「冲击波」的样子。
inline void fxBarImpact(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    const float v = perceptual(st.impact);
    if (v <= 0.0f) return;
    const float reach = 0.12f + 0.88f * st.impact;   // 冲击越强，铺得越开
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const float d = fabsf(u - 0.5f) * 2.0f;  // 到管中央的距离 [0,1]
            if (d > reach) { out[mapPixel(g, (Side)s, u)] = Rgb{0, 0, 0}; continue; }
            // 边缘柔化，免得看起来像一段硬邦邦的色块
            const float edge = 1.0f - (d / reach) * (d / reach) * 0.55f;
            out[mapPixel(g, (Side)s, u)] = hsv(st.hue, 0.85f, v * edge);
        }
    (void)f;
}

// 拍点光点：每拍从管底发到管顶，带拖尾。速度直接由 BPM 决定。
inline void fxBeatRunner(const FxState &st, const FxConfig &c, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    const float hue = f.beat_locked ? fmodf(f.bpm / 240.0f, 1.0f) : 0.55f;
    // 没有信号就彻底熄灭。光点本身带 25% 的底光（否则安静段落里看不见节拍），
    // 但那个底光不该在真正的静音里还亮着 —— 那看起来像灯没关干净。
    if (!(f.rms_fast > 0.003f)) return;
    const float amp = perceptual(f.rms_fast * kFxLevelScale);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            float d = st.runner - u;              // 拖尾只朝身后
            if (d < 0.0f) d += 1.0f;
            const float tail = (d < c.runner_tail) ? (1.0f - d / c.runner_tail) : 0.0f;
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.8f, perceptual(tail) * (0.25f + 0.75f * amp));
        }
}

// 低频从底往上、高频从顶往下，在中间相遇。看的是频谱重心怎么移动。
// 同样乘 dynamics，理由见 fxSpectrumBars。
inline void fxSplitBands(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    float lo = 0.0f, hi = 0.0f;
    for (int i = 0; i < 5; ++i)             lo += st.bar[i];
    for (int i = NUM_BANDS - 6; i < NUM_BANDS; ++i) hi += st.bar[i];
    lo /= 5.0f; hi /= 6.0f;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            Rgb c{0, 0, 0};
            if (u < lo * 0.5f)              c = hsv(0.02f, 0.95f, perceptual(lo));
            else if (u > 1.0f - hi * 0.5f)  c = hsv(0.55f, 0.9f,  perceptual(hi));
            out[mapPixel(g, (Side)s, u)] = c;
        }
    (void)f;
}

// 调性染色：整管的底色由**调**决定，亮度由能量，换和弦时泛起一层波纹。
//
// 这是第一个真正吃音高的效果。前面六个换成纯节拍器画面不会有本质区别 ——
// 它们只知道「有多响、鼓点在哪」。这个能分出 C 大调和 f 小调。
inline void fxKeyWash(const FxState &st, const AudioFrame &f,
                      const Geometry &g, Rgb *out) {
    const float lvl = perceptual(f.rms_fast * kFxLevelScale);
    if (lvl <= 0.0f) return;

    // 调性不明时**退回质心色相**，而不是单纯把饱和度压掉。
    //
    // 失真吉他是典型场景：power chord 没有三度、泛音又密，key_conf 一直很低。
    // 原来的做法（只降饱和）在整首摇滚上都是一片灰白 —— 判断是对的
    // （它诚实地说「我不确定」），但视觉上等于什么都没表达。
    //
    // st.hue 本来就跟着频谱质心走，那是个在失真下依然稳定、依然与音乐相关的量。
    // 按 key_conf 在两者之间插值：调明确时听调的，调不明确时听音色的。
    const float kc  = clamp01(f.key_conf);
    // 色相插值走**最短弧**，直接线性插会在 0/1 接缝处绕一整圈
    float d = st.key_hue - st.hue;
    d -= floorf(d + 0.5f);
    const float base_hue = st.hue + kc * d;
    // 饱和度不再随 key_conf 塌到底，只是略降 —— 因为现在退路本身也是有意义的颜色
    const float sat = 0.62f + 0.28f * kc;

    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            // 和声变化的波纹沿管跑一趟
            const float w = st.flash * expf(-fabsf(u - st.flash) * 6.0f);
            const float hue = base_hue + 0.08f * w;
            out[mapPixel(g, (Side)s, u)] = hsv(hue, sat, clamp01(lvl + 0.45f * w));
        }
}

// 音级环：十二个音级沿管排开，亮度是各自的能量。
//
// 与「频段柱」的区别在横轴：那个是**频率**（43Hz 到 9kHz 一路铺开），
// 这个是**音级**（C 到 B，八度折叠）。同一个音在任何八度都点亮同一格 ——
// 于是旋律线在这里是横向移动，而在频段柱上只是某几格忽明忽暗。
inline void fxChromaRing(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const int   pc = (int)(u * (kChroma - 1) + 0.5f);
            const float e  = st.chroma[pc];
            // 主音那一格加一圈白边，让调中心看得出来
            const bool root = (f.key_root == pc && f.key_conf > 0.3f);
            const float v = perceptual(e) * (root ? 1.0f : 0.82f);
            Rgb c = hsv((float)pc / kChroma, root ? 0.55f : 0.9f, v);
            out[mapPixel(g, (Side)s, u)] = c;
        }
}

// 彩色流动：沿管连续变化的色相带，整体在流动。
//
// 这个效果是给**外壳透光**准备的 —— 真灯管的乳白外壳会把相邻灯珠糊成
// 连续过渡，逐颗跳变的效果在那上面会损失掉一半信息，而连续色带正好相反。
//
// 三个音乐特征各管一件事，互不重叠：
//   氛围 mood  → 色相跨度（静：几乎单色的渐变；躁：整条彩虹）与饱和度
//   能量走向   → 流动方向与快慢（在 fxAdvance 里）
//   换段       → 一次全管泛白，像翻页
inline void fxColorFlow(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    float mo = isfinite(f.mood) ? f.mood : 0.0f;
    if (mo < 0.0f) mo = 0.0f; if (mo > 1.0f) mo = 1.0f;
    const float span = 0.10f + 0.80f * mo;      // 色相跨度
    const float sat  = 0.45f + 0.50f * mo;

    float lvl = f.rms_fast * kFxLevelScale;
    if (!isfinite(lvl) || lvl < 0.0f) lvl = 0.0f;
    if (lvl > 1.0f) lvl = 1.0f;

    // 底色跟调性走，没调性时用色相流动自己的相位
    const float base = (f.key_root >= 0 && f.key_conf > 0.15f)
                     ? (float)f.key_root / (float)kChroma : st.key_hue;

    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const float h = base + st.flow + span * u;
            // 亮度必须**正比于**电平，不能带常数下限 —— 带下限的话静音时
            // 整条管还亮着 0.30，「没声音就熄灯」这条验收直接不过。
            const float v = lvl * (0.5f + 0.5f * lvl) * (0.85f + 0.15f * sinf(6.28318f * u));
            out[mapPixel(g, (Side)s, u)] =
                hsv(h, sat * (1.0f - 0.7f * st.sect), v + 0.6f * st.sect * (1.0f - v));
        }
}

// 旋律线：一个光点停在主旋律当前的音高上，走过的地方留下拖影。
//
// 这是唯一消费 f0 的效果。与「音级环」的区别在于**八度**：音级环把
// C3 和 C5 画在同一格，这里它们相距半根管 —— 旋律的起伏看得见。
inline void fxMelodyLine(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    // 色相取当前音高的音级，于是同一个音无论在哪个八度都是同一个颜色
    const int pc = f.f0_voiced ? pitchClassOf(f.f0_hz) : -1;
    const float hue = (pc >= 0) ? (float)pc / (float)kChroma : st.key_hue;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const float e = st.trail[i];
            if (!(e > 0.002f)) continue;
            // 这里**不套 perceptual()**：拖影存的已经是亮度，不是能量。
            // 再平方一次会把 halo 压成黑的，只剩一个孤零零的亮点。
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.85f, e);
        }
}

// ══ 节拍类 ════════════════════════════════════════════════

// 强拍绽放：强拍从管中央向两端铺开，弱拍只在中央点一下。
// 与冲击柱的分别：那个每拍都一样重，这个**只有强拍是大的** —— 画的是小节，不是拍。
inline void fxDownbeatBloom(const FxState &st, const AudioFrame &f,
                            const Geometry &g, Rgb *out) {
    const float b = perceptual(st.bloom), r = perceptual(st.ripple);
    if (b <= 0.0f && r <= 0.0f) return;
    const float reach = 0.15f + 0.85f * st.bloom;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const float d = fabsf(u - 0.5f) * 2.0f;
            float v = 0.0f, hue = st.hue;
            if (d <= reach) v = b * (1.0f - (d / (reach + 1e-6f)) * 0.6f);
            if (d <= 0.18f) {                       // 弱拍只有中央这一小段
                const float rv = r * (1.0f - d / 0.18f) * 0.55f;
                if (rv > v) { v = rv; hue = st.hue + 0.5f; }
            }
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.85f, v);
        }
    (void)f;
}

// 小节阶梯：管子切成 beats_per_bar 段，走到第几拍就点亮到第几段，
// 每过一小节整体换色。**看得见拍号** —— 3/4 与 4/4 一眼能分开。
inline void fxBarLadder(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    int n = f.beats_per_bar;
    if (n < 2) n = 4;
    if (n > 8) n = 8;
    const int pos = (f.bar_pos >= 0 && f.bar_pos < n) ? f.bar_pos : 0;
    const float lvl = perceptual(f.rms_fast * kFxLevelScale);
    if (lvl <= 0.0f) return;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const int seg = (int)(u * n);
            Rgb c{0, 0, 0};
            if (seg <= pos) {
                // 已经走过的段留一层暗底，当前段全亮 —— 这样能看出走到哪
                const float v = (seg == pos) ? lvl : lvl * 0.22f;
                c = hsv(st.ladder_hue + 0.06f * seg, 0.8f, v);
            }
            out[mapPixel(g, (Side)s, u)] = c;
        }
}

// 鼓组分离：底鼓从底往上顶，军鼓/镲从顶往下压。
// 吃的是**打击路**，所以贝斯和人声不会混进来 —— 这是它与高低分离的分别。
inline void fxKickSnare(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    const float k = perceptual(st.kick), sn = perceptual(st.snare);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            Rgb c{0, 0, 0};
            if (u < st.kick * 0.55f)             c = hsv(0.99f, 0.95f, k);        // 底鼓：深红
            else if (u > 1.0f - st.snare * 0.55f) c = hsv(0.13f, 0.55f, sn);       // 军鼓/镲：米黄
            out[mapPixel(g, (Side)s, u)] = c;
        }
    (void)f;
}

// ══ 旋律类 ════════════════════════════════════════════════

// 音高彗星：一颗亮点跟着基频跑，身后拖一条长尾。
// 与旋律线的分别：那个拖影短、看的是当前音高；这个尾巴长得多，
// 看的是**旋律的轨迹** —— 一段滑音会画出一条连续的线。
inline void fxPitchComet(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    // **彗头要有一圈辉光，不能只点一颗灯珠。** 音高不动的时候拖尾也不动，
    // 单像素在磨砂外壳上就是一个点，看不出是彗星 —— 初稿这么写，
    // 「每个效果都要画得出东西」那条不变量当场报全黑（96 颗里只亮 2 颗）。
    const float head_w = 3.5f / (float)(LEDS_PER_TUBE - 1);   // 半宽约 3 颗
    const bool  lit = f.f0_voiced && f.f0_hz > 20.0f;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            float t = st.comet_trail[i];
            if (lit) {                                        // 叠上彗头
                const float d = fabsf(u - st.comet_u) / head_w;
                const float head = (d < 1.0f) ? (1.0f - d * d) : 0.0f;
                if (head > t) t = head;
            }
            if (t <= 0.001f) { out[mapPixel(g, (Side)s, u)] = Rgb{0, 0, 0}; continue; }
            // 尾巴越旧越偏冷：颜色本身就编码了「多久以前经过这里」
            const float hue = st.key_hue + 0.18f * (1.0f - t);
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.75f + 0.25f * t, perceptual(t));
        }
}

// 和声推移：整管一条随和声缓慢移动的渐变，换和弦时从两端涌进一层波。
// 时间尺度是**乐句**，不是拍 —— 这是它和调性染色的分别（那个只在换调时动）。
inline void fxHarmonyShift(const FxState &st, const AudioFrame &f,
                           const Geometry &g, Rgb *out) {
    const float lvl = perceptual(f.rms_fast * kFxLevelScale);
    if (lvl <= 0.0f) return;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const float wave = st.harm_wave * (1.0f - fabsf(u - 0.5f) * 2.0f);
            out[mapPixel(g, (Side)s, u)] =
                hsv(st.harm_hue + 0.12f * u, 0.7f, lvl * (0.55f + 0.45f * wave));
        }
}

// ══ 人声 / 主旋律类 ═══════════════════════════════════════

// 人声光晕：人声在时，管上按基频高低的位置浮起一团柔光。
// 人声越强，光晕越宽越亮；器乐段落里整管暗下去。
inline void fxVocalHalo(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    // **有声音就得看得见一点东西。** 原来写的是 voc<=0.01 直接 return，
    // 于是人声度低的素材上整根管全黑 —— 用户会以为灯效坏了，而不是
    // 「这段没人声」。与 fxBeatRunner 的 25% 底光同一条规矩：
    // 真静音熄灭，有声音就留一个能看出「它在待命」的最小光点。
    if (f.gated || !(f.rms_fast > 0.003f)) return;
    const float lvl = 0.10f + 0.90f * st.voc;        // 待机 10%
    const float w = 0.06f + 0.32f * st.voc;          // 光晕半宽
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            const float d = fabsf(u - st.voc_u) / w;
            const float v = (d < 1.0f) ? (1.0f - d * d) : 0.0f;   // 抛物线，边缘柔
            out[mapPixel(g, (Side)s, u)] = hsv(st.key_hue + 0.08f, 0.45f, perceptual(v * lvl));
        }
}

// 气息呼吸：整管随人声存在度呼吸。**没有位置信息，只有明暗** ——
// 想要的就是「有人在唱的时候房间亮一点」这种最不打扰的反应。
inline void fxVocalBreath(const FxState &st, const AudioFrame &f,
                          const Geometry &g, Rgb *out) {
    // 底光是为了「器乐段落里别看着像灯灭了」，但**真静音里亮着才像灯坏了**。
    // 与 fxBeatRunner 同一条规矩。
    if (f.gated || !(f.rms_fast > 0.003f)) return;
    const float base = 0.06f;                        // 器乐段落留一点底光，不然像灯坏了
    const float v = base + (1.0f - base) * perceptual(st.voc);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            // 两端略暗，看起来像一根发光的柱子而不是一条灯带
            const float shade = 1.0f - 0.25f * fabsf(u - 0.5f) * 2.0f;
            out[mapPixel(g, (Side)s, u)] = hsv(st.key_hue + 0.05f, 0.35f, v * shade);
        }
    (void)f;
}

// 共振峰带：只画 300–3000Hz 那几段的**谐波**能量，铺满整管。
// 人声可懂度就在这一带，所以唱起来的时候这条带子会明显活起来，
// 而底鼓与镲片几乎不动它。
inline void fxFormantRibbon(const FxState &st, const AudioFrame &f,
                            const Geometry &g, Rgb *out) {
    // 落在 [300,3000] 内的段：按 lamp_bands 的边界算，不写死段号
    int lo = -1, hi = -1;
    for (int i = 0; i < NUM_BANDS; ++i) {
        const float cf = bandCenterHz(i);
        if (cf >= 300.0f && cf <= 3000.0f) { if (lo < 0) lo = i; hi = i; }
    }
    if (lo < 0) return;
    const int n = hi - lo + 1;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            int b = lo + (int)(u * n);
            if (b > hi) b = hi;
            float e = f.bands_h[b] * kFxBandScale;
            if (!isfinite(e) || e < 0.0f) e = 0.0f;
            out[mapPixel(g, (Side)s, u)] =
                hsv(0.08f + 0.34f * (float)(b - lo) / (float)n, 0.8f, perceptual(e));
        }
    (void)st;
}

// 人声伴奏分管：左管画谐波路（人声与旋律乐器），右管画打击路（鼓组）。
// **两根管各说一件事** —— 这是这台灯有两根管才做得到的效果。
inline void fxDuetSplit(const FxState &st, const AudioFrame &f,
                        const Geometry &g, Rgb *out) {
    for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
        const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
        const int b = (int)(u * NUM_BANDS) < NUM_BANDS ? (int)(u * NUM_BANDS) : NUM_BANDS - 1;
        float eh = f.bands_h[b] * kFxBandScale, ep = f.bands_p[b] * kFxBandScale;
        if (!isfinite(eh) || eh < 0.0f) eh = 0.0f;
        if (!isfinite(ep) || ep < 0.0f) ep = 0.0f;
        // 人声那根随人声存在度提亮，让「谁在唱」看得出来
        out[mapPixel(g, SIDE_L, u)] =
            hsv(st.key_hue + 0.06f, 0.55f, perceptual(eh) * (0.55f + 0.45f * st.voc));
        out[mapPixel(g, SIDE_R, u)] = hsv(0.03f, 0.9f, perceptual(ep));
    }
}

// 唱句脉冲：每进来一句人声，整管闪一下再退。**句与句之间是暗的** ——
// 它画的是乐句的起点，不是人声的持续。
inline void fxLyricPulse(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    // 同 fxVocalHalo：句与句之间要暗，但不能暗到看不出效果在跑。
    if (f.gated || !(f.rms_fast > 0.003f)) return;
    // 底光加在 perceptual **之后** —— 加在之前会被平方压没（0.09²=0.008，
    // 折算成 RGB 只有 2，看不见）。
    const float v = 0.10f + 0.90f * perceptual(st.lyric);
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            // 从人声所在位置向两端散开
            const float d = fabsf(u - st.voc_u);
            const float spread = 0.25f + 0.75f * st.lyric;
            const float k = (d < spread) ? (1.0f - d / spread) : 0.0f;
            out[mapPixel(g, (Side)s, u)] = hsv(st.key_hue + 0.42f, 0.6f, v * k);
        }
}

// ══ 氛围类 ════════════════════════════════════════════════

// 段落潮汐：换段时一道色从一端扫到另一端，扫完停在新色上。
// 段内几乎不动 —— 它标记的是**结构**，不是声音。
inline void fxSectionTide(const FxState &st, const AudioFrame &f,
                          const Geometry &g, Rgb *out) {
    if (f.gated || !(f.rms_fast > 0.003f)) return;   // 理由同 fxVocalBreath
    const float lvl = 0.12f + 0.88f * perceptual(f.rms_fast * kFxLevelScale);
    const float front = 1.0f - st.tide;              // 潮头位置 0→1
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            // 潮头之后是新色，之前是旧色；潮头本身亮一圈
            const float hue = (u <= front) ? st.tide_hue : st.tide_hue - 0.31f;
            const float edge = 1.0f - fabsf(u - front) * 6.0f;
            const float boost = (st.tide > 0.0f && edge > 0.0f) ? edge : 0.0f;
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.65f, lvl * (0.7f + 0.3f * boost) + 0.3f * boost);
        }
}

// 情绪渐变：整管一条冷↔暖的渐变，位置由 mood 决定、亮度由 dynamics 决定。
// **几乎不动。** 这是给「放着当氛围灯」用的 —— 音乐一安静它就沉下去，
// 躁起来才偏暖变亮。
inline void fxMoodGradient(const FxState &st, const AudioFrame &f,
                           const Geometry &g, Rgb *out) {
    if (f.gated || !(f.rms_fast > 0.003f)) return;   // 理由同 fxVocalBreath
    float m = f.mood;
    if (!isfinite(m)) m = 0.5f;
    if (m < 0.0f) m = 0.0f; if (m > 1.0f) m = 1.0f;
    float d = f.dynamics;
    if (!isfinite(d) || d < 0.0f) d = 0.0f; if (d > 1.0f) d = 1.0f;
    const float base = 0.58f - 0.55f * m;            // 静=青蓝(0.58) 躁=红(0.03)
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            out[mapPixel(g, (Side)s, u)] =
                hsv(base + 0.10f * u, 0.6f + 0.3f * m, 0.10f + 0.75f * d);
        }
    (void)st;
}

// 极光：三条极慢的波在管上叠加，**不追随任何瞬时声音**，
// 只被几十秒尺度的能量走向轻微推快或推慢。
// 这是唯一一个「音乐停了也还好看」的效果 —— 桌面灯大部分时间需要的正是这个。
inline void fxSlowAurora(const FxState &st, const AudioFrame &f,
                         const Geometry &g, Rgb *out) {
    if (f.gated || !(f.rms_fast > 0.003f)) return;   // 理由同 fxVocalBreath
    float m = f.mood;
    if (!isfinite(m)) m = 0.5f;
    if (m < 0.0f) m = 0.0f; if (m > 1.0f) m = 1.0f;
    for (int s = 0; s < 2; ++s)
        for (uint16_t i = 0; i < LEDS_PER_TUBE; ++i) {
            const float u = (float)i / (float)(LEDS_PER_TUBE - 1);
            // 三条不同周期的正弦叠加：不会看出重复的花样
            float v = 0.0f, hsum = 0.0f;
            const float k[3] = {1.0f, 2.3f, 3.7f};
            for (int q = 0; q < 3; ++q) {
                const float w = 0.5f + 0.5f * sinf(6.2831853f * (st.aur[q] + k[q] * u));
                v += w; hsum += w * (float)q / 3.0f;
            }
            v /= 3.0f;
            const float hue = 0.45f - 0.25f * m + 0.30f * (hsum / (v * 3.0f + 1e-6f));
            out[mapPixel(g, (Side)s, u)] = hsv(hue, 0.55f + 0.2f * m, 0.10f + 0.55f * v * v);
        }
}

// 统一入口。渲染后**统一施加白平衡** —— 各效果自己不碰它，
// 否则总有一个会忘，而忘了的那个偏色，看起来像效果设计得难看。
inline void fxRender(FxId id, FxState &st, const FxConfig &c, const AudioFrame &f,
                     const Geometry &g, bool white_balance, float dt_ms, Rgb *out) {
    fxAdvance(st, c, f, dt_ms);      // 状态先推进，无状态的效果不受影响
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i) out[i] = Rgb{0, 0, 0};
    switch (id) {
        case FX_BEAT_PULSE:  fxBeatPulse(f, g, out);         break;
        case FX_LEVEL_SWEEP: fxLevelSweep(f, g, out);        break;
        case FX_BAR_IMPACT:  fxBarImpact(st, f, g, out);     break;
        case FX_BEAT_RUNNER: fxBeatRunner(st, c, f, g, out); break;
        case FX_SPLIT_BANDS: fxSplitBands(st, f, g, out);    break;
        case FX_KEY_WASH:    fxKeyWash(st, f, g, out);       break;
        case FX_CHROMA_RING: fxChromaRing(st, f, g, out);    break;
        case FX_COLOR_FLOW:  fxColorFlow(st, f, g, out);     break;
        case FX_MELODY_LINE: fxMelodyLine(st, f, g, out);    break;
        case FX_DOWNBEAT_BLOOM: fxDownbeatBloom(st, f, g, out);  break;
        case FX_BAR_LADDER:     fxBarLadder(st, f, g, out);      break;
        case FX_KICK_SNARE:     fxKickSnare(st, f, g, out);      break;
        case FX_PITCH_COMET:    fxPitchComet(st, f, g, out);     break;
        case FX_HARMONY_SHIFT:  fxHarmonyShift(st, f, g, out);   break;
        case FX_VOCAL_HALO:     fxVocalHalo(st, f, g, out);      break;
        case FX_VOCAL_BREATH:   fxVocalBreath(st, f, g, out);    break;
        case FX_FORMANT_RIBBON: fxFormantRibbon(st, f, g, out);  break;
        case FX_DUET_SPLIT:     fxDuetSplit(st, f, g, out);      break;
        case FX_LYRIC_PULSE:    fxLyricPulse(st, f, g, out);     break;
        case FX_SECTION_TIDE:   fxSectionTide(st, f, g, out);    break;
        case FX_MOOD_GRADIENT:  fxMoodGradient(st, f, g, out);   break;
        case FX_SLOW_AURORA:    fxSlowAurora(st, f, g, out);     break;
        case FX_SPECTRUM_BARS:
        default:             fxSpectrumBars(f, g, out);      break;
    }
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        out[i] = applyWhiteBalance(out[i], white_balance);
}

} // namespace lamp
