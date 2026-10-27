#!/bin/bash
# 准备 vendor/arduino-esp32（组件注册表的 3.3.12 原版 + patches/ 补丁）与 libs/（按 libs.lock）。
# 先激活 ESP-IDF v5.4.1 环境（IDF_PATH）；可用 NOVA_IDF_ENV 指向激活脚本。
# 判成败看内容，不看目录在不在：版本号、补丁标记、每个库的 HEAD 都核对。
set -u
H="$(cd "$(dirname "$0")" && pwd)"
fail() { echo "失败：$1"; exit 1; }
if [ -z "${IDF_PATH:-}" ] && [ -n "${NOVA_IDF_ENV:-}" ]; then . "$NOVA_IDF_ENV" > /dev/null 2>&1; fi
[ -n "${IDF_PATH:-}" ] || fail "先激活 ESP-IDF 环境（或设 NOVA_IDF_ENV）"
export IDF_COMPONENT_STORAGE_URL="${IDF_COMPONENT_STORAGE_URL:-https://components-file.espressif.cn}" IDF_COMPONENT_API_TIMEOUT=60

echo "##### 1. vendor/arduino-esp32 #####"
V="$H/vendor/arduino-esp32"
if [ ! -f "$V/.nova-patched" ]; then
  B="$H/.bootstrap"; rm -rf "$V" "$B"; mkdir -p "$B/main"
  # 用一个空工程让组件管理器下载原版（git clone 169 MB 且常断，注册表包 13 MB）
  printf 'cmake_minimum_required(VERSION 3.16)\ninclude($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(bootstrap)\n' > "$B/CMakeLists.txt"
  printf 'idf_component_register()\n' > "$B/main/CMakeLists.txt"
  printf 'dependencies:\n  espressif/arduino-esp32:\n    version: "==3.3.12"\n' > "$B/main/idf_component.yml"
  # arduino-esp32 的 CMakeLists 在配置阶段就强制要求 1000 Hz，空工程也得给
  printf 'CONFIG_FREERTOS_HZ=1000\n' > "$B/sdkconfig.defaults"
  ( cd "$B" && idf.py set-target esp32s3 > "$H/bootstrap.log" 2>&1 ) || fail "下载 arduino-esp32，见 bootstrap.log"
  M="$B/managed_components/espressif__arduino-esp32"
  ver=$(grep -m1 '^version:' "$M/idf_component.yml" 2>/dev/null | tr -d '" ' | cut -d: -f2)
  [ "$ver" = "3.3.12" ] || fail "下载到的版本是 '$ver'"
  mkdir -p "$H/vendor" && cp -R "$M" "$V" && rm -f "$V/.component_hash"
  ( cd "$V" && patch -p1 -s < "$H/patches/arduino-esp32-3.3.12.patch" ) || fail "应用补丁"
  touch "$V/.nova-patched"; rm -rf "$B"
fi
grep -q "maybe_add_component(esp_matter)" "$V/CMakeLists.txt" || fail "vendor 缺 esp_matter 组件名补丁"
grep -q "esp32-hal-i2c-slave-stub.c" "$V/CMakeLists.txt" || fail "vendor 缺 I2C 从机桩"
echo "  arduino-esp32 3.3.12，补丁已应用"

echo "##### 2. libs/ #####"
mkdir -p "$H/libs"
grep -v '^\s*#' "$H/libs.lock" | grep -v '^\s*$' | while read -r name url ref; do
  d="$H/libs/$name"
  [ -d "$d/.git" ] || { git init -q "$d" && git -C "$d" remote add origin "$url"; } || exit 1
  case "$ref" in v*) fetch="refs/tags/$ref" ;; *) fetch="$ref" ;; esac
  git -C "$d" fetch -q --depth 1 origin "$fetch" && git -C "$d" checkout -q FETCH_HEAD || { echo "失败：取 $name $ref"; exit 1; }
  head=$(git -C "$d" rev-parse HEAD)
  case "$ref" in v*) : ;; *) [ "$head" = "$ref" ] || { echo "失败：$name HEAD $head ≠ $ref"; exit 1; } ;; esac
  printf "  %-18s %s  %s\n" "$name" "$ref" "${head:0:9}"
done || exit 1
echo "准备完成"
