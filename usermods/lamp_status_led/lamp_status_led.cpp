#include "wled.h"

/*
 * 台灯扩展板板载状态灯（LED1，链尾第 97 颗，默认像素索引 96）。
 *
 * 状态优先级（高 → 低）：
 *   1. AP 配网模式          → 黄色 1Hz 慢闪
 *   2. WiFi 已配置但断线    → 红色常亮
 *   3. 3.5mm 线路输入插着   → 绿色常亮（AUD_DET/GPIO15 低电平 = 插入）
 *   4. 一切正常             → 不干预，像素归 segment 支配（“简单档”）
 *
 * 挂在 handleOverlayDraw()：所有特效渲染完、strip.show() 之前覆写，
 * 所以覆盖态不会被灯效冲掉，不干预时也不留残影。
 */
class LampStatusLedUsermod : public Usermod {
  private:
    bool enabled = true;
    uint16_t pixel = 96;                 // LED1 在总链中的绝对索引
    static const int8_t DET_PIN = 15;    // AUD_DET：R13 100k 外部上拉，低 = 插入
                                         // 与“插拔自动切源”功能共享，只读不独占，
                                         // 所以不走 PinManager 登记

  public:
    void setup() override {
      pinMode(DET_PIN, INPUT);
    }

    void loop() override {}

    void handleOverlayDraw() override {
      if (!enabled) return;
      uint32_t col;
      if (apActive) {
        // 1Hz 黄闪：亮 500ms / 灭 500ms
        col = (millis() % 1000 < 500) ? RGBW32(255, 160, 0, 0) : 0;
      } else if (!WLED_CONNECTED && WiFi.SSID().length() > 0) {
        col = RGBW32(255, 0, 0, 0);
      } else if (digitalRead(DET_PIN) == LOW) {
        col = RGBW32(0, 255, 40, 0);
      } else {
        return;                          // 正常态：交回 segment
      }
      strip.setPixelColor(pixel, col);   // 越界时 setPixelColor 自身丢弃，安全
    }

    void addToConfig(JsonObject &root) override {
      JsonObject top = root.createNestedObject(F("LampStatusLed"));
      top[F("enabled")] = enabled;
      top[F("pixel")]   = pixel;
    }

    bool readFromConfig(JsonObject &root) override {
      JsonObject top = root[F("LampStatusLed")];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[F("enabled")], enabled, true);
      configComplete &= getJsonValue(top[F("pixel")],   pixel,   96);
      return configComplete;
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

static LampStatusLedUsermod lamp_status_led;
REGISTER_USERMOD(lamp_status_led);
