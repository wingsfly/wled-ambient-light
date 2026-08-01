// usermods/lamp/lamp_color.h
//
// 白平衡增益（设计文档 §4.3）。
// 系数来自逆向阶段实测的原厂满白值，见 docs/10-灯管与驱动.md：
// 线上字节 GRB = DD FF C2 → R=255 G=221 B=194 → 1.000 : 0.867 : 0.761。
// 直接抄这组系数就能得到与原厂一致的中性白，省掉目测调校。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>

namespace lamp {

struct Rgb { uint8_t r, g, b; };

constexpr float WB_R = 1.000f;
constexpr float WB_G = 0.867f;
constexpr float WB_B = 0.761f;

inline uint8_t scaleChannel(uint8_t v, float k) {
    // 中间量用有符号 int。这里 v 是 uint8_t、k 为正，乘积恒 ≥ 0，
    // 不存在 lamp_geometry.h 里那种负浮点转无符号的 UB。
    const int r = (int)(v * k + 0.5f);
    // 上钳制在当前系数下**不可达**（最大 255×1.000+0.5 → 255）。
    // 保留是为了将来有人把某个系数调到 >1 时不至于回绕。
    return (uint8_t)(r > 255 ? 255 : r);
}

inline Rgb applyWhiteBalance(Rgb in, bool enabled) {
    if (!enabled) return in;
    return Rgb{ scaleChannel(in.r, WB_R),
                scaleChannel(in.g, WB_G),
                scaleChannel(in.b, WB_B) };
}

} // namespace lamp
