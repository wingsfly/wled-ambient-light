# idf/ —— 原厂板（iFLYTEK Nova Desk Lamp Pro）三合一固件

把本仓库的 WLED（含 lamp 等 usermod）、esp-matter、讯飞云接入构建成**一个** ESP-IDF 固件，
跑在原厂板 ESP32-S3-WROOM-1-N8R2 上。自制主控板仍走仓库根的 PlatformIO（`env:lamp`），与本目录无关。

为什么不用 PlatformIO：esp-matter v1.4 要求 IDF 5.4.1（原厂固件也是这个版本），PlatformIO 的平台
带不了指定版本的 IDF + esp-matter。这里反过来：以 IDF 为构建系统，arduino-esp32 作为 IDF 组件。

## 栈

| | 版本 |
|---|---|
| ESP-IDF | v5.4.1 |
| esp-matter | release/v1.4 |
| arduino-esp32 | 3.3.12（组件注册表原版 + `patches/`） |
| WLED | 本仓库（上游 main 17.0.0-devV5 + lamp 分支） |
| Arduino 依赖库 | `libs.lock`（照上游 V5 的版本锁定） |

## 用法

```bash
export NOVA_IDF_ENV=/path/to/activate-idf-5.4.1-and-esp-matter.sh
export NOVA_SIGNING_KEY=/path/to/secure_boot_signing_key.pem   # 原厂 Secure Boot V2 密钥，永不入库
(cd .. && npm ci)        # UI 打包要 Node.js
./setup.sh               # 准备 vendor/arduino-esp32 与 libs/（只需一次）
./build.sh               # 构建并核对产物
```

产物 `build/nova_fw.bin` 是 **Secure Boot V2 签名后**的镜像，`build/nova_fw-ota.bin` 是 Matter OTA 包。
**本目录只负责构建，不烧写**；上板方式（Matter OTA）与分区表改造见 Light 仓库 `docs/26-原厂板迁移计划.md`。

## 与原厂对齐的配置

`sdkconfig.defaults` 逐项对齐原厂 `sdkconfig`（DAC 从 `esp_secure_cert` 直读、NVS 用 `nvs_keys` 加密、
VID `0x1656` / PID `0x0001`、Secure Boot V2 RSA、flash 加密 development、malloc 优先 PSRAM）。
这些不对齐也能编过，但上板会静默失效。**唯一有意偏离**：`USE_MINIMAL_MDNS=n`，让 Matter 与 WLED 共用
espressif mdns 组件，避免两个响应器都监听 5353。

`partitions_nova.csv`：A/B 两槽各 2944 K + 1912 K LittleFS；`esp_secure_cert` / `nvs_keys` / `nova_lamp`
三个加密分区与原厂逐项一致（offset 变了密文就解不开）。

## 把 WLED 变成 IDF 组件要处理的事

- **Arduino 库的 IDF 依赖**：arduino-esp32 没把 WiFi 等库用到的 IDF 组件声明成公开依赖 → `components/arduino_deps.cmake`
- **`-Werror=all`**：IDF 加给所有组件，`-Wno-error=all` 撤不回 → 同文件逐项 `-Wno-error=<告警>`（刻意不放 `return-type`）
- **usermod 注册表**：`REGISTER_USERMOD` 放进 `.dynarray.*` 段 → `components/wled/dynarray.lf`（ldgen 的 `KEEP() SORT(init_priority)`）+ `WHOLE_ARCHIVE`；`build.sh` 核对注册表项数
- **arduino-esp32 补丁**（`patches/`）：I2C 从机 HAL 在 IDF 5.4.1 编不过，换成返回 `ESP_ERR_NOT_SUPPORTED` 的桩；`--wrap` 条件补上 Arduino Matter 库开关；补 `maybe_add_component(esp_matter)`；剥掉与 esp-matter 版本冲突的 RainMaker 等依赖

## 现状

- 构建通过：WLED + Matter 签名后约 2.25 MB；讯飞云栈未接入
- `components/wled/usermods/nova_matter`：Matter 接入的骨架，属性回调尚未映射到 WLED 状态，WiFi 初始化顺序未处理
- 尚未上板
