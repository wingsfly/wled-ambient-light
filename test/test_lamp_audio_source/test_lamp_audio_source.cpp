// test/test_lamp_audio_source/test_lamp_audio_source.cpp
#include <unity.h>
#include "lamp_audio_source.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static SourceInputs micOnly() {
    SourceInputs in;            // 全部默认：三路都不活跃，只有 mic_ok = true
    return in;
}

// ── Snapcast ──────────────────────────────────────────────

void test_snap_inactive_when_not_streaming(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = false;
    in.snap_audible   = true;   // 没有流，再响也不算
    TEST_ASSERT_FALSE(a.evaluateActivity(0, in).snap);
}

void test_snap_active_when_streaming_and_audible(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = true;
    TEST_ASSERT_TRUE(a.evaluateActivity(0, in).snap);
}

// 有流但静音：5 秒内仍算活跃，超过才失活。
void test_snap_survives_silence_until_timeout(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = false;
    TEST_ASSERT_TRUE (a.evaluateActivity(0,    in).snap);
    TEST_ASSERT_TRUE (a.evaluateActivity(4999, in).snap);
    TEST_ASSERT_FALSE(a.evaluateActivity(5000, in).snap);  // 边界：等于阈值即失活
}

// 静音计时必须能被重新出声重置。
void test_snap_silence_timer_resets_on_audio(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = false;
    a.evaluateActivity(0, in);
    in.snap_audible = true;
    a.evaluateActivity(4000, in);        // 出声，计时重置
    in.snap_audible = false;
    TEST_ASSERT_TRUE(a.evaluateActivity(8000, in).snap);   // 距重置才 4 秒
}

// ── 3.5mm 线路 ────────────────────────────────────────────

void test_line_inactive_when_not_detected(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.line_detected = false;
    in.line_audible  = true;    // 没插上，再响也不算
    TEST_ASSERT_FALSE(a.evaluateActivity(0, in).line);
}

void test_line_survives_silence_until_timeout(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = false;
    TEST_ASSERT_TRUE (a.evaluateActivity(0,     in).line);
    TEST_ASSERT_TRUE (a.evaluateActivity(9999,  in).line);
    TEST_ASSERT_FALSE(a.evaluateActivity(10000, in).line);
}

// LINE_ALWAYS：插着就算活跃，完全不看信号。
void test_line_always_mode_ignores_silence(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = false;
    TEST_ASSERT_TRUE(a.evaluateActivity(0,       in).line);
    TEST_ASSERT_TRUE(a.evaluateActivity(3600000, in).line);   // 一小时后照样活跃
}

// LINE_MANUAL_ONLY 只影响目标选择，不影响物理活跃。这条只覆盖「有信号」的一半 ——
// 那正是 MANUAL_ONLY 与 ALWAYS 结论相同的场景，判别力在下面的超时测试里。
void test_manual_only_active_when_line_audible(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_MANUAL_ONLY;
    Arbiter a{cfg};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = true;
    TEST_ASSERT_TRUE(a.evaluateActivity(0, in).line);
}

// 与 test_snap_silence_timer_resets_on_audio 对称。缺了这条，「线路判定完全无视
// line_audible」这个变异体能全绿存活 —— 后果是 3.5mm 插上活跃 10 秒后，无论有没有
// 音乐都永久失活，整整一路功能崩溃。
void test_line_silence_timer_resets_on_audio(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = false;
    a.evaluateActivity(0, in);
    in.line_audible = true;
    a.evaluateActivity(9000, in);                            // 出声，计时重置
    in.line_audible = false;
    TEST_ASSERT_TRUE(a.evaluateActivity(18000, in).line);    // 距重置才 9 秒
}

// 判别性场景：MANUAL_ONLY 与 ALWAYS 只有在「静音且跨过 10 秒」时结论才不同 ——
// 正确实现失活，而「把策略泄漏进物理判定」的版本仍活跃。
// 上面那条 test_manual_only_active_when_line_audible 分辨不出两者。
void test_manual_only_still_honors_silence_timeout(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_MANUAL_ONLY;
    Arbiter a{cfg};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = false;
    TEST_ASSERT_TRUE (a.evaluateActivity(0,     in).line);
    TEST_ASSERT_FALSE(a.evaluateActivity(10000, in).line);
}

// 拔出必须解除计时。缺了这条，删掉 !present 分支里的 armed=false 能全绿存活。
// 真实后果：插着静音 9 秒 → 拔出 → 立刻插回（仍静音），新插入只剩 1 秒宽限；
// 若拔出超过 10 秒再插回，新线缆宽限直接为零。
void test_unplug_resets_line_silence_timer(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = false;
    a.evaluateActivity(0, in);                               // 起计时
    in.line_detected = false;
    TEST_ASSERT_FALSE(a.evaluateActivity(9000, in).line);    // 拔出
    in.line_detected = true;
    TEST_ASSERT_TRUE (a.evaluateActivity(9001,  in).line);   // 插回，重获完整宽限
    TEST_ASSERT_TRUE (a.evaluateActivity(19000, in).line);   // 距插回才 9999ms
    TEST_ASSERT_FALSE(a.evaluateActivity(19001, in).line);   // 满 10000ms
}

// ALWAYS 分支里那行清计时器不是冗余。它只在配置中途切换时有意义：
// FALLBACK（起计时）→ ALWAYS（清掉）→ FALLBACK（重获完整宽限）。
// 这是有意的取舍 —— 改配置不该立即引发换源。另一种写法（ALWAYS 期间照常计时、
// 只是判定时不看）会在切回瞬间直接掉到 mic。没有这条测试，Task 2 很容易把那行
// 当冗余删掉。
void test_always_mode_clears_line_timer_so_switching_back_regrants_grace(void) {
    ArbiterConfig cfg;                                       // 默认 FALLBACK
    Arbiter a{cfg};
    SourceInputs in = micOnly();
    in.line_detected = true;
    in.line_audible  = false;
    TEST_ASSERT_TRUE(a.evaluateActivity(0, in).line);        // FALLBACK 下起计时

    cfg.line_mode = LINE_ALWAYS;
    a.setConfig(cfg);
    TEST_ASSERT_TRUE(a.evaluateActivity(30000, in).line);    // 这一步清掉计时器

    cfg.line_mode = LINE_FALLBACK;
    a.setConfig(cfg);
    TEST_ASSERT_TRUE (a.evaluateActivity(30001, in).line);   // 重获完整宽限
    TEST_ASSERT_TRUE (a.evaluateActivity(40000, in).line);   // 距 30001 才 9999ms
    TEST_ASSERT_FALSE(a.evaluateActivity(40001, in).line);
}

// ── 麦克风 ────────────────────────────────────────────────

void test_mic_follows_mic_ok(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    TEST_ASSERT_TRUE(a.evaluateActivity(0, in).mic);
    in.mic_ok = false;                                  // 驱动检出无数据 >3s 时置否
    TEST_ASSERT_FALSE(a.evaluateActivity(0, in).mic);
}

// ── 时钟回绕 ──────────────────────────────────────────────

// millis() 在 49.7 天后回绕。无符号差值运算天然正确，但必须有测试钉住 ——
// 这类 bug 只在连续运行七周后出现，靠人工是发现不了的。
void test_silence_timer_survives_millis_wraparound(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = false;
    const uint32_t near_wrap = 0xFFFFF000u;             // 距回绕 4096ms
    TEST_ASSERT_TRUE(a.evaluateActivity(near_wrap, in).snap);
    TEST_ASSERT_TRUE(a.evaluateActivity(near_wrap + 4999u, in).snap);   // 已回绕
    TEST_ASSERT_FALSE(a.evaluateActivity(near_wrap + 5000u, in).snap);
}

// 上一条测试站在 0xFFFFF000，正好从下面这个洞上面跨过去 —— 读起来像覆盖了回绕，
// 其实没碰到真正危险的那一刻。曾经的实现拿 0xFFFFFFFF 当「未计时」哨兵，
// 而 millis() 每 49.7 天恰好返回一次这个值：静音若从那一毫秒开始，计时器会被
// 误判为未启动并重新计时。这条测试直接站在洞上。
void test_silence_timer_works_when_starting_at_max_uint32(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = false;
    const uint32_t t0 = 0xFFFFFFFFu;
    TEST_ASSERT_TRUE (a.evaluateActivity(t0, in).snap);
    TEST_ASSERT_TRUE (a.evaluateActivity(t0 + 4999u, in).snap);
    TEST_ASSERT_FALSE(a.evaluateActivity(t0 + 5000u, in).snap);
}

// elapsedAtLeast 上面写着「别改成有符号比较」，但在 Task 3 的变异重跑之前没有任何
// 测试钉住它 —— 上面两条回绕测试跨的都是几秒量级的间隔，那里有符号和无符号取值
// 完全相同，改成 (int32_t) 照样全绿。差别只在**经过时长**跨过 2^31ms 时出现。
// 这个时长是可达的：源一旦 present 且持续静音，t.since 就再也不更新（重置只发生在
// 出声或拔出），失活之后计时器仍原地挂着。于是插着 3.5mm 不放音乐，或 Snapcast
// 挂着静音流，24.85 天后差值变成负数，已经失活的源会自己复活再活 24.85 天。
void test_silence_timeout_does_not_revive_after_24_days(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = false;
    TEST_ASSERT_TRUE (a.evaluateActivity(0, in).snap);
    TEST_ASSERT_FALSE(a.evaluateActivity(5000, in).snap);
    TEST_ASSERT_FALSE(a.evaluateActivity(0x80000000u, in).snap);   // 24.85 天，不得复活
}

// 文档声称同一个 now_ms 重复调用是幂等的，但没有测试钉住。补上。
void test_evaluate_activity_is_idempotent_for_same_timestamp(void) {
    Arbiter a{ArbiterConfig{}};
    SourceInputs in = micOnly();
    in.snap_streaming = true;
    in.snap_audible   = false;
    for (int i = 0; i < 5; ++i) TEST_ASSERT_TRUE(a.evaluateActivity(1000, in).snap);
    TEST_ASSERT_TRUE (a.evaluateActivity(5999, in).snap);   // 距 1000 才 4999ms
    TEST_ASSERT_FALSE(a.evaluateActivity(6000, in).snap);
}

// ── 目标选择与滞回 ────────────────────────────────────────

static SourceInputs all(bool snap, bool line, bool mic = true) {
    SourceInputs in;
    in.snap_streaming = snap; in.snap_audible = snap;
    in.line_detected  = line; in.line_audible = line;
    in.mic_ok = mic;
    return in;
}

// 开机时 current 是 SRC_NONE，第一路可用的源必须立刻接管 —— 否则灯要黑 1.5 秒。
void test_first_source_commits_immediately(void) {
    Arbiter a{ArbiterConfig{}};
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, a.update(0, all(false, false)).to);
}

// 升优先级：必须连续满足 1.5 秒。
void test_rising_priority_requires_hold(void) {
    Arbiter a{ArbiterConfig{}};
    a.update(0, all(false, false));                                  // 落在 MIC
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC,  a.update(1000, all(false, true)).to);
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC,  a.update(2499, all(false, true)).to);
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, a.update(2500, all(false, true)).to);  // 满 1500ms
}

// 候选中途消失，计时必须清零重来。
void test_rise_timer_resets_when_candidate_drops(void) {
    Arbiter a{ArbiterConfig{}};
    a.update(0, all(false, false));
    a.update(1000, all(false, true));                 // LINE 开始计时
    a.update(2000, all(false, false));                // LINE 掉了
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC,  a.update(2600, all(false, true)).to);  // 重新计时
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, a.update(4100, all(false, true)).to);
}

// 当前源死掉 → 立即回落，不等 1.5 秒。歧义①的直接断言。
void test_fallback_is_immediate(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;   // 拔出即刻失活，便于构造
    Arbiter a{cfg};
    a.update(0, all(false, true));                    // 立即落在 LINE
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, a.update(10, all(false, true)).to);
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC,  a.update(20, all(false, false)).to);  // 同一刻回落
}

// 高优先级源在位时，低优先级源上线不抢。
void test_higher_priority_wins(void) {
    Arbiter a{ArbiterConfig{}};
    a.update(0, all(true, false));                    // SNAP
    TEST_ASSERT_EQUAL_UINT8(SRC_SNAPCAST, a.update(5000, all(true, true)).to);
}

// 手动锁定：立即生效，不受滞回约束，且压过更高优先级的活跃源。
void test_manual_lock_overrides_immediately(void) {
    ArbiterConfig cfg; cfg.manual_lock = SRC_MIC;
    Arbiter a{cfg};
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, a.update(0, all(true, true)).to);
}

// 歧义③：锁定指向的源死了也不跑，但如实上报 to_is_active。
void test_manual_lock_on_dead_source_reports_inactive(void) {
    ArbiterConfig cfg; cfg.manual_lock = SRC_LINE;
    Arbiter a{cfg};
    ArbiterOutput o = a.update(0, all(true, false));  // LINE 没插，SNAP 却活着
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, o.to);          // 锁定绝对生效
    TEST_ASSERT_FALSE(o.to_is_active);                // 但如实上报
}

// 歧义②：LINE_MANUAL_ONLY 下自动仲裁永不选中 3.5mm。
void test_manual_only_never_auto_selects_line(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_MANUAL_ONLY;
    Arbiter a{cfg};
    a.update(0, all(false, false));
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, a.update(9999, all(false, true)).to);
    // 只看上面这一刻，名字里的「永不」是句空话：LINE 是 9999 才上线的，
    // 「其实把 LINE 选成了目标」的实现此刻也还困在 1500ms 滞回里，两者不可分辨。
    // 必须跨过滞回窗口再看一眼。（变异验证实测：缺这行，去掉 pickTarget 里
    // MANUAL_ONLY 判断的变异体全绿存活。）
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, a.update(99999, all(false, true)).to);
}

// 但手动锁定仍能选中它。
void test_manual_only_still_allows_manual_lock(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_MANUAL_ONLY; cfg.manual_lock = SRC_LINE;
    Arbiter a{cfg};
    ArbiterOutput o = a.update(0, all(false, true));
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, o.to);
    TEST_ASSERT_TRUE(o.to_is_active);
}

// 三路全灭 → SRC_NONE。下游靠这个信号把音乐类特效切成氛围类。
void test_all_dead_yields_none(void) {
    Arbiter a{ArbiterConfig{}};
    ArbiterOutput o = a.update(0, all(false, false, /*mic=*/false));
    TEST_ASSERT_EQUAL_UINT8(SRC_NONE, o.to);
    TEST_ASSERT_FALSE(o.to_is_active);
}

// ── 以下五条由变异验证补出，每条都对应一个曾经全绿存活的变异体 ──────────

// 歧义③的另一半：锁定的「立即」不只是开机那一次。上面三条锁定测试全都从
// current_ == SRC_NONE 起步，于是「开机立即接管」那一条就顺手把它们全兜住了 ——
// 把 immediate 里的 manual_lock 判断整个删掉，那三条依然全绿。
// 这条从已经落在 MIC 的状态起步，锁定指向优先级更高的 SNAP：少了那个判断，
// 它会被当成普通升优先级白等 1500ms。
void test_manual_lock_commits_immediately_even_when_already_on_another_source(void) {
    ArbiterConfig cfg;
    Arbiter a{cfg};
    a.update(0, all(false, false));                   // 自动落在 MIC
    cfg.manual_lock = SRC_SNAPCAST;
    a.setConfig(cfg);                                 // 用户此刻按下「锁定 Snapcast」
    TEST_ASSERT_EQUAL_UINT8(SRC_SNAPCAST, a.update(1, all(true, false)).to);
}

// MIC 这一路的 to_is_active 单独钉一条：isActive 里那一 case 改读 a.line 也能
// 全绿。锁定到 MIC 是唯一能构造出「to 是 MIC 而 MIC 已死」的办法 —— 自动仲裁
// 下 MIC 一死就被 immediate 立刻换掉了，停不在那个状态上。
//
// 附带一提：这条本来是想钉「to_is_active 描述的是 to 而不是正在计时的候选」，
// 但 immediate 补上 !isActive(act, current_) 之后那已经不可能出错了 ——
// 滞回期内 current_ 必然活着（否则 immediate 就触发了），而 target 也必然活着
// （不活的 target 只能是 SRC_NONE，那又会触发 target > current_），两者恒同。
// 把 isActive(act, current_) 改成 isActive(act, target) 因此成了等价变异体。
void test_mic_activity_is_reported_from_mic_not_line(void) {
    ArbiterConfig cfg; cfg.manual_lock = SRC_MIC;
    Arbiter a{cfg};
    ArbiterOutput o = a.update(0, all(false, true, /*mic=*/false));  // 麦挂了，LINE 活着
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, o.to);
    TEST_ASSERT_FALSE(o.to_is_active);
}

// SNAP 这一路的 to_is_active 之前没有任何断言，isActive 里那一 case 改读 a.mic
// 也能全绿。这里让麦坏掉、Snapcast 在放，两者取值必须相反。
void test_snapcast_activity_is_reported_from_snapcast_not_mic(void) {
    Arbiter a{ArbiterConfig{}};
    ArbiterOutput o = a.update(0, all(true, false, /*mic=*/false));
    TEST_ASSERT_EQUAL_UINT8(SRC_SNAPCAST, o.to);
    TEST_ASSERT_TRUE(o.to_is_active);
}

// 立即提交那条路径也必须清掉正在计时的候选。不清的话，一次回落会把旧计时
// 原样留着，下一次升优先级就吃着这份陈旧时间提前发生 —— 这里提前了 200ms，
// 极端情况下可以直接归零。
void test_immediate_commit_clears_pending_rise_timer(void) {
    Arbiter a{ArbiterConfig{}};
    a.update(0,    all(false, true));        // 落在 LINE
    a.update(1000, all(true,  true));        // SNAP 上线，开始计时
    a.update(1100, all(false, false));       // 两路全掉 → 立即回落到 MIC
    a.update(1200, all(true,  false));       // SNAP 重新上线，计时必须从这里重算
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC,      a.update(2500, all(true, false)).to);  // 距 1200 才 1300ms
    TEST_ASSERT_EQUAL_UINT8(SRC_SNAPCAST, a.update(2700, all(true, false)).to);  // 满 1500ms
}

// 当前源死了但替补优先级更高时，也应立即切换。1500ms 滞回是为了保护一个
// **正在工作**的源不被短暂毛刺抢走 —— 当前源已死就没什么可保护的。
// 纯数值比较 (target > current_) 抓不到这一情形：MIC(2) 挂掉而 LINE(1) 活着时
// 1 > 2 不成立，会白等 1.5 秒的氛围模式。
void test_dead_current_yields_immediately_even_to_higher_priority(void) {
    Arbiter a{ArbiterConfig{}};
    a.update(0, all(false, false));                          // 落在 MIC
    SourceInputs in = all(false, true, /*mic=*/false);       // 麦挂了，LINE 活着
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, a.update(10, in).to);  // 同一刻就走
}

// 「当前源活着、但被策略排除」也必须立即提交。这一情形 !isActive 抓不到：
// LINE 物理上仍然活跃，只是 MANUAL_ONLY 不许自动选它，靠的是数值比较
// MIC(2) > LINE(1)。这正是 immediate 里那两条判据缺一不可的原因 ——
// 变异验证实测：补上 !isActive 之后，原先杀掉「删数值比较」的那批测试全被
// 兜住了，没有这一条那个变异体就复活了。
void test_live_but_policy_excluded_source_yields_immediately(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    a.update(0, all(false, true));                    // 落在 LINE，物理活跃
    cfg.line_mode = LINE_MANUAL_ONLY;                 // 改设置；LINE 仍插着且有声
    a.setConfig(cfg);
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, a.update(10, all(false, true)).to);
}

// ── 交叉淡入 ──────────────────────────────────────────────

// 原 test_no_transition_yet_so_from_equals_to_and_weight_is_one 的续任。
// Task 2 用它钉住「本 Task 还没有过渡」，就是为了逼 Task 3 显式面对这一条而不是让
// from/weight 悄悄漂走。Task 3 面对了，结论是**断言一字不改**：开机第一帧从
// SRC_NONE 起步，没有东西可淡出，本来就该是满权重的稳态。改的只有名字和理由 ——
// 原来的名字声称的是「本 Task 还没实现」，那个理由已经过期了。
void test_steady_state_has_no_crossfade(void) {
    Arbiter a{ArbiterConfig{}};
    ArbiterOutput o = a.update(0, all(false, false));
    TEST_ASSERT_EQUAL_UINT8(o.to, o.from);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
}

// 切换瞬间权重为 0，300ms 后为 1，中点为 0.5。
void test_crossfade_ramps_over_300ms(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    a.update(0, all(false, false));                      // MIC
    a.update(1000, all(false, true));                    // LINE 起计时
    ArbiterOutput o = a.update(2500, all(false, true));  // 满 1500ms，提交
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC,  o.from);
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, o.to);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, o.weight);

    o = a.update(2650, all(false, true));                // 过去 150ms
    TEST_ASSERT_EQUAL_FLOAT(0.5f, o.weight);

    o = a.update(2800, all(false, true));                // 满 300ms
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
    TEST_ASSERT_EQUAL_UINT8(o.to, o.from);               // 过渡结束，from 归位
}

// 过渡结束后不得重新开始。
void test_crossfade_stays_finished(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    a.update(0, all(false, false));
    a.update(1000, all(false, true));
    a.update(2500, all(false, true));
    a.update(2800, all(false, true));
    ArbiterOutput o = a.update(9999, all(false, true));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
    TEST_ASSERT_EQUAL_UINT8(o.to, o.from);
}

// crossfade_ms = 0 时必须是硬切，不能除零。
void test_zero_crossfade_is_hard_cut(void) {
    ArbiterConfig cfg; cfg.crossfade_ms = 0; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    a.update(0, all(false, false));
    a.update(1000, all(false, true));
    ArbiterOutput o = a.update(2500, all(false, true));
    TEST_ASSERT_EQUAL_UINT8(SRC_LINE, o.to);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
    TEST_ASSERT_EQUAL_UINT8(o.to, o.from);
}

// 权重必须单调不减，且恒在 [0,1] —— 逐点断言看不见的那类错误。
void test_crossfade_weight_is_monotonic_and_bounded(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    a.update(0, all(false, false));
    a.update(1000, all(false, true));
    float prev = -1.0f;
    for (uint32_t t = 2500; t <= 2900; t += 10) {
        ArbiterOutput o = a.update(t, all(false, true));
        TEST_ASSERT_TRUE(o.weight >= 0.0f && o.weight <= 1.0f);
        TEST_ASSERT_TRUE(o.weight >= prev);
        prev = o.weight;
    }
    TEST_ASSERT_EQUAL_FLOAT(1.0f, prev);
}

// 时钟回绕期间发生切换，权重不得跳变。
void test_crossfade_survives_millis_wraparound(void) {
    ArbiterConfig cfg; cfg.line_mode = LINE_ALWAYS;
    Arbiter a{cfg};
    const uint32_t base = 0xFFFFFF00u;                   // 距回绕 256ms
    a.update(base, all(false, false));
    a.update(base + 1u, all(false, true));
    a.update(base + 1501u, all(false, true));            // 提交，此时已回绕
    ArbiterOutput o = a.update(base + 1651u, all(false, true));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, o.weight);
}

// 上面那条 test_steady_state_has_no_crossfade 只覆盖了「无过渡」契约的一半：它走的是
// 开机提交到 MIC 的路径，commit() 会把 fading_ 显式写成 false，于是它兜不住
// 「fading_ 的初值本身就是错的」。三路全灭时 target == current_ == SRC_NONE，
// commit() 一次都不会被调用，输出组装读到的完全是那个初值 —— 这是整个类里唯一
// 到达得了「从未 commit 过」这条路径的场景，而 test_all_dead_yields_none 只断言了
// to 和 to_is_active，没碰 from/weight。
// （变异验证实测：把 fading_ 初始化成 true，除这一条外全绿。真实后果是开机时麦
// 驱动还没起来又什么都没插，头 300ms 下游会拿到一个 SRC_NONE → SRC_NONE 的假
// 过渡和一个分数权重。）
void test_no_transition_reported_when_nothing_ever_commits(void) {
    Arbiter a{ArbiterConfig{}};
    ArbiterOutput o = a.update(0, all(false, false, /*mic=*/false));
    TEST_ASSERT_EQUAL_UINT8(SRC_NONE, o.to);
    TEST_ASSERT_EQUAL_UINT8(SRC_NONE, o.from);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
    o = a.update(150, all(false, false, /*mic=*/false));   // 仍在 300ms 窗口内
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
}

// 开机从 SRC_NONE 起步时不该淡入 —— 没有东西可淡出，第一帧就该是满权重。
void test_boot_from_none_does_not_fade(void) {
    Arbiter a{ArbiterConfig{}};
    ArbiterOutput o = a.update(0, all(false, false));
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, o.to);
    TEST_ASSERT_EQUAL_UINT8(SRC_MIC, o.from);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, o.weight);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_snap_inactive_when_not_streaming);
    RUN_TEST(test_snap_active_when_streaming_and_audible);
    RUN_TEST(test_snap_survives_silence_until_timeout);
    RUN_TEST(test_snap_silence_timer_resets_on_audio);
    RUN_TEST(test_line_inactive_when_not_detected);
    RUN_TEST(test_line_survives_silence_until_timeout);
    RUN_TEST(test_line_always_mode_ignores_silence);
    RUN_TEST(test_manual_only_active_when_line_audible);
    RUN_TEST(test_line_silence_timer_resets_on_audio);
    RUN_TEST(test_manual_only_still_honors_silence_timeout);
    RUN_TEST(test_unplug_resets_line_silence_timer);
    RUN_TEST(test_always_mode_clears_line_timer_so_switching_back_regrants_grace);
    RUN_TEST(test_mic_follows_mic_ok);
    RUN_TEST(test_silence_timer_survives_millis_wraparound);
    RUN_TEST(test_silence_timer_works_when_starting_at_max_uint32);
    RUN_TEST(test_silence_timeout_does_not_revive_after_24_days);
    RUN_TEST(test_evaluate_activity_is_idempotent_for_same_timestamp);
    RUN_TEST(test_first_source_commits_immediately);
    RUN_TEST(test_rising_priority_requires_hold);
    RUN_TEST(test_rise_timer_resets_when_candidate_drops);
    RUN_TEST(test_fallback_is_immediate);
    RUN_TEST(test_higher_priority_wins);
    RUN_TEST(test_manual_lock_overrides_immediately);
    RUN_TEST(test_manual_lock_on_dead_source_reports_inactive);
    RUN_TEST(test_manual_only_never_auto_selects_line);
    RUN_TEST(test_manual_only_still_allows_manual_lock);
    RUN_TEST(test_all_dead_yields_none);
    RUN_TEST(test_manual_lock_commits_immediately_even_when_already_on_another_source);
    RUN_TEST(test_mic_activity_is_reported_from_mic_not_line);
    RUN_TEST(test_snapcast_activity_is_reported_from_snapcast_not_mic);
    RUN_TEST(test_immediate_commit_clears_pending_rise_timer);
    RUN_TEST(test_dead_current_yields_immediately_even_to_higher_priority);
    RUN_TEST(test_live_but_policy_excluded_source_yields_immediately);
    RUN_TEST(test_steady_state_has_no_crossfade);
    RUN_TEST(test_crossfade_ramps_over_300ms);
    RUN_TEST(test_crossfade_stays_finished);
    RUN_TEST(test_zero_crossfade_is_hard_cut);
    RUN_TEST(test_crossfade_weight_is_monotonic_and_bounded);
    RUN_TEST(test_crossfade_survives_millis_wraparound);
    RUN_TEST(test_no_transition_reported_when_nothing_ever_commits);
    RUN_TEST(test_boot_from_none_does_not_fade);
    return UNITY_END();
}
