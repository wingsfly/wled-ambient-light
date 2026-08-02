// 回绕安全的时间比较。
//
// 单独成一个头文件是因为不止一处要用：音频源仲裁（§3.1）的滞回、
// 风格档位切换（§3.3.3）的驻留计时，都靠它。复制两份迟早会分叉。
#pragma once

#include <stdint.h>

namespace lamp {

// 无符号差值：now 回绕后 (now - since) 依然是正确的经过时长，
// 只要间隔小于 2^32 ms（49.7 天）。**别改成有符号比较。**
inline bool elapsedAtLeast(uint32_t now, uint32_t since, uint32_t ms) {
    return (uint32_t)(now - since) >= ms;
}

} // namespace lamp
