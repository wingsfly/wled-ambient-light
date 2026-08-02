# 部署到 macbook-m4-max（10.147.20.3）

按 PersonalServices 的 inventory 走：规范别名 `macbook-m4-max`，
workspace `~/workspace/codex-deployments/lamp-sim`，
约束 `temporary-workloads-only`（所以不做 launchd 持久化）。

## 部署

```bash
cd /Users/hjma/workspace/iflytek/wled-ambient-light
D=~/workspace/codex-deployments/lamp-sim
rsync -az --delete \
  --include='usermods/' --include='usermods/lamp/' --include='usermods/lamp/*.h' \
  --include='tools/' --include='tools/sim/' \
  --include='tools/lamp_capi.cpp' --include='tools/lamp_fft.h' \
  --include='tools/server.py' --include='tools/build.sh' --include='tools/sim/index.html' \
  --exclude='*' ./ macbook-m4-max:$D/
ssh macbook-m4-max "cd $D/tools && ./build.sh && (nohup python3 -u server.py --port 8080 --host 0.0.0.0 > server.log 2>&1 &)"
```

停：`ssh macbook-m4-max "pkill -f 'server.py --port 8080'"`

## 怎么打开（这一段要紧）

**首选：在 20.3 本机开 `http://localhost:8080`。**

两个理由，都不是小事：

1. **麦克风只在安全上下文可用。** `getUserMedia` 在 `http://10.147.20.3:8080`
   这种非 localhost 的明文来源下会被浏览器**静默拒绝**，不给任何提示。
   `localhost` 是安全上下文的特例，直接可用。

2. **ZeroTier 这条链路跑不动实时音频。** 实测 RTT **274ms**（走中继而非直连，
   因为它是 mobile 设备）。20 秒音频要传 157 秒 —— 实时的十分之一。
   本机 localhost 没有这个问题，实测端到端 **40× 实时**。

从别的机器访问时，用端口转发把它变成本地 localhost：

```bash
ssh -L 8080:localhost:8080 macbook-m4-max
```

然后开 `http://localhost:8080`。这解决麦克风权限，但**解决不了 274ms 的
延迟** —— 麦克风实时仍会卡顿。文件与链接不受影响（它们不要求实时）。

## 三种输入源

| 源 | 说明 |
|---|---|
| 音频文件 | 浏览器 `decodeAudioData` 解码 → 重采样 22.05kHz 单声道 → 分块送出 |
| 音频链接 | 同上，但要对方允许 CORS。被挡就先下载再用文件方式 |
| 麦克风 | 需要安全上下文，见上 |

## 出问题时

- **页面开不了**：`ssh macbook-m4-max "tail -20 ~/workspace/codex-deployments/lamp-sim/tools/server.log"`
- **连不上 WebSocket**：页面右上角的圆点是连接状态，红=断开
- **`找不到 liblamp`**：`ssh macbook-m4-max "cd ~/workspace/codex-deployments/lamp-sim/tools && ./build.sh"`
- **`LampFrameC 布局不一致`**：改过 `lamp_capi.cpp` 的结构体但没同步改 `server.py`
