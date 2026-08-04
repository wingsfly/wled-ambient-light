#!/bin/sh
# 把 WLED **自己的**灯效代码编成主机端动态库，供模拟页面的「固定灯效」页使用。
#
#   ./tools/build_wled.sh
#
# 做法见 tools/hostwled/README.md：把上游源文件软链进构建目录，
# 引号 include 于是先在那里找 —— 放了桩的用桩，没放的顺着 -I wled00 找原件。
#
# **上游文件默认一个字都不改**；确有必要的例外全部登记在 patches/，
# 改动处留 `// [lamp-fork]` 标记。查现有补丁：grep -rn "\[lamp-fork\]" wled00/
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
B=".pio/hostwled"
rm -rf "$B"; mkdir -p "$B"

# 1. 上游头文件与源文件全部软链进来（让它们的「所在目录」变成 $B）
for f in wled00/*.h; do ln -sf "$ROOT/$f" "$B/$(basename "$f")"; done
ln -sf "$ROOT/wled00/src/dependencies/fastled_slim/fastled_slim.h" "$B/fastled_slim.h"
# **这些全是上游原件**：效果、2D 几何、调色板表、字体、粒子系统、色彩数学。
# 它们决定画面长什么样，一行都不能是我写的。
for f in FX.cpp FX_fcn.cpp FX_2Dfcn.cpp colors.cpp palettes.cpp fontmanager.cpp \
         FXparticleSystem.cpp wled_math.cpp util.cpp; do
  [ -f "wled00/$f" ] && ln -sf "$ROOT/wled00/$f" "$B/$f"
done
ln -sf "$ROOT/wled00/src/dependencies/fastled_slim/fastled_slim.cpp" "$B/fastled_slim.cpp"

# 2. 桩覆盖上去。
#
# ⚠️ **必须先 rm 再 cp。** 第一步建的是软链接，直接 `cp 桩 $B/wled.h`
# 会顺着软链**写穿到 wled00/wled.h**，把上游文件覆盖掉 —— 我真的干过一次，
# 是靠 `git status` 才发现的。桩目录里每多一个与上游同名的文件，
# 就多一次写穿上游的机会。
for f in tools/hostwled/*.h tools/hostwled/*.cpp; do
  [ -e "$f" ] || continue
  rm -f "$B/$(basename "$f")"
  cp "$f" "$B/$(basename "$f")"
done
# 带路径的桩（fcn_declare.h 用相对路径 include 真的 JSON 头）。同样先删再拷。
(cd tools/hostwled && find . -mindepth 2 -name '*.h' -exec sh -c '
  mkdir -p "$1/$(dirname "$2")" && rm -f "$1/$2" && cp "$2" "$1/$2"' _ "$ROOT/$B" {} \;)

# 3. 自检：wled00/ 不该有**未提交**的改动。
#
#    比的是工作区与索引，所以 patches/ 里那些已提交的本地补丁不会触发它，
#    而「cp 顺着软链写穿了上游文件」这类事故会当场暴露 —— 那正是要拦的。
#    真要打新补丁，先提交再构建。
if ! git -C "$ROOT" diff --quiet -- wled00/; then
  echo "wled00/ 有未提交的改动，已中止。要么是构建脚本写穿了软链（bug），" >&2
  echo "要么是你正在打的本地补丁还没提交（见 patches/README.md）。受影响文件：" >&2
  git -C "$ROOT" diff --name-only -- wled00/ >&2
  exit 1
fi

# 3. 编译
SRC="$B/fastled_slim.cpp $B/host_glue.cpp"
# util.cpp 里有 beat*/perlin*/utf8_*/extractModeDefaults —— 也是画面的一部分
for f in FX.cpp FX_fcn.cpp FX_2Dfcn.cpp colors.cpp palettes.cpp fontmanager.cpp \
         FXparticleSystem.cpp wled_math.cpp util.cpp; do
  [ -f "$B/$f" ] && SRC="$SRC $B/$f"
done

OUT=tools/libwledfx.dylib
case "$(uname)" in Linux) OUT=tools/libwledfx.so ;; esac

c++ -std=c++17 -O2 -shared -fPIC -w \
    -I "$B" -I wled00 -I wled00/src/dependencies/fastled_slim \
    $SRC -o "$OUT"
echo "已生成 $OUT"
