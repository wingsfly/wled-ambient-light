#pragma once
#include "lamp_ring_buffer.h"

// audioreactive FFT task（生产者，每块 512 样本 @22050Hz）→ lamp 本地管线
// （WLED loop 消费者）的 PCM 分接。SPSC 无锁，TSan 用例守着内存序。
// 2048 = 4 块裕量：loop 停摆超过 ~93ms 才丢样本，丢了也只是灯效少一帧。
namespace lamp {
inline RingBuffer<2048> &pcmTap() {
  static RingBuffer<2048> rb;
  return rb;
}
}
