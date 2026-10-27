include("${CMAKE_CURRENT_LIST_DIR}/../arduino_deps.cmake")
# WLED 作为 IDF 组件：源码就是本仓库（idf/components/wled → 仓库根），原地引用，不复制。
set(W "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(UM "${W}/usermods")
file(GLOB WLED_SRCS "${W}/wled00/*.cpp")
file(GLOB_RECURSE WLED_DEP_SRCS "${W}/wled00/src/*.cpp")
# lib/ 下的 WLED 自带库：PlatformIO 自动编，IDF 下要显式列（ADC manager 供 audioreactive 的模拟输入用）
file(GLOB WLED_LIB_SRCS "${W}/lib/wled_ADCmanager/*.cpp")
# 原厂板用的 usermod；自制板专属的编码器 / INA219 / 状态灯不带。按目录取，不写死文件名
set(UM_SRCS)
foreach(u audioreactive lamp mqtt_heartbeat *auto_save*)
    file(GLOB s "${UM}/${u}/*.cpp")
    list(APPEND UM_SRCS ${s})
endforeach()
# 本工程自己的 usermod（与 WLED 源码分开放）：nova_matter
file(GLOB s "${CMAKE_CURRENT_LIST_DIR}/usermods/*/*.cpp")
list(APPEND UM_SRCS ${s})

idf_component_register(
    SRCS ${WLED_SRCS} ${WLED_DEP_SRCS} ${WLED_LIB_SRCS} ${UM_SRCS}
    INCLUDE_DIRS "${W}/wled00" "${W}/lib/wled_ADCmanager"
    PRIV_INCLUDE_DIRS "${UM}/audioreactive" "${UM}/lamp"
    REQUIRES ${ARDUINO_IDF_DEPS} AsyncTCP ESPAsyncWebServer NeoPixelBus AsyncMqttClient arduinoFFT esp_matter
    # usermod 没有任何符号被引用，只靠 .dynarray 段自注册；不整库链接会被链接器整个丢掉
    WHOLE_ARCHIVE
    LDFRAGMENTS "dynarray.lf")

# 取自 platformio.ini 的 esp32s3 公共 flags + lamp env，按原厂板改
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "WLED_RELEASE_NAME=\"NovaLamp_S3\""
    # 不用 WLED_USE_MY_CONFIG：Wi-Fi 走运行时配网（AP / Improv / Matter），凭据不进固件
    WLED_PS_DONT_REPLACE_FX
    WLED_WATCHDOG_TIMEOUT=0
    # 上游 V5 的 esp32 公共 flag（NeoPixelBus CORE3 下多路共享 RMT）
    WLED_USE_SHARED_RMT
    MQTT_MAX_PACKET_SIZE=1024
    BOARD_HAS_PSRAM
    NDEBUG
    # 板上没有 IR 接收头、没有 DMX；GIF（2D 矩阵播图）和 ESP-NOW 先关，后面按需再开
    WLED_DISABLE_INFRARED
    WLED_DISABLE_ESPNOW
    # 原厂板：上路 GPIO5、下路 GPIO6，各 48 颗；GPIO8 灯带使能，高电平有效
    "DATA_PINS=5,6"
    "PIXEL_COUNTS=48,48"
    RLYPIN=8
    RLYMDE=1
    USERMOD_AUTO_SAVE_ON_BOOT=true)
target_compile_options(${COMPONENT_LIB} PRIVATE ${ARDUINO_WARN_RELAX} -Wno-attributes)
