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
        // ⚠️ **这三行必须有。** _hasRgb/_hasWhite/_hasCCT 不是基类 Bus 构造
        // 里设的，而是 BusDigital / BusPwm 各自在构造里设的
        // （bus_manager.cpp:156-158）。HostBus 直接派生自 Bus，不补就是
        // 未初始化的裸 bool，实测读出来是 false。
        //
        // 后果比看上去严重得多：Segment::refreshLightCapabilities() 于是
        // 判定这个段「没有 RGB 能力」，而 color_from_palette() 的第一行是
        //     if ((palette == 0 && mcol < NUM_COLORS) || !_isRGB) return 段颜色;
        // —— **所有调色板全部失效**，220 个效果里那 194 个调色板驱动的
        // 统统退化成单色/三原色。表现出来就是「WLED 的彩色灯效怎么这么少」。
        _hasRgb   = hasRGB(TYPE_WS2812_RGB);
        _hasWhite = hasWhite(TYPE_WS2812_RGB);
        _hasCCT   = hasCCT(TYPE_WS2812_RGB);
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
// 目前只有一个：「PS Dancing Shadows」。**这是上游的 bug，不是移植垫片的**：
//
//   FX.cpp  uint32_t partidx = PartSys->sprayEmit(PartSys->sources[0]);
//           PartSys->particles[partidx].ttl = ttl;
//
// `ParticleSystem1D::sprayEmit()` 找不到死粒子时返回 **-1**，调用处却用
// uint32_t 接下来直接当下标。而它的准入门槛是 `deadparticles > 5`（≥6 颗），
// 循环却要发 `width = hw_random16(1,10)` 最多 9 颗 —— 第 7 颗必然踩空。
// 实测崩溃现场：deadparticles=6、width=9、usedParticles=49。
//
// 64 位主机上 0xFFFFFFFF 零扩展成 +34GB，直接段错误；ESP32 上 32 位指针
// 算术回绕成 particles[-1]，**真机不崩，只是悄悄改写 ParticleSystem1D
// 结构体尾部** —— 所以上游一直没发现。这里如实灰掉，不假装它能用。
//
// 曾经还屏蔽过「PS GEQ 1D」，那个是**我自己的锅**：Arduino.h 里的 min/max
// 垫片返回了悬垂引用，-O2 下把 calculateNumberOfSources1D() 折成常量 0，
// 于是按 0 个 source 申请内存、按 16 个去写。已修，见该文件的说明。
//
// 另外「Copy Segment」也灰掉，但那是**模拟器的限制不是效果的缺陷**：
// 它要从另一个段取像素，而这里只建了一个 96 颗的段，画面必然全黑。
int32_t wledfx_mode_blocked(int32_t i) {
    const char *d = wledfx_mode_data(i);
    if (!d) return 0;
    return (strncmp(d, "PS Dancing Shadows", 18) == 0 ||
            strncmp(d, "Copy Segment", 12) == 0) ? 1 : 0;
}
int32_t wledfx_palette_count(void) { return (int32_t)getPaletteCount(); }

// ── 调色板 ────────────────────────────────────────────────
//
// **调色板 ID 不是 0..count-1 的连续区间。** 固定板是 0..71，自定义板的 ID
// 从 200 **往下**长（customPalettes[0] 就是 ID 200），usermod 板从 255 往下。
// 界面必须按 ID 走，不能拿 count 当滑块上限 —— 一有自定义板就错位。

// Segment::loadPalette() 是 protected 的，_default_palette 更是 private。
// 与其想办法绕过访问控制（这个仓库刚被一次 UB 咬过，见 Arduino.h 里
// min/max 的说明），不如走上游自己每帧都在走的那条路：
//
//   Segment::beginDraw() → loadPalette(Segment::_currentPalette, palette)
//
// beginDraw 与 getCurrentPalette 都是 public。在主段上临时换个板号跑一次，
// 拿到的就是效果真正会用的那份表 —— 包括 0 号「Default」随效果变、
// 2-5 号由段颜色生成这些细节，一个都不用我重写。
//
// 只写 seg.palette 字段而**不调 setPalette()**：后者会起过渡，
// 我们只是想看一眼，不该改动画面。
namespace {
void hostLoadPalette(CRGBPalette16 &out, int32_t id) {
    Segment &seg = strip.getMainSegment();
    const uint8_t save = seg.palette;
    seg.palette = (uint8_t)id;
    seg.beginDraw(0xFFFFU);          // 0xFFFF：跳过过渡混色，要的是纯粹的目标板
    out = Segment::getCurrentPalette();
    seg.palette = save;
    seg.beginDraw(0xFFFFU);          // 还原 _currentPalette，别把下一帧带歪
}
}  // namespace

// 调色板名字：抽 WLED 自己的 JSON_palette_names，不另抄一份。
// 返回的是静态缓冲，调用方要立刻用掉（服务端有全局锁，够了）。
const char *wledfx_palette_name(int32_t id) {
    static char buf[40];
    if (id >= 0 && id < (int32_t)FIXED_PALETTE_COUNT) {
        buf[0] = 0;
        extractModeName((uint8_t)id, JSON_palette_names, buf, sizeof(buf) - 1);
        if (buf[0]) return buf;
    }
    const int slot = WLED_CUSTOM_PALETTE_ID_BASE - id;
    if (slot >= 0 && slot < (int)customPalettes.size()) {
        snprintf(buf, sizeof buf, "自定义 %d", slot + 1);
        return buf;
    }
    snprintf(buf, sizeof buf, "#%d", (int)id);
    return buf;
}

// 取样：n 个等距点的 RGB，直接来自上面那份表，
// 所以预览色与效果实际取到的色是同一个来源。
int32_t wledfx_palette_swatch(int32_t id, uint8_t *rgb, int32_t n) {
    if (!rgb || n < 2) return 0;
    CRGBPalette16 pal;
    hostLoadPalette(pal, id);
    for (int32_t i = 0; i < n; ++i) {
        const CRGB c = ColorFromPalette(pal, (uint8_t)((i * 255) / (n - 1)), 255, LINEARBLEND);
        rgb[i * 3 + 0] = c.r; rgb[i * 3 + 1] = c.g; rgb[i * 3 + 2] = c.b;
    }
    return n;
}

// 自定义调色板：stops 是 n 个 0xRRGGBB，等距铺开后交给 WLED 自己的
// loadDynamicGradientPalette() 插值 —— 与真机从 /palette{N}.json 读进来
// 的走同一条路（colors.cpp 的 loadCustomPalettes()），所以模拟出的渐变
// 与刷进灯里之后一致。
// slot < 0 表示追加。返回 WLED 的调色板 ID（200 - slot），失败返回 -1。
int32_t wledfx_custom_palette(int32_t slot, const uint32_t *stops, int32_t n) {
    if (!stops || n < 2) return -1;
    if (n > 16) n = 16;
    if (slot < 0) slot = (int32_t)customPalettes.size();
    if (slot >= (int32_t)WLED_MAX_CUSTOM_PALETTES) return -1;

    // 上游用 memset(tcp,255,...) 铺底，末项索引正好是 255 —— 那就是终止标记。
    byte tcp[4 * 16];
    memset(tcp, 255, sizeof tcp);
    for (int32_t i = 0; i < n; ++i) {
        tcp[i * 4 + 0] = (byte)((i * 255) / (n - 1));
        tcp[i * 4 + 1] = (byte)(stops[i] >> 16);
        tcp[i * 4 + 2] = (byte)(stops[i] >>  8);
        tcp[i * 4 + 3] = (byte)(stops[i]);
    }
    CRGBPalette16 p;
    p.loadDynamicGradientPalette(tcp);
    // ID 是往下长的，中间的空位补灰，免得后面的板串位（上游同样这么做）
    while ((int32_t)customPalettes.size() <= slot)
        customPalettes.push_back(CRGBPalette16(CRGB(128, 128, 128)));
    customPalettes[slot] = p;
    return WLED_CUSTOM_PALETTE_ID_BASE - slot;
}

int32_t wledfx_custom_palette_count(void) { return (int32_t)customPalettes.size(); }

// 「彩色度」0..100。
//
// **按实际画出来的像素测，不看 _modeData 声明的颜色槽。** 220 个效果里
// 有 194 个都声明「用调色板」，那个字段区分不出任何东西；真正决定观感的
// 是这个效果把调色板铺开了多少 —— 是扫过整个色环，还是只在橙红区打转。
//
// 口径：色相直方图（24 桶）的归一化熵 × 亮度加权的平均饱和度。
//   · 全黑 / 纯白 / 灰 → 0（饱和度为 0）
//   · 单一色相来回呼吸 → 低（熵为 0）
//   · 铺满色环 → 高
// 熵与饱和度相乘而不是相加：两者缺一就不算「彩色」。
//
// ⚠️ 会推进渲染状态（要真的跑帧才有像素可测）。调用方之后必须重新
// wledfx_set() 一次，服务端靠清掉参数缓存来保证这点。
int32_t wledfx_colorfulness(int32_t mode, int32_t palette, int32_t frames) {
    if (mode < 0 || mode >= (int32_t)strip.getModeCount() || wledfx_mode_blocked(mode)) return -1;
    if (frames < 12) frames = 12;
    Segment &seg = strip.getMainSegment();
    // **测量期间把过渡关掉。** 默认 700ms 的交叉淡入会让开头十几帧都还是
    // 上一个效果的画面 —— 第一版就栽在这里：Colorloop / Colorful 测出来的
    // 像素与 Solid 一模一样（都是 ff3d00），于是全都得 0 分。
    const uint16_t saveTr = strip.getTransition();
    strip.setTransition(0);
    seg.setMode((uint8_t)mode, true);          // 带默认参数，与界面首次选中时一致
    if (palette >= 0) seg.setPalette((uint8_t)palette);

    const int kBins = 24;
    double bins[kBins] = {0};
    double satSum = 0, valSum = 0;
    const uint32_t t0 = hostFxNowMs;
    for (int32_t f = 0; f < frames; ++f) {
        hostFxNowMs = t0 + (uint32_t)(f + 1) * 25u;
        strip.service();
        if (f * 3 < frames) continue;          // 前 1/3 让过渡与效果自身的启动跑完
        for (size_t i = 0; i < g_pixels.size(); ++i) {
            const uint32_t c = g_pixels[i];
            const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            if (mx == 0) continue;
            const double v = mx / 255.0;
            const double sat = (mx - mn) / (double)mx;
            valSum += v;
            satSum += v * sat;
            if (mx == mn) continue;            // 灰：没有色相可言
            const double d = mx - mn;
            double h;                          // 0..6
            if (mx == r)      h = (g - b) / d + (g < b ? 6.0 : 0.0);
            else if (mx == g) h = (b - r) / d + 2.0;
            else              h = (r - g) / d + 4.0;
            int k = (int)(h * kBins / 6.0);
            if (k < 0) k = 0; if (k >= kBins) k = kBins - 1;
            bins[k] += v * sat;                // 越亮越饱和的像素，越能代表这个效果的色相
        }
    }
    strip.setTransition(saveTr);
    if (valSum <= 0) return 0;
    double tot = 0;
    for (int i = 0; i < kBins; ++i) tot += bins[i];
    double H = 0;
    if (tot > 0) {
        for (int i = 0; i < kBins; ++i) {
            const double pr = bins[i] / tot;
            if (pr > 0) H -= pr * log(pr);
        }
        H /= log((double)kBins);
    }
    const double meanSat = satSum / valSum;
    int32_t score = (int32_t)(100.0 * H * meanSat + 0.5);
    return score < 0 ? 0 : (score > 100 ? 100 : score);
}

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
    out[29] = (int32_t)seg.palette;
    // 段的「能力位」。**这一位错了，所有调色板都会失效**：color_from_palette()
    // 开头就是 `if ((palette == 0 && ...) || !_isRGB) return 段颜色;`，
    // 于是每个效果都退化成单色/三原色 —— 看着像是 WLED 的效果不够花哨。
    out[30] = (int32_t)seg.getLightCapabilities();   // bit0=RGB bit1=W bit2=CCT
}

} // extern "C"
