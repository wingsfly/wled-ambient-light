# Arduino 库（WiFi、ESPmDNS、Update…）在 PlatformIO 下头文件路径是全局的；作为 IDF 组件时，
# arduino-esp32 没有把这些库用到的 IDF 组件声明成公开依赖（只给 Console 声明了），
# 下游 #include <WiFi.h> 会找不到 esp_wifi_types.h 等。凡消费 Arduino 库的组件都 REQUIRES 这一组。
set(ARDUINO_IDF_DEPS arduino-esp32 esp_wifi esp_netif esp_event lwip nvs_flash app_update
    esp_psram esp_adc driver mbedtls esp_timer esp_partition spi_flash efuse)
# IDF 给所有组件加 -Werror=all。GCC 的 -Wno-error=all 撤不回它（对告警组不起反向作用），
# 只能逐项 -Wno-error=<告警>。这些第三方代码在 PlatformIO 下从不按错误处理；
# GCC 14 里 uint32_t 是 long unsigned int，%u 等全报。刻意不放 return-type：漏写 return 是真问题。
set(ARDUINO_WARN_RELAX -Wno-error)
foreach(w address array-bounds bool-compare catch-value char-subscripts class-memaccess comment
          dangling-pointer delete-non-virtual-dtor enum-compare format format-overflow
          format-truncation int-in-bool-context logical-not-parentheses maybe-uninitialized
          misleading-indentation nonnull nonnull-compare parentheses pessimizing-move
          range-loop-construct redundant-move reorder restrict sequence-point sign-compare
          sizeof-pointer-div sizeof-pointer-memaccess stringop-overflow stringop-truncation
          strict-aliasing switch tautological-compare uninitialized unknown-pragmas
          unused-value use-after-free zero-length-bounds)
    list(APPEND ARDUINO_WARN_RELAX "-Wno-error=${w}")
endforeach()
