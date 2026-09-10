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

上传身份需要该桶 `maa/blackflow/*` 下的 `cos:GetObject`、`cos:HeadObject`、`cos:PutObject`、`cos:InitiateMultipartUpload`、`cos:ListParts`、`cos:UploadPart`、`cos:CompleteMultipartUpload`、`cos:AbortMultipartUpload` 权限，桶的 `cos:ListMultipartUploads` 权限，以及 `img.lubiao.wiki` 的 `cdn:PurgeUrlsCache` 权限。脚本不修改桶 ACL，不删除现有文件。

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
