#include "wled.h"
#include <Wire.h>

/*
 * 台灯扩展板 U1（INA219，I2C 0x40，R1 = 10mΩ 分流）—— 主路电流/电压监测。
 *
 * ⚠️ 不能用仓库自带的 INA226_v2 usermod：INA226 与 INA219 寄存器布局不同。
 * 这里直接读原始寄存器，不写校准寄存器（POR 默认 0x399F：32V FSR、
 * ±320mV PGA、12bit —— ±320mV/10mΩ = ±32A 量程，1mA 分辨率，本板够用）：
 *   0x01 分流电压，有符号，LSB 10µV → I = V_shunt / 0.01Ω
 *   0x02 母线电压，>>3 后 LSB 4mV
 * 每秒读一次，Info 页显示。SDA=GPIO8 SCL=GPIO9（板上 R2/R3 4.7k 上拉）。
 */
class Ina219LampUsermod : public Usermod {
  private:
    static const uint8_t ADDR = 0x40;
    bool     present = false;
    float    busV = 0, currentA = 0;
    uint32_t lastRead = 0;

    bool read16(uint8_t reg, uint16_t &val) {
      Wire.beginTransmission(ADDR);
      Wire.write(reg);
      if (Wire.endTransmission() != 0) return false;
      if (Wire.requestFrom((int)ADDR, 2) != 2) return false;
      val = ((uint16_t)Wire.read() << 8) | Wire.read();
      return true;
    }

  public:
    void setup() override {
      // 板子 I2C 专用于 U1，引脚固定；WLED 全局 I2C 未启用时自行初始化
      if (i2c_sda < 0) Wire.begin(8, 9);
      uint16_t cfg;
      present = read16(0x00, cfg);
    }

    void loop() override {
      if (!present || millis() - lastRead < 1000) return;
      lastRead = millis();
      uint16_t raw;
      if (read16(0x01, raw)) currentA = (int16_t)raw * 10e-6f / 0.01f; // 10µV LSB / 10mΩ
      if (read16(0x02, raw)) busV = (raw >> 3) * 4e-3f;                // 4mV LSB
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      if (!present) {
        user.createNestedArray(F("INA219")).add(F("not found"));
        return;
      }
      JsonArray v = user.createNestedArray(F("Bus voltage"));
      v.add(serialized(String(busV, 2))); v.add(F(" V"));
      JsonArray a = user.createNestedArray(F("Main current"));
      a.add(serialized(String(currentA, 2))); a.add(F(" A"));
      JsonArray p = user.createNestedArray(F("Main power"));
      p.add(serialized(String(busV * currentA, 1))); p.add(F(" W"));
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

static Ina219LampUsermod ina219_lamp;
REGISTER_USERMOD(ina219_lamp);
