// 主机端粘合层：全局量定义、BusManager 的内存实现，以及给 Python 用的 C API。
//
// 这是**唯一**由我写的、参与到画面里的代码，而它只做一件事：
// 把 WLED 写进「总线」的像素接住，放进一块内存。效果逻辑、调色板、
// 缓动、粒子系统全是上游原件（见 README.md）。
//
// 两处刻意与真机不同，都是为了模拟器能用：
//   · 时间由调用方注入（hostFxNowMs），不是墙上时钟 —— 要能按任意速率喂帧、要能复现
//   · 随机数是确定性的 xorshift —— 同一段输入每次必须得到同一串画面
#include "wled.h"

#include <vector>
#include <memory>
#include <string>
#include <string.h>
#include <stdio.h>

// ── WLED 的全局量 ─────────────────────────────────────────
// 取值就是出厂默认，这样主机端画出来的与真机默认设置下一致。
WS2812FX      strip;
CRGBPalette16 currentPalette;
uint8_t       paletteBlend   = 0;
uint8_t       blendingStyle  = 0;
bool          gammaCorrectCol = true;
bool          gammaCorrectBri = false;
float         gammaCorrectVal = 2.8f;
bool          correctWB      = false;
bool          cctFromRgb     = false;
uint8_t       realtimeMode   = 0;
bool          realtimeOverride = false;
bool          realtimeRespectLedMaps = false;
uint8_t       briS  = 128;
uint8_t       bri   = 128;
uint8_t       briT  = 128;
uint8_t       briOld = 128;
byte          errorFlag = 0;
bool          stateChanged = false;
uint8_t       lastRandomIndex = 0;
bool          useHarmonicRandomPalette = true;
uint16_t      randomPaletteChangeTime  = 5;
uint8_t       randomPaletteChangeState = 0;
bool          useMainSegmentOnly = false;
bool          useParallelI2S = false;
bool          arlsDisableGammaCorrection = true;
bool          useAMPM = false;
uint32_t      localTime = 0;
uint8_t       currentLedmap = 0;
uint8_t       interfaceUpdateCallMode = 0;
uint32_t      ledMaps = 1;
char         *ledmapNames[16] = {nullptr};
char          settingsPIN[64] = "";
char          cmDNS[64] = "wled";
char          serverDescription[64] = "host-sim";
String        escapedMac = String("aabbccddeeff");
bool          correctPIN = true;
bool          jsonBufferLock = false;
uint32_t      lastEditTime = 0;
SemaphoreHandle_t jsonBufferLockMutex = nullptr;
std::vector<BusConfig> busConfigs;
// 类型照 colors.h 里的真实声明，不猜
std::vector<CRGBPalette16>  customPalettes;
std::vector<UsermodPalette> usermodPalettes;
JsonDocument *pDoc = nullptr;
HostSerial    Serial;
HostEsp       ESP;
HostFS        WLED_FS;
HostUpdate    Update;
HostWiFi      WiFi;

// 注入的时间与随机数（见 Arduino.h 的说明）
uint32_t hostFxNowMs    = 0;
uint32_t hostFxRngState = 0x2545F491u;

// 真身在 led.cpp，那文件与灯效无关
uint8_t scaledBri(uint8_t in) { return in; }

// ── BusManager：把像素接进一块内存 ────────────────────────
// FX_fcn.cpp 2166 行里只有这几处碰硬件。
static std::vector<uint32_t> g_pixels;
// 灯管几何。与音乐律动那半边一致：两段各 48 颗。
static const uint16_t kTotalLeds = 96;

// 一条「主机总线」：派生自上游的 Bus，只把像素写进内存。
// Bus 只有两个纯虚函数（show / setPixelColor），派生很轻。
//
// **必须真的把它放进 BusManager::busses。** 第一版的 add() 直接返回 0
// 却什么都不放，而 finalizeInit() 紧接着就 `busses.back()->getBusSize()` ——
// 空 vector 上取 back，段错误。
class HostBus : public Bus {
  public:
    HostBus(uint16_t start, uint16_t len)
      : Bus(TYPE_WS2812_RGB, start, RGBW_MODE_MANUAL_ONLY, len, false, false) {
        _valid = true;
    }
    void     show() override {}
    void     setPixelColor(unsigned pix, uint32_t c) override {
        const unsigned i = _start + pix;
        if (i < g_pixels.size()) g_pixels[i] = c;
    }
    uint32_t getPixelColor(unsigned pix) const override {
        const unsigned i = _start + pix;
        return (i < g_pixels.size()) ? g_pixels[i] : 0;
    }
    size_t   getBusSize() const override { return sizeof(HostBus); }
};

namespace BusManager {
  std::vector<std::unique_ptr<Bus>> busses;
  uint16_t _gMilliAmpsUsed = 0;
  uint16_t _gMilliAmpsMax  = 4500;
  bool     _useABL = false;

  void    initializeABL() {}
  void    removeAll()     { busses.clear(); }
  int     add(const BusConfig &bc, bool) {
      busses.push_back(std::unique_ptr<Bus>(new HostBus(bc.start, bc.count)));
      return (int)busses.size() - 1;
  }
  uint8_t getI(uint8_t, const uint8_t *, uint8_t) { return 0; }
  void    setSegmentCCT(int16_t, bool) {}
  void    show() {}
  unsigned g_setCalls = 0, g_maxIdx = 0, g_nonBlack = 0; uint32_t g_lastC = 0;
  void    setPixelColor(unsigned pix, uint32_t c) {
      ++g_setCalls;
      if (pix > g_maxIdx) g_maxIdx = pix;
      if (c & 0xFFFFFF) { ++g_nonBlack; g_lastC = c; }
      if (pix < g_pixels.size()) g_pixels[pix] = c;
  }
}
uint8_t Bus::_gAWM = 255;
int16_t Bus::_cct  = -1;
size_t  BusConfig::memUsage() const { return 0; }

// 主机端没有 usermod。音频反应类效果因此会走「没有音频数据」的分支 ——
// 那正是我们想要的：这一页展示的是**固定灯效**，音乐律动在另一页。
bool UsermodManager::getUMData(um_data_t **data, uint8_t) {
    if (data) *data = nullptr;
    return false;
}

// ── C API ─────────────────────────────────────────────────
extern "C" {

// 灯管几何。与音乐律动那半边一致：两段各 48 颗。


int32_t wledfx_init(void) {
    g_pixels.assign(kTotalLeds, 0);
    // 建一条 96 颗的总线，再让 WLED 自己 finalizeInit。
    // 段落交给 WLED 的 makeAutoSegments() 建 —— 手动摆段等于替它做决定，
    // 而我们要看的正是「WLED 默认会怎么摆」。
    BusConfig bc(TYPE_WS2812_RGB, (uint8_t[]){2, 255, 255, 255, 255}, 0, kTotalLeds);
    busConfigs.push_back(bc);
    // **gamma 表必须先算。** WLED 在启动流程里调它，而我们没走那条流程 ——
    // 漏掉的话 gamma8inv(255) 返回 0，混合时的不透明度就是 0，画面全黑。
    // 查这个花了不少工夫：效果在跑、段缓冲有像素、show() 也在写总线，
    // 唯独写进去的值全是 0。
    NeoGammaWLEDMethod::calcGammaTable(gammaCorrectVal);
    strip.finalizeInit();
    strip.setBrightness(255);
    strip.makeAutoSegments(true);
    return (int32_t)strip.getModeCount();
}

// 效果的名字与参数表：**直接来自 WLED 自己的 _modeData**。
// 格式是 "Name@参数1,参数2;颜色;调色板;标志;默认值"，界面照着解析即可 ——
// 于是效果列表、滑块标签、默认值全都与真机一致，不用我手抄一份。
const char *wledfx_mode_data(int32_t i) {
    if (i < 0 || i >= (int32_t)strip.getModeCount()) return "";
    return strip.getModeData((uint8_t)i);
}

int32_t wledfx_mode_count(void)    { return (int32_t)strip.getModeCount(); }

// 主机端跑不了的效果 —— 返回 1 表示「界面里灰掉」。
//
// 目前只有一个：「PS GEQ 1D」（粒子系统 + 频谱）在 -O2 下写越界一个字节
// （ASan：heap-buffer-overflow，位置在 FX.cpp 的 `PartSys->sources[i]` 循环），
// -O1 下又不复现。**我没有定位到根因**，因此不确定是这层移植垫片的问题
// 还是上游在 96 颗 1D 灯带上的问题 —— 没有真机可以对照。
//
// 不排除的话，它会把整个模拟服务打挂。如实标注、灰掉，不假装它能用。
int32_t wledfx_mode_blocked(int32_t i) {
    const char *d = wledfx_mode_data(i);
    return (d && strncmp(d, "PS GEQ 1D", 9) == 0) ? 1 : 0;
}
int32_t wledfx_palette_count(void) { return (int32_t)getPaletteCount(); }

void wledfx_set(int32_t mode, int32_t speed, int32_t intensity,
                int32_t palette, uint32_t c0, uint32_t c1, uint32_t c2) {
    Segment &seg = strip.getMainSegment();
    if (mode >= 0 && mode < (int32_t)strip.getModeCount() && !wledfx_mode_blocked(mode))
        seg.setMode((uint8_t)mode, true);
    seg.speed     = (uint8_t)(speed     & 0xFF);
    seg.intensity = (uint8_t)(intensity & 0xFF);
    if (palette >= 0) seg.setPalette((uint8_t)palette);
    seg.setColor(0, c0); seg.setColor(1, c1); seg.setColor(2, c2);
}

// 渲染到 now_ms，把 96 个像素写进 out（RGB，每颗 3 字节）。
//
// **时间由调用方给**，所以模拟器可以按任意速率跑，而且同一串时间戳
// 每次得到同一串画面 —— 这与音乐律动那半边的纪律一致。
void wledfx_render(uint32_t now_ms, uint8_t *out) {
    hostFxNowMs = now_ms;
    strip.service();
    for (uint16_t i = 0; i < kTotalLeds; ++i) {
        const uint32_t c = (i < g_pixels.size()) ? g_pixels[i] : 0;
        out[i * 3 + 0] = (uint8_t)(c >> 16);
        out[i * 3 + 1] = (uint8_t)(c >>  8);
        out[i * 3 + 2] = (uint8_t)(c);
    }
}

void wledfx_reset_rng(uint32_t seed) { hostFxRngState = seed ? seed : 1u; }

// 排查用：把关键内部状态吐出来。线上/本机对不上的时候，猜是猜不出来的。
void wledfx_debug(int32_t *out) {
    Segment &seg = strip.getMainSegment();
    out[0] = (int32_t)strip.getSegmentsNum();
    out[1] = (int32_t)strip.getLengthTotal();
    out[2] = seg.isActive() ? 1 : 0;
    out[3] = seg.on ? 1 : 0;
    out[4] = (int32_t)seg.opacity;
    out[5] = (int32_t)seg.mode;
    out[6] = (int32_t)seg.start;
    out[7] = (int32_t)seg.stop;
    out[8] = (int32_t)strip.getBrightness();
    out[9] = (int32_t)strip.getFrameTime();
    out[10] = strip.isSuspended() ? 1 : 0;
    out[11] = (int32_t)BusManager::busses.size();
    // strip 内部缓冲里有多少非黑 —— 用来分清「效果没画」还是「画了没传出来」
    int lit = 0;
    for (uint16_t i = 0; i < kTotalLeds; ++i) if (strip.getPixelColor(i)) ++lit;
    out[12] = lit;
    out[13] = (int32_t)seg.call;          // 效果函数被调用了几次
    out[14] = (int32_t)strip.getFps();
    out[15] = (int32_t)Segment::maxWidth;
    out[16] = (int32_t)Segment::maxHeight;
    out[17] = (int32_t)seg.virtualLength();
    out[18] = (int32_t)seg.length();
    // 直接从段里读一颗，绕开 strip 的读回路径
    out[19] = (int32_t)(seg.getPixelColor(0) & 0xFFFFFF);
    out[20] = (int32_t)seg.freeze;
    out[21] = (int32_t)BusManager::g_setCalls;
    out[22] = (int32_t)BusManager::g_maxIdx;
    out[23] = (int32_t)BusManager::g_nonBlack;
    out[24] = (int32_t)(BusManager::g_lastC & 0xFFFFFF);
    out[25] = strip.isMatrix ? 1 : 0;
    out[26] = -1;
    out[27] = (int32_t)seg.currentBri();
    out[28] = (int32_t)strip.getTransition();
}

} // extern "C"
