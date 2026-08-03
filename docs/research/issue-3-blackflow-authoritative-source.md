# `BlackFlowTemporary` 权威配置源与发布路径调查

- 调查票据：[确认 BlackFlowTemporary 的权威配置源与发布路径](https://github.com/Shadowfax-YJ/MaaAssistantArknights/issues/3)
- 调查日期：2026-07-20
- 范围：只调查现状、覆盖语义和可行迁移路径；不修改远端配置，不实现功能。

## 结论

1. **当前线上权威源不是本地 `build/bin/Release/cache`，而是公开但标注为内部使用的 [`MaaAssistantArknights/MaaRelease`](https://github.com/MaaAssistantArknights/MaaRelease) 仓库。**任务图位于 [`MaaAssistantArknights/api/resource/tasks.json`](https://github.com/MaaAssistantArknights/MaaRelease/blob/9f8f45da380dbd6946e69aec335a3d2852abbdd0/MaaAssistantArknights/api/resource/tasks.json#L1)，工具箱入口位于 [`MaaAssistantArknights/api/gui/StageActivityV2.json`](https://github.com/MaaAssistantArknights/MaaRelease/blob/9f8f45da380dbd6946e69aec335a3d2852abbdd0/MaaAssistantArknights/api/gui/StageActivityV2.json#L40-L54)。`BlackFlowTemporary` 最初分别由 [`35c80b3`](https://github.com/MaaAssistantArknights/MaaRelease/commit/35c80b324b5d02337b8a5e60a65470616e4d0a00)（任务图）和 [`7c6c973`](https://github.com/MaaAssistantArknights/MaaRelease/commit/7c6c97388aad5c27342e6d64a5ab3dd6c8e68fa1)（入口）提交；截至调查时，任务图最后修改为 [`9f8f45d`](https://github.com/MaaAssistantArknights/MaaRelease/commit/9f8f45da380dbd6946e69aec335a3d2852abbdd0)。这说明 `cache` 中的内容是已发布副本，不是失去历史的临时手工文件。
2. **发布链路是 `MaaRelease/main` → OTA 服务器工作区 → `api.maa.plus`/`api2.maa.plus` → 客户端 `cache`。**`MaaRelease` 的 [`deploy_api_server.yml`](https://github.com/MaaAssistantArknights/MaaRelease/blob/9f8f45da380dbd6946e69aec335a3d2852abbdd0/.github/workflows/deploy_api_server.yml#L3-L7) 在 `main` 每次 push 后通过 SSH 令服务器强制 fetch/checkout/reset 到 `origin/main`（[部署命令](https://github.com/MaaAssistantArknights/MaaRelease/blob/9f8f45da380dbd6946e69aec335a3d2852abbdd0/.github/workflows/deploy_api_server.yml#L50-L69)）。本次调查直接比对了两个公开 API 与同路径的 GitHub raw `main` 内容，均逐字节相等；响应的 `ETag` 与 `Last-Modified` 也与本地 `cache/etag.json`、`cache/last_modified.json` 记录一致。
3. **本地 cache 会覆盖包内同名任务字段。**WPF 从 `gui/StageActivityV2.json` 和 `resource/tasks.json` 下载后写入 `cache/<api path>`（[`StageManager.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Services/StageManager.cs#L198-L246)，[`MaaApiService.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Services/Web/MaaApiService.cs#L46-L88)）。加载时明确先读包内 `resource`，再读 `cache/resource`，并把旧 API 形态的 `cache/resource/tasks.json` 覆盖复制为 `cache/resource/tasks/tasks.json`（[`AsstProxy.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Main/AsstProxy.cs#L469-L548)）。MaaCore 对后加载的同名任务按字段赋值，后值覆盖前值（[`TaskData.cpp`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaCore/Config/TaskData.cpp#L70-L104)）。因此，把相同的 `BlackFlowTemporary@...` 定义仅复制进当前仓库并不能保证它不再受旧热更影响。
4. **若能维护 `MaaRelease`，应继续把它作为线上热更权威源，两个文件必须成对修改并经其 `main` 发布。**不要直接编辑 `build/` 或 `cache/`。但截至调查时，认证用户 `Shadowfax-YJ` 通过 GitHub GraphQL API 获得的 `viewerPermission` 为 `READ`，而 `MaaRelease` README 又明确说明仓库“仅供内部使用”（[`README.md`](https://github.com/MaaAssistantArknights/MaaRelease/blob/9f8f45da380dbd6946e69aec335a3d2852abbdd0/README.md#L1-L7)）；因此对本分支而言，直接维护现有权威源目前不是可依赖的交付路径。
5. **若无法取得 `MaaRelease` 写权限，建议迁入本仓库并更换稳定任务命名空间。**任务图放到受版本控制的 `resource/tasks/MiniGame/BlackFlow.json`（命名和拆分方式参照 [`resource/tasks/MiniGame/OS.json`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/resource/tasks/MiniGame/OS.json#L1)），入口使用新值，例如 `MiniGame@BlackFlow@Begin`。WPF 入口放入 `StageManager.InitializeDefaultMiniGameEntries()` 并在所有 `src/MaaWpfGui/Res/Localizations/*.xaml` 增加名称和提示；这里本来就是永久工具箱入口的版本控制位置（[`StageManager.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Services/StageManager.cs#L300-L311)）。若本分支同时交付 MAAUnified，则也要在其内建入口和本地化表中加入同一新值（[`ToolboxAssetCatalog.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MAAUnified/App/ViewModels/Toolbox/ToolboxAssetCatalog.cs#L23-L75)，[`LoadMiniGameEntries`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MAAUnified/App/ViewModels/Toolbox/ToolboxAssetCatalog.cs#L423-L461)）。新命名空间使任何遗留的 `BlackFlowTemporary@...` cache 无法覆盖新图；同时应从远端入口移除旧项，或在本地解析时抑制旧 `Value`，避免同一功能出现两个入口。迁名还必须同步当前截图插件硬编码的触发节点（[`BlackFlowStoreScreenshotTrigger.hpp`](../../src/MaaCore/Task/Miscellaneous/BlackFlowStoreScreenshotTrigger.hpp#L8-L10)）。

## 当前发布流的完整证据链

```text
MaaRelease/main
  MaaAssistantArknights/api/resource/tasks.json
  MaaAssistantArknights/api/gui/StageActivityV2.json
        │ push
        ▼
deploy_api_server.yml: OTA 服务器 checkout/reset 到 origin/main
        │ 静态 API 路径
        ▼
https://api.maa.plus/MaaAssistantArknights/api/{resource/tasks.json,gui/StageActivityV2.json}
        │ WPF: ETag 请求；成功响应写入 cache/<api path>
        ▼
cache/resource/tasks.json ──复制覆盖──> cache/resource/tasks/tasks.json
cache/gui/StageActivityV2.json ───────> 工具箱动态入口
        │
        └─ MaaCore 加载顺序：发行包 resource → cache/resource（后者覆盖同名字段）
```

客户端使用的 API 基址在 [`MaaUrls.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Constants/MaaUrls.cs#L43-L49) 中固定为 `api.maa.plus` 和 `api2.maa.plus`。`StageManager` 将两个相对路径定义为 `gui/StageActivityV2.json` 与 `resource/tasks.json`（[`StageManager.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Services/StageManager.cs#L41-L45)），下载新任务后请求 MaaCore 在空闲时重新加载资源（[同文件](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Services/StageManager.cs#L231-L244)）。工具箱入口则从 `StageActivityV2` 的 `miniGame` 数组解析并插入内建入口之前（[同文件](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Services/StageManager.cs#L492-L576)）。

旁证：`maa-cli` 独立实现也把同一个 API 作为默认热更源，并把 `resource/tasks.json` 下载到热更资源目录的 `tasks/tasks.json`（[`hot_update.rs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/maa-cli/crates/maa-cli/src/config/cli/hot_update.rs#L27-L80)，[`installer/hot_update.rs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/maa-cli/crates/maa-cli/src/installer/hot_update.rs#L8-L20)）。

## 可行的仓库内迁移与发布方案

### 推荐方案：稳定命名空间 + 包内资源 + 内建入口

1. 将任务图从已发布 `tasks.json` 拆入本仓库 `resource/tasks/MiniGame/BlackFlow.json`，同时把根任务从 `BlackFlowTemporary@Begin` 迁为稳定名字，如 `MiniGame@BlackFlow@Begin`；所有内部边同步改名。
2. 将入口加入 WPF 的 `InitializeDefaultMiniGameEntries()` 与各语言 XAML；若 MAAUnified 也在交付范围内，同步加入 `ToolboxAssetCatalog` 的内建入口及本地化字典。首版只支持简中国服时，其他语言可以给出明确的 fallback 文本，但入口值必须一致。
3. 从 `MaaRelease` 的 `StageActivityV2.json` 移除旧入口，并从其 `resource/tasks.json` 移除旧任务图；若没有权限，客户端在合并动态入口时按旧 `Value == "BlackFlowTemporary@Begin"` 抑制它。旧 cache 即使离线残留，也因任务命名空间不同而不能覆盖新图。
4. 通过本仓库的正常构建/发布链交付。CMake 在 `INSTALL_RESOURCE` 时把仓库 `resource` 安装进产物（[`CMakeLists.txt`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/CMakeLists.txt#L59-L65)）；WPF 项目在本地构建输出中也链接仓库 `resource`（[`MaaWpfGui.csproj`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/MaaWpfGui.csproj#L29-L35)），CI 随后压缩并上传整个 `install` 目录（[`ci.yml`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/.github/workflows/ci.yml#L207-L245)），因此任务图可随完整版本包交付。
5. **现有 `MaaResource` 链不能直接热发该任务图。**上游 [`sync-resource.yml`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/.github/workflows/sync-resource.yml#L38-L49) 虽会复制整个 `resource/**` 到 [`MaaResource`](https://github.com/MaaAssistantArknights/MaaResource)，但后者的 [`resource/.gitignore`](https://github.com/MaaAssistantArknights/MaaResource/blob/68df305857117f2a7e17fe9cbbd18e90fb97da90/resource/.gitignore#L1-L23) 采用白名单且没有放行 `tasks/`，所以 `git add .` 不会提交任务图。若需要由本仓库控制任务图热发布，必须另行把 `resource/tasks/**` 加入 MaaResource 白名单（或使用明确的强制添加/专用同步），再由 WPF 资源更新器下载归档并合并 `resource`（[`ResourceUpdater.cs`](https://github.com/MaaAssistantArknights/MaaAssistantArknights/blob/aa01d211ef059303ba72aec91ab9e14f3d795f88/src/MaaWpfGui/Models/ResourceUpdater.cs#L39-L89)）。工具箱入口仍不能仅靠当前 MaaResource 解决：要么继续作为客户端内建项随完整版本发布，要么新增一个受版本控制、随 MaaResource 发布且由前端读取的小游戏 manifest。

### 不推荐方案：同名双源

把同一组 `BlackFlowTemporary@...` 同时放进本仓库 `resource/tasks/**` 和 `MaaRelease/api/resource/tasks.json` 会形成两个可独立变化的权威源。由于 cache 后加载且逐字段覆盖，运行结果取决于客户端最后成功热更的版本；离线旧 cache 还可能覆盖更新后的包内图。除非明确规定 MaaRelease 仍是唯一权威源，否则不应保留这种布局。

## 在线比对记录

2026-07-20 在认证为 `Shadowfax-YJ` 的 `gh` 会话中取得以下一方结果：

- `MaaAssistantArknights/MaaRelease`：`viewerPermission = READ`；`Shadowfax-YJ/MaaAssistantArknights`：`viewerPermission = ADMIN`。
- `MaaRelease/main` HEAD：`9f8f45da380dbd6946e69aec335a3d2852abbdd0`。
- `https://api.maa.plus/MaaAssistantArknights/api/resource/tasks.json` 与 [`raw main`](https://raw.githubusercontent.com/MaaAssistantArknights/MaaRelease/main/MaaAssistantArknights/api/resource/tasks.json) 内容相等，UTF-8 内容 SHA-256 均为 `875183f9d5778b77e0e1a83b549e7ae33a8581b54bc9495fdd2de3d2f9d8c6c3`。
- `https://api.maa.plus/MaaAssistantArknights/api/gui/StageActivityV2.json` 与 [`raw main`](https://raw.githubusercontent.com/MaaAssistantArknights/MaaRelease/main/MaaAssistantArknights/api/gui/StageActivityV2.json) 内容相等，UTF-8 内容 SHA-256 均为 `4559bfe97e0b54ec8f7cb08630875731b1d2694b2fd241be1a58687f12054942`。
- API 响应分别给出 `ETag: "6a5d0368-32fe"` / `"6a5cb609-2f6a"`，与本地缓存元数据一致；公开响应可直接从 [`resource/tasks.json`](https://api.maa.plus/MaaAssistantArknights/api/resource/tasks.json) 和 [`gui/StageActivityV2.json`](https://api.maa.plus/MaaAssistantArknights/api/gui/StageActivityV2.json) 复核。

## 决策建议

对 [Wayfinder：黑流树海商店三页采集与商品名 OCR](https://github.com/Shadowfax-YJ/MaaAssistantArknights/issues/1) 的后续规格，建议把“配置归属”锁定为：**在无法取得 MaaRelease 写权限的当前条件下，以本仓库为权威源，采用新的稳定任务命名空间；任务图进入 `resource/tasks/MiniGame`，工具箱入口进入前端内建列表与本地化资源；旧动态入口被退役或抑制；首个可行发布载体是完整版本包。**这同时满足版本控制、可评审、随包发布和不受遗留热更覆盖四个目标。若规划要求任务图可独立热发，则需要新增 MaaResource 白名单/同步工作及入口 manifest 工作，不能把它视为现有能力。
