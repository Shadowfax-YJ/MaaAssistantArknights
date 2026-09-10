# 采集版自动更新

Windows 使用现有的独立更新程序安装完整包；macOS 使用 Sparkle。默认优先从国内 CDN 检查和下载，网络失败后尝试本仓库的 `blackflow-updates` Release，不读取普通 MAA 的更新渠道。资源随完整包发布，旧的资源缓存不参与采集版资源加载。

## 用户升级

- Windows 下载完更新包后，按提示重启安装；等待当前任务空闲后重启。可以在更新设置里选择自动安装。
- Windows 保留 `config`、`debug`、`data`、`reports`、`cache` 和 `achievement`。新包只携带 `resource/blackflow-gui-defaults.json`，仅首次启动且没有配置时才使用默认配置。
- macOS 保持应用标识和用户数据目录，替换应用时保留已有设置和采集数据；运行中的任务会推迟更新重启。
- v1.1.1 及更早的采集版没有这个渠道，必须先手动升级一次。Windows 可以用新版实例管理器“从包更新选中”，保持原实例路径。macOS 将新应用替换到原来的 Applications 位置。
- 实例管理器“更新采集版选中”下载一次完整包，校验 SHA256 后更新选中的已停止实例。每个实例保留自己的 ADB 地址、任务配置和日志。运行中的实例会跳过。
- Windows、macOS 和实例管理器均提供“自动（国内 CDN 优先）／国内 CDN／GitHub”选择。手动选择一个源时不会切换；自动模式中 Windows 的清单/下载/校验失败会尝试备用源，macOS 在清单或下载网络错误结束后发起一次 GitHub 后台重试，不重试取消、签名或安装错误。

## COS 和 CDN

配置在 `cdn.json`：`https://img.lubiao.wiki/maa/blackflow`，存储桶 `lubiao-wiki-1450633361`，地域 `ap-shanghai`。客户端内置同一地址；更换域名时需同步客户端并先发布过渡版本。

- 清单：`/maa/blackflow/latest.json` 和 `/maa/blackflow/appcast.xml`。
- 版本包及校验表：`/maa/blackflow/vX.Y.Z/`。同版本文件不可覆盖成不同内容。
- GitHub 清单指向 GitHub 包，CDN 清单指向 CDN 包；两边使用完全相同的安装包、SHA256 和 macOS 签名。

在[仓库 Actions secrets](https://github.com/Shadowfax-YJ/MaaAssistantArknights/settings/secrets/actions)添加 `BLACKFLOW_COS_SECRET_ID` 和 `BLACKFLOW_COS_SECRET_KEY`。只配置在发布端，不写入仓库和客户端。临时凭据还需要 `BLACKFLOW_COS_SESSION_TOKEN`，并应在实际发布时仍然有效。

配置好后可运行 `Release Pipeline`，选择 `build_scope=blackflow-cdn-check`。此检查只在 `maa/blackflow/checks/cos-cdn-v1.bin` 写入固定的 2 MiB 测试文件，验证分块上传、CDN 刷新和公开下载后的完整 SHA256；不构建应用、不创建 Release、不更改版本清单。测试文件保留供后续连通检查复用。

腾讯云检查和发布任务只在 `Shadowfax-YJ/MaaAssistantArknights` 的 `feat/blackflow-automatic-collection` 分支手动触发时运行，不在 PR 构建中注入凭据。公开仓库不会公开 Secrets 的值，fork 也不会复制这些值；但拥有写权限的人可以修改工作流来使用或外传仓库级 Secrets，因此应只给可信任的维护者写权限。日志脱敏不能阻止恶意代码外传密钥。

上传身份需要两份自定义 CAM 策略。本目录提供的 JSON 已填好当前桶、地域和更新路径，可在 CAM 的策略页面通过策略语法创建，再关联到 Actions 密钥所属的子用户。已有业务策略继续保留，以下策略作为额外授权；同一子用户的权限会累加。

- `cos-publish-policy.json`：只增加 `maa/blackflow/*` 下的读取、查看元数据、上传及分块上传权限。脚本直接初始化分块任务，按 SDK 默认的 1 MiB 大小读取并上传，每块校验 MD5，完成后仍校验对象元数据和公开下载的 SHA256；失败时尝试终止本次分块任务。重试会创建新任务，无需 `ListMultipartUploads` 或 `ListParts` 查询权限。此策略不授予删除对象、修改 ACL 或其他目录的对象读写权限。参见 [COS API 授权策略](https://intl.cloud.tencent.com/zh/document/product/436/30580)。
- `cdn-purge-policy.json`：只增加 `cdn:PurgeUrlsCache` 操作。腾讯云当前将此接口列为操作级，要求 `resource: "*"`，不能通过资源字段限制到 `img.lubiao.wiki` 或更新目录。这会授予账号范围内的 CDN URL 刷新权限，不包含域名配置修改或源文件写入权限。发布脚本实际只刷新配置中的更新 URL，但这是脚本行为，并非 CAM 的权限隔离。参见[腾讯云 CDN 接口授权粒度](https://cloud.tencent.com/document/product/598/98110)。

当前发布和连通检查流程均需要这两份授权，缺少 CDN 刷新权限也会失败。仅授予 `lubiao-wiki/sources/*` 或 `lubiao-wiki/incoming/shadowfax/*` 不覆盖 `maa/blackflow/*`；这种情况下，检查会在读取 `/maa/blackflow/latest.json` 时返回 `AccessDenied`。关联策略后可以沿用已有的子用户密钥，无需重新生成；尚需重新执行 `blackflow-cdn-check` 验证实际权限及 CDN 回源下载。

如果使用私有桶，在 CDN 中为更新目录配置 COS 服务授权及私有桶回源鉴权。客户端从 CDN 下载时不携带 COS 密钥、Referer 或临时下载令牌，因此更新目录需允许普通 HTTPS 客户端下载。配置仅针对更新目录；原有图片目录策略按原用途保留。参见[腾讯云源站配置](https://cloud.tencent.com/document/product/228/41334)。

缓存规则仅覆盖更新目录：清单缓存 60 秒，带版本路径的包长期缓存。脚本上传时设置对应 Cache-Control；CDN 的显式规则不能把清单强制缓存更长时间。发版后通过 `PurgeUrlsCache` 刷新对应 URL，再校验公开下载的实际内容。参见[腾讯云缓存配置](https://cloud.tencent.com/document/product/228/44388)。

## 发布

1. 在 `.github/workflows/ci.yml` 增加 `BLACKFLOW_DATA_COLLECTION_VERSION`，同步编写本目录 `RELEASE_NOTES.md`。已发布版本不可重新打包覆盖。
2. 运行 `Release Pipeline`，选择 `build_scope=blackflow-packages`。默认只构建和验证两个平台。
3. 正式发布时勾选 `publish_blackflow`。必须使用正式发布代码的提交。工作流将验证 Windows 包身份、两个平台版本、DMG 构建证明、哈希以及签名公钥。
4. 校验 COS 凭据及已有渠道后，版本包先发布到 `blackflow-vX.Y.Z` Release，随后更新 `blackflow-updates/latest.json` 和 `appcast.xml`。再同步版本包到 COS，经 CDN 公开下载及完整 SHA256 校验成功后更新国内清单。不更改普通版的 Latest Release。发布串行运行且拒绝渠道降级。同一提交、同一字节的包可以重试清单上传；重新构建出不同内容时必须增加版本。COS/CDN 失败会使发布任务失败，已成功发布的 GitHub 版本仍可作为备用源。

发布需要仓库 Secret `BLACKFLOW_SPARKLE_PRIVATE_KEY`：base64 编码的 **32 字节原始 Ed25519 seed**。对应的 32 字节公钥在 `sparkle-public-key.txt`，由构建步骤写入 macOS 应用。私钥须保存在仓库外并备份；丢失会影响已安装版本继续信任后续更新。发布脚本不输出私钥，也不把它放入制品。

此密钥用于 Sparkle 更新签名，应用目前仍沿用 ad-hoc 签名，没有 Apple Developer ID 公证。

## 验证

```powershell
dotnet run --project unit_test/MaaUpdate/MaaUpdate.Tests.csproj -c Release
python -m unittest discover -s tools/BlackFlowDataCollection -p 'test_*.py' -v
./tools/BuildBlackFlowWindowsPackage.ps1 -OutputDirectory build/blackflow-packages
```

Python 签名测试需要 `cryptography>=43,<47`。实际上传还需要 `cos-python-sdk-v5>=1.9,<2` 和 `tencentcloud-sdk-python-cdn>=3,<4`。macOS 的实际编译、版本检查、资源加载、签名验证以及构建证明生成在上述 CI 中执行。

`updates.py` 的 `generate` 子命令只生成两套清单和签名；只有显式传入 `--publish --commit <40位提交>` 才写入 GitHub Release 和 COS/CDN。更新清单首发前，客户端会报告检查失败，不会回退到普通版渠道。
