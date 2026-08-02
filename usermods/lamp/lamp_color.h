// usermods/lamp/lamp_color.h
//
// 白平衡增益（设计文档 §4.3）。
// 系数来自逆向阶段实测的原厂满白值，见 wingsfly/ambient-light 仓库的
// docs/10-灯管与驱动.md：
// 线上字节 GRB = DD FF C2 → R=255 G=221 B=194（下面的系数直接由这三个整数除以 255 得出）。
// 直接抄这组系数就能得到与原厂一致的中性白，省掉目测调校。
//
// 纯逻辑，零 WLED 依赖。
#pragma once

#include <stdint.h>

namespace lamp {

struct Rgb { uint8_t r, g, b; };

// 系数写成「实测值 / 满量程」的分数形式，不写十进制近似。
// 这不只是风格问题：0.867f 与精确的 221/255 差 +0.00033，会让 256 个输入里
// 10 个偏 1 LSB；更要紧的是十进制写法下的小笔误**满白测试抓不住** ——
// 0.867f 误写成 0.866f，255×0.866+0.5 仍得 221，变异测试实测该变异体存活。
// 写成分数后，把 221 误写成 220 或 222 会让满白测试立刻失败。
constexpr float WB_R = 255.0f / 255.0f;
constexpr float WB_G = 221.0f / 255.0f;
constexpr float WB_B = 194.0f / 255.0f;

inline uint8_t scaleChannel(uint8_t v, float k) {
    // 中间量用有符号 int。当前 v 是 uint8_t、k 为正 constexpr，乘积恒 ≥ 0；
    // 但本函数在头文件里公开，未来调用方可能传进运行时的负 k，
    // 所以两端都钳 —— 与 lamp_geometry.h 的 mapPixel 保持同一策略。
    const int r = (int)(v * k + 0.5f);
    // 两端钳制在当前系数下都不可达（最大 255×1.000+0.5 → 255，最小 0）。
    // 保留是为了将来有人把系数调到 >1 或传入负值时得到饱和而不是回绕。
    return (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
}

inline Rgb applyWhiteBalance(Rgb in, bool enabled) {
    if (!enabled) return in;
    return Rgb{ scaleChannel(in.r, WB_R),
                scaleChannel(in.g, WB_G),
                scaleChannel(in.b, WB_B) };
}

} // namespace lamp
