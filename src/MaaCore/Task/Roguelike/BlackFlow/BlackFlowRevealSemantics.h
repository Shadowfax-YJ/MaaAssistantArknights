#pragma once

#include <algorithm>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "BlackFlowModel.h"

namespace asst::blackflow
{
inline constexpr int LightRevealRadius = 3;
inline constexpr int InitialLightRevealRadius = 2;
inline constexpr int TransientRevealObservationMaximumAttempts = 4;

[[nodiscard]] constexpr bool move_endpoint_observation_available(
    bool terminal_on_completion,
    int action_points_after) noexcept
{
    return !terminal_on_completion && action_points_after > 0;
}

[[nodiscard]] inline std::vector<NodeId> move_reveal_origins(
    const MoveCandidate& move,
    NodeId landing,
    bool endpoint_observation_available)
{
    (void)move;
    std::vector<NodeId> result;
    if (!endpoint_observation_available) {
        return result;
    }
    // 只有最终落点产生这一步的新视野。徒步路径本来就在移动前的透明视野闭包内；
    // 曲折密道的入口只是传送目标，不是最终落点，也不会点亮入口周围的节点。
    if (landing != InvalidNodeId) {
        result.emplace_back(landing);
    }
    return result;
}

[[nodiscard]] inline bool initially_unknown_for_reveal(const MapSnapshot& map, const RunState& run, NodeId id)
{
    const Node* node = map.find_node(id);
    return node != nullptr && node->floor == run.floor && node->type != NodeType::Empty &&
           node->progress != NodeProgress::Removed && !node->identity_revealed && !node->identity_from_prediction &&
           !run.revealed_nodes.contains(id);
}

[[nodiscard]] inline std::optional<NodeType> event_reveal_node_type(std::string_view content) noexcept
{
    if (content.starts_with("和平守卫者")) {
        return NodeType::BattleSavage;
    }
    if (content.starts_with("独活")) {
        return NodeType::Employ;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool initial_reveal_candidate(const Node& node) noexcept
{
    return node.type != NodeType::Empty && node.progress != NodeProgress::Removed;
}

// 这些节点的身份不依赖地图揭示规则，本来就在初始画面中可见。它们既不属于
// “起点连线/羽瞰点/襁褓骏鹰”产生的揭示增量，也不能作为额外揭示参与自洽性比较。
[[nodiscard]] inline bool initially_visible_without_reveal(const Node& node) noexcept
{
    if (node.visually_hidden) {
        return false;
    }
    return node.type == NodeType::Door || node.type == NodeType::Light || node.type == NodeType::BattleBoss ||
           node.type == NodeType::Final || (node.type == NodeType::Incident && node.name == "命运所指");
}

// 新层首帧已经包含揭示结果，不能直接复用 MapSnapshot::reveal_through_transparent_nodes：
// 后者会让“本帧刚被揭示”的弥散虚雾节点继续传递视野，反过来污染预期集合。这里仅使用
// 拓扑、透明性和无需揭示便可见的固定身份，重建起点产生的自然视野闭包。
[[nodiscard]] inline std::unordered_set<NodeId>
expected_initial_natural_reveals(const MapSnapshot& map, NodeId entrance)
{
    std::unordered_set<NodeId> result;
    if (map.find_node(entrance) == nullptr) {
        return result;
    }

    std::queue<NodeId> pending;
    std::unordered_set<NodeId> visited { entrance };
    pending.emplace(entrance);
    while (!pending.empty()) {
        const NodeId current = pending.front();
        pending.pop();
        for (const NodeId neighbor : map.neighbors(current, GraphLayer::Confirmed)) {
            const Node* node = map.find_node(neighbor);
            if (node == nullptr || node->progress == NodeProgress::Removed) {
                continue;
            }
            const bool baseline_visible = initially_visible_without_reveal(*node);
            if (node->natural_reveal_suppressed && !baseline_visible) {
                continue;
            }
            result.emplace(neighbor);
            if (visited.emplace(neighbor).second && !node->traversal.blocks_vision) {
                pending.emplace(neighbor);
            }
        }
    }
    return result;
}

// 新地图的第一帧没有可供做差分的旧地图，因此直接从新图拓扑重建初始应见集合：
// 起点沿确认连线形成的透明视野闭包、所有初始羽瞰点曼哈顿距离 2，以及襁褓骏鹰在一/二层的全图点亮。
// 弥散虚雾抑制起点连线与襁褓骏鹰产生的自然揭示，但羽瞰点属于主动照亮，
// 与落点触发羽瞰点时一致，不受弥散虚雾抑制。预期集合绝不反写当前观测。
[[nodiscard]] inline std::unordered_set<NodeId> expected_initial_floor_reveals(
    const MapSnapshot& map,
    NodeId entrance,
    bool swaddled_eagle_full_reveal)
{
    std::unordered_set<NodeId> result;
    const auto add = [&](NodeId id, bool active_reveal = false) {
        const Node* node = map.find_node(id);
        if (node != nullptr && initial_reveal_candidate(*node) && !initially_visible_without_reveal(*node) &&
            (active_reveal || !node->natural_reveal_suppressed)) {
            result.emplace(id);
        }
    };

    if (swaddled_eagle_full_reveal) {
        for (const auto& [id, node] : map.nodes()) {
            if (initial_reveal_candidate(node) && !initially_visible_without_reveal(node) &&
                !node.natural_reveal_suppressed) {
                result.emplace(id);
            }
        }
        return result;
    }

    for (const NodeId id : expected_initial_natural_reveals(map, entrance)) {
        add(id);
    }
    for (const auto& [light_id, light] : map.nodes()) {
        if (light.type != NodeType::Light || light.progress == NodeProgress::Removed) {
            continue;
        }
        for (const NodeId id : map.nodes_within_manhattan(light_id, InitialLightRevealRadius)) {
            add(id, true);
        }
    }
    return result;
}

[[nodiscard]] inline std::unordered_set<NodeId>
observed_initial_floor_reveals(const MapSnapshot& map, NodeId entrance = InvalidNodeId)
{
    std::unordered_set<NodeId> result;
    for (const auto& [id, node] : map.nodes()) {
        if (id != entrance && initial_reveal_candidate(node) && node.identity_revealed && !node.visually_hidden &&
            !initially_visible_without_reveal(node) && node.identity_source != "map_topology_no_ocr_empty") {
            result.emplace(id);
        }
    }
    return result;
}

[[nodiscard]] inline std::unordered_set<NodeId> expected_move_reveals(
    const MapSnapshot& map,
    const RunState& run,
    const MoveCandidate& move,
    NodeId landing,
    bool endpoint_observation_available,
    const std::vector<std::string>& observed_contents = {},
    NodeType landing_type_override = NodeType::Unknown)
{
    std::unordered_set<NodeId> result;
    const auto add_if_initially_unknown = [&](NodeId id) {
        if (initially_unknown_for_reveal(map, run, id)) {
            result.emplace(id);
        }
    };
    for (const NodeId origin : move_reveal_origins(move, landing, endpoint_observation_available)) {
        for (const NodeId id : map.reveal_through_transparent_nodes(origin)) {
            add_if_initially_unknown(id);
        }
    }

    const Node* landed = map.find_node(landing);
    const NodeType landing_type = landing_type_override != NodeType::Unknown
                                      ? landing_type_override
                                      : (landed == nullptr ? NodeType::Unknown : landed->type);
    if (endpoint_observation_available && landed != nullptr && landing_type == NodeType::Light) {
        for (const NodeId id : map.nodes_within_manhattan(landing, LightRevealRadius)) {
            // 羽瞰点是主动照亮，不属于沿连线产生的自然揭示。弥散虚雾只抑制
            // 理想域内的自然视野，不能阻止羽瞰点照亮范围内的节点。
            add_if_initially_unknown(id);
        }
    }

    for (const std::string& content : observed_contents) {
        const auto revealed_type = event_reveal_node_type(content);
        if (!revealed_type.has_value()) {
            continue;
        }
        for (const auto& [id, node] : map.nodes()) {
            if (node.type == *revealed_type && initially_unknown_for_reveal(map, run, id)) {
                result.emplace(id);
            }
        }
    }
    return result;
}

// “和平守卫者/独活”的第一项会先完成原事件，再在回图前免费传送到关联节点。
// 游戏只在回到地图时结算自然视野，因此自然揭示必须从最终回图位置传播；原事件
// 只因确实被进入并完成而揭示自身，不能成为视野传播源。第二项不传送，回图位置
// 仍是原事件。两个选项都会直接揭示关联节点的身份。
[[nodiscard]] inline std::unordered_set<NodeId> expected_linked_encounter_return_reveals(
    const MapSnapshot& map,
    const RunState& run,
    const MoveCandidate& move,
    NodeId event_node,
    NodeId linked_node,
    bool transferred,
    bool endpoint_observation_available)
{
    const NodeId returned_map_landing = transferred ? linked_node : event_node;
    auto result = expected_move_reveals(
        map,
        run,
        move,
        returned_map_landing,
        endpoint_observation_available);
    const auto add_semantic_reveal = [&](NodeId id) {
        if (initially_unknown_for_reveal(map, run, id)) {
            result.emplace(id);
        }
    };
    add_semantic_reveal(event_node);
    add_semantic_reveal(linked_node);
    return result;
}

[[nodiscard]] inline std::unordered_set<NodeId>
observed_move_reveals(const MapSnapshot& before, const RunState& run, const MapSnapshot& after)
{
    std::unordered_set<NodeId> result;
    for (const auto& [id, node] : before.nodes()) {
        if (!initially_unknown_for_reveal(before, run, id)) {
            continue;
        }
        const Node* observed = after.find_node(id);
        // map_topology_no_ocr_empty 只表示模板槽位存在、但该处没有识别到节点身份。
        // 尤其在右上角被行动力 HUD 遮挡时，它会暂时把未知节点写成空位；这不是一次
        // 有视觉证据的揭示，不能触发“比预期多揭示”的自洽性警告。
        if (observed != nullptr && observed->identity_revealed &&
            observed->identity_source != "map_topology_no_ocr_empty") {
            result.emplace(id);
        }
    }
    return result;
}

[[nodiscard]] inline bool should_retry_transient_reveal_observation(
    const std::unordered_set<NodeId>& expected,
    const std::unordered_set<NodeId>& observed,
    const MapSnapshot& after)
{
    for (const NodeId id : expected) {
        if (observed.contains(id)) {
            continue;
        }
        const Node* node = after.find_node(id);
        if (node != nullptr && node->identity_source == "map_topology_no_ocr_empty") {
            return true;
        }
    }
    return false;
}

inline void add_observed_event_reveal_expectations(
    const MapSnapshot& before,
    const RunState& run,
    const MapSnapshot& after,
    const std::vector<std::string>& observed_contents,
    bool resolved_incident_page,
    std::unordered_set<NodeId>& expected)
{
    (void)resolved_incident_page;
    std::unordered_set<NodeType> revealed_types;
    for (const std::string& content : observed_contents) {
        if (const auto type = event_reveal_node_type(content); type.has_value()) {
            revealed_types.emplace(*type);
        }
    }
    if (revealed_types.empty()) {
        return;
    }
    // 事件发生前这些节点可能仍只有“未知”的外观，旧地图里没有足够信息预知具体身份。
    // 回图后可依据事件的确定效果，把新观测为“居民”据点/应急助力的节点归因到事件点亮。
    // 两个事件的效果不能混为一谈：和平守卫者只点亮居民据点，独活只点亮应急助力。
    // 没有事件标题证据时不再用“任意事件页”兜底接纳，否则会掩盖真实的额外揭示。
    for (const auto& [id, node] : before.nodes()) {
        if (!initially_unknown_for_reveal(before, run, id)) {
            continue;
        }
        const Node* observed = after.find_node(id);
        if (observed != nullptr && observed->identity_revealed && revealed_types.contains(observed->type)) {
            expected.emplace(id);
        }
    }
}

struct RevealConsistencyResult
{
    std::vector<NodeId> missing;
    std::vector<NodeId> unexpected;

    [[nodiscard]] bool consistent() const noexcept { return missing.empty() && unexpected.empty(); }
};

[[nodiscard]] inline RevealConsistencyResult compare_move_reveals(
    const std::unordered_set<NodeId>& expected,
    const std::unordered_set<NodeId>& observed)
{
    RevealConsistencyResult result;
    for (const NodeId id : expected) {
        if (!observed.contains(id)) {
            result.missing.emplace_back(id);
        }
    }
    for (const NodeId id : observed) {
        if (!expected.contains(id)) {
            result.unexpected.emplace_back(id);
        }
    }
    std::ranges::sort(result.missing);
    std::ranges::sort(result.unexpected);
    return result;
}

// 首帧没有旧图可做身份差分；但当初始视野核对已经不一致，且本帧仍含有
// map_topology_no_ocr_empty 时，不一致可能来自某个非空节点的瞬时 OCR 漏检。
// 此处只要求再识别一张稳定帧，不改变节点语义：重试耗尽后它依然按空地处理。
[[nodiscard]] inline bool initial_floor_reveal_observation_needs_ocr_retry(
    const MapSnapshot& observed_map,
    NodeId entrance,
    bool swaddled_eagle_full_reveal)
{
    const auto expected =
        expected_initial_floor_reveals(observed_map, entrance, swaddled_eagle_full_reveal);
    const auto observed = observed_initial_floor_reveals(observed_map, entrance);
    if (compare_move_reveals(expected, observed).consistent()) {
        return false;
    }
    return std::ranges::any_of(observed_map.nodes(), [](const auto& entry) {
        return entry.second.identity_source == "map_topology_no_ocr_empty";
    });
}

[[nodiscard]] constexpr int effective_node_weight_at_floor(
    NodeType type,
    int floor,
    std::string_view marker_type = {},
    MovementKind movement = MovementKind::Walk) noexcept
{
    int base = 1;
    switch (type) {
    case NodeType::Empty:
    case NodeType::Final:
    case NodeType::BattleBoss:
    case NodeType::Sacrifice:
    case NodeType::Light:
    case NodeType::Door:
    case NodeType::Employ:
    case NodeType::Expedition:
        base = 0;
        break;
    case NodeType::BattleElite:
    case NodeType::BattleSavage:
    case NodeType::ScrapShop:
    case NodeType::Duel:
        base = 2;
        break;
    case NodeType::Portal:
        // 三层误入奇境的路线价值更高，其余楼层仍按两个有效节点计分。
        base = floor == 3 ? 3 : 2;
        break;
    default:
        break;
    }

    // 藏果地与坎诺特的触须各自提供一个额外探索收益；奖励仍绑定实际落点，
    // 由 record_effective_landing 按稳定节点 ID 去重。
    if (marker_type == "fruit_cache") {
        ++base;
    }
    if (movement == MovementKind::M10) {
        ++base;
    }
    return base;
}

[[nodiscard]] constexpr int effective_node_weight(NodeType type) noexcept
{
    return effective_node_weight_at_floor(type, 0);
}

[[nodiscard]] constexpr bool counts_as_effective_node(NodeType type) noexcept
{
    return effective_node_weight(type) > 0;
}

[[nodiscard]] constexpr int effective_node_weight(NodeType type, std::string_view marker_type) noexcept
{
    return effective_node_weight_at_floor(type, 0, marker_type);
}

[[nodiscard]] constexpr int effective_node_weight(
    NodeType type,
    std::string_view marker_type,
    MovementKind movement) noexcept
{
    return effective_node_weight_at_floor(type, 0, marker_type, movement);
}

[[nodiscard]] constexpr bool counts_as_effective_node(NodeType type, std::string_view marker_type) noexcept
{
    return effective_node_weight(type, marker_type) > 0;
}

inline int record_effective_landing(
    NodeId landing,
    NodeType landing_type,
    std::string_view marker_type,
    MovementKind movement,
    NodeId initial_node,
    const std::unordered_set<NodeId>& previously_entered_nodes,
    std::unordered_set<NodeId>& effective_nodes,
    int floor = 0)
{
    const int weight = effective_node_weight_at_floor(landing_type, floor, marker_type, movement);
    if (landing != InvalidNodeId && landing != initial_node && !previously_entered_nodes.contains(landing) &&
        weight > 0) {
        const auto [_, inserted] = effective_nodes.emplace(landing);
        return inserted ? weight : 0;
    }
    return 0;
}

inline int record_effective_landing(
    NodeId landing,
    NodeType landing_type,
    std::string_view marker_type,
    NodeId initial_node,
    const std::unordered_set<NodeId>& previously_entered_nodes,
    std::unordered_set<NodeId>& effective_nodes)
{
    return record_effective_landing(
        landing,
        landing_type,
        marker_type,
        MovementKind::Walk,
        initial_node,
        previously_entered_nodes,
        effective_nodes);
}

inline int record_effective_landing(
    NodeId landing,
    NodeType landing_type,
    NodeId initial_node,
    const std::unordered_set<NodeId>& previously_entered_nodes,
    std::unordered_set<NodeId>& effective_nodes)
{
    // 集合按稳定节点 ID 去重：本层已进入、路线起点和同一路线重复落脚都不产生收益。
    return record_effective_landing(
        landing,
        landing_type,
        std::string_view {},
        initial_node,
        previously_entered_nodes,
        effective_nodes);
}
} // namespace asst::blackflow
