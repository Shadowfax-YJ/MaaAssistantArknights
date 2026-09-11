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
