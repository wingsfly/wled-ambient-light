#include <unity.h>
#include "audio_stream_probe.h"

using audio_stream_probe::Window;
using audio_stream_probe::chunkHasChangingSamples;
using audio_stream_probe::samplesChanging;
using audio_stream_probe::samplesNonZero;

void setUp(void) {}
void tearDown(void) {}

static void putSample(uint8_t *dst, int32_t sample) {
  const uint32_t raw = uint32_t(sample) << 16;
  dst[0] = uint8_t(raw);
  dst[1] = uint8_t(raw >> 8);
  dst[2] = uint8_t(raw >> 16);
  dst[3] = uint8_t(raw >> 24);
}

static void makeChanging(uint8_t *buf, size_t bytes, int32_t offset = 0) {
  for (size_t i = 0; i + 4 <= bytes; i += 4) {
    putSample(buf + i, offset + int32_t((i / 4) % 7) - 3);
  }
}

void test_zero_and_constant_chunks_are_not_signal(void) {
  uint8_t zero[256] = {};
  uint8_t constant[256];
  for (size_t i = 0; i < sizeof(constant); i += 4) putSample(constant + i, 17);
  TEST_ASSERT_FALSE(chunkHasChangingSamples(zero, sizeof(zero)));
  TEST_ASSERT_FALSE(chunkHasChangingSamples(constant, sizeof(constant)));
}

void test_quiet_noise_is_signal(void) {
  uint8_t noise[256];
  makeChanging(noise, sizeof(noise));
  TEST_ASSERT_TRUE(chunkHasChangingSamples(noise, sizeof(noise)));
}

void test_one_stale_block_then_zero_is_unhealthy(void) {
  uint8_t stale[256];
  uint8_t zero[256] = {};
  makeChanging(stale, sizeof(stale));
  Window probe;
  probe.observe(stale, sizeof(stale), false);
  for (int i = 0; i < 8; ++i) probe.observe(zero, sizeof(zero), i >= 3);
  TEST_ASSERT_FALSE(probe.healthy());
}

void test_repeated_stale_block_is_unhealthy(void) {
  uint8_t stale[256];
  makeChanging(stale, sizeof(stale));
  Window probe;
  for (int i = 0; i < 8; ++i) probe.observe(stale, sizeof(stale), i >= 3);
  TEST_ASSERT_FALSE(probe.healthy());
}

void test_sustained_changing_stream_is_healthy(void) {
  uint8_t chunk[256];
  Window probe;
  for (int i = 0; i < 8; ++i) {
    makeChanging(chunk, sizeof(chunk), i);
    probe.observe(chunk, sizeof(chunk), i >= 3);
  }
  TEST_ASSERT_TRUE(probe.healthy());
}

// ---- getSamples() 输出（int16 刻度的 float）：V5 热切换验流经音源读，不再直读驱动 ----

static void makeChangingFloat(float *buf, size_t n, int32_t offset = 0) {
  for (size_t i = 0; i < n; ++i) buf[i] = float(offset + int32_t(i % 7) - 3);
}

void test_float_zero_and_constant_blocks_are_not_signal(void) {
  float zero[64] = {};
  float constant[64];
  for (size_t i = 0; i < 64; ++i) constant[i] = 17.0f;
  TEST_ASSERT_FALSE(samplesChanging(zero, 64));
  TEST_ASSERT_FALSE(samplesChanging(constant, 64));
}

void test_float_quiet_noise_is_signal(void) {
  float noise[64];
  makeChangingFloat(noise, 64);
  TEST_ASSERT_TRUE(samplesChanging(noise, 64));
}

void test_float_sub_lsb_jitter_is_not_signal(void) {
  // 32bit 字转 int16 刻度后带小数；不到 2 LSB 的抖动不算自噪声
  float jitter[64];
  for (size_t i = 0; i < 64; ++i) jitter[i] = 5.0f + 0.25f * float(i % 3);
  TEST_ASSERT_FALSE(samplesChanging(jitter, 64));
}

void test_float_one_stale_block_then_zero_is_unhealthy(void) {
  float stale[64];
  float zero[64] = {};
  makeChangingFloat(stale, 64);
  Window probe;
  probe.observeSamples(stale, 64, false);
  for (int i = 0; i < 8; ++i) probe.observeSamples(zero, 64, i >= 3);
  TEST_ASSERT_FALSE(probe.healthy());
}

void test_float_repeated_stale_block_is_unhealthy(void) {
  // 读失败时 getSamples() 不写缓冲：调用方不清零的话，看到的就是同一块
  float stale[64];
  makeChangingFloat(stale, 64);
  Window probe;
  for (int i = 0; i < 8; ++i) probe.observeSamples(stale, 64, i >= 3);
  TEST_ASSERT_FALSE(probe.healthy());
}

void test_float_sustained_changing_stream_is_healthy(void) {
  float block[64];
  Window probe;
  for (int i = 0; i < 8; ++i) {
    makeChangingFloat(block, 64, i);
    probe.observeSamples(block, 64, i >= 3);
  }
  TEST_ASSERT_TRUE(probe.healthy());
}

void test_adc_block_nonzero_means_data_arrived(void) {
  // ADC 源读不满时把缓冲清零；活着的模拟前端有偏置残差，不会全零
  float zero[64] = {};
  float biased[64];
  for (size_t i = 0; i < 64; ++i) biased[i] = -38.0f;
  TEST_ASSERT_FALSE(samplesNonZero(zero, 64));
  TEST_ASSERT_TRUE(samplesNonZero(biased, 64));
  TEST_ASSERT_FALSE(samplesNonZero(nullptr, 64));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_zero_and_constant_chunks_are_not_signal);
  RUN_TEST(test_quiet_noise_is_signal);
  RUN_TEST(test_one_stale_block_then_zero_is_unhealthy);
  RUN_TEST(test_repeated_stale_block_is_unhealthy);
  RUN_TEST(test_sustained_changing_stream_is_healthy);
  RUN_TEST(test_float_zero_and_constant_blocks_are_not_signal);
  RUN_TEST(test_float_quiet_noise_is_signal);
  RUN_TEST(test_float_sub_lsb_jitter_is_not_signal);
  RUN_TEST(test_float_one_stale_block_then_zero_is_unhealthy);
  RUN_TEST(test_float_repeated_stale_block_is_unhealthy);
  RUN_TEST(test_float_sustained_changing_stream_is_healthy);
  RUN_TEST(test_adc_block_nonzero_means_data_arrived);
  return UNITY_END();
}
