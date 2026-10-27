#!/bin/bash
# 构建原厂板三合一固件，并按产物核对（27 §5.4：判成败看产物，不看过程打印）。
# 环境：NOVA_IDF_ENV = 激活 IDF 5.4.1 + esp-matter 的脚本；NOVA_SIGNING_KEY = Secure Boot V2 签名私钥。
# 前置：./setup.sh；仓库根已 npm ci（UI 打包要 Node.js）。
set -u
H="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$H/.." && pwd)"
fail() { echo "失败：$1"; exit 1; }
if [ -z "${IDF_PATH:-}" ] && [ -n "${NOVA_IDF_ENV:-}" ]; then . "$NOVA_IDF_ENV" > "$H/env.log" 2>&1; fi
[ -n "${IDF_PATH:-}" ] || fail "IDF_PATH 为空（设 NOVA_IDF_ENV）"
[ -n "${ESP_MATTER_PATH:-}" ] || fail "ESP_MATTER_PATH 为空"
[ -f "$H/vendor/arduino-esp32/.nova-patched" ] || fail "先跑 ./setup.sh"
[ -n "${NOVA_SIGNING_KEY:-}" ] && [ -f "$NOVA_SIGNING_KEY" ] || fail "NOVA_SIGNING_KEY 未指向签名私钥"
ln -sfn "$NOVA_SIGNING_KEY" "$H/secure_boot_signing_key.pem"
export IDF_COMPONENT_STORAGE_URL="${IDF_COMPONENT_STORAGE_URL:-https://components-file.espressif.cn}" IDF_COMPONENT_API_TIMEOUT=60
cd "$H"
echo "开始 $(date +%T)  IDF $(git -C "$IDF_PATH" describe --tags 2>/dev/null)  WLED $(git -C "$R" rev-parse --short HEAD 2>/dev/null)"

# UI：data/ 比 html_ui.h 新就重打包（cdata.js 偶尔误判 already built，先删再打）
if [ ! -f "$R/wled00/html_ui.h" ] || [ -n "$(find "$R/wled00/data" -newer "$R/wled00/html_ui.h" -type f | head -1)" ]; then
  rm -f "$R"/wled00/html_*.h
  ( cd "$R" && npm run build > "$H/ui.log" 2>&1 ) || fail "UI 打包，见 ui.log"
fi
[ "$(ls "$R"/wled00/html_*.h 2>/dev/null | wc -l)" -ge 8 ] || fail "html_*.h 不全"

if [ ! -f sdkconfig ] || [ sdkconfig.defaults -nt sdkconfig ]; then
  rm -f sdkconfig
  idf.py set-target esp32s3 > settarget.log 2>&1 || fail "set-target，见 settarget.log"
fi
idf.py build > build.log 2>&1; rc=$?
BIN="$H/build/nova_fw.bin"; ELF="$H/build/nova_fw.elf"
if [ $rc -ne 0 ] || [ ! -f "$BIN" ]; then
  [ -f build/build.ninja ] && ninja -C build -k 0 > build_all.log 2>&1   # 收齐全部错误
  grep -ohE "FAILED: esp-idf/[^/]+" build_all.log build.log 2>/dev/null | sort | uniq -c | sort -rn
  grep -h -E "error:|fatal error|undefined reference|multiple definition|CMake Error" build_all.log build.log 2>/dev/null \
    | sed -E "s|$R/||" | sort | uniq -c | sort -rn | head -30
  fail "构建（退出码 $rc）"
fi

echo "##### 产物核对 #####"
for k in CONFIG_COMPILER_OPTIMIZATION_SIZE CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES CONFIG_NVS_ENCRYPTION \
         CONFIG_SEC_CERT_DAC_PROVIDER CONFIG_SPIRAM_USE_MALLOC; do
  grep -q "^$k=y" sdkconfig || fail "$k 未生效"
done
sz=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN"); slot=$((2944*1024))
printf "  签名后 %d 字节（%.2f MB）；槽 2944 K；余 %d K\n" "$sz" "$(echo "$sz/1048576" | bc -l)" $(((slot-sz)/1024))
[ "$sz" -le "$slot" ] || fail "超出 app 槽"
NM="$(dirname "$(command -v xtensa-esp32s3-elf-gcc)")/xtensa-esp32s3-elf-nm"
# usermods_begin/end 是 static 数组，符号被修饰，用 -C 解修饰后匹配
b=$("$NM" -C "$ELF" | awk '/ usermods_begin$/{print $1}'); e=$("$NM" -C "$ELF" | awk '/ usermods_end$/{print $1}')
um=$("$NM" -C "$ELF" | grep -oE ' um_[A-Za-z0-9_]+$' | sort -u | tr -d ' ' | tr '\n' ' ')
n=$(( (0x$e - 0x$b) / 4 )); echo "  usermod 注册表 $n 项：$um"
[ "$n" -eq "$(echo $um | wc -w)" ] || fail "注册表项数与 usermod 符号数不符"
"$NM" -C "$ELF" | grep -q " esp_matter::start(" || fail "Matter 栈未链入"
echo "  设备身份 VID $(grep '^CONFIG_DEVICE_VENDOR_ID=' sdkconfig | cut -d= -f2) PID $(grep '^CONFIG_DEVICE_PRODUCT_ID=' sdkconfig | cut -d= -f2)；Matter OTA 包 $(ls build/*-ota.bin 2>/dev/null | xargs -n1 basename)"
idf.py size 2>/dev/null | grep -E "Flash Code|Flash Data|DIRAM"
echo "完成 $(date +%T)"
