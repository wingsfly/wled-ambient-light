#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio_stream_probe {

// I2S digital microphones deliver signed samples in the upper 16 bits of each
// 32-bit DMA word in this build.  A dead ADC->I2S hot switch can still return
// bytes, but those bytes are all zero, constant, or a repeated stale block.
inline bool chunkHasChangingSamples(const uint8_t *data, size_t bytes) {
  if (!data || bytes < 8 * sizeof(int32_t)) return false;

  int32_t lo = 32767;
  int32_t hi = -32768;
  for (size_t i = 0; i + sizeof(int32_t) <= bytes; i += sizeof(int32_t)) {
    const uint32_t raw = uint32_t(data[i])
                       | (uint32_t(data[i + 1]) << 8)
                       | (uint32_t(data[i + 2]) << 16)
                       | (uint32_t(data[i + 3]) << 24);
    const int32_t sample = int32_t(raw) >> 16;
    if (sample < lo) lo = sample;
    if (sample > hi) hi = sample;
  }
  // A real digital microphone has at least a few LSB of self-noise even in a
  // quiet room.  Requiring a span of two rejects zero/constant DMA output.
  return (hi - lo) >= 2;
}

// Same check for a block already decoded by an AudioSource (getSamples output:
// float on the int16 scale).  The V5 hot-switch probe reads through the audio
// source instead of the legacy driver, so it sees these instead of DMA words.
inline bool samplesChanging(const float *samples, size_t n) {
  if (!samples || n < 8) return false;
  float lo = samples[0];
  float hi = samples[0];
  for (size_t i = 1; i < n; ++i) {
    if (samples[i] < lo) lo = samples[i];
    if (samples[i] > hi) hi = samples[i];
  }
  return (hi - lo) >= 2.0f;
}

// The ADC source zero-fills its buffer when it cannot deliver a full block,
// while a live analog front end always shows its bias residue.
inline bool samplesNonZero(const float *samples, size_t n) {
  if (!samples) return false;
  for (size_t i = 0; i < n; ++i) {
    if (samples[i] != 0.0f) return true;
  }
  return false;
}

inline uint32_t chunkHash(const uint8_t *data, size_t bytes) {
  uint32_t hash = 2166136261u; // FNV-1a
  for (size_t i = 0; i < bytes; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

class Window {
 public:
  void observe(const uint8_t *data, size_t bytes, bool late) {
    if (!data || bytes == 0) return;
    note(chunkHasChangingSamples(data, bytes), late, data, bytes);
  }

  void observeSamples(const float *samples, size_t n, bool late) {
    if (!samples || n == 0) return;
    note(samplesChanging(samples, n), late,
         reinterpret_cast<const uint8_t *>(samples), n * sizeof(float));
  }

  bool healthy() const {
    return dataChunks_ >= 4 && signalChunks_ >= 3 && lateSignalChunks_ >= 2
           && lateContentChanged_;
  }

 private:
  void note(bool signal, bool late, const uint8_t *data, size_t bytes) {
    ++dataChunks_;
    if (!signal) return;

    ++signalChunks_;
    if (!late) return;
    ++lateSignalChunks_;

    const uint32_t hash = chunkHash(data, bytes);
    if (!haveLateHash_) {
      firstLateHash_ = hash;
      haveLateHash_ = true;
    } else if (hash != firstLateHash_) {
      lateContentChanged_ = true;
    }
  }

  uint16_t dataChunks_ = 0;
  uint16_t signalChunks_ = 0;
  uint16_t lateSignalChunks_ = 0;
  uint32_t firstLateHash_ = 0;
  bool haveLateHash_ = false;
  bool lateContentChanged_ = false;
};

} // namespace audio_stream_probe
