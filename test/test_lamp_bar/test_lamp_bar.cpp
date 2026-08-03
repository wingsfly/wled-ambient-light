#include <unity.h>
#include <math.h>
#include "lamp_bar.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static BarState  S;
static BarConfig C;

static void reset(void) { C = BarConfig{}; barInit(S); }

// 把一拍拆成 N 帧喂进去，相位 0→1 走一圈。
// **必须逐帧喂**，不能直接跳变 —— 拍点判据靠的是相位回绕，
// 一帧一拍的话相位从没「跨过」过，整个模块不会动。
static const int FPB = 10;      // 每拍帧数

// 返回这一拍里是否报出了强拍
static bool beatOnce(float accent, bool locked = true) {
    bool db = false;
    for (int k = 0; k < FPB; ++k) {
        // 相位从 0.05 走到 0.95，再回绕
        const float ph = (float)((k + 1) % FPB) / (float)FPB;
        barUpdate(S, C, ph, locked, accent);
        if (S.downbeat) db = true;
    }
    return db;
}

// 跑 bars 个小节，每小节按 pat[] 给重音
static void play(const float *pat, int m, int bars) {
    for (int b = 0; b < bars; ++b)
        for (int i = 0; i < m; ++i) beatOnce(pat[i]);
}

// 强拍报在模式的第几个位置上？-1 表示一次都没报。
//
// **不要去断言 `offset` 的具体数值。** beat_ix 的绝对相位取决于「从哪一拍
// 开始收音」，是任意的；offset 的作用正是把它吸收掉。真正的性质是
// 「强拍落在底鼓那一拍上」，与索引约定无关。第一版断言 offset==0，
// 五条测试一起红 —— 红的是夹具约定，不是实现。
static int downbeatSlot(const float *pat, int m, int bars) {
    int slot = -1, n = 0;
    for (int b = 0; b < bars; ++b)
        for (int i = 0; i < m; ++i)
            if (beatOnce(pat[i])) { slot = i; ++n; }
    return n ? slot : -1;
}

// ── 基本行为 ──────────────────────────────────────────────

void test_no_beat_reported_without_a_phase_wrap(void) {
    // 相位单调上升但没回绕 —— 一个拍点都不该报。
    reset();
    for (int k = 1; k <= 9; ++k) barUpdate(S, C, (float)k / 10.0f, true, 1.0f);
    TEST_ASSERT_FALSE_MESSAGE(S.beat, "相位没回绕不算一拍");
    TEST_ASSERT_EQUAL_INT(0, S.beat_ix);
}

void test_phase_wrap_counts_a_beat(void) {
    reset();
    barUpdate(S, C, 0.8f, true, 1.0f);
    barUpdate(S, C, 0.9f, true, 1.0f);
    TEST_ASSERT_FALSE(S.beat);
    barUpdate(S, C, 0.1f, true, 1.0f);          // 回绕
    TEST_ASSERT_TRUE_MESSAGE(S.beat, "相位回绕应当算一拍");
    TEST_ASSERT_EQUAL_INT(1, S.beat_ix);
}

void test_nothing_happens_while_the_beat_is_unlocked(void) {
    // 拍都不准，谈何强拍。锁不上时整个模块停摆。
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    for (int b = 0; b < 40; ++b)
        for (int i = 0; i < 4; ++i) beatOnce(pat[i], false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, S.beat_ix, "没锁定就不该数拍");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, S.conf);
}

void test_no_verdict_during_warmup(void) {
    // 攒够 8 拍之前不下结论 —— 前两拍就定强拍是武断的。
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    for (int i = 0; i < 4; ++i) beatOnce(pat[i]);      // 只有 4 拍
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, S.conf, "热身期内不该有置信度");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, S.bar_ix, "热身期内不该数小节");
}

// ── 强拍定位 ──────────────────────────────────────────────

void test_finds_the_downbeat_when_the_kick_is_on_one(void) {
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};    // 底鼓在第一拍
    play(pat, 4, 12);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, S.beats_per_bar, "应当判成 4/4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, downbeatSlot(pat, 4, 12),
        "强拍应当报在底鼓那一拍上");
    TEST_ASSERT_TRUE_MESSAGE(S.conf > 0.7f, "重音这么明显，置信度应当高");
}

void test_finds_the_downbeat_at_any_offset(void) {
    // 从小节的第三拍开始播。真实场景里「开始收音的那一刻」是任意的，
    // 强拍不会正好落在 beat_ix % 4 == 0 上。
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    for (int i = 2; i < 4; ++i) beatOnce(pat[i]);     // 从小节中间起手
    play(pat, 4, 12);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, S.beats_per_bar, "应当判成 4/4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, downbeatSlot(pat, 4, 12),
        "无论从哪一拍开始收音，强拍都要落在底鼓上");
}

void test_downbeat_fires_once_per_bar(void) {
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(pat, 4, 12);                 // 先让它定下来
    int fired = 0;
    const int bars = 10;
    for (int b = 0; b < bars; ++b)
        for (int i = 0; i < 4; ++i) if (beatOnce(pat[i])) ++fired;
    TEST_ASSERT_EQUAL_INT_MESSAGE(bars, fired, "每小节应当恰好报一次强拍");
}

void test_bar_position_walks_0123(void) {
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(pat, 4, 12);
    int seen[4] = {0, 0, 0, 0};
    for (int b = 0; b < 8; ++b)
        for (int i = 0; i < 4; ++i) { beatOnce(pat[i]); seen[S.pos]++; }
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(8, seen[i], "小节内位置应当均匀走遍 0..3");
}

void test_bar_index_advances(void) {
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(pat, 4, 12);
    const int b0 = S.bar_ix;
    play(pat, 4, 8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, S.bar_ix - b0, "小节计数应当跟着走");
}

// ── 拍号判别 ──────────────────────────────────────────────

void test_waltz_is_detected_as_three_four(void) {
    reset();
    const float pat[3] = {1.0f, 0.15f, 0.15f};        // 强弱弱
    play(pat, 3, 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, S.beats_per_bar, "华尔兹应当判成 3/4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, downbeatSlot(pat, 3, 12), "强拍在第一拍");
}

void test_four_four_is_the_default_under_ambiguity(void) {
    // 完全均匀的重音：两组累加器的对比度都接近 0，3 格不该靠「格子少」赢。
    reset();
    const float pat[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    play(pat, 4, 30);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, S.beats_per_bar, "分不出来时应当留在 4/4");
    TEST_ASSERT_TRUE_MESSAGE(S.conf < 0.2f, "均匀重音的置信度应当很低");
}

void test_four_four_is_not_mistaken_for_three_four(void) {
    // 这条是冲着「3 格天然占便宜」去的。4/4 的底鼓模式喂给 3 格累加器时，
    // 由于 4 与 3 互质，重音会均匀摊到三格上 —— 对比度反而低。
    // 但若少了那道 margin，边界情况仍会翻车。
    reset();
    const float pat[4] = {1.0f, 0.2f, 0.5f, 0.2f};    // 典型 kick-snare
    play(pat, 4, 40);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, S.beats_per_bar, "4/4 不能被判成 3/4");
    TEST_ASSERT_TRUE_MESSAGE(S.conf > 0.5f, "正对照：这个模式确实识别得出");
}

void test_switching_metre_resets_the_bar_count(void) {
    // 换拍号时继续沿用旧计数会让乐句级效果错位。
    reset();
    const float four[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(four, 4, 20);
    TEST_ASSERT_EQUAL_INT(4, S.beats_per_bar);
    TEST_ASSERT_TRUE(S.bar_ix > 10);
    const float three[3] = {1.0f, 0.1f, 0.1f};
    play(three, 3, 60);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, S.beats_per_bar, "应当切到 3/4");
    TEST_ASSERT_TRUE_MESSAGE(S.bar_ix < 60, "换拍号时小节计数应当归零");
}

// ── 遗忘 ──────────────────────────────────────────────────

void test_the_downbeat_can_move(void) {
    // 换了一首歌、强拍换了位置，累加器要能忘掉旧的。
    reset();
    const float a[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(a, 4, 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, downbeatSlot(a, 4, 10), "先认在底鼓上");
    // 现在把重音挪到模式的第 2 格
    const float b[4] = {0.1f, 0.1f, 1.0f, 0.3f};
    play(b, 4, 40);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, downbeatSlot(b, 4, 12),
        "强拍换位置之后应当跟过去");
}

void test_forgetting_is_measured_in_beats_not_seconds(void) {
    // 这条是本模块**刻意与其它模块相反**的地方：遗忘按拍算。
    //
    // 同样播 32 拍，一次每拍 10 帧、一次每拍 40 帧（相当于 BPM 差四倍）。
    // 累加器的状态必须一致 —— 该记住的是「多少小节」，不是「多少秒」。
    float got[2][4];
    const int fpb[2] = {10, 40};
    const float pat[4] = {1.0f, 0.15f, 0.4f, 0.15f};
    for (int r = 0; r < 2; ++r) {
        reset();
        for (int b = 0; b < 8; ++b)
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < fpb[r]; ++k)
                    barUpdate(S, C, (float)((k + 1) % fpb[r]) / (float)fpb[r], true, pat[i]);
        for (int i = 0; i < 4; ++i) got[r][i] = S.acc[0][i];
    }
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, got[0][i], got[1][i],
            "同样的拍数，累加器状态应当一致（与帧率、与 BPM 无关）");
    // 正对照：累加器必须真的有区分度（哪一格无所谓，索引相位是任意的）
    float mx = got[0][0], mn = got[0][0];
    for (int i = 1; i < 4; ++i) { if (got[0][i] > mx) mx = got[0][i]; if (got[0][i] < mn) mn = got[0][i]; }
    TEST_ASSERT_TRUE_MESSAGE(mx > mn * 2.0f, "正对照：重音确实被记下了");
}

// ── 稳健性 ────────────────────────────────────────────────

void test_negative_and_nan_accent_are_dropped(void) {
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(pat, 4, 20);
    const float before = S.acc[0][0];
    for (int i = 0; i < 4; ++i) beatOnce(NAN);
    for (int i = 0; i < 4; ++i) beatOnce(-5.0f);
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_TRUE_MESSAGE(isfinite(S.acc[0][i]), "NaN 不能渗进累加器");
    TEST_ASSERT_TRUE_MESSAGE(S.acc[0][0] < before, "只衰减不累加");
    TEST_ASSERT_TRUE_MESSAGE(S.acc[0][0] >= 0.0f, "累加器不该变负");
}

void test_nan_phase_is_dropped(void) {
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(pat, 4, 20);
    const int ix = S.beat_ix;
    for (int k = 0; k < 20; ++k) barUpdate(S, C, NAN, true, 1.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ix, S.beat_ix, "NaN 相位不该被算成拍点");
}

void test_losing_lock_does_not_fabricate_a_beat(void) {
    // 丢锁再上锁时，相位会从任意值重新开始。不重置 prev_phase 的话
    // 那一下很可能被当成回绕，凭空多一拍、把小节整个错位。
    reset();
    const float pat[4] = {1.0f, 0.1f, 0.3f, 0.1f};
    play(pat, 4, 20);
    const int ix = S.beat_ix;
    barUpdate(S, C, 0.9f, false, 1.0f);       // 丢锁
    barUpdate(S, C, 0.1f, true,  1.0f);       // 重新上锁，相位比丢锁前小
    TEST_ASSERT_EQUAL_INT_MESSAGE(ix, S.beat_ix, "重新上锁那一帧不该凭空多一拍");
}

void test_contrast_of_a_flat_histogram_is_zero(void) {
    float a[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int ix = -1;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, barContrast(a, 4, ix), "均匀分布无对比度");
    float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, barContrast(z, 4, ix), "全零不能除出 NaN");
}

void test_contrast_of_a_single_spike(void) {
    float a[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    int ix = -1;
    // 峰=1、均=0.25 → (1−0.25)/1 = 0.75
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.75f, barContrast(a, 4, ix));
    TEST_ASSERT_EQUAL_INT(0, ix);
    float b[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    barContrast(b, 4, ix);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, ix, "峰位应当被正确报出");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_no_beat_reported_without_a_phase_wrap);
    RUN_TEST(test_phase_wrap_counts_a_beat);
    RUN_TEST(test_nothing_happens_while_the_beat_is_unlocked);
    RUN_TEST(test_no_verdict_during_warmup);
    RUN_TEST(test_finds_the_downbeat_when_the_kick_is_on_one);
    RUN_TEST(test_finds_the_downbeat_at_any_offset);
    RUN_TEST(test_downbeat_fires_once_per_bar);
    RUN_TEST(test_bar_position_walks_0123);
    RUN_TEST(test_bar_index_advances);
    RUN_TEST(test_waltz_is_detected_as_three_four);
    RUN_TEST(test_four_four_is_the_default_under_ambiguity);
    RUN_TEST(test_four_four_is_not_mistaken_for_three_four);
    RUN_TEST(test_switching_metre_resets_the_bar_count);
    RUN_TEST(test_the_downbeat_can_move);
    RUN_TEST(test_forgetting_is_measured_in_beats_not_seconds);
    RUN_TEST(test_negative_and_nan_accent_are_dropped);
    RUN_TEST(test_nan_phase_is_dropped);
    RUN_TEST(test_losing_lock_does_not_fabricate_a_beat);
    RUN_TEST(test_contrast_of_a_flat_histogram_is_zero);
    RUN_TEST(test_contrast_of_a_single_spike);
    return UNITY_END();
}
