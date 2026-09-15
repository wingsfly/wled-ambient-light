#!/bin/bash
# 构建 LampSender.app。
#
# 不用 XcodeGen 也不用 SwiftPM：这个 App 要直接编 usermods/lamp 那批 C++ 头
# 文件（与固件同一份源码），而 SwiftPM 的 target 源码不能跳出 package 目录。
# swiftc 直接编则没有这个限制，也不引入新的工具链依赖 —— 有 Xcode 就够。
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
BUILD=build
APP="$BUILD/LampSender.app"
DEPLOY_TARGET=arm64-apple-macos15.0   # Synchronization 的 Atomic 要 macOS 15+
# 控制面客户端。独立仓库，与本仓库同级 —— iOS App 也依赖同一份。
LAMPKIT=$(cd "$ROOT/../LampKit" 2>/dev/null && pwd || true)

mkdir -p "$BUILD"

echo "[1/6] 构建 LampKit（控制面客户端）"
if [ -z "$LAMPKIT" ]; then
  echo "      ✗ 找不到 ../LampKit —— 控制台窗口需要它" >&2
  exit 1
fi
# 目标三元组必须和下面编 App 的一致，否则模块的最低系统版本对不上。
(cd "$LAMPKIT" && swift build -c release -Xswiftc -target -Xswiftc "$DEPLOY_TARGET" >/dev/null)
LK_MODULES="$LAMPKIT/.build/release/Modules"
LK_OBJECTS=$(ls "$LAMPKIT"/.build/release/LampKit.build/*.o)

echo "[2/6] 编译 liblamp（与固件同一份 C++ 源码）"
# 最低系统版本要和下面 swiftc 的 -target 一致，否则链接器会警告版本不匹配
c++ -std=c++17 -O2 -mmacosx-version-min=15.0 -c "$ROOT/tools/lamp_capi.cpp" \
    -I "$ROOT/usermods/lamp" -I "$ROOT/tools" \
    -o "$BUILD/lamp_capi.o"

echo "[3/6] 编译 Swift"
# shellcheck disable=SC2086  # LK_OBJECTS 要按空格拆成多个参数
xcrun swiftc -O -parse-as-library \
    -target "$DEPLOY_TARGET" \
    -import-objc-header Sources/LampBridging.h \
    -Xcc -I"$ROOT/tools" \
    -I "$LK_MODULES" \
    Sources/*.swift Sources/Console/*.swift "$BUILD/lamp_capi.o" $LK_OBJECTS \
    -lc++ \
    -o "$BUILD/LampSender"

echo "[4/6] 编译 CLAP 模型"
# .mlpackage 是源格式，运行时要的是编译过的 .mlmodelc。126 MB 编一次要几秒，
# 所以只在模型比产物新的时候重编。模型没有就跳过 —— App 会退回纯 liblamp 的
# mood，并在菜单栏标出来（用 clap/export_clap.py 生成模型）。
MLSRC=Resources/ClapAudioTower.mlpackage
MLOUT="$BUILD/ClapAudioTower.mlmodelc"
if [ -d "$MLSRC" ]; then
  if [ ! -d "$MLOUT" ] || [ "$MLSRC" -nt "$MLOUT" ]; then
    rm -rf "$MLOUT" "$BUILD/mlc"
    xcrun coremlc compile "$MLSRC" "$BUILD/mlc" >/dev/null
    mv "$BUILD/mlc/ClapAudioTower.mlmodelc" "$MLOUT"
    rmdir "$BUILD/mlc" 2>/dev/null || true
    echo "      已编译 ($(du -sh "$MLOUT" | cut -f1))"
  else
    echo "      已是最新，跳过"
  fi
else
  echo "      ⚠️  没有 $MLSRC —— 语义情绪不可用（见 clap/README.md）"
fi

# 图标是生成物，不入库（源在 icon/make_icon.swift）。缺了就现画一张。
if [ ! -f Resources/AppIcon.icns ]; then
  echo "      生成应用图标"
  (cd icon && xcrun swift make_icon.swift ../Resources >/dev/null)
  (cd Resources && iconutil -c icns AppIcon.iconset -o AppIcon.icns && rm -rf AppIcon.iconset)
fi

echo "[5/6] 组装 .app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp Resources/Info.plist "$APP/Contents/"
cp "$BUILD/LampSender" "$APP/Contents/MacOS/"
[ -d "$MLOUT" ] && cp -R "$MLOUT" "$APP/Contents/Resources/"
[ -f Resources/anchors.json ] && cp Resources/anchors.json "$APP/Contents/Resources/"
cp Resources/AppIcon.icns "$APP/Contents/Resources/"

echo "[6/6] 签名"
# TCC 按 bundle 身份记授权，未签名的 bundle 拿不到音频捕获权限。
codesign --force --sign - "$APP"

echo
echo "✅ $APP"
echo "   启动：open $APP"
echo "   注意：必须用 open 启动。直接执行 Contents/MacOS/LampSender（比如从 ssh）"
echo "   会被系统判为不属于图形会话，tap 回调照跑但数据全是 0，且不报任何错。"

# ── 安装 ────────────────────────────────────────────────
# ./build.sh 之后跑 ./build.sh install 把 app 装到 ~/Applications。
# SMAppService 记的是 app 的当前位置，登录项必须在装好之后才注册 ——
# 从 build/ 里注册，挪走就指空了。
if [ "${1:-}" = "install" ]; then
  DEST="$HOME/Applications"
  mkdir -p "$DEST"
  # 先停旧实例，否则 cp 覆盖正在运行的可执行文件
  pkill -x LampSender 2>/dev/null || true
  rm -rf "$DEST/LampSender.app"
  cp -R "$APP" "$DEST/"
  codesign --force --sign - "$DEST/LampSender.app"
  echo "已安装到 $DEST/LampSender.app"
fi
