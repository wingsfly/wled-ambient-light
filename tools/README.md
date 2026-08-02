# 离线模拟器

`simulate.cpp` 读 WAV，跑**真实的音频管线**（`usermods/lamp/lamp_pipeline.h`，
与固件同一份代码），输出逐帧 JSON。模拟页面只负责回放。

```bash
c++ -std=c++17 -O2 -I ../usermods/lamp simulate.cpp -o simulate

./simulate song.wav out.json          # 16-bit PCM WAV
./simulate --synth out.json 128       # 合成 128BPM 测试轨
./simulate song.wav out.json 1        # 效果 1（拍点脉冲）
./simulate song.wav out.json 0 --px   # 额外输出 C++ 渲染的 96 个 RGB
```

主机上 FFT 用朴素 DFT（目标板是 esp-dsp）。N=2048 时每帧 210 万次运算，
一分钟音频约十几秒 —— 慢，但零依赖。

## 边界

**能验证的**：BPM、相位、16 段能量、AGC、包络、档位切换 —— 全部由 C++ 算出。

**不能验证的**：灯效。`lamp_fx.h` 只有三个示例效果，模拟页面里还是 JS 复刻的
（为了能实时切换）。真正的效果设计是阶段 4。

**不能验证的**：任何硬件相关的东西 —— 电平转换的时序、WS2812 的实际色温、
ADC 的噪声底、I2S 麦的频响。那些要等扩展板。

## 页面

`sim/simulator.html` 自带三条合成轨的数据。要换成自己的音频：跑一次 `simulate`，
把输出的 JSON 按 `sim/` 下的压缩格式塞回页面（见 `docs/` 里的说明或直接读页面
顶部的 `TRACKS` 常量）。
