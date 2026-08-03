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
    FX_COUNT         = 10,
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
        case FX_SPECTRUM_BARS:
        default:             return "spectrum-bars";
    }
}

// 有时间演化的效果需要状态。前三个效果是无状态的纯函数（同一帧输入永远画出
// 同一幅图），冲击/拖尾这类做不到 —— 它们的当前亮度取决于之前发生过什么。
struct FxState {
    float bar[NUM_BANDS] = {0};   // 每段的包络（split-bands 用）
    float impact = 0.0f;          // **整管**的冲击包络
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
};

struct FxConfig {
    // 冲击的衰减长度 = 拍周期 × 这个系数。**不能写成固定毫秒**：
    // 174BPM 一拍只有 345ms，而 90BPM 有 667ms —— 固定值要么在快歌里糊成一片，
    // 要么在慢歌里早早熄灭。0.55 拍意味着落到 1/e 时下一拍还没到。
    float decay_beats   = 0.55f;
    float decay_free_ms = 260.0f;   // 没锁上节拍时的退路
    float runner_tail   = 0.28f;    // 光点拖尾长度，占管长的比例

    // 彩色流动：一个完整色相环走过多少拍。四拍一圈，慢歌快歌都不至于晕。
    float flow_beats    = 4.0f;
    float flow_free_ms  = 2400.0f;  // 没锁上节拍时的退路
    float sect_tau_ms   = 1200.0f;  // 换段余波的衰减
    float trail_tau_ms  = 500.0f;   // 旋律线拖影的衰减
    float f0_glide_ms   = 70.0f;    // 旋律线位置的平滑
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
    // 用 rms_fast 而不是 peak：peak 自己就带保持衰减，套两层会拖成一片糊。
    {
        float e = f.rms_fast * kFxLevelScale;
        if (!isfinite(e) || e < 0.0f) e = 0.0f;
        if (e > 1.0f) e = 1.0f;
        if (e > st.impact) st.impact = e;
        else               st.impact += a * (e - st.impact);
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
    if (f.section_change) st.sect = 1.0f;
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
        case FX_SPECTRUM_BARS:
        default:             fxSpectrumBars(f, g, out);      break;
    }
    for (uint16_t i = 0; i < TOTAL_LEDS; ++i)
        out[i] = applyWhiteBalance(out[i], white_balance);
}

} // namespace lamp
