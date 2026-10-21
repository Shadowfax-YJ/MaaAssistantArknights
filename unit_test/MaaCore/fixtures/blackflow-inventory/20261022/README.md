# 零件箱回到起点时的回弹

两个 v1.1.25 现场均以 `inventory observation could not confirm the start of the list`
停止。目录来自 `MAAInstanceManager-v0.1.0-win-x64/managed_instances/`：

- MAA-001：`run-20261022-000132-696344`，二层商店后。
  `instance001-first-page.jpg` 原样取自
  `images/001995-inventory-overload-check-completed.jpg`。
  SHA-256：`0569a8c0d744c48a288b57c938787b926dd55b6b42ce7dcffeea665e5a34e0d5c`。
- MAA-003：`run-20261022-000916-465882`，二层枯枝战斗后。
  `instance003-first-page.jpg` 原样取自
  `images/001388-inventory-overload-check-completed.jpg`。
  SHA-256：`fb9ab154ea42ece9e67899d07abfaded2170cf08e0278e2e162dbcf0948fec70`。

`runtime-layouts.json` 保留各实例原始 `debug/asst.log` 中回滑前一次及随后
十次 OCR 的完整名称坐标和时间戳，只去除标题被截断的候选，与生产起点布局
比较使用的筛选条件相同。它不是对保存后的 JPEG 重新 OCR 的结果。
001 首项“标准引擎”的 x 序列为 393、427、393、421、465、401、442、525、
480、395、395；003 首项“气垫底座”为 392、556、588、568、481、573、549、
555、569、393、412。整列仍在回弹，不能通过放宽 3px 容差当作静止。

旧生产布局判定和十次回滑上限对原始序列均无法确认起点。交互回放提取真实
`scan_inventory_frame`、稳定等待、重开及逐页识别方法，注入以上名称坐标，
覆盖被动等待后收敛、持续不收敛后重开、输入失败、停止、页面丢失、无法收起、
无法展开、未知过渡页和重开后仍不稳定。被动等待时回到首帧坐标、重开后的
首屏及空槽末屏属于受控控制器状态，不是现场额外录到的帧；不声称完整实机
回放。原生识别回放另在两张原始 JPEG 上验证展开状态、收起模板和名称 OCR。
