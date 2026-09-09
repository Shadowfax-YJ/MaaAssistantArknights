## v1.1.0

### Highlights

#### 树洞探索与地图记录

完善树洞探索和返回外层地图的衔接，修正树洞模板连线与日志归属，探索笔记保留已探明的事件和关卡名称。

#### 路线规划与事件处理

改进随机落点、流窜居民与加工品的路线评估，完善派遣和祭献流程，并修复零件箱恢复首屏时的卡顿。

<details>
<summary><b>English</b></summary>

#### Tree-hole exploration and map records

Improve tree-hole exploration and return routing, correct template corridors and evidence attribution, and preserve revealed event and stage names in exploration notes.

#### Route planning and event handling

Improve route evaluation for random landings, roaming residents and movement items, refine expedition and sacrifice handling, and recover the inventory panel when scrolling cannot settle.

</details>

----

以下是详细内容：

<details open>
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
