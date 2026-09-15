// liblamp 的 C ABI —— 唯一的一份布局真相。
//
// 这个头文件**必须保持纯 C**：Python 之外还有 Swift（macOS sender App）
// 要按它对齐。凡是 C++ 的东西（namespace lamp、constexpr、std::vector）
// 都不能出现在这里，否则非 C++ 的调用方 include 不进来。
//
// 三个维度常量在这里写成字面量，而不是 include lamp_bands.h 取真值 ——
// 代价是可能与 usermods/lamp 漂开，所以 lamp_capi.cpp 里有三条
// static_assert 把它们钉死。改了那边而没改这里，**编译就断**，
// 不会留到运行期变成读错位的数。
//
//   c++ -std=c++17 -O2 -shared -fPIC -I ../usermods/lamp -I . \
//       lamp_capi.cpp -o liblamp.dylib      (Linux 用 .so)
#ifndef LAMP_CAPI_H
#define LAMP_CAPI_H

#include <stdint.h>

#define LAMP_NUM_BANDS  16   // usermods/lamp/lamp_bands.h  NUM_BANDS
#define LAMP_NUM_CHROMA 12   // usermods/lamp/lamp_chroma.h kChroma
#define LAMP_NUM_LEDS   96   // usermods/lamp/lamp_geometry.h TOTAL_LEDS

#ifdef __cplusplus
extern "C" {
#endif

// 一帧分析结果 + 渲染出的像素。
//
// 字段顺序即内存布局，调用方（server.py 的 ctypes、Swift 的 import）
// 直接按这个结构读。**任何增删都要同步 server.py 的 _fields_**，
// 那边没有编译期检查，只能靠 lamp_frame_size() 在启动时兜一道。
typedef struct LampFrameC {
    float bands[LAMP_NUM_BANDS];
    float chroma[LAMP_NUM_CHROMA];
    float bpm, conf, phase, rms, peak, gain, rate, centroid, flatness;
    float key_conf, harmony;
    float f0, f0_conf, mood, trend, novelty, dynamics, percussive, bar_conf;
    float vocal;                 // 人声/主旋律存在度，见 lamp_vocal.h
    float bands_h[LAMP_NUM_BANDS], bands_p[LAMP_NUM_BANDS];
    int32_t lock, gate, onset, preset, key_root, key_major, f0_voiced, section;
    int32_t bpb, bar_pos, bar_index, downbeat, auto_fx;
    int32_t vocal_onset;
    float auto_score[5];      // 自动选灯效的五个候选分数，排查用
    float auto_duty, auto_jit, auto_perc, auto_split;
    uint8_t px[LAMP_NUM_LEDS * 3];   // C++ 效果层渲染的 96 个 RGB
} LampFrameC;

// 线程模型：每个 handle 只能被一个线程用。调用方给每条连接/每路音频
// 建一个 handle，互不共享。
void *lamp_create(void);
void  lamp_destroy(void *h);

// fx < 0 → 自动模式；否则锁到该灯效。white_balance 非零则开白平衡。
void lamp_set_effect(void *h, int32_t fx, int32_t white_balance);

// 送进管线前的手动增益，补偿麦克风远近。非有限值与非正值被忽略，上限 1000。
void lamp_set_input_gain(void *h, float g);

// 锁到某个档位；preset < 0 表示放开自动切换。
void lamp_lock_preset(void *h, int32_t preset);

// 布局自检：调用方算出的 sizeof 必须与它一致，不一致就该在启动时崩。
int32_t lamp_frame_size(void);

float   lamp_sample_rate(void);
int32_t lamp_num_bands(void);
int32_t lamp_num_leds(void);

// ── 灯效目录 ──────────────────────────────────────────────
// 名字、中文名、特征标签、风格推荐全从 lamp_fx.h 那一份表出，
// 调用方不要另抄一份。返回的 const char* 是静态存储，不必释放。
int32_t     lamp_fx_count(void);
const char *lamp_fx_name(int32_t i);
const char *lamp_fx_name_cn(int32_t i);
int32_t     lamp_fx_tags(int32_t i);     // 位掩码：1=节拍 2=旋律 4=人声 8=氛围 16=频谱
int32_t     lamp_fx_genres(int32_t i);   // 位掩码：1=古典 2=流行 4=摇滚 8=Rap 16=电子

// 喂一批样本，尽可能多地产出分析帧。
//
// 返回本次产出的帧数，最多 max_out。样本按 hop 步进消费，剩余的留在环里
// 等下一批 —— 调用方不必按帧长对齐，来多少喂多少即可。
int32_t lamp_feed(void *h, const float *pcm, int32_t count,
                  LampFrameC *out, int32_t max_out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LAMP_CAPI_H
