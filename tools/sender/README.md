# Mac 端 sender 的部署

`tools/wled_sync_sender.py` 采 BlackHole 的声音、过 liblamp 管线、把 LAMP1 包发给灯。
它在一台 Mac 上常驻运行，这个目录是**把那套运行环境写下来**的地方：在此之前
部署路径和 launchd 定义只存在于目标机上，任何一处丢了都没法复现。

| 文件 | 作用 |
| --- | --- |
| `deploy.sh` | 在**开发机**上跑：把运行所需文件打包送到目标机并应用 |
| `run.sh` | 在**目标机**上跑：建 venv、装/起/停/看服务、轮转日志 |
| `wled-sync.plist.in` | launchd 定义的模板，`run.sh install` 渲染后装进 `~/Library/LaunchAgents` |
| `requirements.txt` | Python 依赖，版本取自正在跑的那套环境 |

## 首次部署

```bash
LAMP_HOST=<SSH 别名> ./tools/sender/deploy.sh     # 别名归 PersonalServices 的 inventory 管
```

送过去的是一个自足目录（默认 `~/.local/wled-sync`）：`sender.py`、`run.sh`、
plist 模板、`requirements.txt`、`liblamp.dylib`。然后在目标机上：

```bash
cd ~/.local/wled-sync
./run.sh venv                                  # 建 venv 装依赖，要联网，torch 约 1GB
./run.sh warm                                  # 预取 CLAP 权重，约 2.2GB，冷机器必做
echo 'xxx.local,192.168.x.x' > target.conf     # 灯的地址，见下
./run.sh install                               # 渲染 plist、加载、开机自启
```

**灯的地址不写在仓库里。** 和 `tools/DEPLOY.md` 一个规矩：IP 会变。放
`target.conf` 第一行，或用 `WLED_SYNC_TARGET`；`deploy.sh` 不会覆盖它。
都没配就不传 `--target`，由 `sender.py` 自己的默认值兜底。

## 日常

```bash
./run.sh status | log | restart | rotate | render | uninstall
LAMP_HOST=<别名> ./tools/sender/deploy.sh        # 改完 sender.py 后重新部署，自动重装重启
```

`render` 只把渲染后的 plist 打到 stdout，不落盘，改模板时先用它看一眼。

可调环境变量：`WLED_SYNC_TARGET`、`WLED_SYNC_INPUT`（声卡名，默认走 sender 的
BlackHole）、`WLED_SYNC_GAIN`、`WLED_SYNC_PYTHON`（不建 venv 而复用现成解释器）、
`WLED_SYNC_LABEL`、`WLED_SYNC_LOG_MAX_MB`、`WLED_SYNC_DEST`。

## 与现在跑着的那一套的差异

2026-09-08 把现场状态收进仓库时，线上实例（macbook-m4-max，已连续跑四天）是这样的：

- 解释器是 `tools/mlenv/bin/python`，**venv 建在固件仓库的工作树里**。仓库一被
  clean/移动，服务就起不来。`run.sh venv` 改建在 `$DEST/venv`。
- `--lib` 用的是 sender.py 里写死的绝对路径，同样指向仓库工作树；而目标机上那份
  `tools/liblamp.dylib` 相对仓库里提交的版本**是改过的**（`git status` 显示 M）。
  `deploy.sh` 送的是仓库里提交的那份，`run.sh` 用 `--lib $DEST/liblamp.dylib` 指过去。
  **重新部署会把运行时用的库换成仓库版本** —— 如果目标机那份改动有意义，先提交它。
- 日志 `sender.log` 当时已经涨到 24MB 且没有轮转。`run.sh` 在 start/install 时
  超过 `WLED_SYNC_LOG_MAX_MB`（默认 32）就转存成 `sender.log.1`。

因为这三条，`deploy.sh` 一跑就会**重启这个正在服务的进程**并切换它的解释器与库。
不是纯粹的「补个文档」，动手前心里有数。

## 一个没解决的冲突

这是个 `RunAtLoad` + `KeepAlive` 的常驻服务，而它跑在 macbook-m4-max 上——
PersonalServices 的 inventory 给这台机器标的约束是 `temporary-workloads-only`
和 `mobile`。`tools/DEPLOY.md` 里模拟器那节还专门写了「刻意没做 launchd 持久化」，
理由正是这条约束。sender 却挂了常驻。

两条路，都得人来定：把这台机器的角色改掉，或者把服务搬到 `persistent-service`
角色的机器上（那台得有 BlackHole 和能听见的音频源）。在定下来之前，这里如实记着。

## CLAP 权重（不在这个仓库里，也不该在）

仓库里跟踪的最大文件是两个 92K / 392K 的 dylib，没有任何模型权重。语义情绪那部分
要两块东西，都不是 pip 依赖，是 laion_clap 在**第一次 `load_ckpt()` 时现下的**：

| 权重 | 体积 | 落在哪 |
| --- | --- | --- |
| `630k-audioset-best.pt`（音频分支） | 1.7G | venv 里 `laion_clap` 的包目录，直接 wget 下来的 |
| `roberta-base`（文本编码器） | 478M | `~/.cache/huggingface`，走 transformers |

所以 `run.sh venv` 建完 venv **还没有权重**，而 plist 里设了 `HF_HUB_OFFLINE=1`
和 `TRANSFORMERS_OFFLINE=1` —— launchd 起的进程拉不动 roberta-base。冷机器上不先跑
`./run.sh warm`，CLAP 会加载失败并**静默降级**：sender 照常跑，mood 只剩管线的
能量近似，日志里看不到 `CLAP 就绪`。这是最容易在新机器上踩到的一脚。

`warm` 会 `cd` 到部署目录再下载：python-wget 把半截文件写在当前目录，
中断一次就留个几百 MB 的 `.tmp`（macbook-m4-max 的仓库根上那个 217M 的就是这么来的，
是垃圾，可以删）。那台机器上音频 ckpt 还在 HF 缓存里另存了一份，两处加起来白占 1.7G。

不把权重收进仓库的理由：单个文件 1.7G，git 存不住也没必要 —— 它是可重新下载的
第三方产物，`requirements.txt` 钉了 `laion_clap==1.1.7`，`load_ckpt()` 默认取的
就是同一个 `630k-audioset-best.pt`。真要离线复现，把 venv 的 `laion_clap` 包目录和
`~/.cache/huggingface/hub/models--roberta-base` 一起拷到新机器，比走 git 快得多。
