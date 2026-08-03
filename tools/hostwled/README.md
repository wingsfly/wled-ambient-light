# WLED 固定灯效的主机端移植

让 WLED **自己的** `FX.cpp`（217 个已注册效果）在 macOS/Linux 上跑起来，
输出像素喂给模拟页面。

## 为什么不在页面上用 JS 重写几个效果

那样看到的是我写的效果，不是 WLED 的。模拟器存在的唯一理由是
「看到的就是将来会看到的」—— 重写一遍就把这个理由取消了。
外壳透光那一轮已经说过同样的话（设计文档 §4.1b）。

## 机制：软链 + 引号 include 的解析顺序

上游文件**一个字都不改**。做法是把 `FX.cpp` 等软链进本目录再编译：

```
tools/hostwled/FX.cpp -> ../../wled00/FX.cpp
```

C++ 的引号 include（`#include "wled.h"`）**先在「源文件所在目录」里找**。
源文件在这里是软链接的位置，所以 `wled.h` / `fcn_declare.h` / `bus_manager.h`
解析到本目录的垫片；而 `FX.h`、`colors.h` 这些**真头文件**没有同名垫片，
就顺着 `-I wled00` 找到上游原件。

于是「哪些用真的、哪些用垫片」由**本目录里放了什么**决定，一目了然，
也不会随上游改动而漂。

## 垫片覆盖什么

| 文件 | 垫掉什么 | 为什么 |
|---|---|---|
| `Arduino.h` | 类型、`millis`、`random`、`PROGMEM`/`F()`、`Serial` | 主机上没有 Arduino |
| `wled.h` | 只声明 FX 真正读的那些全局量 | 真的 `wled.h` 会拖进 WiFi/MQTT/WebServer，56 个符号里绝大多数是它带来的 |
| `fcn_declare.h` | 只声明 FX 用到的函数 | 同上，真的那份声明了 alexa/mqtt/json |
| `bus_manager.h` | 写进内存像素缓冲 | `FX_fcn.cpp` 2166 行里只有约 50 行碰硬件 |
| `pin_manager.h` 等 | 空 | FX 不用 |

**真的**跑起来的是：`FX.cpp`、`FX_fcn.cpp`、`colors.cpp`、`fastled_slim`、
`FXparticleSystem.cpp` —— 效果逻辑、调色板、缓动、粒子系统全是上游原件。

## 构建

```sh
./tools/build_wled.sh          # 产出 tools/libwledfx.dylib
```
