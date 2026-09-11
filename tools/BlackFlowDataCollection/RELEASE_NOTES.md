黑流树海采集版 v1.1.6

### 本次更新

* 修复商店沿用前一轮切页重试次数、可能未刷新就离开的问题。
* 修复实际选券页漏采的问题，保存点击前画面及所选按钮，并关联后续招募界面，重试不重复计券。
* 加强商品购买前的画面、名称和价格核验，货架移动或目标无法确认时重新扫描，避免使用失效坐标。
* 完善购买回执，区分已确认购买、失败、证据不足及金额不一致；只在确认的商店页面读取钱包。

### 升级

* v1.1.2 及以上版本可在采集版内检查更新，下载后按提示重启安装，保留已有连接设置、任务配置和采集数据。
* Windows 旧版若无法启动，或使用 v1.1.1 及更早版本，请完全退出 MAA 后，将新版完整包解压覆盖到原采集版目录，无需重新配置。
* macOS 手动升级时，退出 MAA，将 DMG 中的应用替换到原来的 Applications 位置。首次打开若被系统拦截，请在访达中右键 MAA 并选择“打开”。

### 下载

* [Windows x64 国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.6/MAA-BlackFlow-Data-Collection-v1.1.6-win-x64.zip)
* [macOS 通用包国内下载](https://img.lubiao.wiki/maa/blackflow/v1.1.6/MAA-BlackFlow-Data-Collection-v1.1.6-macos-universal.dmg)
* GitHub 下载见本 Release 附件。

提交数据时，请直接上传 `debug/BlackFlow` 中程序自动生成的原始 ZIP，不要修改内容或重新压缩。
