## v1.1.13

### Highlights

#### 树洞探索与地图记录

完善树洞探索和返回外层地图的衔接，修正树洞模板连线与日志归属，探索笔记保留已探明的事件和关卡名称。

#### 路线规划与事件处理

改进随机落点、流窜居民与加工品的路线评估，完善派遣和祭献流程，并修复零件箱恢复首屏时的卡顿。

#### 独立自动更新与国内下载

Windows 和 macOS 支持采集版独立自动更新，保留已有设置与采集数据；默认使用国内 CDN，支持自动切换到 GitHub 或手动选择更新源。

<details>
<summary><b>English</b></summary>

#### Tree-hole exploration and map records

Improve tree-hole exploration and return routing, correct template corridors and evidence attribution, and preserve revealed event and stage names in exploration notes.

#### Route planning and event handling

Improve route evaluation for random landings, roaming residents and movement items, refine expedition and sacrifice handling, and recover the inventory panel when scrolling cannot settle.

#### Dedicated automatic updates and CDN downloads

Windows and macOS now support a dedicated collection update channel that preserves settings and collected data, with a China CDN by default, automatic GitHub fallback and manual source selection.

</details>

----

以下是详细内容：

<details open>
<summary><b>v1.1.13 (2026-09-23)</b></summary>

### 修复 | Fix

* 修复设置的难度与游戏实际难度不一致时仍继续探索的问题。每局开局前核对实际难度，无法确认时有限重试并停止，保留现场截图。 @Shadowfax-YJ

### 改进 | Improved

* 采集日志分别记录设定难度与实际识别结果，便于核对对局数据。 @Shadowfax-YJ

### 已知问题 | Known issues

* 收起零件箱后仍可能长时间等待，本版尚未修复。遇到时请停止任务并保留该局日志。

</details>

<details>
<summary><b>v1.1.12 (2026-09-23)</b></summary>

### 修复 | Fix

* 修复开局核心干员招募失败后仍继续探索、导致后续战斗无法准备的问题。无法按所选来源招募核心时停止任务，请检查助战及好友限制设置。 @Shadowfax-YJ
* 修复诡意行商刷新弹窗未打开时卡住的问题；刷新失败后有限重试，确认刷新成功后才继续采集商品。 @Shadowfax-YJ
* 修复离开树洞失败后持续反复重试的问题；连续三次恢复失败后停止任务，并保存现场截图。 @Shadowfax-YJ

### 改进 | Improved

* 开局奖励现在记录全部识别到的候选和对应截图，并单独记录最终确认选择的奖励。 @Shadowfax-YJ

### 已知问题 | Known issues

* 收起零件箱后仍可能长时间等待，本版尚未修复。遇到时请停止任务并保留该局日志。

</details>

<details>
<summary><b>v1.1.11 (2026-09-16)</b></summary>

### 修复 | Fix

* 修复领奖后已进入招募界面，仍错误等待并停止任务的问题。 @Shadowfax-YJ
* 修复三层追猎结束后进入四层无法继续探索，以及恢复时反复缩放地图的问题。 @Shadowfax-YJ
* 修复切换移动方式后，面板关闭稍有延迟就被误判失败并放弃探索的问题。 @Shadowfax-YJ

### 已知问题 | Known issues

* 收起零件箱后仍可能长时间等待，本版尚未修复。遇到时请停止任务并保留该局日志。

</details>

<details>
<summary><b>v1.1.10 (2026-09-16)</b></summary>

### 修复 | Fix

* 修复树洞内已成功切换徒步，却误判切换失败并放弃探索的问题。 @Shadowfax-YJ
* 修复奖励界面无响应时持续反复点击的问题。连续领取无进展后会等待一分钟重试；仍未恢复时停止任务并保留当前探索。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.9 (2026-09-14)</b></summary>

### 修复 | Fix

* 修复树洞内直接耗尽行动力后遗漏离开确认、反复等待的问题，保留返回外层地图的恢复能力。 @Shadowfax-YJ
* 修复探索笔记中已确认的节点身份被后续规则预测覆盖的问题。 @Shadowfax-YJ
* 修复流窜居民战斗结算后原节点关卡名丢失的问题，分别保留原关卡与实际居民战斗。 @Shadowfax-YJ
* 修复一层理想源中心战斗结束后紧急作战推断依据丢失的问题；仍明确区分规则推断与实际探明。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.8 (2026-09-14)</b></summary>

### 修复 | Fix

* 修复离开树洞后楼层标题受遮挡时反复等待、无法继续探索的问题，并保留返回途中奖励弹窗的正确楼层归属。 @Shadowfax-YJ
* 修复把地图文字误认成事件、污染实际落点与探索笔记的问题；“线人”“黑诞”等短事件名不再接受单字模糊匹配。 @Shadowfax-YJ
* 修复单挑部署卡在朝向选择，以及追猎、事件转战斗等场景的延迟超载弹窗导致任务停滞的问题。 @Shadowfax-YJ
* 修复未揭示的战斗节点被记为普通作战的问题；证据不足时保留未知类型和已识别的关卡名。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.7 (2026-09-12)</b></summary>

### 修复 | Fix

* 修复离开树洞并继续探索后，重复缩放地图导致楼层标题识别失败、任务停止的问题。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.6 (2026-09-12)</b></summary>

### 修复 | Fix

* 修复商店沿用前一轮切页重试次数、可能未刷新就离开的问题。 @Shadowfax-YJ
* 修复实际选券页漏采的问题，保存点击前画面及所选按钮，并关联后续招募界面，重试不重复计券。 @Shadowfax-YJ
* 加强商品购买前的画面、名称和价格核验，货架移动或目标无法确认时重新扫描，避免使用失效坐标。 @Shadowfax-YJ

### 改进 | Improved

* 完善购买回执，区分已确认购买、失败、证据不足及金额不一致；只在确认的商店页面读取钱包。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.5 (2026-09-12)</b></summary>

### 修复 | Fix

* 修复连续探索时沿用上一局树洞效果、导致地图重建失败的问题。 @Shadowfax-YJ
* 修正“未亡者遗怨”标题识别，避免选择错误的树洞地图模板。 @Shadowfax-YJ
* 修复离开树洞后可能反复识别楼层、无法继续探索的问题，完善返回外层地图和中断重试。 @Shadowfax-YJ

### 改进 | Improved

* 完善采集日志的对局标识，便于区分、核对和整理不同对局的数据。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.4 (2026-09-11)</b></summary>

### 修复 | Fix

* 修复刷等级策略在开局奖励和放弃招募确认时卡住的问题。 @Shadowfax-YJ

### 改进 | Improved

* 刷等级快速飞三层策略到达第三层后检查实托邦并保存截图，再按原流程重开。 @Shadowfax-YJ
* 每局采集日志记录所选难度，便于整理和核对数据。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.3 (2026-09-11)</b></summary>

### 修复 | Fix

* 修复 Windows v1.1.2 提示未知 DLL、导致无法启动的问题。 @Shadowfax-YJ
* 从探索中的地图启动采集时，先放弃已有探索再重新开局，避免生成半途开始的不完整记录。 @Shadowfax-YJ

### 改进 | Improved

* 自动检查并打包完整对局日志，支持校验原始 ZIP；提交数据时无需再手工压缩。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.2 (2026-09-11)</b></summary>

### 新增 | New

* Windows 新增采集版独立自动更新，支持检查、下载和重启安装，保留已有连接设置、任务配置与采集数据。 @Shadowfax-YJ
* 新增国内 CDN 与 GitHub 更新源选择，自动模式优先使用国内 CDN，失败时尝试 GitHub，并校验安装包版本、渠道身份与 SHA256。 @Shadowfax-YJ

### 改进 | Improved

* 采集版资源随完整程序包更新，避免普通版资源和旧资源缓存影响采集流程。 @Shadowfax-YJ

### 修复 | Fix

* 使用「失与得」标题区域的模板识别物品选择页，覆盖选择前后的状态，并完善流程恢复，修复停留在该界面无法继续的问题。 @Shadowfax-YJ
* 调整 Windows 自带 .NET 运行时的打包方式，避免依赖特定补丁版本的目录布局。 @Shadowfax-YJ

### MaaMacGui

#### 新增 | New

* 新增采集版 Sparkle 自动更新与签名校验，支持国内 CDN 和 GitHub 更新源，等待任务空闲后安装并保留已有设置与采集数据。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.1 (2026-09-10)</b></summary>

### 修复 | Fix

* 修复医疗、近卫招募页出现阿米娅时，职业校验无法通过并提前结束任务的问题。 @Shadowfax-YJ
* 修复地图上的「失与得」节点被误认成祭献选择页的问题，并限制连续选择重试次数，避免长时间循环。 @Shadowfax-YJ
* 每次移动前核对实际装载方式，修复游戏切回徒步后仍沿用加工品缓存的问题。 @Shadowfax-YJ
* 理想域中心须经两个识别分支一致确认后缓存，避免不确定结果持续影响遮蔽范围与节点身份。 @Shadowfax-YJ
* 关闭零件箱后等待界面变化，减少关闭动画尚未结束时的重复点击，并保存关闭失败的现场截图。 @Shadowfax-YJ

### 改进 | Improved

* 减少全图识别产生的无效警告与重复职业识别警告，保留识别失败区域便于排查。 @Shadowfax-YJ

</details>

<details>
<summary><b>v1.1.0 (2026-09-10)</b></summary>

### 新增 | New

* 完善树洞内的探索规划与返回外层地图后的路线衔接。 @Shadowfax-YJ
* 完善先行一步派遣和祭献物品选择、确认及连续处理流程。 @Shadowfax-YJ
* 新增「强买强卖」作战配置。 @Shadowfax-YJ

### 改进 | Improved

* 调整探索与发育收益计分、随机落点期望比较和加工品使用顺序，完善流窜居民的后续安全判断。 @Shadowfax-YJ
* 改进节点标记、事件选项与招募干员识别，保留移动预览揭示的具体节点名称。 @Shadowfax-YJ
* 统一节点归属信息与追加说明的保存方式，提供历史 attribution.txt 迁移工具。 @Shadowfax-YJ

### 修复 | Fix

* 修正源石之城下排多余连线，以及换心联结与田字形树洞的拓扑区分；为全部 9 种树洞建立完整连线回归检查。 @Shadowfax-YJ
* 树洞日志目录使用所属外层编号，修复小八界随机落点进入跨层事件后，截图被归入下一层且探索笔记漏记的问题。 @Shadowfax-YJ
* 修复「无效验尸」等没有自动作战配置的关卡在情报探查时反复等待、关卡名未写入探索笔记的问题。 @Shadowfax-YJ
* 修复零件箱滚动无法稳定时的首屏恢复，避免恢复过程中重复开关面板。 @Shadowfax-YJ
* 修复先行一步成功派遣凯尔希·思衡托后精英化状态未更新的问题。 @Shadowfax-YJ
* 修正「赤金厄运」中古米的部署位置。 @Shadowfax-YJ

</details>
