#!/bin/sh
# 在**目标机**上装/管 wled-sync（Mac 端 sender）这个 launchd 常驻服务。
#
#   ./run.sh venv       首次：建 venv 装依赖（要联网，torch 约 1GB，慢）
#   ./run.sh install    渲染 plist → ~/Library/LaunchAgents → 加载
#   ./run.sh start | stop | restart | status | log | rotate | uninstall
#   ./run.sh render     只把 plist 打到 stdout（看渲染结果，不落盘）
#   ./run.sh apply      deploy.sh 用：装过就重装并重启，没装过就打印首次步骤
#
# 这个脚本连同 sender.py、plist 模板、requirements.txt 由 deploy.sh 一起送来，
# 落在同一个目录（默认 ~/.local/wled-sync）。**运行期不依赖固件仓库的工作树** ——
# 之前那套依赖了（解释器在 tools/mlenv、liblamp 走仓库里的绝对路径），
# 仓库一动服务就可能起不来。
set -e
DEST=$(cd "$(dirname "$0")" && pwd)
LABEL=${WLED_SYNC_LABEL:-com.hjma.wled-sync}
PLIST=$HOME/Library/LaunchAgents/$LABEL.plist
TEMPLATE=${WLED_SYNC_TEMPLATE:-$DEST/wled-sync.plist.in}
SENDER=$DEST/sender.py
LOG=$DEST/sender.log
LOG_MAX_MB=${WLED_SYNC_LOG_MAX_MB:-32}
UID_=$(id -u)

# 灯的地址**不写在仓库里**（和 tools/DEPLOY.md 一个规矩：IP 会变）。
# 放 $DEST/target.conf 第一行非注释，或用 WLED_SYNC_TARGET；都没有就不传
# --target，由 sender.py 自己的默认值兜底。deploy.sh 不会覆盖 target.conf。
if [ -z "$WLED_SYNC_TARGET" ] && [ -f "$DEST/target.conf" ]; then
  WLED_SYNC_TARGET=$(grep -v '^[[:space:]]*#' "$DEST/target.conf" | grep -v '^[[:space:]]*$' | head -1 | tr -d '[:space:]')
fi

python_bin() {
  if [ -n "$WLED_SYNC_PYTHON" ]; then echo "$WLED_SYNC_PYTHON"
  elif [ -x "$DEST/venv/bin/python" ]; then echo "$DEST/venv/bin/python"
  else echo ""; fi
}

# ProgramArguments 里 sender.py 之后的参数。空参数不出现在 plist 里 ——
# 只给 --target 时渲染结果和 2026-09-08 现场那份逐字节一致。
# 一律 if/fi，不写 `[ 条件 ] && 动作` —— 有 set -e 时条件为假会让整个函数
# 当场退出（参数块被吞掉，plist 里的 --target 就这么没的）。
sender_args() {
  set --
  if [ -n "$WLED_SYNC_TARGET" ]; then set -- "$@" --target "$WLED_SYNC_TARGET"; fi
  if [ -n "$WLED_SYNC_INPUT" ];  then set -- "$@" --input  "$WLED_SYNC_INPUT"; fi
  if [ -f "$DEST/liblamp.dylib" ]; then set -- "$@" --lib "$DEST/liblamp.dylib"; fi
  if [ -n "$WLED_SYNC_GAIN" ];   then set -- "$@" --gain  "$WLED_SYNC_GAIN"; fi
  for a in "$@"; do printf '\t\t<string>%s</string>\n' "$a"; done
}

render() {
  py=$(python_bin)
  [ -n "$py" ] || { echo "找不到解释器：先跑 ./run.sh venv，或设 WLED_SYNC_PYTHON" >&2; exit 1; }
  [ -f "$SENDER" ] || { echo "缺 $SENDER —— 先从开发机跑 tools/sender/deploy.sh" >&2; exit 1; }
  # 两处坑，改之前先看：
  #  · 参数块走环境变量，不走 awk -v —— -v 的值不能带换行（报 "newline in string"）。
  #  · awk 变量别叫 log —— 那是内置函数名，gsub 会把它当函数、渲染出 "-inf"。
  #  · 续行 \ 的下一行不能是注释，否则赋值和 awk 断成两条命令，环境变量传不过去。
  WLED_SYNC_ARGS_BLOCK=$(sender_args) \
  awk -v label="$LABEL" -v python="$py" -v sender="$SENDER" -v logpath="$LOG" '
    { gsub(/@LABEL@/, label); gsub(/@PYTHON@/, python); gsub(/@SENDER@/, sender); gsub(/@LOG@/, logpath) }
    /^@ARGS@$/ { if (ENVIRON["WLED_SYNC_ARGS_BLOCK"] != "") printf "%s\n", ENVIRON["WLED_SYNC_ARGS_BLOCK"]; next }
    { print }' "$TEMPLATE"
}

rotate() {
  [ -f "$LOG" ] || return 0
  sz=$(stat -f %z "$LOG" 2>/dev/null || echo 0)
  if [ "$sz" -gt $((LOG_MAX_MB * 1024 * 1024)) ]; then
    mv "$LOG" "$LOG.1"; : > "$LOG"
    echo "日志超过 ${LOG_MAX_MB}MB，已轮转到 $(basename "$LOG").1"
  fi
}

started() { launchctl print "gui/$UID_/$LABEL" >/dev/null 2>&1; }

case "${1:-status}" in
venv)
  py=${PYTHON_BIN:-python3}
  echo "用 $py（$($py -V 2>&1)）建 venv 到 $DEST/venv"
  "$py" -m venv "$DEST/venv"
  "$DEST/venv/bin/python" -m pip install -q --upgrade pip
  "$DEST/venv/bin/python" -m pip install -r "$DEST/requirements.txt"
  echo "依赖装完。CLAP 权重首次加载时联网拉取（之后 plist 里是离线模式）。"
  ;;
install)
  mkdir -p "$HOME/Library/LaunchAgents"
  rotate
  render > "$PLIST.new"
  plutil -lint "$PLIST.new" >/dev/null
  mv "$PLIST.new" "$PLIST"
  launchctl bootout "gui/$UID_/$LABEL" 2>/dev/null || true
  launchctl bootstrap "gui/$UID_" "$PLIST" 2>/dev/null || launchctl load -w "$PLIST"
  echo "已安装并加载 $LABEL"
  sleep 2; "$0" status
  ;;
render)  render ;;
start)   rotate; launchctl bootstrap "gui/$UID_" "$PLIST" 2>/dev/null || launchctl load -w "$PLIST"; "$0" status ;;
stop)    launchctl bootout "gui/$UID_/$LABEL" 2>/dev/null || launchctl unload -w "$PLIST"; echo "已停" ;;
restart) "$0" stop 2>/dev/null || true; sleep 1; "$0" start ;;
rotate)  rotate ;;
status)
  if started; then
    pid=$(launchctl print "gui/$UID_/$LABEL" 2>/dev/null | awk '/^\tpid = /{print $3}')
    echo "$LABEL 已加载（pid ${pid:-无，未在运行}）"
  else
    echo "$LABEL 未加载"
  fi
  if [ -f "$LOG" ]; then echo "日志 $(du -h "$LOG" | awk '{print $1}')，最后三行："; tail -3 "$LOG"; fi
  ;;
log)     tail -f "$LOG" ;;
uninstall)
  launchctl bootout "gui/$UID_/$LABEL" 2>/dev/null || launchctl unload -w "$PLIST" 2>/dev/null || true
  rm -f "$PLIST"; echo "已卸载（$DEST 与日志保留）"
  ;;
apply)
  if [ -f "$PLIST" ]; then "$0" install
  else
    echo "还没装过。首次步骤："
    echo "  1) cd $DEST && ./run.sh venv"
    echo "  2) echo '<灯的地址>' > $DEST/target.conf   # 例如 xxx.local,192.168.x.x"
    echo "  3) ./run.sh install"
  fi
  ;;
*) echo "用法: $0 {venv|install|start|stop|restart|status|log|rotate|render|uninstall|apply}" >&2; exit 2 ;;
esac
