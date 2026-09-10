黑流树海采集版 v1.1.2

* Windows 和 macOS 新增独立自动更新渠道，保留连接设置、任务配置与采集数据。
* 默认使用国内 CDN，支持自动回退到 GitHub，也可以在更新设置中固定选择国内 CDN 或 GitHub。
* 更新包经过版本、渠道身份与 SHA256 校验，macOS 更新额外校验 Sparkle 签名；运行任务时会等待空闲后重启安装。
* 资源与程序一起更新，避免普通版资源和旧资源缓存影响采集流程。
* 使用「失与得」标题区域的模板识别选择页，修复该界面卡住的问题。
* 修复 Windows 自带 .NET 运行时对补丁版本目录布局的依赖。

### 下载

* [Windows x64 国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.2/MAA-BlackFlow-Data-Collection-v1.1.2-win-x64.zip)
* [macOS 通用包国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.2/MAA-BlackFlow-Data-Collection-v1.1.2-macos-universal.dmg)
* GitHub 下载见本 Release 附件；国内源与 GitHub 包的内容及 SHA256 相同。

### 从旧版本升级

v1.1.1 及更早版本需要手动升级这一次，升级到 v1.1.2 后即可使用内置检查更新。

* Windows：退出原程序，将新 ZIP 解压覆盖到原目录，保留原来的 `config`、`debug`、`data` 等用户数据目录。新包不携带用户配置，不需要重新配置实例。
* macOS：退出原程序，打开 DMG，将 MAA 替换到原来的 Applications 位置，保留原应用的用户数据目录。

macOS 应用沿用 ad-hoc 签名，尚未进行 Apple Developer ID 公证。Sparkle 更新签名用于验证后续更新包。
