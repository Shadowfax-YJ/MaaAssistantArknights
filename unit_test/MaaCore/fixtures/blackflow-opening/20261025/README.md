# 开局奖励弹窗覆盖支援页

003 采集版 v1.1.31 的 `run-20261025-022119-022379` 在 02:21:29 选择林间代步，
确认后出现报废假肢获得弹窗，约五秒后因 `reward_page_lost_before_confirmation_retry` 停止。
原图来自 sequence 17/18/21，未修改像素；来源和 SHA-256 见 `provenance.json`。
安装 Core 摘要与 v1.1.31 发布包一致，诊断现场另存 `build/diagnostics/20261025/003-opening-022119`。

- `reward-candidates.jpg`：操作前原候选页面。
- `reward-confirming.jpg`：已选择中间卡、尚未点击确认。
- `item-popup.jpg`：实际失败现场；前景 X 弹窗和背景“选择支援”同时存在。
- `next-popup.jpg`：另一局的实际通用 NEXT 奖励图，只验证共用按钮识别与分流；不冒充开局现场。

原生回放运行实际 `hijack_reward` 自动采集方法体、OCR、模板及页面判断，仅设备响应和
持久化边界受控；熵固定并每次重置，使同一原图始终选择林间代步，生产随机规则不改。
父任务解析真实资源后必须进入开局来源的截图保护关闭入口，不在确认检测中自行点击关闭。
未知画面、单帧弹窗、保存失败、原确认页和正常招募/组合校验均作为反例。

`opening-popup-transition-events.json` 经实际 Lifecycle 记录 confirming/transition 回调导出，
图片来自上述原图，Session 与时间是受控契约上下文，不是改写历史运行日志。
消费者仅保留这两张诊断图，不派发 OCR、不重复生成奖励；实际物品仍由原正式弹窗记录处理。
