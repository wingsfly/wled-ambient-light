#!/bin/sh
# 从开发机把模拟器推到 20.3 并重启。
#
#   ./tools/deploy.sh
#
# 用 tar over ssh 而不是 rsync/scp —— 后两者在这条 ZeroTier 链路上经常
# 中途 "Connection closed"（RTT 200ms+ 且时通时断）。tar 管道一次成型，
# 失败就整体重来，不会留下半个文件。
set -e
cd "$(dirname "$0")/.."
# 主机与路径都从环境变量取，**不写死在仓库里**：
#   IP 会变、别名不会，而别名归 PersonalServices 的 inventory 管。
#   查目标：deploy/inventory/targets.json
HOST=${LAMP_HOST:?请先设置 LAMP_HOST（PersonalServices 里的规范 SSH 别名）}
DEST=${LAMP_DEST:-'~/lamp-sim'}
TRIES=${TRIES:-5}

# libwledfx 带**编好的二进制**过去，不在目标机重编。
#
# 理由：重编需要整个 wled00/（十几 MB 源码）跟着走，而这条 ZeroTier 链路
# RTT 200ms+、时通时断。两台都是 arm64 macOS，动态库直接通用。
# 换成别的架构/系统的目标机时，得改成同步源码 + 远端跑 build_wled.sh。
FILES="usermods/lamp tools/lamp_capi.cpp tools/lamp_fft.h tools/server.py
       tools/build.sh tools/run.sh tools/sim/index.html"
[ -f tools/libwledfx.dylib ] && FILES="$FILES tools/libwledfx.dylib"
[ -f tools/libwledfx.so ]    && FILES="$FILES tools/libwledfx.so"

# 输出落到临时文件再看，**不要**写成 `... | ssh ... | tail -4`：
# 管道的退出码是最后一个命令的，而 tail 永远成功 —— ssh 连不上也会报
# 「部署完成」，下面那五次重试全是死代码。这个 bug 藏了一阵子，
# 直到目标机休眠时才露出来（超时了还说部署成功）。
LOG=$(mktemp -t lampdeploy)
trap 'rm -f "$LOG"' EXIT

i=1
while [ $i -le $TRIES ]; do
  printf "第 %d/%d 次… " "$i" "$TRIES"
  # 包在 if 里跑：`set -e` 对 if 的条件部分豁免，裸写管道的话失败会直接
  # 终止脚本，连 $? 都取不到。管道最后一个命令是 ssh，退出码就是它的。
  if tar czf - $FILES 2>/dev/null | ssh -o ConnectTimeout=25 -o ServerAliveInterval=5 \
       "$HOST" "mkdir -p $DEST/tools/sim && cd $DEST && tar xzf - \
                && cd tools && chmod +x build.sh run.sh && ./run.sh restart" >"$LOG" 2>&1
  then rc=0; else rc=$?; fi
  tail -4 "$LOG"
  if [ $rc -eq 0 ]; then
    echo "部署完成"
    ssh -o ConnectTimeout=25 "$HOST" "cd $DEST/tools && ./run.sh status" 2>/dev/null || true
    exit 0
  fi
  echo "失败（退出码 $rc）"
  i=$((i + 1))
  [ $i -le $TRIES ] && sleep 5
done
echo "连续 $TRIES 次失败 —— 先确认 $HOST 在线（PersonalServices 的 preflight.py）" >&2
exit 1
