# 狭路相逢准备阶段样本

`preparation.json` 来自 `run_blackflow_duel_preparation_replay.ps1` 调用生产方法
`select_duel_preparation_tier`、`record_duel_preparation` 和 `run_preparation_phase` 的载荷。
两场作战各三档，分别保留选择和部署完成两个阶段。控制器、OCR 和持久化反馈为受控输入，
不声称是真实整局录像。格子另对照实际 TilePack 地图，搏杀为 `(1,3)/(4,3)/(7,3)`，
共斗各增加同档右邻格 `(2,3)/(5,3)/(8,3)`；战后镜头和战斗部署计划保持不变。

`producer-events.json` 将相同载荷装入合成的 schema 1 事件封套。选择是
`node.battle.duel.select / selected / planned`，不证明已经部署、开战或获胜；
开始前图片沿用 `node.event.capture / duel-before-start`，保留原节点的事件名和节点目录。
图片封套用于独立签名验证与消费者兼容检查，不是识别样本；不新增图像 OCR 任务。
