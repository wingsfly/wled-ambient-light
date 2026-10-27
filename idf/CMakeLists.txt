cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# esp-matter 的接法照原厂工程与 esp-matter 示例（ESP_MATTER_PATH 由 env.sh 导出）
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
set(ENV{ESP_MATTER_DEVICE_PATH} ${ESP_MATTER_PATH}/device_hal/device/esp32s3_devkit_c)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)
# vendor/arduino-esp32：组件注册表的 arduino-esp32 3.3.12 + patches/ 里的补丁，由 setup.sh 生成（不入库）
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/vendor"
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})
# 上游 WLED V5 的做法：不让 arduino 核心初始化板载 RGB LED —— 它走新 RMT 驱动，和 legacy RMT
# 同时链入会开机 abort（"rmt(legacy): CONFLICT! driver_ng is not allowed..."）。要作用到 arduino 核心，所以设成全局。
idf_build_set_property(COMPILE_DEFINITIONS "ESP32_ARDUINO_NO_RGB_BUILTIN" APPEND)
# Matter OTA 包头与 Basic Information 的版本取自这里。原厂系列在 129–133；新固件从 200 起，
# 从原厂固件 OTA 过来时满足"版本必须更高"，也和原厂版本一眼区分。
set(PROJECT_VER "2.0.0-wled")
set(PROJECT_VER_NUMBER 200)
project(nova_fw)
# 与原厂顶层 CMakeLists 相同：最后出现的 -Os 覆盖 IDF 默认的 -Og
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
