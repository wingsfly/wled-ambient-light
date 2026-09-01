#include "wled.h"

// MQTT 心跳 —— 给 wallex(中控屏 + 自建云)做在线检测用。
//
// ★ 为什么非要它:WLED 自带的 LWT 只覆盖"TCP 断了"这一种掉线。
//   publishMqtt() **只在状态变化时被调用,没有任何周期推送**(led.cpp 的
//   colorUpdated/stateUpdated、web 请求、WebSocket 三个入口),而且 json.cpp 里是
//   `if (stateChanged) stateUpdated()` —— 所以对端**发一个"把当前值设成当前值"的
//   空操作来探活也是徒劳的,会被静默丢弃、不产生任何回报。
//
//   结论:没有这个 usermod,对端就只能靠 LWT,盲区是"TCP 还在但设备已经不干活"
//   (MQTT 客户端卡死、网络半开连接)。这类故障恰恰最难查 —— 界面上一直显示在线。
//
//   中控屏侧接 zigbee 设备时踩过同一个坑的两面:
//     · 米家无线开关有 60.3 分钟 Basic 心跳 → "14 小时零帧"能直接判定掉线
//     · 人体传感器没有心跳          → "18 小时零帧"什么都证明不了,曾据此误判
//   **有没有心跳,决定了对端能不能对"静默"下结论。** 这就是加它的全部理由。
//
// 发布:<deviceTopic>/hb,JSON,默认 30s 一条。载荷除了证明"我还活着",
// 还带上排查现场故障最常用的三个值 —— 掉线往往不是"网没了",而是信号差/内存耗尽/刚重启过。
//
//   {"up":12345,"rssi":-58,"heap":142000,"bri":128,"on":true,"ip":"192.168.0.81"}
//     up    运行秒数 —— 悄悄重启过的话这个值会突然变小,比任何日志都直观
//     rssi  WiFi 信号(dBm)
//     heap  剩余堆(字节)
//     bri   当前亮度   ┐ 顺带捎上,让对端在没收到 /g /c 时也能对齐状态
//     on    是否点亮   ┘ (WLED 不勾"保留亮度与颜色消息"时,重启后对端拿不到这两个值)
//     ip    本机 IP —— 对端要靠它去 HTTP 拉 /json/eff、/json/pal(240 个灯效 +
//           72 个调色板的名字表)。这两个表**只能从 HTTP 拿**:MQTT 侧的 /v 是
//           XML 且不含它们,而对端不可能写死 —— 换个固件版本效果表就变了。
//           放在心跳里而不是只发一次,是为了 DHCP 换址后能自动跟上。
//
// 不改变任何既有行为:只增发一条 MQTT 消息,不碰灯效、不碰渲染、不订阅任何 topic。
// 关掉(UI 里 enabled=false)或不编译进来,系统行为与没有它时完全一致。

class MqttHeartbeatUsermod : public Usermod {
  private:
    bool enabled = true;
    unsigned long lastBeat = 0;
    // 30s:对端按 3 倍窗口(90s)判离线,与 LWT 的 keepalive 60×1.5≈90s 同量级,
    // 两条路径的判定延迟一致,不会出现"一个说在线一个说离线"的分裂状态。
    uint16_t intervalSec = 30;

    static const char _name[];
    static const char _enabled[];
    static const char _interval[];

  public:
    void setup() override {}

    void loop() override {
      if (!enabled || strip.isUpdating()) return;
      unsigned long now = millis();
      // 减法写法可抗 millis() 溢出(49.7 天),别写成 now > lastBeat + N
      if (now - lastBeat < (unsigned long)intervalSec * 1000UL) return;
      lastBeat = now;

#ifndef WLED_DISABLE_MQTT
      // 未连上 broker 时不发:此时 publish 只会失败,徒增日志噪音。
      // 真正掉线的判定交给对端的超时窗口,而不是在这里自证。
      if (!WLED_MQTT_CONNECTED || mqttDeviceTopic[0] == 0) return;

      char topic[MQTT_MAX_TOPIC_LEN + 8];
      snprintf(topic, sizeof(topic) - 1, "%s/hb", mqttDeviceTopic);

      char payload[128];
      snprintf(payload, sizeof(payload) - 1,
               "{\"up\":%lu,\"rssi\":%d,\"heap\":%u,\"bri\":%u,\"on\":%s,\"ip\":\"%s\"}",
               millis() / 1000UL, WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
               (unsigned)bri, (bri > 0 ? "true" : "false"),
               WiFi.localIP().toString().c_str());

      // retain=false:心跳的意义在于"刚刚还活着",留底会让重连的对端读到一条
      // 陈旧心跳、误判为在线 —— 那正好毁掉这个 usermod 存在的理由。
      mqtt->publish(topic, 0, false, payload);
#endif
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_interval)] = intervalSec;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;
      getJsonValue(top[FPSTR(_enabled)], enabled, true);
      getJsonValue(top[FPSTR(_interval)], intervalSec, 30);
      // 下限 5s:再短对在线判定没有帮助,只是白白占用 WiFi 与 broker。
      if (intervalSec < 5) intervalSec = 5;
      return true;
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char MqttHeartbeatUsermod::_name[]     PROGMEM = "MQTTHeartbeat";
const char MqttHeartbeatUsermod::_enabled[]  PROGMEM = "enabled";
const char MqttHeartbeatUsermod::_interval[] PROGMEM = "intervalSec";

static MqttHeartbeatUsermod mqtt_heartbeat_usermod;
REGISTER_USERMOD(mqtt_heartbeat_usermod);
