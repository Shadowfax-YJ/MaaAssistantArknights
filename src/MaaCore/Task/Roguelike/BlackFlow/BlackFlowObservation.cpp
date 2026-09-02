#include "BlackFlowObservation.h"

#include <algorithm>
#include <unordered_set>

#include "Vision/Roguelike/BlackFlow/BlackFlowFloor.h"

namespace asst::blackflow
{
namespace
{
NodeIdentityState identity_state_for(std::string_view type) noexcept
{
    if (type == "unclassified") {
        return NodeIdentityState::Unclassified;
    }
    if (type == "hide_battle" || type == "hide_invisible") {
        return NodeIdentityState::Hidden;
    }
    return NodeIdentityState::Classified;
}

bool identity_revealed_for(std::string_view type) noexcept
{
    return type != "unclassified" && type != "hide_battle" && type != "hide_invisible";
}
} // namespace

std::optional<NodeType> BlackFlowObservationAdapter::map_node_type(std::string_view type) noexcept
{
    return node_type_from_string(type);
}

std::vector<GridPosition> BlackFlowObservationAdapter::expected_grid_positions(int floor)
{
    std::vector<GridPosition> result;
    const auto profile = perception::floor_profile(floor);
    if (!profile.has_value()) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(profile->rows * profile->columns));
    for (int row = 0; row < profile->rows; ++row) {
        for (int column = 0; column < profile->columns; ++column) {
            result.emplace_back(GridPosition { row, column });
        }
    }
    return result;
}

std::optional<NormalizedPerceptionObservation>
    BlackFlowObservationAdapter::normalize(const BlackFlowMapObservation& source, std::string* error) const
{
    if (!source.recognition_ok || !source.graph_connected) {
        if (error != nullptr) {
            *error = "perception result failed the recognition or connectivity gate";
        }
        return std::nullopt;
    }
    if (source.floor < 1 || source.current_marker_temporary_id < 0) {
        if (error != nullptr) {
            *error = "perception result has no valid observed floor or current marker";
        }
        return std::nullopt;
    }

    NormalizedPerceptionObservation result;
    result.map.floor = source.floor;
    result.map.coverage = source.coverage;
    result.map.covered_positions = source.covered_positions;
    if (result.map.coverage == ObservationCoverage::FullMap && result.map.covered_positions.empty()) {
        result.map.covered_positions = expected_grid_positions(source.floor);
    }
    result.hud_action_points = source.hud_action_points;
    result.viewport_revision = source.viewport_revision;
    result.summary.observation_id = source.observation_id;
    result.summary.floor = source.floor;
    result.summary.floor_from_ocr = source.floor_from_ocr;
    result.summary.screenshot_us = source.screenshot_us;
    result.summary.recognition_us = source.recognition_us;
    result.summary.attempt_count = source.attempt_count;
    result.summary.retry_count = source.retry_count;
    result.summary.topology_template_id = source.topology_template_id;
    result.summary.topology_source_digest = source.topology_source_digest;
    result.summary.topology_base_edge_count = source.topology_base_edge_count;
    result.summary.topology_extra_edge_count = source.topology_extra_edge_count;
    result.summary.topology_match_score = source.topology_match_score;

    const std::size_t visible_resident_marker_count = static_cast<std::size_t>(std::ranges::count_if(
        source.nodes,
        [](const PerceptionNodeObservation& node) {
            return node.exists && node.marker_type == "savage";
        }));
    // 一张地图初始会有 3 个流窜“居民”。若已经看到了居民标记但不足 3 个，其他
    // 已识别标记可能是两种标记重叠后只匹配到了上层外观。必须保留视觉实际看到的
    // 藏果地/线人类型，再用独立标志让路线避让和据点多假设推断处理这种可能性。
    const bool infer_overlapped_resident_markers =
        visible_resident_marker_count > 0 && visible_resident_marker_count < 3;

    std::unordered_map<int, const PerceptionNodeObservation*> by_temporary_id;
    std::unordered_map<int, NodeId> stable_ids;
    for (const auto& source_node : source.nodes) {
        if (!source_node.exists) {
            continue;
        }
        const auto type = map_node_type(source_node.type);
        const auto stable = make_stable_node_id(source.floor, source_node.position);
        if (!type.has_value() || !stable.has_value() || source_node.temporary_id < 0 ||
            !by_temporary_id.emplace(source_node.temporary_id, &source_node).second) {
            if (error != nullptr) {
                *error = "perception result contains an unknown node type or invalid duplicate node";
            }
            return std::nullopt;
        }
        stable_ids.emplace(source_node.temporary_id, *stable);

        ObservedNode node;
        node.position = source_node.position;
        // “流窜居民”是会在节点之间移动的标记，不是节点类型。保留空地、不期而遇等
        // 节点本身的身份，路线规划只在当前首个动作按 marker_type 避让它。
        node.type = *type;
        node.name = source_node.displayed_name;
        node.fate_event = *type == NodeType::Incident && source_node.displayed_name == "命运所指";

        // 确定性规则可以提前知道节点的语义类型，但不能代替游戏视野。比如非“希望的沃土”
        // 的实托邦中心必为紧急作战，在地图尚未点亮时预览标题仍只会显示“未知的凶戾”。
        // 因此预测节点沿用现场的隐藏状态，不能仅因 type 已被改写就标记为视觉已揭示。
        const bool predicted_but_visually_hidden =
            source_node.identity_from_prediction && source_node.visually_hidden;
        node.identity_state = predicted_but_visually_hidden ? NodeIdentityState::Hidden
                                                            : identity_state_for(source_node.type);
        node.identity_revealed =
            predicted_but_visually_hidden ? false : identity_revealed_for(source_node.type);
        node.visually_hidden = source_node.visually_hidden;
        node.identity_from_topology = source_node.identity_from_topology;
        node.identity_from_prediction = source_node.identity_from_prediction;
        node.prediction_rule = source_node.prediction_rule;
        // 理想域识别是一个允许拒识的视觉模型。同一张地图某一帧已经确认某节点位于
        // “弥散虚雾”理想域后，后续帧偶发拒识不能把这个事实反向清掉；换图时地图本身
        // 会换代/重置。因而这里只把肯定结果写入增量观测。
        if (source_node.natural_reveal_suppressed) {
            node.natural_reveal_suppressed = true;
        }
        node.existence_source = source_node.existence_source;
        node.identity_source = source_node.identity_source;
        node.detected_by_vision = source_node.detected_by_vision;
        node.confirmed_by_topology = source_node.confirmed_by_topology;
        // 初始流窜“居民”只会出现在林间空地。其他标记只有落在空地上时，才可能
        // 实际遮住一个居民标记；战斗、事件、商店等节点上的标记不能参与该假设。
        const bool overlapped_resident_marker = infer_overlapped_resident_markers &&
                                                *type == NodeType::Empty &&
                                                !source_node.marker_type.empty() &&
                                                source_node.marker_type != "savage";
        node.marker_type = source_node.marker_type;
        // “线人与线索”落在命运所指上时，游戏将这枚标记命名为“谜题与谜底”；
        // 这是标记名称，不是节点的具体事件名，不能覆盖 node.name。
        node.marker_display_name =
            source_node.marker_type == "informant" && *type == NodeType::Incident &&
                    source_node.displayed_name == "命运所指"
                ? "谜题与谜底"
                : source_node.marker_display_name;
        node.marker_score = source_node.marker_score;
        node.marker_resident_overlap_possible = overlapped_resident_marker;
        node.badged = source_node.badged;
        if (source_node.transfer_target.has_value()) {
            node.transfer_target = source_node.transfer_target;
        }
        result.map.nodes.emplace_back(std::move(node));

        if (!source_node.icon_rect.empty()) {
            result.viewport.emplace_back(
                NodeObservation {
                    *stable,
                    source_node.icon_rect,
                    source_node.text_rect,
                    source_node.confidence,
                    source_node.confidence,
                });
        }
        ++result.summary.node_count;
        if (source_node.type == "unclassified") {
            ++result.summary.unclassified_count;
        }
    }

    const auto current = stable_ids.find(source.current_marker_temporary_id);
    if (current == stable_ids.end()) {
        if (error != nullptr) {
            *error = "current marker does not reference an existing imported node";
        }
        return std::nullopt;
    }
    result.current_node = current->second;
    result.summary.current_node = current->second;

    for (const auto& source_edge : source.edges) {
        if (!source_edge.connected) {
            continue;
        }
        const auto first = by_temporary_id.find(source_edge.temporary_first);
        const auto second = by_temporary_id.find(source_edge.temporary_second);
        if (first == by_temporary_id.end() || second == by_temporary_id.end() || first == second) {
            if (error != nullptr) {
                *error = "connected perception edge references an absent or identical endpoint";
            }
            return std::nullopt;
        }
        const EdgeKnowledge knowledge =
            source_edge.forced_by_connectivity_constraint ? EdgeKnowledge::Unknown : EdgeKnowledge::Confirmed;
        result.map.edges.emplace_back(
            ObservedEdge {
                first->second->position,
                second->second->position,
                knowledge,
                EdgeEvidence {
                    source_edge.probability,
                    source_edge.cnn_connected,
                    source_edge.forced_by_connectivity_constraint,
                    source_edge.decision_source,
                },
            });
        if (knowledge == EdgeKnowledge::Confirmed) {
            ++result.summary.confirmed_edge_count;
        }
        if (source_edge.forced_by_connectivity_constraint) {
            ++result.summary.forced_edge_count;
        }
    }
    return result;
}
} // namespace asst::blackflow
