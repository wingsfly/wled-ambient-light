#!/bin/sh
# 在 20.3 本机启停模拟服务。
#
#   ./run.sh start    构建（若需要）并后台启动
#   ./run.sh stop
#   ./run.sh restart
#   ./run.sh status
#   ./run.sh log      跟踪日志
#
# 这台机器是 temporary-workloads-only，所以刻意**不做** launchd 持久化 ——
# 重启后要自己再 start 一次。要常驻的话该换到 persistent-service 角色的机器。
set -e
cd "$(dirname "$0")"
PORT=${PORT:-8080}
PAT="server.py --port $PORT"
EXT=so; [ "$(uname)" = "Darwin" ] && EXT=dylib

case "${1:-status}" in
  start)
    if pgrep -f "$PAT" >/dev/null 2>&1; then
      echo "已经在跑：$(pgrep -f "$PAT")"; exit 0
    fi
    [ -f "liblamp.$EXT" ] || ./build.sh
    # 源码比库新就重编，省得改了 C++ 忘记构建、对着旧行为纳闷
    for f in lamp_capi.cpp lamp_fft.h ../usermods/lamp/*.h; do
      [ "$f" -nt "liblamp.$EXT" ] && { echo "源码有更新，重新构建"; ./build.sh; break; }
    done
    # 套一层重启循环。C++ 侧的段错误会直接杀掉整个 Python 进程（线上遇到过
    # SIGBUS），Python 的 try/except 拦不住 —— 只能从外面把它拉起来。
    # 崩了会在 server.log 里留一行时间戳，别让它静默重启掩盖问题。
    nohup sh -c 'while :; do
        python3 -u server.py --port '"$PORT"' --host 0.0.0.0
        echo "[$(date "+%F %T")] 进程退出（码 $?），2 秒后重启 —— 若反复出现请查崩溃报告"
        sleep 2
    done' > server.log 2>&1 &
    sleep 2
    if pgrep -f "$PAT" >/dev/null 2>&1; then
      echo "已启动 pid=$(pgrep -f "$PAT")"
      echo
      sed -n '1,3p' server.log
      echo
      echo "本机打开：http://localhost:$PORT   ← 麦克风要用这个"
      echo "远程访问：ssh -L $PORT:localhost:$PORT macbook-m4-max"
    else
      echo "启动失败："; cat server.log; exit 1
    fi ;;
  stop)
    pkill -f 'while :; do' 2>/dev/null || true
    sleep 1
    pkill -f "$PAT" 2>/dev/null && echo "已停止" || echo "没在跑" ;;
  restart) "$0" stop; sleep 1; "$0" start ;;
  status)
    if pgrep -f "$PAT" >/dev/null 2>&1; then
      echo "运行中 pid=$(pgrep -f "$PAT")"
      curl -s -o /dev/null -w "localhost:$PORT → HTTP %{http_code} (%{time_total}s)\n" \
        "http://localhost:$PORT/" || true
    else
      echo "未运行"
    fi ;;
  log) tail -f server.log ;;
  *) echo "用法: $0 {start|stop|restart|status|log}"; exit 2 ;;
esac
