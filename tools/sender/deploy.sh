#!/bin/sh
# 从开发机把 Mac 端 sender 推到目标机并应用。
#
#   LAMP_HOST=<SSH 别名> ./tools/sender/deploy.sh
#
# 送过去的是一个**自足目录**（默认 ~/.local/wled-sync）：sender.py、run.sh、
# plist 模板、requirements.txt、liblamp.dylib。target.conf 和 venv 留在目标机上，
# 这里不覆盖也不传。
#
# 和 tools/deploy.sh 同样用 tar over ssh：rsync/scp 在这条 ZeroTier 链路上
# 经常中途断，tar 管道失败就整体重来，不会留半个文件。
set -e
cd "$(dirname "$0")/../.."
# 主机不写死在仓库里：别名归 PersonalServices 的 inventory 管
# （deploy/inventory/targets.json，先跑 preflight.py）。
HOST=${LAMP_HOST:?请先设置 LAMP_HOST（PersonalServices 里的规范 SSH 别名）}
DEST=${WLED_SYNC_DEST:-'~/.local/wled-sync'}
TRIES=${TRIES:-5}

STAGE=$(mktemp -d -t wledsync)
LOG=$(mktemp -t wledsyncdeploy)
trap 'rm -rf "$STAGE" "$LOG"' EXIT

cp tools/wled_sync_sender.py "$STAGE/sender.py"
cp tools/sender/run.sh tools/sender/wled-sync.plist.in tools/sender/requirements.txt "$STAGE/"
# liblamp 带编好的二进制过去（和模拟器同样的理由：两台都是 arm64 macOS，
# 而这条链路不适合同步整个 wled00/ 再远端重编）。换架构就得改成远端 build.sh。
if [ -f tools/liblamp.dylib ]; then cp tools/liblamp.dylib "$STAGE/"; fi

# 输出落临时文件再看。**不要**写成 `... | ssh ... | tail`：管道退出码是最后一个
# 命令的，tail 永远成功 —— 连不上也会报「部署完成」。tools/deploy.sh 栽过这个。
i=1
while [ $i -le $TRIES ]; do
  printf "第 %d/%d 次… " "$i" "$TRIES"
  if tar czf - -C "$STAGE" . 2>/dev/null | ssh -o ConnectTimeout=25 -o ServerAliveInterval=5 \
       "$HOST" "mkdir -p $DEST && cd $DEST && tar xzf - && chmod +x run.sh && ./run.sh apply" >"$LOG" 2>&1
  then rc=0; else rc=$?; fi
  tail -8 "$LOG"
  if [ $rc -eq 0 ]; then echo "部署完成"; exit 0; fi
  echo "失败（退出码 $rc）"
  i=$((i + 1))
  # if/fi 而不是 `[ ... ] && sleep`：set -e 下条件为假会让循环体退出码为 1，脚本直接终止。
  if [ $i -le $TRIES ]; then sleep 5; fi
done
echo "连续 $TRIES 次失败 —— 先确认 $HOST 在线（PersonalServices 的 preflight.py）" >&2
exit 1
