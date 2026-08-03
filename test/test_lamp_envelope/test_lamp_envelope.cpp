#include <unity.h>
#include <math.h>
#include "lamp_envelope.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// ── 帧 RMS ────────────────────────────────────────────────

void test_rms_of_unit_sine_is_one_over_sqrt2(void) {
    static float x[512];
    for (size_t t = 0; t < 512; ++t)
        x[t] = sinf(6.283185307f * 8.0f * (float)t / 512.0f);   // 整数周期
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.70711f, frameRms(x, 512));
}

void test_rms_of_dc_is_the_dc_value(void) {
    static float x[64];
    for (size_t t = 0; t < 64; ++t) x[t] = -0.25f;
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.25f, frameRms(x, 64));   // RMS 取绝对值
}

void test_rms_of_silence_is_zero_not_nan(void) {
    static float x[64];
    for (size_t t = 0; t < 64; ++t) x[t] = 0.0f;
    const float r = frameRms(x, 64);
    TEST_ASSERT_FALSE(isnan(r));
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, 0.0f, r);
}

// ── IIR 系数 ──────────────────────────────────────────────

// a = 1 - exp(-dt/τ)。这是零阶保持的**精确**离散化，不是近似 dt/τ。
// 近似式在 dt 接近 τ 时会算出 a>1，包络直接发散。
void test_coeff_is_exponential_not_linear(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f - expf(-1.0f), envCoeff(10.0f, 10.0f));
    // dt 远大于 τ 时 a 必须收敛到 1，绝不能 >1
    TEST_ASSERT_TRUE(envCoeff(1.0f, 1000.0f) <= 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, envCoeff(1.0f, 1000.0f));
    // τ 远大于 dt 时 a 趋近 dt/τ
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.001f, envCoeff(1000.0f, 1.0f));
}

void test_coeff_rejects_degenerate_inputs(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, envCoeff(0.0f, 10.0f));    // τ=0 → 直通
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, envCoeff(10.0f, 0.0f));    // dt=0 → 不动
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, envCoeff(10.0f, -5.0f));   // 负 dt → 不动
}

// ── 包络：时间常数的定义 ───────────────────────────────────

// 阶跃输入经过一个 τ 应达到 1-1/e = 0.6321。这是 τ 的定义本身。
void test_step_response_reaches_one_minus_1_over_e_at_tau(void) {
    const EnvelopeConfig cfg = {100.0f, 100.0f, 100.0f};
    Envelope e;
    envelopeInit(e, cfg, 5.0f);
    for (int k = 0; k < 20; ++k) envelopeUpdate(e, 1.0f);   // 20×5ms = 100ms = τ
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.63212f, e.fast);
}

// 快包络必须比慢包络跟得紧 —— 这条钉住「两个时间常数不是同一个」。
void test_fast_leads_slow(void) {
    const EnvelopeConfig cfg = {30.0f, 2000.0f, 400.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    for (int k = 0; k < 10; ++k) envelopeUpdate(e, 1.0f);   // 100ms
    TEST_ASSERT_TRUE_MESSAGE(e.fast > 0.9f,  "30ms 包络在 100ms 后应基本跟上");
    TEST_ASSERT_TRUE_MESSAGE(e.slow < 0.10f, "2s 包络在 100ms 后应几乎没动");
}

// ── 跨档一致性：本轮的验收线 ───────────────────────────────

// 同一段物理时间，不同 hop（即不同 dt）算出的包络必须一致。
//
// 一阶 IIR 用 a = 1-exp(-dt/τ) 时 (1-a)^k = exp(-k·dt/τ)，只要 k·dt 相同，
// 阶跃响应就**精确**相同 —— 与 dt 无关。若把 a 写成常数（比如 0.1），
// 切档改了 hop，时间常数就跟着漂，用户看到的是「换档后灯的反应快慢变了」。
void test_envelope_is_invariant_to_hop(void) {
    const EnvelopeConfig cfg = {100.0f, 800.0f, 300.0f};
    const float dts[3]   = {5.0f, 10.0f, 25.0f};      // 三种 hop
    const int   steps[3] = { 40,    20,     8   };    // 都是 200ms
    float ref_fast = 0, ref_slow = 0, ref_peak = 0;

    for (int c = 0; c < 3; ++c) {
        Envelope e;
        envelopeInit(e, cfg, dts[c]);
        for (int k = 0; k < steps[c]; ++k) envelopeUpdate(e, 0.8f);
        if (c == 0) { ref_fast = e.fast; ref_slow = e.slow; ref_peak = e.peak; continue; }
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, ref_fast, e.fast, "快包络随 hop 漂了");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, ref_slow, e.slow, "慢包络随 hop 漂了");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, ref_peak, e.peak, "峰值随 hop 漂了");
    }
}

// 切档时改系数、**保状态**。丢状态的话包络归零，灯会在切档瞬间暗一下 ——
// 与 §3.3.4 第 2 条「不要丢掉 BPM 锁定」同一个道理。
void test_retime_keeps_state(void) {
    const EnvelopeConfig cfg = {100.0f, 800.0f, 300.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    for (int k = 0; k < 10; ++k) envelopeUpdate(e, 0.7f);
    const float f = e.fast, s = e.slow, p = e.peak;

    envelopeRetime(e, cfg, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, f, e.fast, "retime 不该动状态");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, s, e.slow, "retime 不该动状态");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, p, e.peak, "retime 不该动状态");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(100.0f, 25.0f), e.a_fast);  // 系数确实换了
}

// 中途换 hop，后续轨迹仍须与全程用新 hop 的一致（在相同物理时间上）。
// 这比上一条强：它检验 retime 之后的**演化**，不只是当下的状态。
void test_trajectory_continues_correctly_after_retime(void) {
    const EnvelopeConfig cfg = {100.0f, 800.0f, 300.0f};
    Envelope a, b;
    envelopeInit(a, cfg, 10.0f);
    envelopeInit(b, cfg, 10.0f);

    for (int k = 0; k < 10; ++k) { envelopeUpdate(a, 0.6f); envelopeUpdate(b, 0.6f); }
    // a 换成 25ms 步长再走 200ms；b 保持 10ms 走同样的 200ms
    envelopeRetime(a, cfg, 25.0f);
    for (int k = 0; k <  8; ++k) envelopeUpdate(a, 0.6f);
    for (int k = 0; k < 20; ++k) envelopeUpdate(b, 0.6f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, b.fast, a.fast, "换 hop 后轨迹偏了");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, b.slow, a.slow, "换 hop 后轨迹偏了");
}

// ── 峰值 ──────────────────────────────────────────────────

// 峰值立刻跟上瞬态，然后按自己的时间常数回落。
void test_peak_attacks_instantly_and_decays(void) {
    const EnvelopeConfig cfg = {30.0f, 2000.0f, 100.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    envelopeUpdate(e, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 1.0f, e.peak, "峰值应无延迟地跟上");
    TEST_ASSERT_TRUE_MESSAGE(e.fast < 0.5f, "快包络不该也无延迟 —— 那样两者就没区别了");

    for (int k = 0; k < 10; ++k) envelopeUpdate(e, 0.0f);    // 100ms = τ_peak
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 0.36788f, e.peak, "峰值应按 exp 回落");
}

void test_peak_never_below_current(void) {
    const EnvelopeConfig cfg = {30.0f, 2000.0f, 100.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    for (int k = 0; k < 50; ++k) {
        envelopeUpdate(e, (k % 7 == 0) ? 0.9f : 0.1f);
        TEST_ASSERT_TRUE(e.peak >= e.fast - 1e-6f);
    }
}

// ── AGC ───────────────────────────────────────────────────

// 稳态：收敛后 gain × 输入 ≈ target。
void test_agc_converges_to_target(void) {
    const AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    for (int k = 0; k < 3000; ++k) agcUpdate(g, cfg, 0.05f);   // 30s，够 release 收敛
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * cfg.target, cfg.target, g.gain * 0.05f,
        "AGC 未收敛到目标电平");
}

// 静音必须门限，且增益不得爬到上限 —— 这是 §4 的验收条目
// 「静音：频段趋零，AGC 不失控放大底噪」。
void test_agc_gates_silence_and_does_not_run_away(void) {
    AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    for (int k = 0; k < 6000; ++k) agcUpdate(g, cfg, 0.0002f);  // 60s 底噪，低于 squelch
    TEST_ASSERT_TRUE_MESSAGE(g.gated, "低于 squelch 应报 gated");
    TEST_ASSERT_TRUE_MESSAGE(g.gain < cfg.gain_max * 0.5f,
        "静音期间增益爬升了 —— 底噪会被放大");
}

// 门限有滞回，否则信号在阈值附近抖动时灯会闪。
void test_agc_gate_has_hysteresis(void) {
    AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    agcUpdate(g, cfg, cfg.squelch * 2.0f);
    TEST_ASSERT_FALSE(g.gated);
    // 略降到 squelch 之下但仍在滞回带内 —— 不应立刻关门
    agcUpdate(g, cfg, cfg.squelch * 0.9f);
    TEST_ASSERT_FALSE_MESSAGE(g.gated, "刚跌破阈值就关门 —— 缺滞回");
    // 明显低于滞回带 —— 关门
    agcUpdate(g, cfg, cfg.squelch * 0.2f);
    TEST_ASSERT_TRUE(g.gated);
}

// 变响要快压（防削顶），变轻要慢放（防歌曲间隙呼吸）。非对称是必须的。
void test_agc_attack_is_much_faster_than_release(void) {
    AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    for (int k = 0; k < 3000; ++k) agcUpdate(g, cfg, 0.05f);
    const float settled = g.gain;

    // 突然变响 10 倍：1 秒内增益应已压下大半
    for (int k = 0; k < 100; ++k) agcUpdate(g, cfg, 0.5f);
    const float after_loud = g.gain;
    TEST_ASSERT_TRUE_MESSAGE(after_loud < settled * 0.25f, "attack 太慢，会削顶");

    // 突然变轻回去：1 秒内增益应几乎没回升
    for (int k = 0; k < 100; ++k) agcUpdate(g, cfg, 0.05f);
    TEST_ASSERT_TRUE_MESSAGE(g.gain < after_loud * 3.0f, "release 太快，会呼吸");
}

void test_agc_gain_stays_within_bounds(void) {
    AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    for (int k = 0; k < 2000; ++k) {
        agcUpdate(g, cfg, 5.0f);                   // 极响
        TEST_ASSERT_TRUE(g.gain >= cfg.gain_min);
        TEST_ASSERT_TRUE(g.gain <= cfg.gain_max);
    }
    for (int k = 0; k < 20000; ++k) {
        agcUpdate(g, cfg, cfg.squelch * 3.0f);     // 很轻但未静音
        TEST_ASSERT_TRUE(g.gain >= cfg.gain_min);
        TEST_ASSERT_TRUE_MESSAGE(g.gain <= cfg.gain_max, "增益冲出上限");
    }
}

// AGC 也必须跨 hop 一致 —— 与包络同一个理由。
void test_agc_is_invariant_to_hop(void) {
    const AgcConfig cfg;
    const float dts[3]   = {5.0f, 10.0f, 25.0f};
    const int   steps[3] = {1000,  500,   200 };   // 都是 5s
    float ref = 0;
    for (int c = 0; c < 3; ++c) {
        Agc g;
        agcInit(g, cfg, dts[c]);
        for (int k = 0; k < steps[c]; ++k) agcUpdate(g, cfg, 0.05f);
        if (c == 0) { ref = g.gain; continue; }
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * ref, ref, g.gain, "AGC 增益随 hop 漂了");
    }
}

// 从静音进来的第一帧不能瞬间把增益顶到上限。
void test_agc_starts_at_unity_not_max(void) {
    const AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, g.gain);
}

// 非有限输入不得污染状态 —— ADC 掉线或除零上游都可能送来 NaN。
void test_non_finite_input_does_not_poison_state(void) {
    const EnvelopeConfig ecfg = {30.0f, 2000.0f, 400.0f};
    Envelope e;
    envelopeInit(e, ecfg, 10.0f);
    for (int k = 0; k < 10; ++k) envelopeUpdate(e, 0.5f);
    const float f = e.fast;
    envelopeUpdate(e, NAN);
    envelopeUpdate(e, INFINITY);
    TEST_ASSERT_FALSE_MESSAGE(isnan(e.fast), "NaN 渗进了包络状态");
    TEST_ASSERT_FALSE(isnan(e.slow));
    TEST_ASSERT_FALSE(isnan(e.peak));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, f, e.fast, "非有限输入应被整帧丢弃");

    const AgcConfig acfg;
    Agc g;
    agcInit(g, acfg, 10.0f);
    for (int k = 0; k < 100; ++k) agcUpdate(g, acfg, 0.05f);
    const float gg = g.gain;
    agcUpdate(g, acfg, NAN);
    TEST_ASSERT_FALSE(isnan(g.gain));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, gg, g.gain);
}

// 负 RMS 是上游的错，但不能让它把增益推成负数。
void test_negative_rms_is_rejected(void) {
    const AgcConfig cfg;
    Agc g;
    agcInit(g, cfg, 10.0f);
    for (int k = 0; k < 50; ++k) agcUpdate(g, cfg, -1.0f);
    TEST_ASSERT_TRUE(g.gain >= cfg.gain_min);

    // 门限关着时走不到增益那一段，所以再单独试一次开着门的情况
    AgcConfig open = cfg;
    open.squelch = 0.0f;
    Agc h;
    agcInit(h, open, 10.0f);
    for (int k = 0; k < 50; ++k) agcUpdate(h, open, -1.0f);
    TEST_ASSERT_TRUE_MESSAGE(h.gain >= open.gain_min, "负输入把增益推到下限以下");
    TEST_ASSERT_FALSE(isnan(h.gain));
}

// squelch 配成 0 时，静音输入不得产生 inf。
// 正常配置下门限会先拦住 slow_rms=0，除零保护是唯一的第二道防线。
void test_zero_squelch_does_not_produce_inf(void) {
    AgcConfig cfg;
    cfg.squelch = 0.0f;
    Agc g;
    agcInit(g, cfg, 10.0f);
    for (int k = 0; k < 200; ++k) agcUpdate(g, cfg, 0.0f);
    TEST_ASSERT_FALSE_MESSAGE(isinf(g.gain), "squelch=0 且静音时增益变成 inf");
    TEST_ASSERT_FALSE(isnan(g.gain));
    TEST_ASSERT_TRUE(g.gain <= cfg.gain_max);
}

// 极轻的信号（want 远超 gain_max）不得让增益以远快于 release 的速度爬升。
// 只断言「最终在界内」是不够的 —— 事后夹一次也能满足，但过程已经不是 release
// 该有的样子了。
//
// 这条**显式压低 gain_max**，不用默认值。默认配置下 gain_max(250) 高于
// target/squelch(125)，「want 超过上限」那个分支根本走不到 —— 那是有意的
// （见下一条），但意味着这里必须自己造出可达的场景，否则测的是空气。
void test_agc_release_rate_is_not_inflated_by_huge_want(void) {
    AgcConfig cfg;
    cfg.gain_max = 20.0f;
    cfg.squelch  = 0.0005f;      // 放低门限，让弱信号能走到增益那一段
    Agc g;
    agcInit(g, cfg, 10.0f);
    // want = 0.25/0.0021 ≈ 119，是 gain_max 的六倍
    for (int k = 0; k < 100; ++k) agcUpdate(g, cfg, 0.0021f);   // 1 秒
    // release τ=6s，1 秒后从 1.0 朝 20 走应只到 ~4；若 want 未夹则朝 119 走，到 ~19
    TEST_ASSERT_TRUE_MESSAGE(g.gain < 8.0f, "增益爬升过快 —— want 未夹到 gain_max");
    TEST_ASSERT_TRUE(g.gain <= cfg.gain_max);
}

// 默认配置必须满足 gain_max ≥ target/squelch。
//
// 违反它会造出一片「没被门限拦住、却又拉不到目标」的死区：信号明明还在，
// AGC 顶格却够不着 target，频段能量始终偏低、灯几乎不亮。
// 实测就撞上过 —— 手机节拍器对着笔记本麦克风，AGC 恒为 40.0（旧上限），
// 而那时 target/squelch 是 125。
void test_gain_max_covers_the_whole_ungated_range(void) {
    const AgcConfig c;
    const float need = c.target / c.squelch;
    TEST_ASSERT_TRUE_MESSAGE(c.gain_max >= need,
        "gain_max 低于 target/squelch —— 弱信号会卡在够不着目标的死区里");

    // 正好在门限边缘的信号，收敛后应当真的到达 target
    Agc g;
    agcInit(g, c, 10.0f);
    const float weak = c.squelch * 1.05f;
    for (int k = 0; k < 20000; ++k) agcUpdate(g, c, weak);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f * c.target, c.target, g.gain * weak,
        "门限边缘的信号没能被拉到目标电平");
}

// 包络也必须拒绝负输入 —— 上一条测的是 AGC，两者是不同的函数。
void test_envelope_rejects_negative_rms(void) {
    const EnvelopeConfig cfg = {30.0f, 2000.0f, 400.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    for (int k = 0; k < 10; ++k) envelopeUpdate(e, 0.5f);
    const float f = e.fast, s = e.slow, p = e.peak;
    for (int k = 0; k < 10; ++k) envelopeUpdate(e, -0.5f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, f, e.fast, "负输入改动了快包络");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, s, e.slow, "负输入改动了慢包络");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, p, e.peak, "负输入改动了峰值");
}

// Init 必须把脏对象清干净。测试里都用新声明的 Envelope，成员有默认初始化，
// 于是「不清」和「清了」看不出区别 —— 必须显式喂一个脏对象。
void test_init_clears_a_dirty_object(void) {
    const EnvelopeConfig cfg = {30.0f, 2000.0f, 400.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    for (int k = 0; k < 100; ++k) envelopeUpdate(e, 0.9f);
    TEST_ASSERT_TRUE(e.fast > 0.5f);            // 确认真的脏了（正对照）

    envelopeInit(e, cfg, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, e.fast, "Init 没清快包络");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, e.slow, "Init 没清慢包络");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, e.peak, "Init 没清峰值");
}

// retime 之后三条支路都得真的在动。
//
// 差分测试的经典陷阱：test_trajectory_continues_correctly 比的是两个对象，
// 若某条支路的系数被破坏成 0，两边同样不动，比较照样相等。**必须有正对照。**
void test_retime_updates_all_three_coefficients(void) {
    const EnvelopeConfig cfg = {100.0f, 800.0f, 300.0f};
    Envelope e;
    envelopeInit(e, cfg, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(100.0f, 10.0f), e.a_fast);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(800.0f, 10.0f), e.a_slow);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, envCoeff(300.0f, 10.0f), e.a_peak);

    envelopeRetime(e, cfg, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(100.0f, 25.0f), e.a_fast, "a_fast 没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(800.0f, 25.0f), e.a_slow, "a_slow 没换");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, envCoeff(300.0f, 25.0f), e.a_peak, "a_peak 没换");

    // 正对照：三条支路喂常量后都必须离开 0
    for (int k = 0; k < 40; ++k) envelopeUpdate(e, 0.5f);
    TEST_ASSERT_TRUE_MESSAGE(e.fast > 1e-3f, "快包络没动 —— 系数是 0");
    TEST_ASSERT_TRUE_MESSAGE(e.slow > 1e-3f, "慢包络没动 —— 系数是 0");
    TEST_ASSERT_TRUE_MESSAGE(e.peak > 1e-3f, "峰值没动 —— 系数是 0");
}

// 空帧不得产生 NaN。
void test_rms_of_empty_frame_is_zero(void) {
    static float x[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float r = frameRms(x, 0);
    TEST_ASSERT_FALSE_MESSAGE(isnan(r), "n=0 时除零");
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, 0.0f, r);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_rms_of_unit_sine_is_one_over_sqrt2);
    RUN_TEST(test_rms_of_dc_is_the_dc_value);
    RUN_TEST(test_rms_of_silence_is_zero_not_nan);
    RUN_TEST(test_coeff_is_exponential_not_linear);
    RUN_TEST(test_coeff_rejects_degenerate_inputs);
    RUN_TEST(test_step_response_reaches_one_minus_1_over_e_at_tau);
    RUN_TEST(test_fast_leads_slow);
    RUN_TEST(test_envelope_is_invariant_to_hop);
    RUN_TEST(test_retime_keeps_state);
    RUN_TEST(test_trajectory_continues_correctly_after_retime);
    RUN_TEST(test_peak_attacks_instantly_and_decays);
    RUN_TEST(test_peak_never_below_current);
    RUN_TEST(test_agc_converges_to_target);
    RUN_TEST(test_agc_gates_silence_and_does_not_run_away);
    RUN_TEST(test_agc_gate_has_hysteresis);
    RUN_TEST(test_agc_attack_is_much_faster_than_release);
    RUN_TEST(test_agc_gain_stays_within_bounds);
    RUN_TEST(test_agc_is_invariant_to_hop);
    RUN_TEST(test_agc_starts_at_unity_not_max);
    RUN_TEST(test_non_finite_input_does_not_poison_state);
    RUN_TEST(test_negative_rms_is_rejected);
    RUN_TEST(test_zero_squelch_does_not_produce_inf);
    RUN_TEST(test_agc_release_rate_is_not_inflated_by_huge_want);
    RUN_TEST(test_gain_max_covers_the_whole_ungated_range);
    RUN_TEST(test_envelope_rejects_negative_rms);
    RUN_TEST(test_init_clears_a_dirty_object);
    RUN_TEST(test_retime_updates_all_three_coefficients);
    RUN_TEST(test_rms_of_empty_frame_is_zero);
    return UNITY_END();
}
