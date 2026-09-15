// swiftc 的 -import-objc-header 入口。liblamp 的 C ABI 由 tools/lamp_capi.h
// 单独定义（那份是纯 C，Python 的 ctypes 与这里共用同一份布局真相）。
#include "lamp_capi.h"
