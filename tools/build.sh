#!/bin/sh
# 构建模拟服务器要用的动态库。跑的是 usermods/lamp 那批头文件本身。
set -e
cd "$(dirname "$0")"
EXT=so; [ "$(uname)" = "Darwin" ] && EXT=dylib
c++ -std=c++17 -O2 -shared -fPIC -I ../usermods/lamp -I . \
    lamp_capi.cpp -o "liblamp.$EXT"
echo "liblamp.$EXT"
