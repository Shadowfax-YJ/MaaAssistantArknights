黑流树海采集版 v1.1.5

### 本次更新

* 每局记录稳定 UUID、采集契约版本和已观察范围，便于中心去重及兼容不同采集版本。
* 安装包附带固定版本的数据验证契约与样本，统一客户端和分析端的校验规则。
* 验证工具分别标记旧版未签名数据、不支持的新版本及运行依赖缺失，便于保留原件并继续处理。

### 升级

* v1.1.2 及以上版本可在采集版内检查更新，下载后按提示重启安装，保留已有连接设置、任务配置和采集数据。
* Windows 旧版若无法启动，或使用 v1.1.1 及更早版本，请完全退出 MAA 后，将新版完整包解压覆盖到原采集版目录，无需重新配置。
* macOS 手动升级时，退出 MAA，将 DMG 中的应用替换到原来的 Applications 位置。首次打开若被系统拦截，请在访达中右键 MAA 并选择“打开”。

### 下载

* [Windows x64 国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.5/MAA-BlackFlow-Data-Collection-v1.1.5-win-x64.zip)
* [macOS 通用包国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.5/MAA-BlackFlow-Data-Collection-v1.1.5-macos-universal.dmg)
* GitHub 下载见本 Release 附件。

提交数据时，请直接上传 `debug/BlackFlow` 中程序自动生成的原始 ZIP，不要修改内容或重新压缩。
