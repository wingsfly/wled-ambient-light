# 部署模拟器

服务跑在一台远程机器上，页面也由它提供。**目标主机不写在这个仓库里** ——
IP 会变、别名不会，而别名归 PersonalServices 的 inventory 管
（`deploy/inventory/targets.json`）。部署前先跑那边的 `preflight.py` 确认可达。

## 部署

```bash
LAMP_HOST=<你的 SSH 别名> ./tools/deploy.sh
```

带 5 次重试。用 tar over ssh 而不是 rsync/scp —— 后两者在高延迟且时通时断的
链路上经常中途 `Connection closed`，而 tar 管道一次成型，失败就整体重来，
不会在对端留下半个文件。

路径默认 `~/lamp-sim`，用 `LAMP_DEST` 覆盖。

## 在目标机上启停

```bash
cd <部署路径>/tools
./run.sh start      # 源码比 liblamp 新会自动重编
./run.sh status
./run.sh log
./run.sh stop
```

`liblamp` 在目标机上编译，是那台机器的原生库，不跨架构。

刻意**没做** launchd 持久化：如果目标机的角色是 `temporary-workloads-only`，
挂常驻服务不合规，重启后自己 `start` 一次。要常驻就换到 `persistent-service`
角色的机器。

> 注意：**Mac 端 sender 是另一回事，它确实挂了 launchd 常驻**，而且就在一台标着
> `temporary-workloads-only` 的机器上。那是个已知的冲突，记在
> [tools/sender/README.md](sender/README.md)，不是这一节的例外。

## 怎么打开 —— 这一段要紧

**在目标机本机开 `http://localhost:8080`。**

两个理由都不是小事：

1. **麦克风只在安全上下文可用。** `getUserMedia` 在
   `http://<局域网 IP>:8080` 这种非 localhost 的明文来源下会被浏览器
   **静默拒绝**，不给任何提示。`localhost` 是安全上下文的特例。

2. **高延迟链路跑不动实时音频。** 实测经 ZeroTier 中继时 RTT 274ms，
   20 秒音频要传 157 秒 —— 实时的十分之一。本机 localhost 实测 **40–54× 实时**。

从别的机器访问就做端口转发，把它变成本地 localhost：

```bash
ssh -L 8080:localhost:8080 <目标主机>
```

这解决麦克风权限，但**解决不了链路延迟** —— 麦克风实时仍会卡。
文件与链接不受影响（它们不要求实时）。

## 三种输入源

| 源 | 说明 |
|---|---|
| 音频文件 | 浏览器 `decodeAudioData` 解码 → 重采样 22.05kHz 单声道 → 分块送出 |
| 音频链接 | 同上，但要对方允许 CORS。被挡就先下载再用文件方式 |
| 麦克风 | 需要安全上下文，见上 |

麦克风离声源远近能差一两个数量级，页面上有**输入增益**滑块（1×–100×）现场补偿。

## 出问题时

页面底部有事件日志，记录麦克风生命周期每一步和 2 秒一次的状态心跳。
「收发」计数（N 块 / M 帧）能直接看出卡在哪一环：

| 现象 | 含义 |
|---|---|
| 块数不涨 | 采集停了（看门狗会同时报警） |
| 块数涨、帧数不涨 | 数据发出去了但服务端没回 |
| 都在涨但灯不亮 | 纯粹是电平问题，推输入增益滑块 |

服务端日志：`./run.sh log`。若出现「进程退出（码 …），2 秒后重启」，
说明 C++ 侧崩了 —— 那行时间戳能对上系统的崩溃报告。

- **`找不到 liblamp`**：`./build.sh`
- **`LampFrameC 布局不一致`**：改过 `lamp_capi.cpp` 的结构体但没同步改 `server.py`

## Mac 端 sender（另一套东西）

这一篇讲的是**模拟器**（`~/lamp-sim`，页面在目标机 8080）。给灯发 LAMP1 包的
Mac 端 sender 是独立的常驻服务，部署脚本和 launchd 定义在 [tools/sender/](sender/)。
