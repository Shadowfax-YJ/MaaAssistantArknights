# 通用事件与扫描状态共享契约

`lifecycle-payloads.json` 从实际事件生产方法的受控 I/O 回放导出，覆盖实际启用回退选项、
独立后续页确认与 acquisition_status=unresolved；事件名称用于通用路径覆盖，不是假称取得了对应新实机整局。
`producer-events.json` 再通过真实 Session、TaskPort、record_encounter_evidence 与运行日志器落盘导出，
包含普通选择、完整扫描但关闭失败、未知身份的现场证据，以及无独立弹窗要求的事件结果观察。
`evidence-frame.jpg` 是封包测试使用的受控颜色帧，仅用于图片/签名契约，不是真实游戏界面。
原图识别和失败现场回放见相邻 blackflow-civilization/20261025，真实过程/受控边界须分别解释。

生产与 analysis 共享同一 producer-events.json；独立 v1 签名验证器检查新旧可选字段和图片封包，
消费者整理选择与 scan_complete/panel_closed，不把输入/结果截图派成新增物品。
