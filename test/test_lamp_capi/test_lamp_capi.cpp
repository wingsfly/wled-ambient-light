#include <unity.h>
#include <math.h>
#include "lamp_capi.cpp"      // 直接编进来：要测的是 lamp_feed 的缓冲管理

void setUp(void) {}
void tearDown(void) {}

// 这个套件专测 lamp_feed 的样本缓冲，重点是**档位切换那一帧**。
// 线上崩过一次：SIGBUS，栈顶 _platform_memmove ← lamp_feed，
// 整个 Python 进程被内核杀掉，连 traceback 都没有。

static float pcm[8192];
static LampFrameC out[64];

static void fillBeat(int base, int n, float bpm) {
    const float per = 60000.0f / bpm;
    for (int i = 0; i < n; ++i) {
        const float ms = (base + i) * 1000.0f / 22050.0f;
        const float ph = fmodf(ms, per) / per;
        const float e = (ph < 0.05f) ? ph / 0.05f : expf(-(ph - 0.05f) * 10.0f);
        pcm[i] = 0.5f * e * sinf(6.283185307f * 70.0f * ms / 1000.0f)
               + 0.2f * e * sinf(6.283185307f * 3000.0f * ms / 1000.0f);
    }
}

void test_create_and_destroy(void) {
    void *h = lamp_create();
    TEST_ASSERT_NOT_NULL(h);
    lamp_destroy(h);
    lamp_destroy(nullptr);            // 不得崩
}

void test_feed_rejects_bad_args(void) {
    void *h = lamp_create();
    TEST_ASSERT_EQUAL_INT(0, lamp_feed(nullptr, pcm, 100, out, 64));
    TEST_ASSERT_EQUAL_INT(0, lamp_feed(h, nullptr, 100, out, 64));
    TEST_ASSERT_EQUAL_INT(0, lamp_feed(h, pcm, 0, out, 64));
    TEST_ASSERT_EQUAL_INT(0, lamp_feed(h, pcm, -5, out, 64));
    TEST_ASSERT_EQUAL_INT(0, lamp_feed(h, pcm, 100, nullptr, 64));
    TEST_ASSERT_EQUAL_INT(0, lamp_feed(h, pcm, 100, out, 0));
    lamp_destroy(h);
}

void test_feed_produces_frames(void) {
    void *h = lamp_create();
    int total = 0;
    for (int k = 0; k < 40; ++k) {
        fillBeat(k * 2048, 2048, 128.0f);
        total += lamp_feed(h, pcm, 2048, out, 64);
    }
    TEST_ASSERT_TRUE_MESSAGE(total > 120 && total < 200, "产出帧数不合预期");
    lamp_destroy(h);
}

// ── 档位切换：崩溃就出在这里 ──────────────────────────────

// 大帧长切小帧长时，filled 可能已经超过新的 n。`n - filled` 是 size_t 减法，
// 会下溢成天文数字，随后的 memcpy 直接写出边界。
void test_survives_shrinking_frame_length(void) {
    void *h = lamp_create();
    lamp_lock_preset(h, 0);                       // 氛围档：N=2048
    for (int k = 0; k < 20; ++k) {
        fillBeat(k * 2048, 2048, 100.0f);
        lamp_feed(h, pcm, 2048, out, 64);
    }
    lamp_lock_preset(h, 3);                       // 极限打点：N=512，缩到四分之一
    int got = 0;
    for (int k = 0; k < 20; ++k) {
        fillBeat((20 + k) * 2048, 2048, 100.0f);
        got += lamp_feed(h, pcm, 2048, out, 64);  // ← 原崩溃点
    }
    TEST_ASSERT_TRUE_MESSAGE(got > 0, "切到小帧长后不再产出帧");
    lamp_destroy(h);
}

// 反向：小帧长切大帧长。扩容若用 assign 会把已攒的样本清零 ——
// 不崩，但那一帧读到静音，表现为灯突然黑一下。
void test_grows_frame_length_without_losing_samples(void) {
    void *h = lamp_create();
    lamp_lock_preset(h, 3);                       // N=512
    for (int k = 0; k < 20; ++k) {
        fillBeat(k * 2048, 2048, 100.0f);
        lamp_feed(h, pcm, 2048, out, 64);
    }
    lamp_lock_preset(h, 0);                       // N=2048
    int got = 0; float maxrms = 0.0f;
    for (int k = 0; k < 24; ++k) {
        fillBeat((20 + k) * 2048, 2048, 100.0f);
        const int n = lamp_feed(h, pcm, 2048, out, 64);
        for (int i = 0; i < n; ++i) if (out[i].rms > maxrms) maxrms = out[i].rms;
        got += n;
    }
    TEST_ASSERT_TRUE_MESSAGE(got > 0, "切到大帧长后不再产出帧");
    TEST_ASSERT_TRUE_MESSAGE(maxrms > 0.01f, "切档后读到的全是静音 —— 缓冲被清零了");
    lamp_destroy(h);
}

void test_survives_repeated_preset_thrashing(void) {
    void *h = lamp_create();
    const int order[6] = {0, 3, 1, 0, 2, 3};
    for (int r = 0; r < 6; ++r) {
        lamp_lock_preset(h, order[r]);
        for (int k = 0; k < 8; ++k) {
            fillBeat((r * 8 + k) * 2048, 2048, 140.0f);
            lamp_feed(h, pcm, 2048, out, 64);
        }
    }
    lamp_destroy(h);
    TEST_ASSERT_TRUE(true);           // 跑到这里没崩就算过
}

// 自动切档也要安全 —— 线上崩的那次没人手动锁档。
void test_survives_automatic_preset_switching(void) {
    void *h = lamp_create();
    lamp_lock_preset(h, -1);
    for (int k = 0; k < 200; ++k) {
        fillBeat(k * 2048, 2048, k < 100 ? 70.0f : 180.0f);
        lamp_feed(h, pcm, 2048, out, 64);
    }
    lamp_destroy(h);
    TEST_ASSERT_TRUE(true);
}

// 不按帧长对齐地喂 —— 麦克风就是这样，来多少给多少。
void test_accepts_arbitrary_chunk_sizes(void) {
    void *h = lamp_create();
    const int sizes[7] = {1, 7, 63, 128, 511, 1023, 4096};
    int total = 0;
    for (int r = 0; r < 12; ++r)
        for (int i = 0; i < 7; ++i) {
            fillBeat(r * 5000 + i * 700, sizes[i], 120.0f);
            total += lamp_feed(h, pcm, sizes[i], out, 64);
        }
    TEST_ASSERT_TRUE_MESSAGE(total > 0, "碎块输入产不出帧");
    lamp_destroy(h);
}

// 一次喂的量远超 max_out 能装下的帧数，不得越界写 out。
void test_respects_max_out(void) {
    void *h = lamp_create();
    lamp_lock_preset(h, 3);                       // hop=128
    fillBeat(0, 8192, 150.0f);
    TEST_ASSERT_TRUE_MESSAGE(lamp_feed(h, pcm, 8192, out, 4) <= 4,
        "产出帧数超过了 max_out");
    lamp_destroy(h);
}

void test_frame_size_matches_struct(void) {
    TEST_ASSERT_EQUAL_INT((int)sizeof(LampFrameC), lamp_frame_size());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_create_and_destroy);
    RUN_TEST(test_feed_rejects_bad_args);
    RUN_TEST(test_feed_produces_frames);
    RUN_TEST(test_survives_shrinking_frame_length);
    RUN_TEST(test_grows_frame_length_without_losing_samples);
    RUN_TEST(test_survives_repeated_preset_thrashing);
    RUN_TEST(test_survives_automatic_preset_switching);
    RUN_TEST(test_accepts_arbitrary_chunk_sizes);
    RUN_TEST(test_respects_max_out);
    RUN_TEST(test_frame_size_matches_struct);
    return UNITY_END();
}
