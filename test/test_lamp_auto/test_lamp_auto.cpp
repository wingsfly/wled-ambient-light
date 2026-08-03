#include <unity.h>
#include <math.h>
#include "lamp_auto.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static AutoState  S;
static AutoConfig C;
static const float DT = 23.22f;

static void reset(void) { C = AutoConfig{}; autoInit(S, C, DT); }

// 一帧「什么都不突出」的底噪。各条测试在此之上只改自己关心的那几个字段 ——
// 这样「是这个特征选中了它」才立得住。
// 按给定比例把 bands 拆成谐波/打击两路。
//
// 选择器**从两路能量算打击占比**，不读 f.percussive —— 后者只是给界面看的。
// 夹具必须照着真实数据流来设，否则测的就不是实际会跑的那条路径。
static void setPerc(AudioFrame &f, float ratio) {
    for (int i = 0; i < NUM_BANDS; ++i) {
        f.bands_p[i] = f.bands[i] * ratio;
        f.bands_h[i] = f.bands[i] * (1.0f - ratio);
    }
    f.percussive = ratio;
}

static AudioFrame plain(void) {
    AudioFrame f;
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.25f;   // 平坦谱
    f.rms_fast = 0.3f; f.peak = 0.4f;
    f.bpm = 120.0f; f.bpm_conf = 0.6f; f.beat_locked = true;
    f.key_conf = 0.1f; f.key_root = -1;
    f.f0_voiced = false; f.f0_conf = 0.0f;
    f.energy_trend = 0.0f; f.section_novelty = 0.0f; f.mood = 0.4f;
    return f;
}

// 一直喂同一帧，直到切换稳定下来（或超时）。返回最终选中的效果。
static FxId settle(AudioFrame f, float perc, float ms = 90000.0f) {
    setPerc(f, perc);
    const int n = (int)(ms / DT);
    for (int k = 0; k < n; ++k)
        autoUpdate(S, C, f, (uint32_t)((float)k * DT));
    return S.current;
}

// ── 每个候选都必须可达（第 24 条）──────────────────────────

void test_clear_melody_selects_the_melody_line(void) {
    reset();
    AudioFrame f = plain();
    f.f0_voiced = true; f.f0_hz = 330.0f; f.f0_conf = 0.9f;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_MELODY_LINE, settle(f, 0.15f),
        "人声旋律持续清晰时应当选旋律线");
}

void test_clear_key_without_drums_selects_the_key_wash(void) {
    reset();
    AudioFrame f = plain();
    f.key_conf = 0.9f; f.key_root = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_KEY_WASH, settle(f, 0.05f),
        "调明确、以谐波为主时应当选调性染色");
}

void test_drums_with_a_locked_beat_select_the_bar_impact(void) {
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 0.95f;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_BAR_IMPACT, settle(f, 0.85f),
        "以鼓为主、节拍锁得住时应当选冲击柱");
}

void test_drums_with_a_split_spectrum_select_split_bands(void) {
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 0.95f;
    // 808 在低端、hi-hat 在高端、中间空 —— rap 的典型频谱
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.02f;
    for (int i = 0; i < 4; ++i)               f.bands[i] = 0.9f;
    for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i) f.bands[i] = 0.9f;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_SPLIT_BANDS, settle(f, 0.90f),
        "两端分层明显时应当选高低分离而不是冲击柱");
}

void test_unlockable_beat_selects_the_color_flow(void) {
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 0.05f; f.beat_locked = false;   // rubato / 古典
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_COLOR_FLOW, settle(f, 0.10f),
        "锁不上拍时应当选彩色流动");
}

void test_active_structure_also_selects_the_color_flow(void) {
    // 彩色流动有两条互不相同的入口：锁不上拍、或者段落/走向本身很活跃。
    // 上一条走的是第一条，这条走第二条 —— 两条都得证明可达。
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 0.9f;                    // 拍锁得很牢，走不了 rubato 那条
    f.energy_trend = 0.8f; f.section_novelty = 0.4f;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_COLOR_FLOW, settle(f, 0.10f),
        "段落与走向活跃时也应当选彩色流动");
}

void test_nothing_stands_out_stays_on_the_fallback(void) {
    reset();
    AudioFrame f = plain();
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_SPECTRUM_BARS, settle(f, 0.30f),
        "什么都不突出时应当留在兜底的频段柱上");
}

// ── 判据本身 ──────────────────────────────────────────────

void test_jittery_pitch_is_not_a_melody(void) {
    // 有声占比一样是 100%，唯一的差别是 f0 **稳不稳**。
    //
    // 失真吉他的 power chord 就是这样：HPS 在和弦各分音之间逐帧乱跳，
    // 有声率满格，但画成旋律线是一团噪点。实测半音/秒：流行 6.6、失真吉他 21–23。
    reset();
    AudioFrame steady = plain(), jitter = plain();
    steady.f0_voiced = jitter.f0_voiced = true;
    steady.f0_conf   = jitter.f0_conf   = 0.9f;
    setPerc(steady, 0.15f); setPerc(jitter, 0.15f);
    steady.f0_hz = 330.0f;

    // 稳的：选中旋律线
    for (int k = 0; k < (int)(60000.0f / DT); ++k)
        autoUpdate(S, C, steady, (uint32_t)((float)k * DT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_MELODY_LINE, S.current, "稳定的旋律应当选旋律线");
    TEST_ASSERT_TRUE_MESSAGE(S.f0_jitter < C.f0_jit_lo, "稳定音的抖动率应当很低");

    // 抖的：每帧在 ±0.35 半音之间跳 → 约 30 半音/秒
    reset();
    for (int k = 0; k < (int)(60000.0f / DT); ++k) {
        jitter.f0_hz = 330.0f * powf(2.0f, ((k % 2) ? 0.35f : -0.35f) / 12.0f);
        autoUpdate(S, C, jitter, (uint32_t)((float)k * DT));
    }
    TEST_ASSERT_TRUE_MESSAGE(S.f0_jitter > C.f0_jit_hi, "抖动的音高抖动率应当很高");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(FX_MELODY_LINE, S.current,
        "逐帧乱跳的音高不该被画成旋律线");
}

void test_jitter_is_measured_per_second_not_per_frame(void) {
    // 各档 hop 差 8 倍。按帧算的话，同一段音乐在打点档的抖动率会是氛围档的 1/8 ——
    // 于是「稳不稳」的判据在不同档位下含义完全不同。
    const float dt[2]  = {5.805f, 46.44f};
    float got[2];
    for (int r = 0; r < 2; ++r) {
        C = AutoConfig{}; autoInit(S, C, dt[r]);
        AudioFrame f = plain();
        f.f0_voiced = true; f.f0_conf = 0.9f; setPerc(f, 0.15f);
        // **同样的物理滑行速率**，与帧率无关：三角波，周期 1.2s、峰峰 12 半音
        // → 斜率恒为 24/1.2 = 20 半音/秒。
        //
        // 第一版用的是锯齿波，结果在 12→0 的回绕处有一个 12 半音的瞬间跳变，
        // 那一帧的速率高达 2000 半音/秒，把均值从 20 拉到了 38 ——
        // 测的是回绕伪影，不是滑行速率。
        const int steps = (int)(30000.0f / dt[r]);
        for (int k = 0; k < steps; ++k) {
            const float ph   = fmodf((float)k * dt[r] / 1200.0f, 1.0f);
            const float semi = 12.0f * (1.0f - fabsf(2.0f * ph - 1.0f));
            f.f0_hz = 330.0f * powf(2.0f, semi / 12.0f);
            autoUpdate(S, C, f, (uint32_t)((float)k * dt[r]));
        }
        got[r] = S.f0_jitter;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.5f, got[0], got[1],
        "同样的物理滑行速率，两档量出的抖动率应当一致");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2.0f, 20.0f, got[0],
        "正对照：三角波斜率 24 半音 / 1.2 秒 = 20 半音/秒");
}

void test_melody_needs_duration_not_a_single_frame(void) {
    // rap 和失真吉他都会时不时冒出一个 f0。按帧判会让灯效来回横跳，
    // 所以判据是**占比**。
    reset();
    AudioFrame on = plain(), off = plain();
    setPerc(on, 0.15f); setPerc(off, 0.15f);
    on.f0_voiced = true; on.f0_hz = 330.0f; on.f0_conf = 0.9f;
    // 每 5 帧只有 1 帧有声 → 占比 0.2，远低于 f0_duty_full=0.55
    for (int k = 0; k < 8000; ++k)
        autoUpdate(S, C, (k % 5 == 0) ? on : off, (uint32_t)((float)k * DT));
    TEST_ASSERT_TRUE_MESSAGE(S.f0_duty < 0.35f, "断续的 f0 不该攒出高占比");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(FX_MELODY_LINE, S.current,
        "偶尔冒一个 f0 不能选中旋律线");
}

void test_melody_loses_to_drums_when_percussion_dominates(void) {
    // 有旋律但打击占比很高（比如带唱段的摇滚）：旋律线的分数要被压下去。
    reset();
    AudioFrame f = plain();
    f.f0_voiced = true; f.f0_hz = 330.0f; f.f0_conf = 0.9f;
    f.bpm_conf = 0.95f;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(FX_MELODY_LINE, settle(f, 0.90f),
        "打击占绝对主导时不该选旋律线");
}

void test_split_needs_a_real_split_not_just_drums(void) {
    // 打击占比一样高，但频谱平坦 —— 高低分离画出来是两根呆立的柱子。
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 0.95f;                       // 谱是平坦的（plain 里就是）
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_BAR_IMPACT, settle(f, 0.85f),
        "谱不分层时应当选冲击柱，不是高低分离");
}

void test_split_contrast_measures_the_ends_against_the_middle(void) {
    float flat[NUM_BANDS], split[NUM_BANDS], middle[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; ++i) { flat[i] = 0.5f; split[i] = 0.02f; middle[i] = 0.02f; }
    for (int i = 0; i < 4; ++i)                     split[i]  = 0.9f;
    for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i) split[i]  = 0.9f;
    for (int i = 6; i < 10; ++i)                    middle[i] = 0.9f;
    TEST_ASSERT_TRUE_MESSAGE(splitContrast(split) > 0.5f, "两端强、中间空 → 高对比");
    TEST_ASSERT_TRUE_MESSAGE(splitContrast(flat) < 0.1f,  "平坦 → 无对比");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, splitContrast(middle),
        "能量全在中间 → 对比为零，不能给出负数");
    float z[NUM_BANDS]; for (int i = 0; i < NUM_BANDS; ++i) z[i] = 0.0f;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-9f, 0.0f, splitContrast(z), "全零不能除出 NaN");
}

// ── 切换纪律 ──────────────────────────────────────────────

void test_a_new_candidate_must_win_for_hold_ms(void) {
    reset();
    AudioFrame f = plain();
    f.f0_voiced = true; f.f0_hz = 330.0f; f.f0_conf = 0.9f; setPerc(f, 0.15f);
    // 只跑 2 秒 —— 远短于 hold_ms=5000
    for (int k = 0; k < (int)(2000.0f / DT); ++k)
        autoUpdate(S, C, f, (uint32_t)((float)k * DT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_SPECTRUM_BARS, S.current,
        "候选还没连续胜出够久就不该切");
}

void test_dwell_blocks_a_second_switch(void) {
    // dwell 是从**上一次切换的时刻**起算的。
    // 第一版这条测试先跑满 20 秒再换素材，那时距上次切换早就超过 15 秒了 ——
    // 它量的是「热身跑了多久」，不是 dwell。必须抓住切换发生的那一刻。
    reset();
    AudioFrame mel = plain();
    mel.f0_voiced = true; mel.f0_hz = 330.0f; mel.f0_conf = 0.9f; setPerc(mel, 0.15f);
    uint32_t t_switch = 0;
    bool got = false;
    for (int k = 0; k < (int)(40000.0f / DT) && !got; ++k) {
        const uint32_t t = (uint32_t)((float)k * DT);
        if (autoUpdate(S, C, mel, t)) { got = true; t_switch = t; }
    }
    TEST_ASSERT_TRUE_MESSAGE(got, "前提：应当先切到旋律线");
    TEST_ASSERT_EQUAL_INT(FX_MELODY_LINE, S.current);

    // 从切换那一刻起喂「以鼓为主」，只喂 12 秒 —— 超过 hold(5s)、不足 dwell(15s)
    AudioFrame drum = plain(); drum.bpm_conf = 0.95f; setPerc(drum, 0.9f);
    int fired = 0, k = 0;
    for (; (float)k * DT < 12000.0f; ++k)
        if (autoUpdate(S, C, drum, t_switch + (uint32_t)((float)k * DT))) ++fired;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fired, "距上次切换不足 dwell_ms 时应当挡住");
    TEST_ASSERT_EQUAL_INT(FX_MELODY_LINE, S.current);

    for (; (float)k * DT < 25000.0f; ++k)
        autoUpdate(S, C, drum, t_switch + (uint32_t)((float)k * DT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_BAR_IMPACT, S.current, "越过 dwell 之后应当切过去");
}

void test_hysteresis_keeps_a_near_tie_from_flapping(void) {
    // 两个候选分数接近时，没有 margin 的话会在 hold_ms 边缘反复交替，
    // 永远凑不满驻留 —— 结果是「音乐明明没变，灯效却在抖」。
    //
    // 构造一个真的接近的平局：冲击柱与高低分离由同一个 split 因子反向分家，
    // split ≈ 0.5 就是它们的交点。
    // 第一版拿 key_conf 卡在门槛上做「平局」，改成真门槛之后两边都是 0 ——
    // 那不是平局，那是没有候选，测试变成空的。
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 1.0f; setPerc(f, 0.85f);
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.10f;
    for (int i = 0; i < 4; ++i)                     f.bands[i] = 0.433f;
    for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i) f.bands[i] = 0.433f;
    settle(f, 0.85f);

    // 自检：这确实是一个平局，否则下面测的就不是滞回
    TEST_ASSERT_TRUE_MESSAGE(fabsf(S.score[2] - S.score[3]) < C.margin,
        "前提：两个候选的分数必须落在 margin 之内");
    TEST_ASSERT_TRUE_MESSAGE(S.score[2] > 0.2f && S.score[3] > 0.2f,
        "前提：两者都得真的在竞争，不能都是零");

    const FxId first = S.current;
    int switches = 0;
    for (int k = 0; k < (int)(120000.0f / DT); ++k) {
        const float e = 0.433f * ((k % 2) ? 1.01f : 0.99f);
        for (int i = 0; i < 4; ++i)                     f.bands[i] = e;
        for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i) f.bands[i] = e;
        setPerc(f, 0.85f);
        if (autoUpdate(S, C, f, (uint32_t)((float)k * DT) + 200000u)) ++switches;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, switches, "分数在交点上抖动不该引起切换");
    TEST_ASSERT_EQUAL_INT(first, S.current);
}

void test_pulsed_drums_still_commit(void) {
    // 回归，真机上抓到的：**判据输入必须是时间平均的，而且要按能量加权。**
    //
    // 120BPM 的鼓，一记底鼓的打击占比能到 0.92，但它只占 500ms 里的 50ms。
    //   · 逐帧看：want 每几帧翻一次，cand_since 不断重置，hold 永远凑不满 ——
    //     实测「打击占比 0.93、节拍也锁上了，45 秒里一次都没切」。
    //   · 平均**比值**：被大量间隙帧稀释到 0.1，「以鼓为主」还是判不出来。
    //   · 平均**能量**再取比值：那一记鼓的份量本来就该更重 —— 这才对。
    //
    // 之前所有测试每帧喂的都是同一个常数，瞬时值与平均值恰好相等，
    // 天然碰不到这个 bug（docs/17 第 20 条的形状）。
    reset();
    AudioFrame hit_f = plain(), gap_f = plain();
    hit_f.bpm_conf = gap_f.bpm_conf = 0.9f;
    for (int i = 0; i < NUM_BANDS; ++i) { hit_f.bands[i] = 0.9f;  gap_f.bands[i] = 0.02f; }
    setPerc(hit_f, 0.95f);      // 击打帧：几乎全是打击能量
    setPerc(gap_f, 0.10f);      // 间隙帧：残响，几乎全是谐波

    // 每 500ms 一记，占空 10%（约合 120BPM 的四踩）
    const int period = (int)(500.0f / DT + 0.5f);
    const int on_n   = (int)(50.0f  / DT + 0.5f);
    for (int k = 0; k < (int)(60000.0f / DT); ++k)
        autoUpdate(S, C, (k % period < on_n) ? hit_f : gap_f, (uint32_t)((float)k * DT));

    TEST_ASSERT_TRUE_MESSAGE(S.current == FX_BAR_IMPACT || S.current == FX_SPLIT_BANDS,
        "脉冲式的鼓也必须被判成「以鼓为主」并真的切过去");
}

void test_two_close_challengers_still_get_committed(void) {
    // 契约测试（不杀变异体，明说）：两个咬得很紧的候选也必须定下来一个。
    //
    // 写这条是因为我一度以为真机上「rap 打击占比 0.93 却停在频段柱」是
    // 挑战者逐帧交替、cand_since 反复重置造成的。**诊断是错的** ——
    // 挑选循环的基准是 `best + margin`，本来就带这层滞回；补的「修复」是死代码，
    // 加不加这条测试都过。撤掉了那段代码，留下这条测试把这个性质钉住。
    reset();
    AudioFrame f = plain();
    f.bpm_conf = 1.0f; setPerc(f, 0.85f);
    // split ≈ 0.5，正是冲击柱与高低分离的交点
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = 0.10f;
    for (int i = 0; i < 4; ++i)                     f.bands[i] = 0.433f;
    for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i) f.bands[i] = 0.433f;

    // 让两者的分数每帧极微幅互换领先
    for (int k = 0; k < (int)(40000.0f / DT); ++k) {
        const float e = 0.433f * ((k % 2) ? 1.01f : 0.99f);
        for (int i = 0; i < 4; ++i)                     f.bands[i] = e;
        for (int i = NUM_BANDS - 4; i < NUM_BANDS; ++i) f.bands[i] = e;
        setPerc(f, 0.85f);
        autoUpdate(S, C, f, (uint32_t)((float)k * DT));
    }
    TEST_ASSERT_TRUE_MESSAGE(fabsf(S.score[2] - S.score[3]) < C.margin,
        "前提：两个候选确实咬得很紧");
    TEST_ASSERT_TRUE_MESSAGE(S.current == FX_BAR_IMPACT || S.current == FX_SPLIT_BANDS,
        "两个强候选咬得紧，也必须定下来一个，不能一直挂在兜底上");
}

void test_falls_back_when_the_music_stops_standing_out(void) {
    // 人声停了、鼓也停了，灯效不能僵在旋律线上不动。
    reset();
    AudioFrame mel = plain();
    mel.f0_voiced = true; mel.f0_hz = 330.0f; mel.f0_conf = 0.9f; setPerc(mel, 0.15f);
    settle(mel, 0.15f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_MELODY_LINE, S.current, "前提：先切到旋律线");

    AudioFrame dull = plain();          // 什么都不突出
    for (int k = 0; k < (int)(90000.0f / DT); ++k)
        autoUpdate(S, C, dull, 300000u + (uint32_t)((float)k * DT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(FX_SPECTRUM_BARS, S.current,
        "现任自己掉到门槛以下时应当退回兜底");
}

void test_switch_is_reported_exactly_once(void) {
    reset();
    AudioFrame f = plain();
    f.f0_voiced = true; f.f0_hz = 330.0f; f.f0_conf = 0.9f; setPerc(f, 0.15f);
    int fired = 0;
    for (int k = 0; k < (int)(60000.0f / DT); ++k)
        if (autoUpdate(S, C, f, (uint32_t)((float)k * DT))) ++fired;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fired, "一次切换应当只报一次");
}

// ── 时间与稳健性 ──────────────────────────────────────────

void test_duty_window_is_defined_in_physical_time(void) {
    const float dt[2]  = {5.805f, 46.44f};
    const int   stp[2] = {320, 40};      // 同为 1857.6ms
    float got[2];
    for (int r = 0; r < 2; ++r) {
        C = AutoConfig{}; autoInit(S, C, dt[r]);
        AudioFrame f = plain();
        f.f0_voiced = true; f.f0_hz = 330.0f; f.f0_conf = 0.9f; setPerc(f, 0.15f);
        for (int k = 0; k < stp[r]; ++k)
            autoUpdate(S, C, f, (uint32_t)((float)k * dt[r]));
        got[r] = S.f0_duty;
    }
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, dt[0]*stp[0], dt[1]*stp[1], "总时长必须相等");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, got[0], got[1],
        "同样的物理时长，快档与慢档的 f0 占比应当一致");
    TEST_ASSERT_TRUE_MESSAGE(got[0] > 0.2f && got[0] < 0.9f, "必须停在爬升中段");
}

void test_scores_stay_finite_on_degenerate_input(void) {
    reset();
    AudioFrame f = plain();
    for (int i = 0; i < NUM_BANDS; ++i) f.bands[i] = NAN;
    f.bpm_conf = NAN; f.key_conf = NAN; f.energy_trend = NAN; f.section_novelty = NAN;
    setPerc(f, NAN);
    autoUpdate(S, C, f, 1000);
    for (int i = 0; i < kAutoCands; ++i)
        TEST_ASSERT_TRUE_MESSAGE(isfinite(S.score[i]) && S.score[i] >= 0.0f,
            "NaN 输入不能让分数变成 NaN —— 那样 argmax 的结果是随机的");
}

void test_every_candidate_is_distinct(void) {
    // 候选表写重了的话，某个「可达性」测试会莫名其妙地过，而那一维其实没人消费。
    for (int i = 0; i < kAutoCands; ++i)
        for (int j = i + 1; j < kAutoCands; ++j)
            TEST_ASSERT_TRUE_MESSAGE(autoCandidate(i) != autoCandidate(j),
                "候选表里有重复项");
    for (int i = 0; i < kAutoCands; ++i)
        TEST_ASSERT_TRUE_MESSAGE((int)autoCandidate(i) < (int)FX_COUNT,
            "候选指向了不存在的效果");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_clear_melody_selects_the_melody_line);
    RUN_TEST(test_clear_key_without_drums_selects_the_key_wash);
    RUN_TEST(test_drums_with_a_locked_beat_select_the_bar_impact);
    RUN_TEST(test_drums_with_a_split_spectrum_select_split_bands);
    RUN_TEST(test_unlockable_beat_selects_the_color_flow);
    RUN_TEST(test_active_structure_also_selects_the_color_flow);
    RUN_TEST(test_nothing_stands_out_stays_on_the_fallback);
    RUN_TEST(test_jittery_pitch_is_not_a_melody);
    RUN_TEST(test_jitter_is_measured_per_second_not_per_frame);
    RUN_TEST(test_melody_needs_duration_not_a_single_frame);
    RUN_TEST(test_melody_loses_to_drums_when_percussion_dominates);
    RUN_TEST(test_split_needs_a_real_split_not_just_drums);
    RUN_TEST(test_split_contrast_measures_the_ends_against_the_middle);
    RUN_TEST(test_a_new_candidate_must_win_for_hold_ms);
    RUN_TEST(test_dwell_blocks_a_second_switch);
    RUN_TEST(test_hysteresis_keeps_a_near_tie_from_flapping);
    RUN_TEST(test_pulsed_drums_still_commit);
    RUN_TEST(test_two_close_challengers_still_get_committed);
    RUN_TEST(test_falls_back_when_the_music_stops_standing_out);
    RUN_TEST(test_switch_is_reported_exactly_once);
    RUN_TEST(test_duty_window_is_defined_in_physical_time);
    RUN_TEST(test_scores_stay_finite_on_degenerate_input);
    RUN_TEST(test_every_candidate_is_distinct);
    return UNITY_END();
}
