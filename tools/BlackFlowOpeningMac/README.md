# macOS 开拓者专项包

基于 `0aa4624c2a9742617270832f0c467fa16a856df7` 加已验证的 opening recovery-v4 修复，
GUI 固定 `2bc3b7b17a50dad7ddd1d55cc98841ea1b34d41f`。构建入口是本分支的 `ci.yml`。
分队卡片拖动锚点、页面恢复及网络断线诊断与 Windows v4 相同，专用 SwiftUI 入口调用
`Custom` / `MiniGame@BlackFlow@Begin`。包名和配置域独立，不推进普通采集版更新渠道。

`prepare.py` 在构建目录生成专用界面，使用固定随包资源，移除应用更新入口，
将日志、图片及任务数据根目录设为 `~/Library/Application Support/MAAPioneer`。
UI 具备连接设置、开始、停止、日志和采集目录入口。连接仍使用原生 MAA 的 PlayCover / ADB 实现。

## 数据契约

检查依据：主仓库 `docs/blackflow/data-contract.md`、analysis 的
`docs/data-pipeline/coordination.md`，以及 Windows recovery-v4 的 build-info。
本包不携带主工作区中尚未发布的自动采集改动。

- `opening_cycles/<cycle-id>` 下的 processing-item 与 parts-box 文件名、全帧布局、
  稳定等待和采集范围保持不变；macOS 只更改应用外层数据根目录。
- `opening_failures` 与采集周期目录分开。缺图保留，按周期配对，不把失败图计入零件分母。
- 这是一组独立本机专项图片，不是自动采集 archive，不新增签名包或 schema。
  旧数据继续可读；未知 archive schema 的既有拒绝规则不变。
- 不改独立验证器、标准化、业务插件、缓存或人工审核，不重算历史数据；
  后续统计仍从各平台指定根目录读取同一相对结构。

## 验证及边界

CI 为 arm64 和 x86_64 分别编译 Core，运行开局采集契约测试，合成通用库和 GUI；
验证资源 SHA-256、双架构、实际 dylib 加载、AsstCreate/Destroy、开局任务注册，
再验证 ad-hoc 签名与 DMG 完整性。产物带版本、源码摘要、macos-verification.json 和 SHA256SUMS。
GUI 最低系统版本及实际依赖要求以包内 Info.plist / Mach-O 为准。

Windows v4 之前完成了 37 项恢复回放、7 项采集测试和三实例各五组实机截图。
本轮 macOS CI 没有连接游戏，不声称完成 Mac 实机对局。使用前读随包使用说明。
