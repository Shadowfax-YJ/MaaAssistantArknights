# 物品弹窗关闭前保存

`producer-events.json` 由生产 `capture_collection_popup`、
`persist_node_evidence_capture` 和 `persist_collection_popup_capture` 的方法体回放导出。
只有截图、存储、会话和归属 I/O 被替换。第一条是随机落点尚未确定归属时的持久临时证据，
第二条是沿用既有归属解析后的最终弹窗事件；不是两件物品，也不冒充原始实机记录。

交互回放同时执行生产 ProcessTask 动作派发和截图插件方法，覆盖成功、短暂/持续写盘失败、
空图、停止、弹窗自行消失、下一动作不继承旧跳过标记、动画背景及归属未定时的落盘失败。

原生回放使用 `blackflow-encounter/20261024/cargo-reward.jpg` 检查真实按钮模板，
以连续两张相同原图模拟两次弹窗，再切回实际出售页；不声称记录了白模中间缺失画面。
出售页 `blackflow-merchant/20261024/eerie-sale-after.jpg` 原样来自
archive `2026-10-19/3568854851/6020.zip` 中
`run-20261018-150925-758825/images/003858-store-eerie-sale-select-started.jpg`，
SHA-256 `3af99129fb5f25ef70c1c3ea9da1753741b0824ed0a9c6d252e790f5ec68be7e`。

同一契约样本同步到 analysis `tests/fixtures/blackflow-popup-pending-events.json`。
独立签名归档验证和消费者检查要求保留两张证据，仅生成一个最终弹窗识别任务。
