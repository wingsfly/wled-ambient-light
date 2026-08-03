// Arduino 垫片（主机端）。
//
// 只提供 WLED 的 FX.cpp / FX_fcn.cpp / colors.cpp / FXparticleSystem.cpp
// 真正用到的那些东西 —— 实测是：millis / micros / delay / yield /
// hw_random* / constrain / map / PROGMEM 一族 / Serial / ESP。
//
// **时间是注入的，不是墙上时钟。** 见 hostFxSetNow()：
// 模拟器要能按任意速率喂帧、要能复现，读真实 millis() 两条都做不到。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <type_traits>

// wled_math.cpp 只 include 了 Arduino.h，PI 一族要在这里给全
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#ifndef M_PI
#define M_PI PI
#endif
#ifndef M_TWOPI
#define M_TWOPI (2.0 * M_PI)
#endif
#ifndef HALF_PI
#define HALF_PI (M_PI / 2.0)
#endif
#ifndef TWO_PI
#define TWO_PI M_TWOPI
#endif
#define radians(d) ((d) * M_PI / 180.0)
#define degrees(r) ((r) * 180.0 / M_PI)
#define sq(x) ((x) * (x))
#define word(...) ((uint16_t)0)
#define sprintf_P   sprintf
#define snprintf_P  snprintf
#define strncmp_P      strncmp
#define strncasecmp_P  strncasecmp
#define strcasecmp_P   strcasecmp

typedef uint8_t  byte;
typedef bool     boolean;

// PROGMEM 在主机上是普通内存。WLED 的效果名与参数表全是 PROGMEM 字符串，
// 界面要读它们，所以这几个宏必须是**能真的读到数据**的空操作。
#define PROGMEM
#define PGM_P            const char *
#define PSTR(s)          (s)
#define F(s)             (s)
#define FPSTR(s)         (reinterpret_cast<const char *>(s))
#define pgm_read_byte(a)  (*(const uint8_t  *)(a))
#define pgm_read_word(a)  (*(const uint16_t *)(a))
// ⚠️ **不能写成 `*(const uint32_t*)a`。**
//
// ESP32 上指针是 32 位，所以 WLED 里有 `(byte*)pgm_read_dword(&gGradientPalettes[i])`
// 这样的写法 —— 用它读一个**指针**。主机是 64 位，按 uint32_t 读会把指针砍掉一半，
// 得到 0x42c800 这种野指针，一解引用就是段错误。
//
// 改成按**目标处的实际类型**读，宽度自然正确：指针数组读出指针，
// 数值数组读出数值。这个 bug 花了不少工夫才找到 —— 现象是「效果 38 Aurora 崩溃」，
// 而根因在一个看起来人畜无害的宏里。
// 指针类型的目标 → 原样返回指针（避免截断）；其余 → 按 uint32_t 读。
// fastled_slim 里 `u.dword = pgm_read_dword(progent)` 要的是 uint32_t，
// 而 FX_fcn 里 `(byte*)pgm_read_dword(&gGradientPalettes[i])` 要的是指针，
// 两种用法必须都对。
template <class T>
static inline typename std::enable_if<std::is_pointer<T>::value, T>::type
hostPgmRead(const T *p) { return *p; }
template <class T>
static inline typename std::enable_if<!std::is_pointer<T>::value, uint32_t>::type
hostPgmRead(const T *p) { return *(const uint32_t *)p; }
#define pgm_read_dword(a) hostPgmRead(a)
#define strlen_P          strlen
#define strncpy_P         strncpy
#define strcpy_P          strcpy
#define memcpy_P          memcpy
#define strcmp_P          strcmp
#define strcat_P          strcat
#define pgm_read_byte_near(a)  (*(const uint8_t  *)(a))
#define pgm_read_word_near(a)  (*(const uint16_t *)(a))
#define pgm_read_dword_near(a) hostPgmRead(a)
// util.cpp 里的 IDF 版本判定与 SHA1（OTA 用的，与灯效无关）
// **冒充 ESP32，不是 ESP8266。**
//
// 真实目标是 ESP32-S3，而 WLED 有大量按架构分档的常量（粒子系统的
// MAXPARTICLES/MAXSOURCES、缓冲上限……，光 const.h 就有 28 处）。
// 冒充 ESP8266 只为了让 util.cpp 的一段加锁代码编过，代价却是
// **整个 WLED 都按另一块芯片编译** —— 那样模拟出来的就不是这台灯。
//
// 代价是要补三个 FreeRTOS 符号，值得。
#define ARDUINO_ARCH_ESP32 1
static inline bool can_yield() { return true; }
typedef void *SemaphoreHandle_t;
#define pdFALSE 0
#define pdTRUE  1
static inline int  xSemaphoreTakeRecursive(SemaphoreHandle_t, int) { return pdTRUE; }
static inline int  xSemaphoreGiveRecursive(SemaphoreHandle_t)      { return pdTRUE; }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex()   { return nullptr; }

#define ESP_IDF_VERSION           0
#define ESP_IDF_VERSION_VAL(a,b,c) 1
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define DRAM_ATTR
#define IRAM_ATTR
#define ICACHE_RAM_ATTR

class __FlashStringHelper;

#ifndef min
template <class T, class U> constexpr auto min(T a, U b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <class T, class U> constexpr auto max(T a, U b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
#endif
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
// **不要 #define abs** —— 它会把 std::abs 也砸掉（FX.cpp 2084 行用了）。
// C++ 标准库本来就有 abs 的各种重载。
static inline long map(long x, long a, long b, long c, long d) {
    return (b == a) ? c : (x - a) * (d - c) / (b - a) + c;
}

// ── 时间 ──────────────────────────────────────────────────
//
// **由调用方注入。** 模拟器要按任意速率喂帧（比实时快得多），
// 而且同一段输入必须每次得到同一串画面 —— 读墙上时钟两条都做不到。
// 这与音乐律动那半边的纪律一致：那边的 now_ms 也是从已消费样本数算的。
extern uint32_t hostFxNowMs;
static inline uint32_t millis() { return hostFxNowMs; }
static inline uint32_t micros() { return hostFxNowMs * 1000u; }
static inline void delay(uint32_t)           {}
static inline void delayMicroseconds(uint32_t) {}
static inline void yield()                   {}

// ── 随机 ──────────────────────────────────────────────────
//
// 也用确定性的发生器，理由同上：不可复现的画面没法比对，也没法回归。
extern uint32_t hostFxRngState;
static inline uint32_t hostFxRand() {
    // xorshift32，够用且确定
    uint32_t x = hostFxRngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return hostFxRngState = x;
}
// **hw_random* 不在这里定义** —— 用 fcn_declare.h 里 WLED 自己的那份。
// 那些取模/缩放算术是效果画面的一部分（比如 hw_random16(lim) 用的是
// `(r * lim) >> 16` 而不是 `r % lim`，分布不同）。这里只换熵源：
// wled.h 把 REG_READ(WDEV_RND_REG) 指到上面的 hostFxRand()。

// ── 日志与芯片信息：全部吞掉 ──────────────────────────────
struct HostSerial {
    template <class T> void print(T)        {}
    template <class T> void println(T)      {}
    void println()                          {}
    template <class... A> void printf(A...) {}
    void begin(...)                         {}
    operator bool() const { return false; }
};
extern HostSerial Serial;

struct HostEsp {
    // 报一个**充裕且自洽**的内存量。粒子系统会按可用堆给自己定尺寸，
    // 报得太小、或者「空闲」与「最大连续块」互相矛盾，它算出的条目数
    // 会超过实际分到的缓冲 —— 表现为 mode_particle1DGEQ 写越界。
    uint32_t getFreeHeap()           const { return 4u * 1024u * 1024u; }
    uint32_t getMaxAllocHeap()       const { return 4u * 1024u * 1024u; }
    uint32_t getHeapSize()           const { return 8u * 1024u * 1024u; }
    uint32_t getFreePsram()          const { return 0; }
    uint32_t getPsramSize()          const { return 0; }
    void     restart()               const {}
    uint32_t getFlashChipId()        const { return 0x1640EF; }
    uint32_t getFlashChipSize()      const { return 4u * 1024u * 1024u; }
    uint32_t getChipId()             const { return 0xABCDEF; }
    uint32_t getFlashChipVendorId()  const { return 0xEF; }
    uint32_t getCpuFreqMHz()         const { return 240; }
    const char *getSdkVersion()      const { return "host"; }
};
extern HostEsp ESP;

// WLED 若干处用 String 只做日志与名字，这里给个最小可用的实现
#include <string>
struct String : public std::string {
    String() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}
    String(int v)      : std::string(std::to_string(v)) {}
    String(unsigned v) : std::string(std::to_string(v)) {}
    const char *c_str() const { return std::string::c_str(); }
    bool endsWith(const char *s) const {
        const size_t n = strlen(s);
        return size() >= n && compare(size() - n, n, s) == 0;
    }
    bool startsWith(const char *s) const { return rfind(s, 0) == 0; }
    int  indexOf(const char *s) const { auto p = find(s); return p == npos ? -1 : (int)p; }
    int  indexOf(char c)        const { auto p = find(c); return p == npos ? -1 : (int)p; }
    int  indexOf(char c, size_t from)        const { auto p = find(c, from); return p == npos ? -1 : (int)p; }
    int  indexOf(const char *s, size_t from) const { auto p = find(s, from); return p == npos ? -1 : (int)p; }
    String substring(size_t a)           const { return String(std::string::substr(a)); }
    String substring(size_t a, size_t b) const { return String(std::string::substr(a, b - a)); }
    char   charAt(size_t i) const { return i < size() ? (*this)[i] : '\0'; }
    int    toInt()   const { return atoi(c_str()); }
    float  toFloat() const { return (float)atof(c_str()); }
};
