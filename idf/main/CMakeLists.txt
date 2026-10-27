# 入口在 WLED（wled_main.cpp 的 setup/loop，由 CONFIG_AUTOSTART_ARDUINO 调起）
idf_component_register(SRCS "nova_main.cpp" REQUIRES wled)
