黑流树海采集版 v1.1.3

### 修复

* 修复 Windows v1.1.2 首次启动时，将安装包自带的 .NET/WPF 运行库误判为未知 DLL、导致无法打开的问题。继续检查并拒绝未知 DLL。
* 从探索中的地图启动采集时，先退出并放弃已有探索，再重新开局，避免接管半途对局生成不完整记录。

### 日志校验与签名

* 记录期间维护逐文件 SHA-512 摘要；归档前检查文件增删改、连续事件序号、开局与结束记录和截图引用。
* 压缩后再次校验实际内容，并为整个 ZIP 添加本地 Ed25519 签名。校验失败时保留原日志目录，原因记录在 `asst.log`。
* 请直接上传程序自动生成的原始 ZIP。修改内容、重新压缩或复制旧签名到新包，均不能通过原包校验。无需单独上传签名文件。
* 每局使用独立的内存密钥；本地签名不证明官方客户端来源，控制电脑或修改开源程序的人仍可伪造数据和签名。
* Windows 包和 macOS DMG 均附带 `tools/VerifyBlackFlowRunArchive.py`。维护者安装 Python 和 `cryptography` 后，可运行 `python tools/VerifyBlackFlowRunArchive.py "run-example.zip" --json` 验包。

### 下载

* [Windows x64 国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.3/MAA-BlackFlow-Data-Collection-v1.1.3-win-x64.zip)
* [macOS 通用包国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.3/MAA-BlackFlow-Data-Collection-v1.1.3-macos-universal.dmg)
* GitHub 下载见本 Release 附件；国内源与 GitHub 包的内容及 SHA256 相同。

### 升级

* 能正常启动的 v1.1.2 可使用采集版内置更新，默认优先国内 CDN，并支持 GitHub 备用源。
* Windows v1.1.2 若因未知 DLL 提示而无法启动，请先完全退出 MAA，将新版完整 ZIP 解压覆盖到原采集版目录。保留 `config`、`debug`、`data` 等用户数据目录，无需重新配置实例。v1.1.1 及更早版本同样手动覆盖升级。
* macOS：退出原程序，打开 DMG，将 MAA 替换到原来的 Applications 位置，保留原应用的用户数据目录。

macOS 应用沿用 ad-hoc 签名，尚未进行 Apple Developer ID 公证。Sparkle 更新签名用于验证后续更新包。
