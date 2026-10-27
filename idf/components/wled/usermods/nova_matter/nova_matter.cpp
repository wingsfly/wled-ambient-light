// nova_matter：把 esp-matter 接进 WLED 的 usermod。
//
// 现在是 2a.2 的体积探针 + 2b 的骨架：真正创建扩展彩色灯端点并启动 Matter，
// 让链接器保留完整的 Matter 栈（只链接不调用会被 --gc-sections 丢掉，体积就被低估）。
// 尚未完成（见 docs/26 阶段 2b）：
//   - 属性回调没有映射到 WLED 的开关 / 亮度 / 颜色
//   - WLED 的 WiFi.begin() 与 Matter 的 InitWiFiStack() 的初始化顺序没有处理
#include "wled.h"
#include <esp_matter.h>

using namespace esp_matter;

namespace {

esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                       uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data) {
  return ESP_OK;
}

esp_err_t on_identify(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                      uint8_t effect_variant, void *priv_data) {
  return ESP_OK;
}

void on_event(const ChipDeviceEvent *event, intptr_t arg) {}

}  // namespace

class NovaMatterUsermod : public Usermod {
 public:
  void setup() override {
    node::config_t node_config;
    node_t *node = node::create(&node_config, on_attribute, on_identify);
    // 与原厂一致：一个 Extended Color Light（0x010D）端点
    endpoint::extended_color_light::config_t light_config;
    endpoint::extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
  }

  void connected() override {
    if (!started) started = (esp_matter::start(on_event) == ESP_OK);
  }

  void loop() override {}

 private:
  bool started = false;
};

static NovaMatterUsermod nova_matter;
REGISTER_USERMOD(nova_matter);
