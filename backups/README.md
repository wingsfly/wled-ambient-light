# 板上配置与固件快照

每目录一次快照：cfg.json + presets.json（板子导出）+ 当时构建的 firmware bin。
bin 不入 git（可由对应 tag/commit 重建），cfg/presets 入库版本化。
恢复：cfg/presets 经 http://<板子>/upload 上传；bin 经 /update OTA。

- 2026-08-25-baseline —— 首板实测基线（tag field-baseline-2026-08-25）
- 2026-09-02-featured —— 功能全量里程碑（tag featured-2026-09-02）
