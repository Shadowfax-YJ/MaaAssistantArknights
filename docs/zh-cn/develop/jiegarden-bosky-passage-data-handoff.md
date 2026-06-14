---
order: 9
icon: mdi:file-document-outline
---

# 界园树洞（是非境）勘探数据交接说明

本文面向后处理和数据分析同学，说明当前「树洞 / 是非境 / Bosky Passage」勘探产物的目录、JSONL 结构、字段语义和推荐整理方式。

代码侧主要入口：

- `src/MaaCore/Task/Roguelike/Map/RoguelikeBoskyPassageRoutingTaskPlugin.cpp`
- `src/MaaCore/Task/Roguelike/RoguelikeDataCollection.cpp`
- `src/MaaCore/Task/Roguelike/RoguelikeStageEncounterTaskPlugin.cpp`
- `src/MaaCore/Task/Roguelike/RoguelikeShoppingTaskPlugin.cpp`
- `resource/tasks/Roguelike/routing.json`

## 产物位置

每次数据收集会创建一个 session 目录：

```text
<MAA 实例目录>/debug/roguelike/data_collection/<yyyy.MM.dd-HH.mm.ss.xxx>/
```

树洞相关的核心产物：

```text
data_collection/<session>/
  bosky_passage.jsonl
  bosky_passage/
    <timestamp>_map_raw.png
    <timestamp>_map_overlay.png
    <timestamp>_Legend_options.png
    <timestamp>_Legend_relieving_options.png
    <timestamp>_Disaster_detail.png
    <timestamp>_Omissions_detail.png
    <timestamp>_Scheme_options.png
    <timestamp>_Playtime_options.png
    <timestamp>_OldShop_options.png
    <timestamp>_YiTrader.png
```

相关旁路产物：

- `events.jsonl`：通用事件流水，也会记录 `bosky_passage_map`、`bosky_passage_node`、`bosky_auto_exit_abandon` 等事件。
- `agents.jsonl`：岁兽代理人选项记录，包含由树洞节点触发的代理人来源。
- `traders.jsonl`：行商记录；在是非境内的易与节点也会以 `YiTrader` 类型写入。
- `images/`：会链接或复制部分图片，便于统一浏览；树洞分析建议以 `bosky_passage/` 下相对路径为准。

## bosky_passage.jsonl 的记录单位

`bosky_passage.jsonl` 是权威的树洞勘探摘要文件。

它是 JSONL，但每一行不是单个对象，而是一个 JSON 数组。通常一行对应一次进入并勘探是非境的完整过程。

数组内通常包含：

1. 一个 `record_type == "map"` 的地图记录。
2. 若干个节点勘探记录，按 `route_index` 表示本次是非境内实际路由顺序。

示意：

```json
[
  {
    "record_type": "map",
    "floor": "是非境",
    "width": 7,
    "height": 5,
    "node_count": 28,
    "edge_count": 32,
    "entry_map_image": "bosky_passage\\2026.06.10-01.22.23.854_map_raw.png",
    "entry_map_overlay": "bosky_passage\\2026.06.10-01.22.23.879_map_overlay.png",
    "nodes": [],
    "edges": []
  },
  {
    "route_index": 1,
    "node_type": "Legend",
    "node_name": "传说",
    "map_node_index": 5,
    "grid": [5, 0],
    "image": "bosky_passage\\2026.06.10-01.22.30.702_Legend_options.png",
    "event_name": "生机",
    "events": []
  }
]
```

注意：

- 地图记录会在勘探过程中被更新，最终写出的 `nodes[*].visited` 更接近本次勘探结束前的状态，不是严格的初始状态。
- `entry_map_image` 和 `entry_map_overlay` 是进入是非境时保存的原始地图和 overlay 图。
- 图片路径均为相对 session 目录的路径，后处理时应拼接到 `data_collection/<session>/`。

## 地图记录字段

地图记录由 `record_bosky_map_snapshot()` 写入。

核心字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `record_type` | string | 固定为 `"map"`。 |
| `floor` | string | 当前为 `"是非境"`。 |
| `width` | number | 树洞网格宽度，当前为 `7`。 |
| `height` | number | 树洞网格高度，当前为 `5`。 |
| `center_index` | number | 中心节点索引。 |
| `center_grid` | `[x, y]` | 中心节点网格坐标。 |
| `node_count` | number | 当前识别到的节点数。 |
| `edge_count` | number | 当前识别到的无向边数。 |
| `nodes` | array | 节点列表。 |
| `edges` | array | 无向边列表，每条边只写一次。 |
| `view_config` | object | 截图坐标到网格坐标的参数。 |
| `entry_map_image` | string | 进入是非境时的原图相对路径。 |
| `entry_map_overlay` | string | 进入是非境时的识别 overlay 相对路径。 |

### nodes

`nodes` 内每个元素表示一个识别到的地图节点。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `index` | number | 节点索引，计算方式为 `y * width + x`。 |
| `grid` | `[x, y]` | 网格坐标。 |
| `node_type` | string | 英文枚举，建议作为分析主键使用。 |
| `node_name` | string | 中文展示名。 |
| `is_open` | boolean | 当前是否可点开/可探索。 |
| `visited` | boolean | 本轮勘探流程内是否已经被路由处理。 |
| `sub_type` | string | 可选；当前用于常乐子类型，可能为 `Ling`、`Shu`、`Nian`。 |

常见 `node_type`：

| node_type | node_name | 当前处理方式 |
| --- | --- | --- |
| `Legend` | 传说 | 进入节点，保存完整选项截图和事件信息。 |
| `Disaster` | 祸乱 | 只点开详情页，截图后关闭，不进入。 |
| `Omissions` | 拾遗 | 只点开详情页，截图后关闭，不进入。 |
| `Scheme` | 筹谋 | 进入节点，保存选项；天圆地方有衍生事件。 |
| `Playtime` | 常乐 | 进入节点，保存事件名和选项截图。 |
| `YiTrader` | 易与 | 进入商店页，保存商店截图。 |
| `OldShop` | 故肆 | 按事件节点处理，保存选项。 |
| `Boons` | 抉择 | 参与地图节点和边识别；当前不作为勘探目标。 |
| `Doubts` | 杂疑 | 参与地图节点和边识别；当前不作为勘探目标。 |

### edges

`edges` 表示地图里的无向边。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `from` | number | 起点节点 `index`。 |
| `to` | number | 终点节点 `index`。 |
| `from_grid` | `[x, y]` | 起点网格坐标。 |
| `to_grid` | `[x, y]` | 终点网格坐标。 |

约定：

- 每条无向边只出现一次，当前写出时满足 `to > from`。
- 端点应能在同一条记录的 `nodes[*].index` 中找到。
- `Boons`（抉择）和 `Doubts`（杂疑）也参与建图和边识别。

### view_config

`view_config` 主要服务于调试和视觉复核，后处理通常不需要依赖。

字段包括：

- `origin_x`
- `origin_y`
- `middle_x`
- `middle_y`
- `last_x`
- `last_y`
- `node_width`
- `node_height`
- `column_offset`
- `row_offset`
- `roi_margin`

如果需要把 `grid` 反算成截图上的节点左上角，可使用：

```text
pixel_x = origin_x + grid_x * column_offset
pixel_y = origin_y + grid_y * row_offset
```

节点中心约为：

```text
center_x = pixel_x + node_width / 2
center_y = pixel_y + node_height / 2
```

## 节点勘探记录

节点记录由 `record_bosky_passage_node()`、`record_bosky_passage_event_node()`、`record_bosky_passage_legend_relieving()` 等函数写入。

所有节点记录的公共字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `route_index` | number | 本次是非境内第几个被路由到的节点，从 `1` 开始。 |
| `node_type` | string | 英文节点类型。 |
| `node_name` | string | 中文节点名。 |
| `map_node_index` | number | 对应地图节点 `index`。 |
| `grid` | `[x, y]` | 对应地图网格坐标。 |
| `image` | string | 此节点主截图相对路径。 |

`route_index` 只在单次是非境内有意义，不要跨行当作全局 ID。推荐用 `(session, bosky_run_index, map_node_index)` 或 `(session, bosky_run_index, grid_x, grid_y)` 作为地图节点级联结键。

### 传说 Legend

普通传说事件记录：

```json
{
  "route_index": 1,
  "node_type": "Legend",
  "node_name": "传说",
  "map_node_index": 5,
  "grid": [5, 0],
  "image": "bosky_passage\\..._Legend_options.png",
  "event_name": "生机",
  "events": [
    {
      "event_name": "生机",
      "image": "bosky_passage\\..._Legend_options.png",
      "options": ["席地而坐", "还是算了", "岁兽代理人的援助尽在匪"]
    }
  ]
}
```

特殊事件 `禳解`：

```json
{
  "route_index": 3,
  "node_type": "Legend",
  "node_name": "传说",
  "event_name": "禳解",
  "image": "bosky_passage\\..._Legend_relieving_options.png",
  "images": [
    "bosky_passage\\..._Legend_relieving_options.png"
  ],
  "option_groups": [
    ["向水缸中滴入鲜血", "不信则无"],
    ["竟有此事!"]
  ],
  "events": [
    {
      "event_name": "禳解",
      "images": [],
      "option_groups": []
    }
  ]
}
```

语义：

- `禳解` 会循环选择第一个选项，直到事件自然结束或识别/点击流程退出。
- 每次选择前都会保存一张 `Legend_relieving_options` 图。
- `option_groups[i]` 对应 `images[i]`，表示第 `i` 次循环前识别到的选项列表。

### 祸乱 Disaster / 拾遗 Omissions

这两类是 detail-only 节点，只点开节点详情页并截图，不进入节点。

```json
{
  "route_index": 5,
  "node_type": "Disaster",
  "node_name": "祸乱",
  "map_node_index": 31,
  "grid": [3, 4],
  "image": "bosky_passage\\..._Disaster_detail.png",
  "detail_only": true
}
```

处理语义：

- 点击节点图标。
- 等待详情页出现。
- 保存 `Disaster_detail` 或 `Omissions_detail`。
- 点击左下角边缘点 3 次关闭详情页。

后处理时如果看到 `Disaster` 或 `Omissions` 缺少 `detail_only: true`，应视为异常样本。

### 筹谋 Scheme

筹谋节点会进入事件页并保存选项。

普通结构：

```json
{
  "route_index": 18,
  "node_type": "Scheme",
  "node_name": "筹谋",
  "map_node_index": 19,
  "grid": [5, 2],
  "image": "bosky_passage\\..._Scheme_options.png",
  "options": ["点花", "布伥", "下刀", "算了"],
  "events": [
    {
      "event_name": "天圆地方",
      "image": "bosky_passage\\..._Scheme_options.png",
      "options": ["掷钱", "置钱", "抛出钱盒", "远去", "岁兽代理人的援助尽在"]
    },
    {
      "event_name": "天圆地方-1",
      "image": "bosky_passage\\..._Scheme_options.png",
      "options": ["点花", "布伥", "下刀", "算了"]
    }
  ]
}
```

当前特判：

- `天圆地方`：无论识别到 4 个还是 5 个选项，都选择第 2 个选项。
- `天圆地方-1`：无论识别到 2 个还是 4 个选项，都选择最后一个选项。

注意：

- 顶层 `options` 会随着同一节点内后续事件被更新；对多阶段事件，应优先读取 `events[*].options`。
- 顶层 `image` 是节点主图；多阶段事件的每张图以 `events[*].image` 为准。

### 常乐 Playtime

常乐会进入事件页，保存事件名和选项。

```json
{
  "route_index": 19,
  "node_type": "Playtime",
  "node_name": "常乐",
  "map_node_index": 29,
  "grid": [1, 4],
  "image": "bosky_passage\\..._Playtime_options.png",
  "event_name": "种因得果",
  "events": [
    {
      "event_name": "种因得果",
      "image": "bosky_passage\\..._Playtime_options.png",
      "options": ["种下恳切的愿望", "种下轻慢的愿望", "还是算了", "岁兽代理人的援助尽在匪"]
    }
  ]
}
```

地图节点的 `sub_type` 可用于常乐子类型：

| sub_type | 对应事件 |
| --- | --- |
| `Ling` | 掷地有声 |
| `Shu` | 种因得果 |
| `Nian` | 三缺一 |

### 故肆 OldShop

故肆当前按事件节点路径处理，保存选项列表。

```json
{
  "route_index": 20,
  "node_type": "OldShop",
  "node_name": "故肆",
  "map_node_index": 15,
  "grid": [1, 2],
  "image": "bosky_passage\\..._OldShop_options.png",
  "options": ["..."],
  "events": [
    {
      "event_name": "...",
      "image": "bosky_passage\\..._OldShop_options.png",
      "options": ["..."]
    }
  ]
}
```

### 易与 YiTrader

易与会走商店页截图逻辑。

```json
{
  "route_index": 21,
  "node_type": "YiTrader",
  "node_name": "易与",
  "map_node_index": 23,
  "grid": [2, 3],
  "image": "bosky_passage\\..._YiTrader.png"
}
```

同一次易与也会在 `traders.jsonl` 中出现，`type` 为 `YiTrader`。

## 当前路由顺序

默认树洞勘探优先级来自 `resource/tasks/Roguelike/routing.json`：

```text
传说 > 祸乱 > 拾遗 > 筹谋 > 常乐 > 易与 > 故肆
```

实现语义：

- 按类型优先级查找可用且未处理节点。
- 同一类型内部随机抽取，不按地图 index 固定顺序。
- 祸乱和拾遗只截详情页。
- 传说、筹谋、常乐、易与、故肆会进入节点内部。
- 当没有可处理节点时离开是非境；若本次是由目标烛火路径进入，会设置放弃本局。

## 目标烛火路径相关数据

外层 `相随` 事件中，如果第一个选项标题包含 `烛火`，且正文匹配目标文本，会执行：

1. 选择该烛火选项。
2. 进入衍生事件。
3. 选择衍生事件第一个选项进入是非境。
4. 是非境勘探完成后自动 abandon 本局。

相关记录：

- `agents.jsonl` 中会记录该代理人选项，包含：
  - `target_candle: true`
  - `option_text`
  - `derived_selected_option`
- `events.jsonl` 中会记录：
  - `bosky_auto_exit_abandon`
  - `run_abandon`
  - `run_end`

这部分不是 `bosky_passage.jsonl` 的核心地图/节点数据，但对追踪一次是非境为何进入、为何退出很有用。

## 推荐后处理表结构

建议先把 `bosky_passage.jsonl` 展平为以下表。

### bosky_runs

一行对应 `bosky_passage.jsonl` 的一行。

| 字段 | 来源 |
| --- | --- |
| `session_id` | session 目录名 |
| `run_id` | 文件内行号，从 1 开始 |
| `map_image` | map.`entry_map_image` |
| `map_overlay` | map.`entry_map_overlay` |
| `width` / `height` | map |
| `node_count` / `edge_count` | map |
| `center_index` / `center_x` / `center_y` | map |

### bosky_map_nodes

一行对应 map.`nodes[*]`。

| 字段 |
| --- |
| `session_id` |
| `run_id` |
| `node_index` |
| `grid_x` |
| `grid_y` |
| `node_type` |
| `node_name` |
| `is_open` |
| `visited` |
| `sub_type` |

### bosky_map_edges

一行对应 map.`edges[*]`。

| 字段 |
| --- |
| `session_id` |
| `run_id` |
| `from_index` |
| `to_index` |
| `from_x` |
| `from_y` |
| `to_x` |
| `to_y` |

### bosky_routes

一行对应一个节点勘探记录。

| 字段 |
| --- |
| `session_id` |
| `run_id` |
| `route_index` |
| `map_node_index` |
| `grid_x` |
| `grid_y` |
| `node_type` |
| `node_name` |
| `detail_only` |
| `event_name` |
| `image` |

### bosky_route_events

一行对应节点记录中的 `events[*]`。对于 `禳解`，也可以进一步拆成 `bosky_relieving_steps`。

| 字段 |
| --- |
| `session_id` |
| `run_id` |
| `route_index` |
| `event_order` |
| `event_name` |
| `image` |
| `options_json` |
| `images_json` |
| `option_groups_json` |

### bosky_relieving_steps

仅用于 `禳解`。

| 字段 |
| --- |
| `session_id` |
| `run_id` |
| `route_index` |
| `step_index` |
| `image` |
| `options_json` |

## 推荐校验规则

后处理导入时建议做以下校验：

1. 每行 JSON 必须解析为数组。
2. 每个数组应存在且最好只存在一个 `record_type == "map"` 记录。
3. 所有 `edges[*].from/to` 都应存在于 `nodes[*].index`。
4. 同一 map 内无向边不应重复。
5. 所有节点记录的 `map_node_index` 都应存在于当前 map。
6. 节点记录的 `grid` 应与 map 中同 `map_node_index` 的 `grid` 一致。
7. 同一行内 `route_index` 应唯一且大体递增。
8. `Disaster` / `Omissions` 应有 `detail_only: true`，且通常不应有 `events`。
9. 多阶段事件以 `events[*]` 为准，不要只读取顶层 `options`。
10. 所有 `image` / `images[*]` 路径都应能在 session 目录下找到。

## 字段使用建议

- 分析主键使用英文 `node_type`，中文 `node_name` 只用于展示。
- 地图拓扑使用 `nodes` + `edges`，不要从图片文件名或 route 顺序反推边。
- 节点身份在一次是非境内优先使用 `map_node_index`；跨 session 不要比较 `map_node_index` 的绝对含义。
- 事件选项分析优先读取 `events[*].options`。
- 截图人工复核优先使用 `entry_map_overlay` 和节点记录里的 `image`。
- `events.jsonl` 适合查运行时间线和异常退出原因，但树洞结构化分析应以 `bosky_passage.jsonl` 为主。
