#include <unity.h>
#include "audio_stream_probe.h"

using audio_stream_probe::Window;
using audio_stream_probe::chunkHasChangingSamples;

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

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_zero_and_constant_chunks_are_not_signal);
  RUN_TEST(test_quiet_noise_is_signal);
  RUN_TEST(test_one_stale_block_then_zero_is_unhealthy);
  RUN_TEST(test_repeated_stale_block_is_unhealthy);
  RUN_TEST(test_sustained_changing_stream_is_healthy);
  return UNITY_END();
}
