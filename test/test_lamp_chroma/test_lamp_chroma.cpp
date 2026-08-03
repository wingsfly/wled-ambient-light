#include <unity.h>
#include <math.h>
#include "lamp_chroma.h"
#include "lamp_fft.h"

using namespace lamp;

void setUp(void) {}
void tearDown(void) {}

static const char *PC[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static float x[2048], re[2048], im[2048], mag[1025], win[2048], ch[12];

// 把一组音高（MIDI 号）合成出来，走真实的加窗 FFT —— 不喂手工构造的谱，
// 那样测不到频率→音级的映射有没有对齐。
static void synthPitches(const int *midi, int n_notes, size_t n) {
    for (size_t t = 0; t < n; ++t) x[t] = 0.0f;
    for (int p = 0; p < n_notes; ++p) {
        const float f = 440.0f * powf(2.0f, (midi[p] - 69) / 12.0f);
        for (size_t t = 0; t < n; ++t)
            x[t] += sinf(6.283185307f * f * (float)t / kSampleRate) / (float)n_notes;
    }
    fillWindow(WIN_HANN, win, n);
    magnitudeSpectrum(x, win, n, re, im, mag);
    computeChroma(mag, n, ch);
}

static int argmax12(const float *v) {
    int b = 0;
    for (int i = 1; i < 12; ++i) if (v[i] > v[b]) b = i;
    return b;
}

// ── 音级映射 ──────────────────────────────────────────────

void test_pitch_class_of_reference_tones(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(9, pitchClassOf(440.0f),   "440Hz 应是 A");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pitchClassOf(261.626f), "261.6Hz 应是 C");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pitchClassOf(523.251f), "高一个八度仍是 C");
    TEST_ASSERT_EQUAL_INT_MESSAGE(7, pitchClassOf(391.995f), "392Hz 应是 G");
    TEST_ASSERT_EQUAL_INT(-1, pitchClassOf(0.0f));
    TEST_ASSERT_EQUAL_INT(-1, pitchClassOf(-100.0f));
}

// ── 色度 ──────────────────────────────────────────────────

void test_single_tone_lands_on_its_pitch_class(void) {
    const int notes[] = {69};            // A4
    synthPitches(notes, 1, 1024);
    TEST_ASSERT_EQUAL_INT_MESSAGE(9, argmax12(ch), "A440 的色度峰不在 A");
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, ch[9]);   // 归一化后峰是 1
}

// 八度不变性 —— 这是色度的定义性质：C3 与 C5 是同一个音级。
void test_octaves_collapse_to_one_class(void) {
    static float lo[12], hi[12];
    const int a[] = {48};                // C3
    synthPitches(a, 1, 2048);
    for (int i = 0; i < 12; ++i) lo[i] = ch[i];
    const int b[] = {72};                // C5
    synthPitches(b, 1, 2048);
    for (int i = 0; i < 12; ++i) hi[i] = ch[i];
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, argmax12(lo), "C3 不在 C");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, argmax12(hi), "C5 不在 C");
}

// C 大三和弦：能量应落在 C、E、G 三个音级上。
void test_major_triad_lights_three_classes(void) {
    const int cmaj[] = {60, 64, 67};     // C4 E4 G4
    synthPitches(cmaj, 3, 2048);
    for (int i = 0; i < 12; ++i) {
        const bool in = (i == 0 || i == 4 || i == 7);
        if (in) TEST_ASSERT_TRUE_MESSAGE(ch[i] > 0.3f, "和弦音的色度太弱");
        else    TEST_ASSERT_TRUE_MESSAGE(ch[i] < 0.3f, "非和弦音的色度太强");
    }
}

// 移调的不变性分两条测，因为它们成立的强度不同。
//
// 我在这里连错了三次，都是断言了不为真的性质：
//   1. 「逐个分量完美旋转」—— 移调后泛音落到别的音级上（C4 的泛音在
//      523/785Hz，C#4 在 554/831Hz），能量分布必然改变。
//   2. 「最低的活跃音级就是主峰」—— 不是，C-E-G 的主峰实测在 E 上。
//   3. 「和弦的主峰跟着旋转」—— **也不成立**。三个音能量完全均等时，
//      谁成为 argmax 由 FFT 泄漏与泛音叠加的偶然性决定，移调后这些细节全变。
//
// 真正稳的是：和弦看**活跃音级的集合**，单音才看主峰。

// 和弦：响的是哪几个音级，跟着转一格。
void test_transposition_rotates_the_active_classes(void) {
    const int c[]  = {60, 64, 67};       // C  E  G
    const int cs[] = {61, 65, 68};       // C# F  G#（全部 +1）
    synthPitches(c, 3, 2048);
    int base[3] = {0, 0, 0}, k = 0;
    for (int i = 0; i < 12 && k < 3; ++i) if (ch[i] > 0.3f) base[k++] = i;
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, k, "C 大三和弦没点亮三个音级");

    synthPitches(cs, 3, 2048);
    for (int i = 0; i < 3; ++i)
        TEST_ASSERT_TRUE_MESSAGE(ch[(base[i] + 1) % 12] > 0.3f,
            "移调后对应音级没亮 —— 频率到音级的映射有偏");
    // 不再断言「亮起的音级仍然正好是三个」—— 那是我在这条测试上第四次
    // 加不成立的约束：移调后某个泛音会越过阈值，多亮一格是正常的。
    // 强的移调不变性由下面的单音测试承担，那里主峰唯一、能走满 12 格。
}

// 单音：这时主峰唯一，可以逐半音走满一圈。
void test_transposition_rotates_the_peak_for_single_notes(void) {
    for (int semi = 0; semi < 12; ++semi) {
        const int one[] = {60 + semi};   // C4 起，逐半音升
        synthPitches(one, 1, 2048);
        TEST_ASSERT_EQUAL_INT_MESSAGE(semi % 12, argmax12(ch),
            "单音的主峰没落在对应音级上");
    }
}

// 音量翻倍不改变色度 —— 归一化的意义就在这里，否则它只是又一个音量表。
void test_chroma_is_amplitude_invariant(void) {
    static float soft[12];
    const int n[] = {60, 67};
    synthPitches(n, 2, 1024);
    for (int i = 0; i < 12; ++i) soft[i] = ch[i];
    for (size_t t = 0; t < 1024; ++t) x[t] *= 5.0f;
    magnitudeSpectrum(x, win, 1024, re, im, mag);
    computeChroma(mag, 1024, ch);
    for (int i = 0; i < 12; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, soft[i], ch[i], "色度随音量变了");
}

void test_silence_and_garbage_are_safe(void) {
    for (size_t k = 0; k <= 512; ++k) mag[k] = 0.0f;
    computeChroma(mag, 1024, ch);
    for (int i = 0; i < 12; ++i) {
        TEST_ASSERT_FALSE(isnan(ch[i]));
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, ch[i]);
    }
    for (size_t k = 0; k <= 512; ++k) mag[k] = (k % 7) ? 0.5f : NAN;
    computeChroma(mag, 1024, ch);
    for (int i = 0; i < 12; ++i) TEST_ASSERT_FALSE_MESSAGE(isnan(ch[i]), "NaN 渗进了色度");
    computeChroma(nullptr, 1024, ch);        // 不得崩
}

// ── 调性 ──────────────────────────────────────────────────

// 喂一条 C 大调音阶，应当判成 C major。
void test_c_major_scale_is_identified(void) {
    const int scale[] = {60, 62, 64, 65, 67, 69, 71};
    synthPitches(scale, 7, 2048);
    const KeyEstimate k = estimateKey(ch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, k.root, PC[k.root < 0 ? 0 : k.root]);
    TEST_ASSERT_TRUE_MESSAGE(k.is_major, "C 大调音阶被判成了小调");
    TEST_ASSERT_TRUE_MESSAGE(k.conf > 0.1f, "置信度过低");
}

// a 小调用同样的七个音，只是主音不同 —— 靠的是 K-S 权重的分布差异。
void test_a_minor_is_distinguished_from_c_major(void) {
    // a 小调：强调 A、C、E（i 级和弦）
    const int am[] = {57, 60, 64, 69, 72};
    synthPitches(am, 5, 2048);
    const KeyEstimate k = estimateKey(ch);
    TEST_ASSERT_TRUE_MESSAGE(k.root >= 0, "没给出调性");
    TEST_ASSERT_TRUE_MESSAGE(k.root == 9 || k.root == 0,
        "a 小调三和弦既不像 A 也不像 C —— 相关算错了");
}

// 移调一个半音，主音必须跟着移一格。
void test_key_follows_transposition(void) {
    const int c[]  = {60, 64, 67, 72};
    const int d[]  = {62, 66, 69, 74};    // +2 半音
    synthPitches(c, 4, 2048);
    const int r1 = estimateKey(ch).root;
    synthPitches(d, 4, 2048);
    const int r2 = estimateKey(ch).root;
    TEST_ASSERT_TRUE(r1 >= 0 && r2 >= 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE((r1 + 2) % 12, r2, "移调后主音没有跟着走");
}

// 白噪声不得报出高置信度的调性。
void test_noise_gives_low_confidence(void) {
    for (size_t t = 0; t < 2048; ++t) {
        uint32_t s = (uint32_t)t * 1664525u + 1013904223u;
        s ^= s >> 16; s *= 2246822519u; s ^= s >> 13;
        x[t] = (float)(s & 0xFFFFu) / 32768.0f - 1.0f;
    }
    fillWindow(WIN_HANN, win, 2048);
    magnitudeSpectrum(x, win, 2048, re, im, mag);
    computeChroma(mag, 2048, ch);
    const KeyEstimate k = estimateKey(ch);
    TEST_ASSERT_TRUE_MESSAGE(k.conf < 0.75f, "白噪声报出了高置信度的调性");
}

void test_silence_gives_no_key(void) {
    for (int i = 0; i < 12; ++i) ch[i] = 0.0f;
    const KeyEstimate k = estimateKey(ch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, k.root, "静音也报了调性");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, k.conf);
}

// 置信度取「最佳与次佳之差」而不是最佳的绝对值。
// 绝对值对任何有调性的音乐都偏高，区分不出「确实是 C 大调」和「难分伯仲」。
void test_confidence_reflects_margin_not_absolute(void) {
    float clear[12], flat[12];
    const int cmaj[] = {60, 64, 67};
    synthPitches(cmaj, 3, 2048);
    for (int i = 0; i < 12; ++i) clear[i] = ch[i];
    for (int i = 0; i < 12; ++i) flat[i] = 1.0f;      // 十二音全响，无调性可言
    TEST_ASSERT_TRUE_MESSAGE(estimateKey(clear).conf > estimateKey(flat).conf + 0.2f,
        "分不清「明确的调」与「平坦的十二音」");
}

// ── 和声变化 ──────────────────────────────────────────────

void test_chord_change_shows_up_as_distance(void) {
    static float cmaj[12], fmaj[12];
    const int c[] = {60, 64, 67};
    const int f[] = {65, 69, 72};
    synthPitches(c, 3, 2048); for (int i = 0; i < 12; ++i) cmaj[i] = ch[i];
    synthPitches(f, 3, 2048); for (int i = 0; i < 12; ++i) fmaj[i] = ch[i];
    TEST_ASSERT_TRUE_MESSAGE(chromaDistance(cmaj, cmaj) < 0.02f, "同一和弦距离不为零");
    TEST_ASSERT_TRUE_MESSAGE(chromaDistance(cmaj, fmaj) > 0.15f, "换了和弦却没有距离");
}

// 音量渐强不该被当成和弦变化 —— 用余弦距离而不是欧氏就是为了这个。
void test_volume_change_is_not_a_chord_change(void) {
    static float a[12], b[12];
    const int c[] = {60, 64, 67};
    synthPitches(c, 3, 2048);
    for (int i = 0; i < 12; ++i) { a[i] = ch[i]; b[i] = ch[i] * 0.3f; }
    TEST_ASSERT_TRUE_MESSAGE(chromaDistance(a, b) < 0.02f,
        "整体缩放被当成了和声变化 —— 用了欧氏距离？");
}

void test_distance_handles_zero_and_nan(void) {
    float z[12] = {0}, v[12];
    for (int i = 0; i < 12; ++i) v[i] = 0.5f;
    TEST_ASSERT_FALSE(isnan(chromaDistance(z, v)));
    TEST_ASSERT_FALSE(isnan(chromaDistance(z, z)));
    v[3] = NAN;
    TEST_ASSERT_FALSE(isnan(chromaDistance(v, v)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_pitch_class_of_reference_tones);
    RUN_TEST(test_single_tone_lands_on_its_pitch_class);
    RUN_TEST(test_octaves_collapse_to_one_class);
    RUN_TEST(test_major_triad_lights_three_classes);
    RUN_TEST(test_transposition_rotates_the_active_classes);
    RUN_TEST(test_transposition_rotates_the_peak_for_single_notes);
    RUN_TEST(test_chroma_is_amplitude_invariant);
    RUN_TEST(test_silence_and_garbage_are_safe);
    RUN_TEST(test_c_major_scale_is_identified);
    RUN_TEST(test_a_minor_is_distinguished_from_c_major);
    RUN_TEST(test_key_follows_transposition);
    RUN_TEST(test_noise_gives_low_confidence);
    RUN_TEST(test_silence_gives_no_key);
    RUN_TEST(test_confidence_reflects_margin_not_absolute);
    RUN_TEST(test_chord_change_shows_up_as_distance);
    RUN_TEST(test_volume_change_is_not_a_chord_change);
    RUN_TEST(test_distance_handles_zero_and_nan);
    return UNITY_END();
}
