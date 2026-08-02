// usermods/lamp/lamp_audio_source.h
//
// 三路音频源的仲裁（设计文档 §3.1）。
//
// 本文件**不认识硬件**：三个驱动各自把「有没有流 / 响不响 / 插没插」上报为布尔量，
// 仲裁器只吃这些布尔量加一个注入的单调时钟，输出「用哪一路 + 交叉淡入权重」。
// 时钟从外面传进来而不是内部调 millis()，是为了让全部时序行为可在主机上测。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>

namespace lamp {

// 数值即优先级，越小越高。SRC_NONE 取 255，于是「优先级更低」直接写成数值更大。
enum SourceId : uint8_t {
    SRC_SNAPCAST = 0,
    SRC_LINE     = 1,
    SRC_MIC      = 2,
    SRC_NONE     = 255,
};

// 「插着但无信号」的三档处理（§3.1）。
enum LineSilenceMode : uint8_t {
    LINE_ALWAYS      = 0,  // 总是用线路输入：插着就算活跃，不看信号
    LINE_FALLBACK    = 1,  // 无信号超时后退回麦克风（默认）
    LINE_MANUAL_ONLY = 2,  // 禁用自动：只有手动锁定能选中 3.5mm
};

// 三路驱动每次上报的原始状态。
struct SourceInputs {
    bool snap_streaming = false;  // Snapcast 有活跃流
    bool snap_audible   = false;  // 且非静音
    bool line_detected  = false;  // 3.5mm 已插入（Ring2 被插头套筒短到地）
    bool line_audible   = false;  // 线路信号 RMS 超底噪
    bool mic_ok         = true;   // 麦驱动正常（§6.1：缓冲全零 >3s 由驱动置否）
};

struct ArbiterConfig {
    uint32_t rise_hold_ms    = 1500;   // 升优先级需连续满足这么久才提交
    uint32_t crossfade_ms    = 300;
    uint32_t snap_silence_ms = 5000;   // 有流但静音超过这么久 → 失活
    uint32_t line_silence_ms = 10000;  // 插着但无信号超过这么久 → 失活
    LineSilenceMode line_mode   = LINE_FALLBACK;
    SourceId        manual_lock = SRC_NONE;   // SRC_NONE = 交给自动仲裁
};

// 三路各自的**物理**活跃状态。注意 LINE_MANUAL_ONLY 不体现在这里 ——
// 那一档影响的是目标选择，不是物理事实。
struct SourceActivity {
    bool snap = false;
    bool line = false;
    bool mic  = false;
};

// 仲裁结果。交叉淡入期间 from != to，消费者按 weight 混音。
struct ArbiterOutput {
    SourceId from   = SRC_NONE;   // 淡出源；无过渡时等于 to
    SourceId to     = SRC_NONE;   // 当前 / 淡入源
    float    weight = 1.0f;       // to 的权重 [0,1]，1.0 = 过渡完成
    bool     to_is_active = false;// to 这一路此刻是否真的在出数据
};

class Arbiter {
  public:
    explicit Arbiter(const ArbiterConfig &cfg) : cfg_(cfg) {}

    void setConfig(const ArbiterConfig &cfg) { cfg_ = cfg; }
    const ArbiterConfig &config() const { return cfg_; }

    // 有状态：内部维护两个静音计时器。同一个 now_ms 重复调用是幂等的。
    // 调用方必须把**同一次** millis() 快照传给三路，不要各自取时。
    // now 若倒退哪怕 1ms，无符号差值会回绕成巨大值而立刻判超时 ——
    // 这与「距上次已过 49.7 天」在数值上不可区分。
    SourceActivity evaluateActivity(uint32_t now_ms, const SourceInputs &in);

    // 主入口。每个音频块调一次，now_ms 用 millis()。
    ArbiterOutput update(uint32_t now_ms, const SourceInputs &in);

  private:
    ArbiterConfig cfg_;

    // 静音计时器。用独立的 armed 标志而不是拿某个时刻值当哨兵 ——
    // millis() 的 2^32 个取值全都合法，任何哨兵都会在某一毫秒和真实时刻撞上。
    struct SilenceTimer {
        uint32_t since = 0;
        bool     armed = false;
    };
    SilenceTimer snap_silent_;
    SilenceTimer line_silent_;

    // 无符号差值：now 回绕后 (now - since) 依然是正确的经过时长，
    // 只要间隔小于 2^32 ms（49.7 天）。别改成有符号比较。
    static bool elapsedAtLeast(uint32_t now, uint32_t since, uint32_t ms) {
        return (uint32_t)(now - since) >= ms;
    }

    // 通用的「有信号则重置、无信号则计时」判定。
    bool holdWhileSilent(uint32_t now, bool present, bool audible,
                         uint32_t timeout_ms, SilenceTimer &t) {
        if (!present) { t.armed = false; return false; }
        if (audible)  { t.armed = false; return true;  }
        if (!t.armed) { t.armed = true; t.since = now; }
        return !elapsedAtLeast(now, t.since, timeout_ms);
    }

    SourceId current_     = SRC_NONE;
    SourceId rise_target_ = SRC_NONE;   // 正在计时的升级候选
    uint32_t rise_since_  = 0;

    static bool isActive(const SourceActivity &a, SourceId s) {
        switch (s) {
            case SRC_SNAPCAST: return a.snap;
            case SRC_LINE:     return a.line;
            case SRC_MIC:      return a.mic;
            default:           return false;
        }
    }

    SourceId pickTarget(const SourceActivity &a) const {
        if (cfg_.manual_lock != SRC_NONE) return cfg_.manual_lock;
        if (a.snap) return SRC_SNAPCAST;
        if (a.line && cfg_.line_mode != LINE_MANUAL_ONLY) return SRC_LINE;
        if (a.mic)  return SRC_MIC;
        return SRC_NONE;
    }
};

inline SourceActivity Arbiter::evaluateActivity(uint32_t now_ms, const SourceInputs &in) {
    SourceActivity a;
    a.snap = holdWhileSilent(now_ms, in.snap_streaming, in.snap_audible,
                             cfg_.snap_silence_ms, snap_silent_);
    if (cfg_.line_mode == LINE_ALWAYS) {
        a.line = in.line_detected;                 // 插着就算活跃
        // 清掉计时器不是冗余：配置从 ALWAYS 切回 FALLBACK 时，线路应重获完整宽限。
        // 改配置不该立即引发换源 —— 若在 ALWAYS 期间照常计时，切回瞬间就会掉到 mic。
        line_silent_.armed = false;
    } else {
        a.line = holdWhileSilent(now_ms, in.line_detected, in.line_audible,
                                 cfg_.line_silence_ms, line_silent_);
    }
    a.mic = in.mic_ok;
    return a;
}

inline ArbiterOutput Arbiter::update(uint32_t now_ms, const SourceInputs &in) {
    const SourceActivity act    = evaluateActivity(now_ms, in);
    const SourceId       target = pickTarget(act);

    if (target != current_) {
        // 立即提交的四种情形：
        //   开机（从 SRC_NONE 起步，否则灯要黑 1.5 秒）
        //   当前源已死 —— 1500ms 滞回是为了保护一个**正在工作**的源不被毛刺抢走，
        //     当前源都死了就没什么可保护的。注意这一条不能用 (target > current_)
        //     代替：MIC(2) 挂掉而 LINE(1) 活着时 1 > 2 不成立，会白等 1.5 秒
        //   目标优先级更低 —— 例如配置切到 MANUAL_ONLY 把仍然活着的 LINE 排除掉
        //   手动锁定
        const bool immediate = (current_ == SRC_NONE)
                            || !isActive(act, current_)
                            || (target > current_)
                            || (cfg_.manual_lock != SRC_NONE);
        if (immediate) {
            current_     = target;
            rise_target_ = SRC_NONE;
        } else if (rise_target_ != target) {
            rise_target_ = target;
            rise_since_  = now_ms;
        } else if (elapsedAtLeast(now_ms, rise_since_, cfg_.rise_hold_ms)) {
            current_     = target;
            rise_target_ = SRC_NONE;
        }
    } else {
        rise_target_ = SRC_NONE;      // 候选变回当前源，计时作废
    }

    ArbiterOutput out;
    out.from         = current_;
    out.to           = current_;
    out.weight       = 1.0f;
    out.to_is_active = isActive(act, current_);
    return out;
}

} // namespace lamp
