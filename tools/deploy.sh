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
HOST=${HOST:-macbook-m4-max}
DEST=${DEST:-'~/workspace/codex-deployments/lamp-sim'}
TRIES=${TRIES:-5}

FILES="usermods/lamp tools/lamp_capi.cpp tools/lamp_fft.h tools/server.py
       tools/build.sh tools/run.sh tools/sim/index.html"

i=1
while [ $i -le $TRIES ]; do
  printf "第 %d/%d 次… " "$i" "$TRIES"
  if tar czf - $FILES 2>/dev/null | ssh -o ConnectTimeout=25 -o ServerAliveInterval=5 \
       "$HOST" "mkdir -p $DEST/tools/sim && cd $DEST && tar xzf - \
                && cd tools && chmod +x build.sh run.sh && ./run.sh restart" 2>&1 \
     | tail -4; then
    echo "部署完成"
    ssh -o ConnectTimeout=25 "$HOST" "cd $DEST/tools && ./run.sh status" 2>/dev/null || true
    exit 0
  fi
  echo "失败"
  i=$((i + 1))
  [ $i -le $TRIES ] && sleep 5
done
echo "连续 $TRIES 次失败 —— 确认 20.3 在线（ping 10.147.20.3）后重试" >&2
exit 1
