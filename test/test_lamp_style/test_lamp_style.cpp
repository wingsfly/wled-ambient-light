#include <unity.h>
#include <math.h>
#include "lamp_style.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

// ── 档位参数表 ────────────────────────────────────────────

// 四个档位的 (N, 窗, hop) 必须与设计 §3.3.1 的表一致。
void test_preset_table_matches_spec(void) {
    struct Row { StylePreset p; uint16_t n; WindowType w; uint16_t h; };
    const Row rows[4] = {
        {STYLE_AMBIENT,    2048, WIN_BLACKMAN_HARRIS, 1024},
        {STYLE_GENERAL,    1024, WIN_HANN,             512},
        {STYLE_EDM,        1024, WIN_HANN,             256},
        {STYLE_PERCUSSIVE,  512, WIN_HANN,             128},
    };
    for (int i = 0; i < 4; ++i) {
        const PresetParams q = presetParams(rows[i].p);
        TEST_ASSERT_EQUAL_UINT16(rows[i].n, q.n);
        TEST_ASSERT_EQUAL_INT(rows[i].w, q.wt);
        TEST_ASSERT_EQUAL_UINT16(rows[i].h, q.hop);
    }
}

// 约束 H ≥ N/8（§3.3.1）：大 N 配极小 hop 两头不讨好。
void test_all_presets_satisfy_hop_constraint(void) {
    for (int i = 0; i < STYLE_COUNT; ++i) {
        const PresetParams q = presetParams((StylePreset)i);
        TEST_ASSERT_TRUE_MESSAGE(q.hop >= q.n / 8, "违反 H >= N/8");
    }
}

// hop 换算成毫秒，供包络/AGC 的 retime 用。
void test_hop_ms_matches_frame_rate(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 46.44f, presetHopMs(STYLE_AMBIENT));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 23.22f, presetHopMs(STYLE_GENERAL));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 11.61f, presetHopMs(STYLE_EDM));
    TEST_ASSERT_FLOAT_WITHIN(0.05f,  5.80f, presetHopMs(STYLE_PERCUSSIVE));
}

// 档位序号必须按「越快越大」排列 —— quantize 的阈值比较依赖这个次序。
void test_preset_order_is_by_increasing_speed(void) {
    TEST_ASSERT_TRUE(STYLE_AMBIENT < STYLE_GENERAL);
    TEST_ASSERT_TRUE(STYLE_GENERAL < STYLE_EDM);
    TEST_ASSERT_TRUE(STYLE_EDM     < STYLE_PERCUSSIVE);
    for (int i = 1; i < STYLE_COUNT; ++i)
        TEST_ASSERT_TRUE_MESSAGE(presetHopMs((StylePreset)i) < presetHopMs((StylePreset)(i-1)),
            "档位序号变大时 hop 应变小");
}

// ── 速度需求标量 ──────────────────────────────────────────

static StyleFeatures slowBallad(void) {
    StyleFeatures f;
    f.onset_rate = 0.6f; f.bpm = 70.0f; f.bpm_conf = 0.5f;
    f.centroid_hz = 500.0f; f.flatness = 0.10f;
    return f;
}

static StyleFeatures fastEdm(void) {
    StyleFeatures f;
    f.onset_rate = 7.5f; f.bpm = 174.0f; f.bpm_conf = 0.95f;
    f.centroid_hz = 3500.0f; f.flatness = 0.55f;
    return f;
}

void test_demand_is_bounded_and_ordered(void) {
    const StyleConfig c;
    const float slow = speedDemand(c, slowBallad());
    const float fast = speedDemand(c, fastEdm());
    TEST_ASSERT_TRUE(slow >= 0.0f && slow <= 1.0f);
    TEST_ASSERT_TRUE(fast >= 0.0f && fast <= 1.0f);
    TEST_ASSERT_TRUE_MESSAGE(fast > slow + 0.3f, "快慢曲的需求值应拉得开");
}

// 每个特征单独变大都必须把需求推高 —— 这条钉住「四个判据都真的参与了」。
// 只测组合值的话，某个权重被写成 0 完全看不出来。
void test_every_feature_moves_the_demand(void) {
    const StyleConfig c;
    StyleFeatures base;
    base.onset_rate = 4.0f; base.bpm = 120.0f; base.bpm_conf = 1.0f;
    base.centroid_hz = 1200.0f; base.flatness = 0.4f;
    const float d0 = speedDemand(c, base);

    StyleFeatures f = base; f.onset_rate = 8.0f;
    TEST_ASSERT_TRUE_MESSAGE(speedDemand(c, f) > d0 + 0.01f, "onset 密度没起作用");
    f = base; f.bpm = 190.0f;
    TEST_ASSERT_TRUE_MESSAGE(speedDemand(c, f) > d0 + 0.01f, "BPM 没起作用");
    f = base; f.centroid_hz = 5000.0f;
    TEST_ASSERT_TRUE_MESSAGE(speedDemand(c, f) > d0 + 0.01f, "频谱质心没起作用");
    f = base; f.flatness = 0.9f;
    TEST_ASSERT_TRUE_MESSAGE(speedDemand(c, f) > d0 + 0.01f, "频谱平坦度没起作用");
}

// onset 密度是主判据，权重必须比其余任何一个都大。
void test_onset_rate_dominates(void) {
    const StyleConfig c;
    TEST_ASSERT_TRUE(c.w_onset > c.w_bpm);
    TEST_ASSERT_TRUE(c.w_onset > c.w_centroid);
    TEST_ASSERT_TRUE(c.w_onset > c.w_flatness);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 1.0f,
        c.w_onset + c.w_bpm + c.w_centroid + c.w_flatness, "权重之和应为 1");
}

// BPM 置信度低时，那一项应退回中性值而不是 0 —— 退回 0 等于「没节奏就是慢曲」，
// 但白噪声和纯人声都是无节奏的，前者该快后者该慢，BPM 在这里本就不该有话语权。
void test_low_bpm_confidence_falls_back_to_neutral(void) {
    const StyleConfig c;
    StyleFeatures hi;
    hi.onset_rate = 4.0f; hi.bpm = 40.0f; hi.bpm_conf = 0.0f;
    hi.centroid_hz = 1200.0f; hi.flatness = 0.4f;
    StyleFeatures lo = hi; lo.bpm = 200.0f;   // 置信度为 0 时 BPM 取值不该有影响
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, speedDemand(c, hi), speedDemand(c, lo),
        "置信度为 0 时 BPM 仍在影响结果");

    // 而且中性值不是 0：同样的特征，把 conf 拉满且 BPM 很低，需求应更低
    StyleFeatures sure = hi; sure.bpm_conf = 1.0f;
    TEST_ASSERT_TRUE_MESSAGE(speedDemand(c, sure) < speedDemand(c, hi),
        "高置信度的慢 BPM 应把需求拉低到中性值以下");
}

// **每一个**特征都要单独试 NaN。质心那一项有自己的 isfinite 检查，
// 只测它等于没测 clamp01 —— 其余三项全靠 clamp01 挡。
void test_demand_rejects_non_finite_features(void) {
    const StyleConfig c;
    const int N = 5;
    for (int i = 0; i < N; ++i) {
        StyleFeatures f = fastEdm();
        switch (i) {
            case 0: f.centroid_hz = NAN;      break;
            case 1: f.onset_rate  = NAN;      break;
            case 2: f.flatness    = NAN;      break;
            case 3: f.bpm         = NAN;      break;
            case 4: f.bpm_conf    = NAN;      break;
        }
        const float d = speedDemand(c, f);
        TEST_ASSERT_FALSE_MESSAGE(isnan(d), "NaN 特征渗进了需求值");
        TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
    }
}

// 极端大的特征值不得把需求顶出 [0,1]。
// 下游 quantize 按阈值比较，>1 的需求值本身不会立刻出错，但它会让
// 「需求值」这个量失去意义，后面任何按比例解读它的代码都会跟着错。
void test_demand_stays_bounded_under_absurd_input(void) {
    const StyleConfig c;
    StyleFeatures f;
    f.onset_rate = 1000.0f; f.bpm = 9999.0f; f.bpm_conf = 5.0f;
    f.centroid_hz = 1e6f;   f.flatness = 12.0f;
    const float d = speedDemand(c, f);
    TEST_ASSERT_TRUE_MESSAGE(d <= 1.0f, "需求值冲出上限");
    TEST_ASSERT_TRUE(d >= 0.0f);

    StyleFeatures neg;
    neg.onset_rate = -50.0f; neg.bpm = -100.0f; neg.bpm_conf = -1.0f;
    neg.centroid_hz = -3000.0f; neg.flatness = -2.0f;
    const float dn = speedDemand(c, neg);
    TEST_ASSERT_TRUE_MESSAGE(dn >= 0.0f, "需求值跌破下限");
    TEST_ASSERT_FALSE(isnan(dn));
}

// 质心按**对数**归一化，不是线性 —— 听感上倍频程才是等距的。
// 200→400 与 400→800 都是一个倍频程，对需求的贡献必须相同。
void test_centroid_is_logarithmic_not_linear(void) {
    const StyleConfig c;
    StyleFeatures f;
    f.onset_rate = 0.0f; f.bpm = 0.0f; f.bpm_conf = 0.0f; f.flatness = 0.0f;

    f.centroid_hz =  400.0f; const float d400  = speedDemand(c, f);
    f.centroid_hz =  800.0f; const float d800  = speedDemand(c, f);
    f.centroid_hz = 1600.0f; const float d1600 = speedDemand(c, f);

    const float oct1 = d800  - d400;
    const float oct2 = d1600 - d800;
    TEST_ASSERT_TRUE_MESSAGE(oct1 > 0.01f, "质心项没起作用");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f * oct1, oct1, oct2,
        "相邻倍频程的贡献不相等 —— 质心用了线性归一化");
}

// 需求值恰好落在阈值上时应算作「达到」。
// 阈值是可配置项，用户配一个整数、需求值又恰好等于它，并不罕见。
void test_demand_exactly_at_threshold_counts_as_reached(void) {
    StyleConfig c;
    c.hysteresis = 0.0f;                 // 先关掉滞回，单看边界比较本身
    TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_GENERAL, quantize(c, c.thresholds[0], STYLE_AMBIENT),
        "需求值恰等于阈值时未判定为达到 —— 比较写成了 > 而非 >=");
}

// ── 量化（带滞回）───────────────────────────────────────

void test_quantize_covers_all_four_presets(void) {
    const StyleConfig c;
    TEST_ASSERT_EQUAL_INT(STYLE_AMBIENT,    quantize(c, 0.05f, STYLE_AMBIENT));
    TEST_ASSERT_EQUAL_INT(STYLE_GENERAL,    quantize(c, 0.42f, STYLE_GENERAL));
    TEST_ASSERT_EQUAL_INT(STYLE_EDM,        quantize(c, 0.66f, STYLE_EDM));
    TEST_ASSERT_EQUAL_INT(STYLE_PERCUSSIVE, quantize(c, 0.95f, STYLE_PERCUSSIVE));
}

// 阈值处必须有滞回。
//
// 这层滞回不是可有可无的装饰：下面「连续胜出 3 秒」要求候选**稳定**。
// 需求值若在阈值上下抖动，候选就一直在变，永远凑不满 3 秒 —— 音乐明明变了却不切档，
// 那是比抖动更糟的 bug。
void test_quantize_has_hysteresis_at_thresholds(void) {
    StyleConfig c;
    const float th = c.thresholds[1];          // GENERAL ↔ EDM 的边界

    // 从 GENERAL 出发，略微越过阈值 —— 不该升
    TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_GENERAL, quantize(c, th + 0.5f * c.hysteresis, STYLE_GENERAL),
        "刚过阈值就升档 —— 缺滞回");
    // 明确越过滞回带 —— 升
    TEST_ASSERT_EQUAL_INT(STYLE_EDM, quantize(c, th + 1.5f * c.hysteresis, STYLE_GENERAL));

    // 从 EDM 出发，略微跌破阈值 —— 不该降
    TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_EDM, quantize(c, th - 0.5f * c.hysteresis, STYLE_EDM),
        "刚跌破阈值就降档 —— 缺滞回");
    TEST_ASSERT_EQUAL_INT(STYLE_GENERAL, quantize(c, th - 1.5f * c.hysteresis, STYLE_EDM));
}

// 滞回带内反复抖动时，量化结果必须完全不动。
void test_quantize_is_stable_under_dither(void) {
    const StyleConfig c;
    const float th = c.thresholds[0];
    StylePreset cur = STYLE_GENERAL;
    for (int k = 0; k < 200; ++k) {
        const float d = th + ((k % 2) ? 0.4f : -0.4f) * c.hysteresis;
        cur = quantize(c, d, cur);
        TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_GENERAL, cur, "滞回带内抖动导致了档位变化");
    }
}

// 需求值大幅跳变时可以跨多档，不必逐级爬。
void test_quantize_can_jump_multiple_levels(void) {
    const StyleConfig c;
    TEST_ASSERT_EQUAL_INT(STYLE_PERCUSSIVE, quantize(c, 0.98f, STYLE_AMBIENT));
    TEST_ASSERT_EQUAL_INT(STYLE_AMBIENT,    quantize(c, 0.02f, STYLE_PERCUSSIVE));
}

// ── 切换纪律 ──────────────────────────────────────────────

static StyleFeatures demandOf(float target) {
    // 构造一个大致给出指定需求值的特征组合（只调 onset，其余取中性）
    StyleFeatures f;
    f.onset_rate = target * 8.0f;
    f.bpm = 60.0f + target * 120.0f; f.bpm_conf = 1.0f;
    f.centroid_hz = 200.0f * powf(2.0f, target * 4.9f);
    f.flatness = target;
    return f;
}

void test_starts_at_general(void) {
    StyleSelector s;
    TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_GENERAL, s.current, "默认档应为通用");
}

// 新档位需连续胜出 3 秒（§3.3.3）。
void test_requires_three_seconds_of_sustained_win(void) {
    const StyleConfig c;
    StyleSelector s;
    const StyleFeatures f = demandOf(0.95f);

    TEST_ASSERT_FALSE(styleUpdate(s, c, f, 1000));
    TEST_ASSERT_FALSE_MESSAGE(styleUpdate(s, c, f, 3999), "不足 3 秒就切了");
    TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, f, 4000), "满 3 秒仍未切");
    TEST_ASSERT_EQUAL_INT(STYLE_PERCUSSIVE, s.current);
}

// 候选中途变了，计时必须重来。
void test_candidate_change_restarts_the_timer(void) {
    const StyleConfig c;
    StyleSelector s;
    styleUpdate(s, c, demandOf(0.95f), 1000);
    styleUpdate(s, c, demandOf(0.95f), 3000);       // 已积累 2 秒
    styleUpdate(s, c, demandOf(0.02f), 3500);       // 候选换成 AMBIENT，计时重来
    TEST_ASSERT_FALSE_MESSAGE(styleUpdate(s, c, demandOf(0.02f), 6000),
        "换了候选却沿用旧计时");
    TEST_ASSERT_TRUE(styleUpdate(s, c, demandOf(0.02f), 6500));
    TEST_ASSERT_EQUAL_INT(STYLE_AMBIENT, s.current);
}

// 距上次切换需 ≥8 秒（§3.3.3）。
void test_dwell_blocks_a_second_switch(void) {
    const StyleConfig c;
    StyleSelector s;
    // 候选在第一次见到时才建立，cand_since=now，所以同一帧不可能满足「持续 3 秒」
    styleUpdate(s, c, demandOf(0.95f), 1000);
    TEST_ASSERT_TRUE(styleUpdate(s, c, demandOf(0.95f), 4000));   // 第一次切换 @4000

    // 立刻转向另一个档位并持续 3 秒以上，但驻留期未满
    const StyleFeatures back = demandOf(0.02f);
    styleUpdate(s, c, back, 4100);
    TEST_ASSERT_FALSE_MESSAGE(styleUpdate(s, c, back, 8000),
        "距上次切换不足 8 秒就又切了");
    TEST_ASSERT_EQUAL_INT(STYLE_PERCUSSIVE, s.current);
    // 12000 时距上次切换 8 秒、候选已持续 7.9 秒 —— 两个条件都满足
    TEST_ASSERT_TRUE(styleUpdate(s, c, back, 12000));
    TEST_ASSERT_EQUAL_INT(STYLE_AMBIENT, s.current);
}

// 开机后的第一次切换不受驻留限制 —— 否则要空等 8 秒才肯离开默认档。
//
// 这条同时防住哨兵陷阱：last_switch 初值若取 0 并当作「从未切换」，
// 那么 millis 真的走到 0 附近（回绕后）时会被误判。用独立的标志位。
void test_first_switch_is_not_blocked_by_dwell(void) {
    const StyleConfig c;
    StyleSelector s;
    const StyleFeatures f = demandOf(0.95f);
    styleUpdate(s, c, f, 0);
    TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, f, 3000),
        "开机首切被驻留期挡住了");
}

// millis 回绕（49.7 天）不得让切换卡死。
//
// ⚠️ `t0` 必须近到让 `t0 + 间隔` 真的越过 2^32。上一轮 AudioSource 用了
// `0xFFFFF000`（距回绕 4096ms），加 3000ms 后是 4294966200 —— **没越过**，
// 那条测试从洞上方笔直飞了过去。这里取距回绕 256ms。
void test_survives_millis_wraparound(void) {
    const StyleConfig c;
    StyleSelector s;
    const uint32_t t0 = 0xFFFFFF00u;              // 距回绕 256ms
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)(t0 + 3000) < t0, "这个 t0 没有真的回绕");

    const StyleFeatures f = demandOf(0.95f);
    styleUpdate(s, c, f, t0);
    TEST_ASSERT_FALSE(styleUpdate(s, c, f, (uint32_t)(t0 + 2999)));
    TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, f, (uint32_t)(t0 + 3000)),
        "跨过回绕点后切换卡死");
}

// 回绕点恰好落在驻留期中间。
void test_dwell_survives_wraparound(void) {
    const StyleConfig c;
    StyleSelector s;
    const uint32_t t0 = 0xFFFFFF00u;
    const StyleFeatures fast = demandOf(0.95f);
    styleUpdate(s, c, fast, t0);
    TEST_ASSERT_TRUE(styleUpdate(s, c, fast, (uint32_t)(t0 + 3000)));   // 切换点跨过回绕

    const StyleFeatures slow = demandOf(0.02f);
    styleUpdate(s, c, slow, (uint32_t)(t0 + 3100));
    TEST_ASSERT_FALSE(styleUpdate(s, c, slow, (uint32_t)(t0 + 9000)));  // 驻留未满
    TEST_ASSERT_TRUE(styleUpdate(s, c, slow, (uint32_t)(t0 + 11000)));
}

// 间隔超过 2^31 ms（24.8 天）时仍须正常工作。
//
// 这才是有符号比较真正失效的地方，回绕点反而不是 —— `(uint32_t)(now-since)`
// 在回绕处照样给出正确的小差值，转成 int32 也还是正数。但差值一旦超过 2^31，
// int32 就变负，`< hold_ms` 恒成立，**切换永久卡死**。
//
// 现实可达：灯开着 25 天、期间一直是同一种音乐没切过档，然后换了首歌。
void test_survives_intervals_beyond_signed_range(void) {
    const StyleConfig c;
    const uint32_t huge = 0x90000000u;   // 约 27.9 天，超过 2^31

    // hold：候选建立后 27.9 天才复查，必须判定为「早就够 3 秒了」
    {
        StyleSelector s;
        const StyleFeatures f = demandOf(0.95f);
        styleUpdate(s, c, f, 1000);
        TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, f, (uint32_t)(1000 + huge)),
            "hold 判据在 >24.8 天的间隔上卡死了 —— 多半用了有符号比较");
    }
    // dwell：切换后 27.9 天没再切，之后必须允许再切
    {
        StyleSelector s;
        styleUpdate(s, c, demandOf(0.95f), 1000);
        TEST_ASSERT_TRUE(styleUpdate(s, c, demandOf(0.95f), 4000));
        const StyleFeatures slow = demandOf(0.02f);
        styleUpdate(s, c, slow, (uint32_t)(4000 + huge));
        TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, slow, (uint32_t)(4000 + huge + 3000)),
            "dwell 判据在 >24.8 天的间隔上卡死了");
    }
}

// 切换恰好发生在 now_ms == 0 时，驻留期照样要生效。
//
// 这条专治「拿 last_switch==0 当作从未切换过」的哨兵写法：0 是合法的 millis 值，
// 回绕后真的会走到那里，那时驻留期会被整段跳过。
void test_switch_at_time_zero_still_starts_the_dwell(void) {
    const StyleConfig c;
    StyleSelector s;
    // 候选要在 0 之前整整 hold_ms 建立，提交才恰好落在 0 这一刻
    const uint32_t t0 = (uint32_t)(0u - c.hold_ms);
    const StyleFeatures fast = demandOf(0.95f);

    styleUpdate(s, c, fast, t0);
    TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, fast, 0u), "回绕到 0 的那一帧没能提交");
    TEST_ASSERT_EQUAL_UINT32(0u, s.last_switch);

    const StyleFeatures slow = demandOf(0.02f);
    styleUpdate(s, c, slow, 100);
    TEST_ASSERT_FALSE_MESSAGE(styleUpdate(s, c, slow, 5000),
        "last_switch 恰为 0 时驻留期被跳过了 —— 用了哨兵值而非独立标志位");
    TEST_ASSERT_TRUE(styleUpdate(s, c, slow, 8000));
}

// ── 手动锁定 ──────────────────────────────────────────────

// 手动锁定优先级最高，立即生效，不等 3 秒也不等 8 秒（§3.3.3）。
void test_manual_lock_takes_effect_immediately(void) {
    StyleConfig c;
    StyleSelector s;
    c.manual_lock = STYLE_PERCUSSIVE;
    TEST_ASSERT_TRUE(styleUpdate(s, c, demandOf(0.02f), 100));
    TEST_ASSERT_EQUAL_INT(STYLE_PERCUSSIVE, s.current);
}

void test_manual_lock_holds_against_contrary_features(void) {
    StyleConfig c;
    StyleSelector s;
    c.manual_lock = STYLE_AMBIENT;
    for (uint32_t t = 0; t < 60000; t += 500)
        styleUpdate(s, c, demandOf(0.98f), t);
    TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_AMBIENT, s.current, "自动判据顶开了手动锁定");
}

// 锁定期间候选跟踪不能停：解锁那一刻要有一个**新鲜**的候选可用，
// 否则会先用一个几分钟前的过时判断，过 3 秒才纠正过来。
void test_candidate_keeps_tracking_while_locked(void) {
    StyleConfig c;
    StyleSelector s;
    c.manual_lock = STYLE_AMBIENT;
    for (uint32_t t = 0; t < 30000; t += 500)
        styleUpdate(s, c, demandOf(0.98f), t);
    TEST_ASSERT_EQUAL_INT_MESSAGE(STYLE_PERCUSSIVE, s.candidate,
        "锁定期间候选没有跟踪特征");

    // 解锁：候选已持续很久，驻留期也早过了，应立刻提交
    c.manual_lock = STYLE_COUNT;
    TEST_ASSERT_TRUE_MESSAGE(styleUpdate(s, c, demandOf(0.98f), 30500),
        "解锁后没有立刻用上已经攒够时间的候选");
    TEST_ASSERT_EQUAL_INT(STYLE_PERCUSSIVE, s.current);
}

void test_manual_lock_to_current_preset_reports_no_change(void) {
    StyleConfig c;
    StyleSelector s;
    c.manual_lock = STYLE_GENERAL;      // 就是默认档
    TEST_ASSERT_FALSE_MESSAGE(styleUpdate(s, c, demandOf(0.5f), 100),
        "锁到当前档也报了切换");
}

// ── 稳定性 ────────────────────────────────────────────────

// 稳定输入下不得反复切换。一分钟的稳定音乐最多切一次。
void test_stable_input_switches_at_most_once(void) {
    const StyleConfig c;
    StyleSelector s;
    const StyleFeatures f = demandOf(0.95f);
    int switches = 0;
    for (uint32_t t = 0; t < 60000; t += 100)
        if (styleUpdate(s, c, f, t)) ++switches;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, switches, "稳定输入下切换不止一次");
}

// 需求值在阈值上反复横跳时，切换频率必须受驻留期约束。
void test_thrashing_input_is_rate_limited(void) {
    const StyleConfig c;
    StyleSelector s;
    int switches = 0;
    for (uint32_t t = 0; t < 60000; t += 100) {
        const float d = ((t / 5000) % 2) ? 0.95f : 0.02f;   // 每 5 秒翻转
        if (styleUpdate(s, c, d > 0.5f ? demandOf(0.95f) : demandOf(0.02f), t)) ++switches;
    }
    // 60 秒 / 8 秒驻留 = 最多 7 次；实际因还要连续胜出 3 秒会更少
    TEST_ASSERT_TRUE_MESSAGE(switches <= 7, "切换频率超过了驻留期允许的上限");
    TEST_ASSERT_TRUE_MESSAGE(switches >= 1, "音乐反复变化却一次都没切 —— 可能是死锁");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_preset_table_matches_spec);
    RUN_TEST(test_all_presets_satisfy_hop_constraint);
    RUN_TEST(test_hop_ms_matches_frame_rate);
    RUN_TEST(test_preset_order_is_by_increasing_speed);
    RUN_TEST(test_demand_is_bounded_and_ordered);
    RUN_TEST(test_every_feature_moves_the_demand);
    RUN_TEST(test_onset_rate_dominates);
    RUN_TEST(test_low_bpm_confidence_falls_back_to_neutral);
    RUN_TEST(test_demand_rejects_non_finite_features);
    RUN_TEST(test_demand_stays_bounded_under_absurd_input);
    RUN_TEST(test_centroid_is_logarithmic_not_linear);
    RUN_TEST(test_demand_exactly_at_threshold_counts_as_reached);
    RUN_TEST(test_quantize_covers_all_four_presets);
    RUN_TEST(test_quantize_has_hysteresis_at_thresholds);
    RUN_TEST(test_quantize_is_stable_under_dither);
    RUN_TEST(test_quantize_can_jump_multiple_levels);
    RUN_TEST(test_starts_at_general);
    RUN_TEST(test_requires_three_seconds_of_sustained_win);
    RUN_TEST(test_candidate_change_restarts_the_timer);
    RUN_TEST(test_dwell_blocks_a_second_switch);
    RUN_TEST(test_first_switch_is_not_blocked_by_dwell);
    RUN_TEST(test_survives_millis_wraparound);
    RUN_TEST(test_dwell_survives_wraparound);
    RUN_TEST(test_survives_intervals_beyond_signed_range);
    RUN_TEST(test_switch_at_time_zero_still_starts_the_dwell);
    RUN_TEST(test_manual_lock_takes_effect_immediately);
    RUN_TEST(test_manual_lock_holds_against_contrary_features);
    RUN_TEST(test_candidate_keeps_tracking_while_locked);
    RUN_TEST(test_manual_lock_to_current_preset_reports_no_change);
    RUN_TEST(test_stable_input_switches_at_most_once);
    RUN_TEST(test_thrashing_input_is_rate_limited);
    return UNITY_END();
}
