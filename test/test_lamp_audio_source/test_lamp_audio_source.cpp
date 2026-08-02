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
    RUN_TEST(test_evaluate_activity_is_idempotent_for_same_timestamp);
    return UNITY_END();
}
