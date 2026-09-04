// usermods/lamp/lamp_geometry.h
//
// 灯效几何抽象（设计文档 §4.1）。
// 特效只写 (side, u)，由本层翻译成物理像素索引。接线怎么变都不改特效。
//
// 纯逻辑，零 WLED 依赖 —— 可在主机上编译测试。
#pragma once

#include <stdint.h>

namespace lamp {

enum Side : uint8_t { SIDE_L = 0, SIDE_R = 1 };

constexpr uint16_t LEDS_PER_TUBE = 48;
constexpr uint16_t TOTAL_LEDS    = LEDS_PER_TUBE * 2;

// 每根管 48 颗的物理三段布局（2026-09-05 用 /leddebug 逐颗实测，两管一致，B 管 +48）：
//   底座环   12 颗  索引 0..11   编号递增 = 顺时针
//   底部灯柱  6 颗  索引 12..17  360° 短柱，编号递增 = 逆时针
//   主灯柱   30 颗  索引 18..47  编号递增 = 向上
// 效果的纵向坐标 u 只铺在主灯柱上；底座环与底部灯柱是派生氛围区（fillZones）。
constexpr uint16_t RING_LEDS = 12, RING_OFS = 0;
constexpr uint16_t BOT_LEDS  = 6,  BOT_OFS  = 12;
constexpr uint16_t MAIN_LEDS = 30, MAIN_OFS = 18;
static_assert(RING_LEDS + BOT_LEDS + MAIN_LEDS == LEDS_PER_TUBE, "三段之和必须等于每管颗数");

enum Zone : uint8_t { ZONE_RING = 0, ZONE_BOTTOM = 1, ZONE_MAIN = 2 };

// 三项都靠装机时点亮索引 0 观察确定，见 docs/lamp-calibration.md。
struct Geometry {
    bool s1_is_left  = true;   // S1 这条总线接的是左管吗
    bool s1_reversed = false;  // S1 数据链首颗在管顶吗（false = 在管底）
    bool s2_reversed = false;  // S2 同上
};

// (side, u) → **主灯柱**的物理像素索引。
// u ∈ [0,1]：0 = 主柱底（索引 18），1 = 主柱顶（索引 47）。越界会被钳制。
// S1 固定占 [0,47]、S2 占 [48,95]，这由 WLED 的总线顺序决定，不是配置项。
//
// 量化约定：LED 中心落在 u = n/29。**端点桶是半宽的** —— u 均匀变化时，
// 中间每颗占 1/29 的区间，而 n = 0 与 n = 29 各只占 0.5/29，正好 2 倍差。
// 效果按 48 个 u 采样写入时会有相邻采样落到同一颗（后写覆盖），无害。
// 这是该约定固有的，不是 bug，但意味着首尾两颗在连续扫描下停留时间减半。
inline uint16_t mapPixel(const Geometry &g, Side side, float u) {
    // 用 !(u > 0) 而不是 u < 0。这不是风格问题：NaN 同时通不过 `u < 0` 和
    // `u > 1` 两个比较，会直接落到下面的浮点转整型上 —— 那是 UB。arm64 的
    // fcvtzs 碰巧把 NaN 映射到 0，但目标平台是 xtensa，不能指望。
    // u 将来会来自 FFT 的比值，0/0 是可达的，这条路径是活的。
    if (!(u > 0.0f)) u = 0.0f;
    if (u > 1.0f)    u = 1.0f;

    const bool     use_s1   = ((side == SIDE_L) == g.s1_is_left);  // XNOR，四种组合见文档
    const bool     reversed = use_s1 ? g.s1_reversed : g.s2_reversed;
    const uint16_t base     = use_s1 ? 0 : LEDS_PER_TUBE;

    // 中间量用有符号 int 而非直接转 uint16_t。这是**纵深防御，不修活 bug** ——
    // 上面的钳制已经保证 u ≥ 0，正常路径走不到负数。
    // 但一旦钳制被误删：负浮点转无符号整型是 UB，-O0 下恰好得 0（故障被掩盖，
    // 单测照样全绿），-O2 下实测返回 4294967062，连返回类型都装不下。
    // 变异测试实证：删掉下钳制后，加固前的实现 -O2 下越界，加固后仍返回 0。
    int n = (int)(u * (MAIN_LEDS - 1) + 0.5f);
    if (n < 0)             n = 0;                      // 纵深防御：钳制哪天被改坏也不越界
    if (n > MAIN_LEDS - 1) n = MAIN_LEDS - 1;
    if (reversed)          n = (MAIN_LEDS - 1) - n;
    return (uint16_t)(base + MAIN_OFS + n);
}

// (side, zone, k) → 底座环 / 底部灯柱 / 主柱第 k 颗的物理索引。k 越界钳制。
// 环与底柱不受 reversed 影响（它们的方向是圆周，不是上下）。
inline uint16_t zonePixel(const Geometry &g, Side side, Zone z, uint16_t k) {
    const bool     use_s1 = ((side == SIDE_L) == g.s1_is_left);
    const uint16_t base   = use_s1 ? 0 : LEDS_PER_TUBE;
    uint16_t ofs = MAIN_OFS, len = MAIN_LEDS;
    if (z == ZONE_RING)        { ofs = RING_OFS; len = RING_LEDS; }
    else if (z == ZONE_BOTTOM) { ofs = BOT_OFS;  len = BOT_LEDS;  }
    if (k >= len) k = len - 1;
    return (uint16_t)(base + ofs + k);
}

} // namespace lamp
