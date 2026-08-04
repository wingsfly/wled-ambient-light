# 模拟器

两条路子，跑的都是**固件的同一份代码**（`usermods/lamp/`），不是复刻品。

| | 用途 |
|---|---|
| **实时版**（`sim/index.html` + `server.py`） | 边放音乐边看灯效。浏览器采 PCM → WebSocket → 服务端 C++ 分析并渲染 → 页面只画十六进制串 |
| **离线版**（`simulate.cpp`） | 读 WAV 出逐帧 JSON。不需要浏览器，适合批量回归与写脚本对比 |

## 实时版

```bash
./tools/build.sh          # liblamp —— usermods/lamp 那批头文件
./tools/build_wled.sh     # libwledfx —— WLED 自己的 220 个固定灯效
./tools/run.sh start      # start / stop / restart / status / log
```

部署到远程机器见 [DEPLOY.md](DEPLOY.md)。目标主机不写在仓库里，走
`LAMP_HOST=<SSH 别名>`。

页面两个 tab：

- **音乐律动** —— 我们自己的算法。音频源可选文件、直链或麦克风；
  可以边播边看，耳朵和眼睛能对上。
- **WLED 固定灯效** —— 上游 `FX.cpp` 原件，通过一层主机端垫片编译成
  `libwledfx`（做法见 [hostwled/README.md](hostwled/README.md)）。
  带调色板选择器、自定义调色板、按实测彩色度过滤排序。

**页面里没有任何算法。** 连 96 个 RGB 都是 C++ 渲染好再传过来的 ——
模拟器存在的唯一价值是「看到的就是将来会看到的」，在浏览器里重写一遍
就把这个价值取消了。

## 离线版

```bash
c++ -std=c++17 -O2 -I ../usermods/lamp simulate.cpp -o simulate

./simulate song.wav out.json          # 16-bit PCM WAV
./simulate --synth out.json 128       # 合成 128BPM 测试轨
./simulate song.wav out.json 1        # 效果 1（拍点脉冲）
./simulate song.wav out.json 0 --px   # 额外输出 C++ 渲染的 96 个 RGB
```

主机上 FFT 用朴素 DFT（目标板是 esp-dsp）。N=2048 时每帧 210 万次运算，
一分钟音频约十几秒 —— 慢，但零依赖。

另有 [`lamp_calib.cpp`](lamp_calib.cpp)：跑完整管线、打印**判据函数的实参**，
用来标定自动选灯效的阈值。时长是命令行参数，`for MS in 30000 45000 90000;
do ./calib $MS; done` 就是一次跨时长复现性检验。

## 边界

**能验证的**：BPM、相位、16 段能量、AGC、包络、档位切换、调性/基频/氛围/
小节、自动选灯效，以及全部灯效画面 —— 全部由 C++ 算出。

**不能验证的**：任何硬件相关的东西 —— 电平转换的时序、WS2812 的实际色温、
ADC 的噪声底、I2S 麦的频响、外壳透光的实际扩散半径（页面里那个 1.2 颗是
估的，要拿真灯标）。那些要等扩展板。
