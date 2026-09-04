# PlatformIO pre-script：把 git 短 SHA + 月日注入为 LAMP_FW_ID，/leddbg 回显，
# 用来确认板上跑的是哪一版（OTA 后失联/回滚时不用猜）。
Import("env")
import datetime, subprocess
try:
    sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=env["PROJECT_DIR"]).decode().strip()
except Exception:
    sha = "nogit"
env.Append(CPPDEFINES=[("LAMP_FW_ID", env.StringifyMacro(sha + "-" + datetime.date.today().strftime("%m%d")))])
