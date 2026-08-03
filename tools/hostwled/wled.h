// wled.h 垫片（主机端）。
//
// 真的那份会拖进 WiFi / MQTT / AsyncWebServer / LittleFS —— 实测缺 56 个符号，
// 其中绝大多数与灯效毫无关系。这里只留 FX 真正需要的东西。
//
// **`fcn_declare.h` 用的是上游原件，不是桩。** `sin8` / `beatsin8` /
// `hw_random16` / 调色板取色全在那里面，那些是效果画面的一部分，
// 换成我自己写的就不是 WLED 了。它签名里出现的网络/JSON 类型在下面
// 前向声明或给个空定义 —— 只要类型完整，声明就能编过，而那些函数
// 一个都不会被调用。
#pragma once

#include <Arduino.h>
#include <vector>
#include <cstddef>

// ── 只为让 fcn_declare.h 的声明成立的类型 ────────────────
// 这些函数在主机端一个都不会被调用；类型完整即可。
class EspalexaDevice;
class AsyncWebServerRequest; class AsyncWebServer;   class AsyncWebHandler;
class AsyncWebSocket;        class AsyncWebSocketClient;
class AsyncClient;           class AsyncMqttClient;
class AsyncUDP;              class AsyncUDPPacket;
class DNSServer;             class WiFiUDP;
class SPIFFSEditor;
// ArduinoJson 的类型：fcn_declare.h 里按值/按引用都有，得给完整定义。
// 这些函数在主机端一个都不会被调用。
struct JsonVariant  {
                      // 只留一个隐式转换到 int —— 多留几个会让
                      // `constrain(v, -1, 1)` 这类比较变成二义
                      operator int() const { return 0; }
                      // util.cpp 有 `const char* s = elem;`。固定成这一个重载
                      // （而不是模板转换），才不会让 `constrain(v,-1,1)` 变二义。
                      operator const char *() const { return nullptr; }
                      template <class T> T as() const { return T(); }
                      template <class T> JsonVariant &operator=(T) { return *this; }
                      bool isNull() const { return true; }
                      template <class T> bool is() const { return false; }
                      size_t size() const { return 0; }
                      JsonVariant operator[](int) const { return {}; }
                      const JsonVariant *begin() const { return nullptr; }
                      const JsonVariant *end()   const { return nullptr; } };
struct JsonObject   { JsonVariant operator[](const char *) const { return {}; }
                      bool isNull() const { return true; }
                      template <class T> bool containsKey(T) const { return false; } };
struct JsonArray    { JsonArray() {}
                      JsonArray(const JsonVariant &) {}   // colors.cpp 有隐式转换
                      JsonVariant operator[](int) const { return {}; }
                      size_t size() const { return 0; }
                      bool isNull() const { return true; }
                      const JsonVariant *begin() const { return nullptr; }
                      const JsonVariant *end()   const { return nullptr; } };
struct JsonDocument { template <class T> T as() const { return T(); }
                      JsonVariant operator[](const char *) { return {}; }
                      void clear() {} };
struct e131_packet_t { uint8_t data[1]; };
typedef int AwsEventType;
typedef int WiFiEvent_t;
typedef int WebRequestMethodComposite;
enum { HTTP_GET = 1, HTTP_POST = 2, HTTP_PUT = 4, HTTP_PATCH = 8 };
#define CONTENT_TYPE_JSON "application/json"

struct Print {
    template <class... A> size_t printf_P(A...) { return 0; }
    template <class... A> size_t printf(A...)   { return 0; }
    virtual size_t write(uint8_t) { return 0; }
    virtual size_t write(const uint8_t *, size_t n) { return n; }
    template <class T> size_t print(T)   { return 0; }
    template <class T> size_t println(T) { return 0; }
};

struct IPAddress {
    uint8_t b[4] = {0, 0, 0, 0};
    IPAddress() {}
    IPAddress(uint8_t a, uint8_t c, uint8_t d, uint8_t e) { b[0]=a; b[1]=c; b[2]=d; b[3]=e; }
    IPAddress(uint32_t) {}
    uint8_t  operator[](int i) const { return b[i & 3]; }
    uint8_t &operator[](int i)       { return b[i & 3]; }
    operator uint32_t() const { return 0; }
    bool operator==(const IPAddress &) const { return true; }
};

// ── 主机上不存在的平台设施 ────────────────────────────────
// 随机数：**只替换熵源**。WLED 自己的 hw_random16(limit) 等取模算术
// 仍然走 fcn_declare.h 里的原件 —— 那是画面的一部分。
#define WDEV_RND_REG 0
#define REG_READ(x)  ((uint32_t)hostFxRand())

// ESP-IDF 的堆接口
static inline void  *heap_caps_malloc(size_t n, int)  { return malloc(n); }
static inline void  *heap_caps_calloc(size_t n, size_t m, int) { return calloc(n, m); }
static inline void  *heap_caps_realloc(void *p, size_t n, int) { return realloc(p, n); }
static inline void   heap_caps_free(void *p)          { free(p); }
static inline size_t heap_caps_get_free_size(int)          { return 4u * 1024u * 1024u; }
static inline size_t heap_caps_get_largest_free_block(int) { return 4u * 1024u * 1024u; }

#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_INTERNAL 0
#define LEDC_CHANNEL_MAX     8
#define LEDC_SPEED_MODE_MAX  1
#ifndef M_TWOPI
#define M_TWOPI (2.0 * M_PI)
#endif

#include "const.h"
#include "colors.h"
#include "fcn_declare.h"
#include "bus_manager.h"
#include "FX.h"

// 调试宏：全部吞掉
#define DEBUG_PRINT(...)     do {} while (0)
#define DEBUG_PRINTLN(...)   do {} while (0)
#define DEBUG_PRINTF(...)    do {} while (0)
#define DEBUG_PRINTF_P(...)  do {} while (0)
#define DEBUGFX_PRINT(...)   do {} while (0)
#define DEBUGFX_PRINTLN(...) do {} while (0)
#define DEBUGFX_PRINTF(...)  do {} while (0)
#define DEBUGFX_PRINTF_P(...) do {} while (0)
// 文件系统：只有 ledmap 加载用得到，而模拟器不需要自定义映射。
// 给个 exists() 恒 false 的空壳，那条分支自然走「没有 ledmap」的路径。
struct File {
    operator bool() const { return false; }
    size_t read(uint8_t *, size_t) { return 0; }
    size_t readBytes(char *, size_t) { return 0; }
    int    read()  { return -1; }
    size_t size()  const { return 0; }
    bool   find(const char *) { return false; }
    void   close() {}
    bool   seek(size_t) { return false; }
    int    available() const { return 0; }
    size_t readBytesUntil(char, char *, size_t) { return 0; }
    File   openNextFile() { return {}; }
    const char *name() const { return ""; }
    bool   isDirectory() const { return false; }
    size_t position() const { return 0; }
};
struct HostFS {
    bool exists(const char *) const { return false; }
    File open(const char *, const char * = "r") const { return {}; }
};
extern HostFS WLED_FS;
template <size_t N> struct StaticJsonDocument : JsonDocument {};
extern JsonDocument *pDoc;

// 位操作宏（Arduino 提供，主机没有）
#define bitRead(v, b)   (((v) >> (b)) & 1)
#define bitSet(v, b)    ((v) |=  (1UL << (b)))
#define bitClear(v, b)  ((v) &= ~(1UL << (b)))
#define bitWrite(v, b, x) ((x) ? bitSet(v, b) : bitClear(v, b))

// 时间相关：FX_fcn 的 nightlight/时钟效果会读，主机上给固定值
static inline int hour(uint32_t)   { return 12; }
static inline int minute(uint32_t) { return 0; }
static inline int second(uint32_t) { return 0; }
static inline int day(uint32_t)    { return 1; }
static inline int month(uint32_t)  { return 1; }
static inline int year(uint32_t)   { return 2026; }
static inline const char *monthShortStr(int) { return "Jan"; }
static inline const char *monthStr(int)      { return "January"; }
static inline const char *dayShortStr(int)   { return "Mon"; }
static inline const char *dayStr(int)        { return "Monday"; }
static inline int weekday(uint32_t)          { return 2; }

// ── FX.cpp / FX_fcn.cpp 真正读到的全局量 ──────────────────
// 定义在 host_glue.cpp 里。取值就是 WLED 的出厂默认，
// 这样主机端画出来的与真机默认设置下一致。
extern WS2812FX      strip;
extern CRGBPalette16 currentPalette;
extern uint8_t       paletteBlend;
extern uint8_t       blendingStyle;
extern bool          gammaCorrectCol;
extern bool          gammaCorrectBri;
extern float         gammaCorrectVal;
extern bool          correctWB;
extern bool          cctFromRgb;
extern uint8_t       realtimeMode;
extern uint8_t       briS;
extern uint8_t       bri;
extern byte          errorFlag;
extern bool          stateChanged;
extern uint8_t       lastRandomIndex;
extern bool          useHarmonicRandomPalette;
extern uint16_t      randomPaletteChangeTime;
extern uint8_t       randomPaletteChangeState;
extern uint8_t       briT;
extern uint8_t       briOld;
extern bool          realtimeOverride;
extern bool          useMainSegmentOnly;
extern bool          useParallelI2S;
extern bool          arlsDisableGammaCorrection;
extern bool          useAMPM;
extern uint32_t      localTime;
extern std::vector<BusConfig> busConfigs;
extern uint8_t  currentLedmap;
extern uint8_t  interfaceUpdateCallMode;
// util.cpp 里网络/设置相关的全局（与灯效无关，但它同文件里有 perlin/beatsin）
extern char     cmDNS[];
extern char     serverDescription[];
extern String   escapedMac;
extern bool     correctPIN;
extern bool     jsonBufferLock;
extern uint32_t lastEditTime;
extern SemaphoreHandle_t jsonBufferLockMutex;
extern uint32_t ledMaps;
extern char    *ledmapNames[16];      // 指针数组：util.cpp 里 free() + 赋 nullptr
extern char     settingsPIN[64];
// bl_* 由 util.cpp 自己定义（RTC_NOINIT_ATTR），不要在这里 extern
static inline uint32_t getRtcMillis() { return hostFxNowMs; }

// ESP 复位原因：util.cpp 的崩溃统计要用，主机上恒为「软复位」
typedef int esp_reset_reason_t;
enum { ESP_RST_SW = 3, ESP_RST_PANIC = 4, ESP_RST_INT_WDT = 5,
       ESP_RST_TASK_WDT = 6, ESP_RST_WDT = 7, ESP_RST_BROWNOUT = 9,
       RTCWDT_BROWN_OUT_RESET = 9 };
static inline esp_reset_reason_t esp_reset_reason()        { return ESP_RST_SW; }
static inline int                rtc_get_reset_reason(int) { return ESP_RST_SW; }
#define SOC_DRAM_LOW  0x3FC80000u
#define SOC_DRAM_HIGH 0x3FCE0000u

struct HostUpdate { bool isRunning() const { return false; }
                    bool canRollBack() const { return false; }
                    bool rollBack()    const { return false; } };
extern HostUpdate Update;
struct HostWiFi   { int status() const { return 0; }
                    String macAddress() const { return String("AABBCCDDEEFF"); }
                    void   macAddress(uint8_t *m) const { if (m) memset(m, 0xAB, 6); } };
extern HostWiFi WiFi;
