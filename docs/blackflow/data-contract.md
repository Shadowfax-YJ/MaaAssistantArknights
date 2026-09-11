# BlackFlow 采集契约与下游联动

本文记录当前原始数据约束和变更流程。通用后处理与宿主插件由 `lubiao-analysis/docs/data-pipeline/` 维护；部署操作见该仓库 `operations.md`，中心处理跟随机器人运行。

## 当前权威实现

- [BlackFlowRunLog.cpp](../../src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowRunLog.cpp)：manifest、事件、图片及其引用。
- [BlackFlowRunIntegrity.cpp](../../src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowRunIntegrity.cpp)：采集中维护内容账本，封存逐文件 SHA-512 和完整性清单。
- [BlackFlowRunArchive.cpp](../../src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowRunArchive.cpp)：封包、本地签名、临时文件到最终归档的交接。
- [VerifyBlackFlowRunArchive.py](../../tools/VerifyBlackFlowRunArchive.py)：不解压的独立验证器；发布工作流已分发该工具。
- [完整性测试](../../unit_test/MaaCore/BlackFlowRunIntegrityTest.cpp)与[归档样本工装](../../unit_test/MaaCore/BlackFlowArchiveTestFixture.h)：验证生产者约束和篡改/缺失/中途开局等行为。

现有完整封包要求起始、确认全新开局及结束事件。完整性清单与 ZIP 签名分别有 schema；本地签名明确 `origin_attested=false`，不能描述为官方/未修改客户端来源认证。旧无签名包和特殊诊断目录不因此自动变成损坏包；消费者根据相应 profile 判定可用证据。

## 新增的原始契约声明

manifest 新增 `run_uuid`（UUID v4）、`contract_id/version`、`archive_schema_version`、`image_layout_version`、`capabilities`、`coverage=observed-events-only`。采集模式来源为 `event.state.profile`。当前没有资源文件内容摘要，因此 `resource_identity=not-recorded`，不假定同程序版本使用相同资源。能力声明只表示支持记录某种证据，不证明每层都有观测。

`python tools/BuildBlackFlowContract.py --output OUTPUT` 导出原始契约描述、独立验证器、真实 C++ 封包样本、预期验证结果及逐文件摘要。Windows 包和 macOS DMG 打包流程已调用此入口。bundle 记录来源 commit 与相关源文件是否未提交；dirty 导出不是已发布上游版本。analysis 固定 bundle 摘要后随包分发，不在机器人中复制验证器。

验证器退出码：0 本地签名/内容通过；1 损坏；2 未签名/旧版/重打包；3 未支持契约；4 缺少依赖。旧版兼容解析仍须经过路径、大小、版本和事件检查。

更新封包或必要成员时，同时检查消费者限制。已核对的机器人默认上限是压缩包 256 MiB、10,000 成员及 2 GiB 解压量；当前独立验证器上限不同。限额是部署条件，不能靠新包内容自动抬高宿主上限。

## 修改采集程序时的检查范围

新增/删除/改名字段、改变事件触发时机、修改图像位置或分辨率、改变地图/楼层身份、实际消耗判定、精简日志、快跑模式、封包和哈希/签名规则，都需要检查下游验证、处理和分析。程序功能或性能修改若不影响这些输出，记录无数据契约影响的理由即可。

对受影响变更：

1. 用最小输入说明前后区别，更新生产者测试样本与预期业务含义。
2. 联动本仓库独立验证器和原始契约，再更新 analysis 的版本适配、标准事实和受影响专题。检查三层专用记录、完整对局和未到达楼层的差异。
3. PtilopsisBot 与 quark-timed-sync 的业务逻辑放在 analysis 插件；只有宿主钩子/协议/通用传输能力变化才修改宿主核心。
4. 同一批样本验证导出 → 验证 → 标准化 → 统计；记录引用、去重、缓存/审核失效及旧数据兼容。涉及新格式发布时先部署兼容消费者，再发布生产者。

完整流程读取本次已定位的 `lubiao-analysis/docs/data-pipeline/coordination.md`。本机兄弟仓库名称和盘符可能不同，按 Git remote 或工作区映射定位；不要把固定磁盘路径写入采集契约。

## v1.1.5 发布兼容性核对

本版包含 a2094476df 的增量 manifest 声明，以及树洞效果跨局缓存清理、标题识别和返回外层地图的恢复修复。原始契约、事件、图片布局和封包 schema 均维持 1；不增加新的业务事件字段或截图类别。返回恢复会增加既有 `process.*` 轨迹，外层楼层归属仍由 Session 恢复，既有实际地图合并规则继续适用。

2026-09-12 核对：PtilopsisBot 依赖固定的 analysis 提交 c762533c1a15b55bc1508fe424bb537c966953c8 已包含本版契约的消费者。其 bundle 来源为 a2094476df，锁定 SHA-256 为 `45ab48bef170a6cb34b7210c19e1d07047d5ce4a9d4ec9e62378c4de015847a8`；验证器、契约描述和签名样本与生产者逐字节一致。旧 manifest 继续走兼容分支，新 `run_uuid` 用于对局身份；未知契约仍保留原件并标记 unsupported。宿主协议不变，无需因本次采集器修复改动两个通用宿主。

本次不改变 OCR 模型、标准事实 schema、专题统计方法或已有审核输入，因此不全量失效旧缓存和审核。修复可使新对局继续探索并增加实际采到的证据，但不回填旧失败对局、不把未采到的楼层计为阴性，也不将错误树洞地图自动计入实托邦或每局数量统计。历史问题仍按原证据和审核处理。

本地验证通过：346 个 C++ 回归用例（5735 个断言）、跨局缓存与树洞返回的 12 个真实方法回放场景、4 个原始契约测试、33 个更新与封包测试、analysis 的 17 个流水线兼容用例；共享签名样本和 v1.1.4 原始诊断包均通过消费者校验。消费者代码已兼容，本次未操作收集电脑的部署环境。

## v1.1.6 选券与购买证据兼容

原始契约、事件、图片布局和封包 schema 继续为 1。manifest 增加可选能力
`recruitment-choice-v1`、`store-purchase-evidence-v2`，表示支持下述证据，不保证每次都有完整记录。
图片仍使用原有 JPEG、完整性账本、节点目录及延迟归属机制。

- `node.get-drop.capture` 新增实际 `GetDropSelectRecruit` 选券页。`selection_kind=recruitment_voucher`、
  `capture_kind=selection_frame_before_click`；保存匹配帧、实际按钮框、检测到的候选框及可证明的选项编号。
  完整券名没有运行时识别结果时记录 `selected_name_status=image_evidence_only`，由原图恢复；
  非二/三选布局只保留实际按钮框，不编造选项数。
- `recruitment_choice_id` 标识一次选券。`reward.recruitment.select` 记录 `click_dispatched` 和失败原因；
  后续 `node.recruitment.capture` 使用同一编号和 `choice_resolution=opened_after_selection`。
  重试沿用编号，招募页打开后的下一次选券使用新编号。仅发出点击不等于已经获得一张券。
  `recruitment_choice_attempt` 区分同一选券的每次尝试，招募页引用已发出点击的那一次；
  后续失败重试不覆盖之前的有效点击，因此可以定位真正关联的截图与按钮。
- `source_context` 固定记录原局修订、地图代次、页面修订、楼层、事务和节点。
  延迟落盘保留原值；消费者不能拿落盘时刻的新状态覆盖原选券来源。
- 商店选择事件增加 `purchase_id` 和 `selection_verification`，使用最后核验帧作为证据。
  回执 `node.store.purchase` 增加 `planned_item_name`、`actual_item_name`、`expected_price`、
  `wallet_page_verified`、`item_consumption_verified`、`purchase_status`，并保存反馈截图。
  `purchase_status` 为 `confirmed / failed / mismatch / unknown`。目前仅稳定商店页面上的准确扣款，
  加上购买前明确可购买、购买后同位置同名卡片的“售罄”反馈，共同支持 `confirmed`。
  证据不足时 `actual_item_name`、兼容字段 `item_name` 为空，`purchase_succeeded=false` 不等价于已证明购买失败；
  以 `purchase_status` 和 `verification` 为准。错误页面不读取钱包，也不补造 `ingots_after`。

独立验证器没有封闭的业务 action/capability 白名单，已有实现接受这些增量字段，继续检查序列、签名、
文件哈希和图片引用；无需修改验证器或固定的 contract bundle。
生产者与 analysis 使用相同的合成契约样本 `recruitment-store-events.json`，验证新旧回执共存。
这些合成样本用于字段与时序验证，不冒充现场选券截图。

analysis 的适配器更新为 `blackflow-events-1.1.2`、任务规则为 `task-families-3`：保留新字段，
按编号与完整来源配对；旧回执保留原声明并标为 `legacy_unverified`，不升级为实际商品身份证明。
新选券图使用完整选择区域补读券名，不能从干员职业推断完整券型。宿主插件协议与封包限额不变，
PtilopsisBot 和 quark-timed-sync 无需改动通用核心。

适配器/处理方法摘要变化会产生新事实修订和语义结果；未改变的底层 OCR 裁剪可复用缓存，
新增选券区域需要自身的识别结果。既有快照与人工审核仍绑定原输入，不自动迁移到新修订。
本次不重跑全量 OCR，不改历史归档；缺少实际选券图的 217 条历史记录仍不可恢复。
新的可配对券数与确认购买数可能变化，不能将重试截图增加的数量当成新增券数，
也不能把未知购买当成失败或阴性。严格子集、三星过滤、实托邦及小八界专题分母保持原口径。

本地验证覆盖生产方法的模拟控制器回放和 OpenCV 帧比较、核心编译、C++ 回归、独立验证器与 analysis。
控制器/OCR/持久化边界在回放中注入，尚无新版实机完整对局证据；源码构建通过不代表已发布或实例已升级。

v1.1.6 发布前，analysis 0.3.4 已推送至 `9607ccba859d052c6c830c72f465c80908486cd9`，
PtilopsisBot 的安装依赖由 `f8c623be19bb6568a8070e1578b45486475b95dc` 固定到该提交。
新增能力、旧日志与 0.3.0–0.3.3 插件配置兼容测试通过。此次不操作收集电脑的运行环境，
中心按既有依赖安装与重启流程升级。验证器和固定 bundle 不变。
本地结果：349 个 C++ 用例（5779 个断言）、24 个控制器回放、64 个下游用例、
14 个签名归档/契约用例通过；更新发布测试共 33 个，25 个通过、8 个环境条件测试跳过。
