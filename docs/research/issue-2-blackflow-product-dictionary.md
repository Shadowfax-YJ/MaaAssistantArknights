# 简中国服黑流商店标准商品名词典调查

> 对应票据：[整理简中国服黑流商店标准商品名词典](https://github.com/Shadowfax-YJ/MaaAssistantArknights/issues/2)  
> 调查日期：2026-07-20（Asia/Shanghai）  
> 范围：调查与决策建议，不实现功能，不修改热更新配置。

## 结论

可交付的标准商品名词典应以 PRTS 固定修订为其已枚举名称的拼写权威，并以固定版本的简中客户端数据补足目录外名称与完整性：从
[`roguelike_topic_table.json`](https://github.com/Kengxxiao/ArknightsGameData/blob/6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4/zh_CN/gamedata/excel/roguelike_topic_table.json)
的 `details.rogue_6.items` 中选取 `RELIC`、`ACTIVE_TOOL`、`SCRAP`、`RECRUIT_TICKET`、`UPGRADE_TICKET`，对 `.name` 做 NFC 后按码点精确去重；与 PRTS 重叠的 269 名必须逐码点一致，否则更新失败并交由人工审阅。固定在客户端提交
`6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4` 时，420 条记录得到 **365 个**标准商品名。

- PRTS 两个目录给出 269 个精确名称，是最终 365 名的严格子集，但单独使用不完整。
- 客户端相对 PRTS 增加 96 名：39 个 `-α/-β/-γ` 收藏品变体、47 种招募券、10 种进阶券。
- 仓库五份旧 `shopping.json` 的有效 `priority` 并集与最终集合相交 239 名；`PRTS ∪ 旧表` 仍漏四种“精英……限定招募券”，因此旧表只能交叉校验，不能补作权威来源。
- 六张现有商店总览快照共出现 39 个不同的非空商品名；其中五种基础招募券不在 PRTS 两目录内，但 39 名全部属于最终 365 名。
- 简中通用 Word OCR 字表覆盖最终集合所需的全部 **674 个不同字符**，包括 `α/β/γ`；当前无需改字表或模型。这个结论只证明可输出字符，不能替代实图准确率验收。

这 365 名是五种可售商品类型的保守超集，足以回答“是否覆盖十个非投资商品槽位”的**覆盖性**问题；现有来源没有单独列出“标准模式实际售卖池”，所以不能把它误称为精确售卖池。

## 权威来源与版本

### 1. 简中客户端数据：完整性补全与目录外名称

- 仓库：[Kengxxiao/ArknightsGameData](https://github.com/Kengxxiao/ArknightsGameData)
- 固定提交：[6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4](https://github.com/Kengxxiao/ArknightsGameData/commit/6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4)，提交时间 `2026-07-17T09:13:41Z`，消息 `[CN UPDATE] Client:2.7.51 Data:26-07-16-07-20-24_daf69e`
- 固定文件：[blob](https://github.com/Kengxxiao/ArknightsGameData/blob/6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4/zh_CN/gamedata/excel/roguelike_topic_table.json) / [raw](https://raw.githubusercontent.com/Kengxxiao/ArknightsGameData/6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4/zh_CN/gamedata/excel/roguelike_topic_table.json)
- 文件 SHA-256：`00fc51c87ed1bf759298fbdc8c356681c43e85240633440a1993cb61d080bd9b`

客户端记录按类型的清单规模：

| 类型 | 原始行 | 精确名称 | 说明 |
|---|---:|---:|---|
| `RELIC` | 272 | 272 | 含 39 个希腊字母后缀变体 |
| `ACTIVE_TOOL` | 6 | 6 | 六种支援道具 |
| `SCRAP` | 30 | 30 | 与“引擎配件目录”完全一致 |
| `RECRUIT_TICKET` | 94 | 47 | 每名有普通与 `_candle` 两个 ID |
| `UPGRADE_TICKET` | 18 | 10 | 八名另有 `_from_relic` ID |
| **合计** | **420** | **365** | 按名称精确去重 |

重复 ID 不是名称冲突：47 种招募券各重复一次，10 种进阶券中八种重复一次；同名记录可合并名称，但应保留全部来源 ID 便于审计。

凡 PRTS 两个固定目录已经枚举的名称，标准拼写以 PRTS 为准；当前客户端对应名称全部逐码点一致。客户端只为目录没有枚举的 96 名提供标准拼写，并为全部候选提供类型与来源 ID。未来若两者发生分歧，不得静默以客户端覆盖 PRTS，而应让版本更新校验失败。

### 2. PRTS：目录文本与分类交叉校验

| 目录 | 页面 / 固定修订 | 记录 | 修订时间（UTC） |
|---|---|---:|---|
| 拟造物质编目 | [页面](https://prts.wiki/w/%E6%B2%89%E6%B2%A6%E8%80%85%E7%9A%84%E9%BB%91%E6%B5%81%E6%A0%91%E6%B5%B7/%E6%8B%9F%E9%80%A0%E7%89%A9%E8%B4%A8%E7%BC%96%E7%9B%AE) / [rev 409595](https://prts.wiki/index.php?oldid=409595) | 239 | 2026-07-19T14:17:47Z |
| 引擎配件目录 | [页面](https://prts.wiki/w/%E6%B2%89%E6%B2%A6%E8%80%85%E7%9A%84%E9%BB%91%E6%B5%81%E6%A0%91%E6%B5%B7/%E5%BC%95%E6%93%8E%E9%85%8D%E4%BB%B6%E7%9B%AE%E5%BD%95) / [rev 409409](https://prts.wiki/index.php?oldid=409409) | 30 | 2026-07-18T23:43:49Z |

[固定修订内容 API](https://prts.wiki/api.php?action=query&prop=revisions&revids=409595%7C409409&rvprop=ids%7Ctimestamp%7Csha1%7Ccontent&rvslots=main&format=json&formatversion=2)
中，拟造物质的 239 个 ID 从 `001` 到 `239` 连续且唯一；引擎配件为自然物 12、加工品 12、概念体 6。两个页面内部无重名、彼此无交集，合计 269 名；所有名称均已是 NFC/NFKC。

名称清单按 LF 连接、无尾随 LF 的 SHA-256：拟造物质 `9405849bb25a2a8a7d450ca6836368df2f8306e483f62ea483c5d48dbbb1c307`，引擎配件 `dd60d6c0064de3acb91fb3b103b0758e6d00b3ea14a4e297596ff67ea310a95d`。

PRTS 的[诡意行商](https://prts.wiki/index.php?oldid=409328)修订把对话分为收藏品、战术道具、零件、招募券；[秘境行商](https://prts.wiki/index.php?oldid=409335)列出加工品、自然物、概念体。商人页面不枚举 SKU，也不声明十槽结构。“拟造物质编目”还标有 `{{编辑中}}`；因此 PRTS 可作为固定修订文本权威，但不是完整客户端商品表。

## 精确提取与归一化规则

1. 从 PRTS 两个固定修订提取 269 个名称，作为这些名称的标准拼写。
2. 读取固定客户端提交中 `details.rogue_6.items` 的值，只保留 `RELIC`、`ACTIVE_TOOL`、`SCRAP`、`RECRUIT_TICKET`、`UPGRADE_TICKET`。
3. 客户端名称直接取 `.name`，不从描述或其他表二次解析；拒绝空值。断言 PRTS 269 名都是客户端候选的逐码点精确子集，再补入客户端独有的 96 名。
4. 仅做 Unicode NFC。当前 365 名在 NFC/NFKC 下都不变化，但规范只承诺 NFC；不得用 NFKC、大小写折叠、标点删除、希腊/拉丁字母替换来模糊去重。
5. 用 Unicode 码点精确相等去重；同名多 ID 合并为一个标准商品名并保留来源 ID 列表。
6. 生成稳定文件时用 ordinal 顺序排序，以 UTF-8、LF 输出。365 名按 `.NET StringComparer.Ordinal` 排序、LF 连接且无尾随 LF 的 SHA-256 是 `8981c39f89d6e074f02c395cdf7707c3c26796357527b78b42aff970c7cfb9fa`。
7. 空槽 sidecar 值 `""` 是槽位状态，不是词典项。

用于查看原始候选的 `jq` 过滤器：

```sh
jq -r '.details.rogue_6.items | to_entries[] | .value
  | select(.type == "RELIC" or .type == "ACTIVE_TOOL" or .type == "SCRAP"
        or .type == "RECRUIT_TICKET" or .type == "UPGRADE_TICKET")
  | [.id, .type, .name] | @tsv' roguelike_topic_table.json
```

生成器还应断言：原始行 420、精确名 365、PRTS 固定修订的 269 名全部包含于结果、字符覆盖差集为空。上游版本变化时这些数字允许通过人工审阅后的版本更新而改变，不应静默容错。

PRTS 的可复现提取规则是读取固定修订 raw wikitext，只接受 `(?m)^\|名称=([^\r\n]*)$`。不得抓取渲染 HTML，也不得把效果、描述、交互标题（例如秘境行商页面的“种子”标题）当成第二个名称记录。

## PRTS 之外的 96 名

### 39 个收藏品变体

对下列 13 个基础名分别附加精确后缀 `-α`、`-β`、`-γ`，其中连字符是 U+002D，希腊小写字母分别是 U+03B1、U+03B2、U+03B3：

`橙味风暴、地形图、古旧乐谱残章、尖叫樱桃、教堂救济餐券、解约协议、精选兽肉罐头、咖啡平原咖啡糖、凉拌海草、林间夜话、女妖之吻、皮特水果什锦、神音海螺`。

### 47 种招募券

`5星随机直升临时招募券、6星随机直升临时招募券、堡垒协议招募券、辅助招募券、高级辅助招募券、高级近卫招募券、高级狙击招募券、高级人事调度函、高级人事资深招募券、高级术师招募券、高级特种招募券、高级先锋招募券、高级医疗招募券、高级重装招募券、高级资深干员招募券、后方协调招募券、后方协调资深招募券、近卫招募券、精锐辅助招募券、精锐近卫招募券、精锐狙击招募券、精锐术师招募券、精锐特种招募券、精锐先锋招募券、精锐医疗招募券、精锐重装招募券、精英堡垒限定招募券、精英堡垒招募券、精英破坏限定招募券、精英破坏招募券、精英突击限定招募券、精英突击招募券、精英远程限定招募券、精英远程招募券、狙击招募券、破坏协议招募券、前线统合招募券、前线统合资深招募券、人事调度函、术师招募券、特别人事调度函、特种招募券、突击协议招募券、先锋招募券、医疗招募券、远程协议招募券、重装招募券`。

### 10 种进阶券

`辅助进阶券、高级物资配给券、近卫进阶券、狙击进阶券、临时物资配给券、术师进阶券、特种进阶券、先锋进阶券、医疗进阶券、重装进阶券`。

## 标点、大小写和 Unicode

最终名称必须逐码点保留：

- PRTS 目录有 21 个名称使用 U+201C/U+201D 弯引号，九个使用 U+300A/U+300B 书名号；没有名称使用 ASCII 双引号。
- PRTS 有 32 个 U+002D 名称，客户端另有上述 39 个变体；不能用其他 dash 字符替换。
- `沙盘α`、`沙盘β` 与 39 个变体使用真正的希腊小写字母，不是拉丁 `a/b/y`、全角字符或相似字形。
- `Scout的狙击镜`、`Blaze的电锯` 保留 ASCII 大小写。

旧表暴露了标点折叠的风险：相对于客户端/PRTS，历史数据中可找到 `“古堡的子嗣”` 对 `古堡的子嗣`、`“荣耀绶带”` 对 `荣耀绶带`，以及错误闭引号的 `维多利亚“废铁“勋章` 对 `维多利亚“废铁”勋章`。这些只能作为旧数据差异，不能进入标准商品名别名规则；低置信 OCR 应保留原文并标记未匹配。

## 与仓库旧商品表的交集

仓库现有五份 [`shopping.json`](../../resource/roguelike/) 是购买优先级，不是新主题完整商品目录。[`RoguelikeShoppingConfig.cpp`](../../src/MaaCore/Config/Roguelike/RoguelikeShoppingConfig.cpp#L7) 只解析 `priority`，完全不加载 JSON 中的 `other`；购买逻辑再用 OCR 子串与优先级名称匹配（[`RoguelikeShoppingTaskPlugin.cpp`](../../src/MaaCore/Task/Roguelike/RoguelikeShoppingTaskPlugin.cpp#L112)）。

| 旧主题 | `priority` 行 | 文件内精确名 |
|---|---:|---:|
| JieGarden | 308 | 308 |
| Mizuki | 295 | 294 |
| Phantom | 217 | 216 |
| Sami | 296 | 295 |
| Sarkaz | 308 | 308 |
| **跨主题合计** | **1,424** | **654（并集）** |

- PRTS 269 与旧表并集的精确交集：147。
- 最终 365 与旧表并集的精确交集：239，分型为 `RELIC 180 / ACTIVE_TOOL 6 / SCRAP 0 / RECRUIT_TICKET 43 / UPGRADE_TICKET 10`。
- 旧表补到了 PRTS 外 96 名中的 92 名；`PRTS ∪ 旧表` 只覆盖 361/365，缺 `精英堡垒限定招募券、精英破坏限定招募券、精英突击限定招募券、精英远程限定招募券`。
- 单文件 `priority` 自身已有重复：Mizuki 的 `医者-妙手`、Phantom 的 `医者-妙手`、Sami 的 `全局作战文件-γ`。跨主题重复更多，必须精确去重。

因此，旧表适合做回归交集与差异报告，不适合作为生成输入或标准名优先级来源。

## 六张现有快照的覆盖检查

检查了被 `build/` 忽略的六张 1280×720 快照：

`build/bin/Release/debug/roguelike/black_flow_store/2026.07.20-03.56.35.67_raw.png` 至 `2026.07.20-04.21.29.628_raw.png`。

人工读取到 39 个不同非空商品名：34 个来自 PRTS 目录，目录外五个为 `辅助招募券、近卫招募券、术师招募券、先锋招募券、医疗招募券`。五者都在客户端 47 种招募券中，39/39 全被最终词典覆盖。样本证明“两目录单独不完整”，但样本数量不足以反向证明实际售卖池只有已见名称。

## OCR 字表检查

简中通用 Word OCR 使用 [`resource/PaddleOCR/rec/keys.txt`](../../resource/PaddleOCR/rec/keys.txt)；桌面加载代码把同目录的 `inference.onnx` 与 `keys.txt` 一起交给识别器（[`OcrPack.cpp`](../../src/MaaCore/Config/Miscellaneous/OcrPack.cpp#L66)）。

- 字表 6,901 行、6,828 个不同字符，SHA-256 `38055a5ea5937ac7ea96f114fb2deaccab8c32f89d94a05a31acbf1293f9f83c`。
- 最终 365 名含 674 个不同字符，Word OCR 缺失 **0**。
- 关键行：`-` 28、`”` 121、`S` 1217、`》` 1692、`B` 3588、`“` 4533、`《` 5209、`α` 5674、`β` 5976、`γ` 6797。
- [`PaddleCharOCR`](../../resource/PaddleCharOCR/rec/keys.txt) 对这 674 字缺 661 字，不能用于商品名。OCR 参数默认 `use_char_model = false`（[`OCRerConfig.h`](../../src/MaaCore/Vision/Config/OCRerConfig.h#L13)），且 [`OCRer.cpp`](../../src/MaaCore/Vision/OCRer.cpp#L22) 在 false 时选择 Word OCR；新的商品名任务应保持这一设置。
- [`ocr_config.json`](../../resource/ocr_config.json) 当前没有全局等价类，符合“不折叠标点/希腊字母”的要求。

字表全覆盖不表示模型在小字号、遮挡、缩放下必然识别正确；实现票据仍需用槽位 ROI 和快照做端到端 OCR 验收。不要为这项功能单独向 `keys.txt` 追加字符；字表顺序与识别模型输出类别绑定。

## 推荐决议与剩余不确定性

建议地图将本票据决议写为：

1. 标准商品名词典以 PRTS 固定修订、简中客户端数据提交、五种类型过滤器和最终集合 SHA-256 共同版本化；PRTS 决定已枚举名称的标准拼写，客户端数据只负责完整性补全与来源 ID。
2. 首版采用 365 名保守超集，不尝试从 PRTS 或旧购买优先级表推导精确标准模式售卖池。严格匹配输出标准名；不可靠结果保留 OCR 原文并标记 `unmatched`。
3. 每次更新客户端数据时产出按类型的新增/删除/改名差异，重新验证 PRTS 子集、四类 Unicode、样本覆盖和 Word OCR 字表差集；任何数量或指纹变化均需人工审阅。
4. 旧 `shopping.json` 只用于回归检查；禁止其错引号、附加引号和重复项覆盖客户端标准名。

仍未由本调查解决的只有“365 名中哪些 SKU 会在简中标准模式的特定商店页实际出现”。这不妨碍覆盖型 OCR 词典实现；若产品要求**精确售卖池**而非保守超集，需要另开调查，找到客户端的商店生成/权重表或用足量实机样本验证。

领域文档现已定义“标准商品名”“标准商品名词典”；本调查没有提出 ADR，仓库当前也没有 ADR 与该建议冲突。尚需在规格中明确“词典是覆盖超集而非精确售卖池”及客户端数据更新节奏。
