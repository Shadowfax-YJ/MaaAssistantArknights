#include "BlackFlowModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <queue>
#include <set>
#include <tuple>
#include <utility>

namespace asst::blackflow
{
NodeType move_landing_type(const MoveCandidate& candidate, NodeId landing) noexcept
{
    const auto found = candidate.landing_node_types.find(landing);
    return found == candidate.landing_node_types.end() ? NodeType::Unknown : found->second;
}

bool move_landing_is_terminal(const MoveCandidate& candidate, NodeId landing) noexcept
{
    return is_exit_node_type(move_landing_type(candidate, landing));
}

bool move_preview_updates_target_identity(const MoveCandidate& candidate) noexcept
{
    // 定向移动的预览标题描述的是玩家点选的目标；小八界的点选节点只负责激活随机移动，
    // 既不是承诺落点，也不能因为标题揭示了它就取消本次随机移动并重规划。
    return candidate.controllable;
}

bool is_generic_battle_name(NodeType type, std::string_view name) noexcept
{
    if (type == NodeType::BattleNormal || type == NodeType::BattleElite) {
        return name.empty() || name == "作战" || name == "紧急作战";
    }
    if (type == NodeType::BattleBoss) {
        return name.empty() || name == "险路恶敌";
    }
    return false;
}

std::string_view battle_stage_name(const Node& node) noexcept
{
    if ((node.type != NodeType::BattleNormal && node.type != NodeType::BattleElite &&
         node.type != NodeType::BattleBoss) ||
        is_generic_battle_name(node.type, node.name)) {
        return {};
    }
    return node.name;
}

namespace
{
const std::vector<NodeType> AllTargetTypes = {
    NodeType::Unknown,   NodeType::BattleElite, NodeType::BattleNormal, NodeType::BattleSavage, NodeType::Duel,
    NodeType::Door,      NodeType::Employ,      NodeType::Expedition,   NodeType::HideBattle,   NodeType::HideInvisible,
    NodeType::Incident,  NodeType::Light,       NodeType::Portal,       NodeType::Rest,         NodeType::Sacrifice,
    NodeType::ScrapShop, NodeType::Shop,        NodeType::Wish,         NodeType::Empty,        NodeType::Evacuate,
    NodeType::Final,     NodeType::BattleBoss,
};

const std::vector<NodeType> NonCombatTargetTypes = {
    NodeType::Duel,  NodeType::Door,   NodeType::Employ,     NodeType::Expedition, NodeType::HideInvisible,
    NodeType::Incident, NodeType::Light, NodeType::Portal,   NodeType::Rest,       NodeType::Sacrifice,
    NodeType::ScrapShop, NodeType::Shop, NodeType::Wish,     NodeType::Empty,      NodeType::Evacuate,
    NodeType::Final,
};

const std::vector<NodeType> ShopTargetTypes = {
    NodeType::ScrapShop,
    NodeType::Shop,
};

std::string normalize_preview_identity_name(std::string_view name)
{
    std::string normalized(name);
    std::erase(normalized, '"');
    for (const std::string_view quote : { std::string_view("“"), std::string_view("”") }) {
        for (std::size_t position = normalized.find(quote); position != std::string::npos;
             position = normalized.find(quote, position)) {
            normalized.erase(position, quote.size());
        }
    }
    return normalized;
}

bool preview_identity_names_equal(std::string_view map_name, std::string_view preview_name)
{
    return normalize_preview_identity_name(map_name) == normalize_preview_identity_name(preview_name);
}

bool same_edge(const Edge& lhs, const Edge& rhs) noexcept
{
    return lhs.first == rhs.first && lhs.second == rhs.second;
}

Edge normalized_edge(Edge edge) noexcept
{
    if (edge.second < edge.first) {
        std::swap(edge.first, edge.second);
    }
    return edge;
}

bool edge_visible_in_layer(const Edge& edge, GraphLayer layer) noexcept
{
    if (edge.knowledge == EdgeKnowledge::Absent) {
        return false;
    }
    if (layer == GraphLayer::Relaxed) {
        return true;
    }
    return edge.knowledge == EdgeKnowledge::Confirmed && !edge.evidence.forced_by_connectivity_constraint;
}

NodeProgress effective_progress(const Node& node, const RunState& state) noexcept
{
    const auto found = state.node_progress.find(node.id);
    return found == state.node_progress.end() ? node.progress : found->second;
}

NodeType effective_node_type(const Node& node, const RunState& state) noexcept
{
    if (effective_progress(node, state) == NodeProgress::Completed && !node.traversal.repeatable &&
        node.type != NodeType::Empty) {
        return NodeType::Empty;
    }
    return node.type;
}

bool is_targetable(const Node& node, const RunState& state) noexcept
{
    const NodeProgress progress = effective_progress(node, state);
    if (!node.traversal.enterable || progress == NodeProgress::Removed) {
        return false;
    }
    return progress != NodeProgress::Completed || node.traversal.repeatable;
}

bool is_walk_transparent(const Node& node, const RunState& state, GraphLayer layer) noexcept
{
    return !node.traversal.blocks_walk || state.visited_nodes.contains(node.id) ||
           effective_progress(node, state) == NodeProgress::Completed ||
           (layer == GraphLayer::Relaxed && node.identity_state == NodeIdentityState::Unclassified);
}

int predicted_node_gain(const Node& node, const RunState& state) noexcept
{
    return node.type == NodeType::Light && !state.consumed_one_time_nodes.contains(node.id) ? 1 : 0;
}

std::uint32_t encode_grid_component(int value, bool* ok) noexcept
{
    constexpr int Minimum = -4'194'304;
    constexpr int Maximum = 4'194'303;
    if (value < Minimum || value > Maximum) {
        *ok = false;
        return 0;
    }
    const std::int64_t wide = value;
    return static_cast<std::uint32_t>((wide << 1) ^ (wide >> 63));
}
} // namespace

std::size_t GridPositionHash::operator()(const GridPosition& position) const noexcept
{
    std::size_t seed = std::hash<int> {}(position.row);
    seed ^= std::hash<int> {}(position.column) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

std::optional<NodeId> make_stable_node_id(int floor, GridPosition position) noexcept
{
    if (floor < 0 || floor > 65534) {
        return std::nullopt;
    }
    bool valid = true;
    const std::uint32_t row = encode_grid_component(position.row, &valid);
    const std::uint32_t column = encode_grid_component(position.column, &valid);
    if (!valid || row >= (1U << 24U) || column >= (1U << 24U)) {
        return std::nullopt;
    }
    return (static_cast<NodeId>(floor) << 48U) | (static_cast<NodeId>(row) << 24U) | column;
}

bool MapSnapshot::upsert_node(Node node)
{
    if (node.id == InvalidNodeId) {
        return false;
    }
    const auto expected = make_stable_node_id(node.floor, node.position);
    if (!expected.has_value() || *expected != node.id) {
        return false;
    }
    // 林间空地在游戏地图上不绘制标题，但节点身份本身仍有正式名称。
    // 统一在模型边界补全，避免地图 OCR 的“无文字”与移动预览的“林间空地”
    // 被误判成两种身份并触发无休止的取消预览/重规划。
    if (node.type == NodeType::Empty) {
        node.name = EmptyNodeName;
    }
    const auto existing = m_nodes.find(node.id);
    if (existing != m_nodes.end() && existing->second == node) {
        return true;
    }
    m_nodes.insert_or_assign(node.id, std::move(node));
    ++revision;
    return true;
}

bool MapSnapshot::remove_node(NodeId id)
{
    if (m_nodes.erase(id) == 0) {
        return false;
    }
    std::erase_if(m_edges, [id](const Edge& edge) { return edge.first == id || edge.second == id; });
    for (auto& [other_id, node] : m_nodes) {
        (void)other_id;
        if (node.transfer_target == id) {
            node.transfer_target.reset();
        }
    }
    ++revision;
    return true;
}

bool MapSnapshot::upsert_edge(Edge edge)
{
    edge = normalized_edge(edge);
    if (edge.first == InvalidNodeId || edge.second == InvalidNodeId || edge.first == edge.second ||
        !m_nodes.contains(edge.first) || !m_nodes.contains(edge.second)) {
        return false;
    }
    if (auto iter = std::ranges::find_if(m_edges, [&](const Edge& current) { return same_edge(current, edge); });
        iter != m_edges.end()) {
        if (iter->knowledge != edge.knowledge || iter->evidence.probability != edge.evidence.probability ||
            iter->evidence.cnn_connected != edge.evidence.cnn_connected ||
            iter->evidence.forced_by_connectivity_constraint != edge.evidence.forced_by_connectivity_constraint ||
            iter->evidence.decision_source != edge.evidence.decision_source) {
            *iter = std::move(edge);
            ++revision;
        }
    }
    else {
        m_edges.emplace_back(edge);
        ++revision;
    }
    return true;
}

const Node* MapSnapshot::find_node(NodeId id) const noexcept
{
    const auto iter = m_nodes.find(id);
    return iter == m_nodes.end() ? nullptr : &iter->second;
}

const Node* MapSnapshot::find_node(int floor, GridPosition position) const noexcept
{
    const auto id = make_stable_node_id(floor, position);
    return id.has_value() ? find_node(*id) : nullptr;
}

EdgeKnowledge MapSnapshot::edge_knowledge(NodeId first, NodeId second) const noexcept
{
    const Edge* edge = find_edge(first, second);
    return edge == nullptr ? EdgeKnowledge::Unknown : edge->knowledge;
}

const Edge* MapSnapshot::find_edge(NodeId first, NodeId second) const noexcept
{
    const Edge key = normalized_edge({ first, second, EdgeKnowledge::Unknown, {} });
    const auto iter = std::ranges::find_if(m_edges, [&](const Edge& edge) { return same_edge(edge, key); });
    return iter == m_edges.end() ? nullptr : &*iter;
}

std::vector<NodeId> MapSnapshot::neighbors(NodeId id, GraphLayer layer) const
{
    std::vector<NodeId> result;
    for (const auto& edge : m_edges) {
        if (!edge_visible_in_layer(edge, layer)) {
            continue;
        }
        if (edge.first == id) {
            result.emplace_back(edge.second);
        }
        else if (edge.second == id) {
            result.emplace_back(edge.first);
        }
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::unordered_set<NodeId> MapSnapshot::reveal_through_transparent_nodes(NodeId origin) const
{
    std::unordered_set<NodeId> revealed;
    if (!m_nodes.contains(origin)) {
        return revealed;
    }
    std::queue<NodeId> pending;
    pending.emplace(origin);
    revealed.emplace(origin);
    while (!pending.empty()) {
        const NodeId current = pending.front();
        pending.pop();
        for (const NodeId neighbor : neighbors(current)) {
            const Node* node = find_node(neighbor);
            if (node == nullptr || node->progress == NodeProgress::Removed) {
                continue;
            }
            // 弥散虚雾只阻止理想域内未揭示节点作为连线视野收益。直接进入的 origin
            // 已在队列中，会正常揭示本身；之前已经揭示的透明域内节点仍可传递视野。
            if (node->natural_reveal_suppressed && !node->identity_revealed) {
                continue;
            }
            const bool inserted = revealed.emplace(neighbor).second;
            if (inserted && !node->traversal.blocks_vision) {
                pending.emplace(neighbor);
            }
        }
    }
    return revealed;
}

std::unordered_set<NodeId> MapSnapshot::nodes_within_manhattan(NodeId origin, int distance) const
{
    std::unordered_set<NodeId> result;
    const Node* center = find_node(origin);
    if (center == nullptr || distance < 0) {
        return result;
    }
    for (const auto& [id, node] : m_nodes) {
        if (node.progress == NodeProgress::Removed || node.floor != center->floor) {
            continue;
        }
        const int manhattan = std::abs(node.position.row - center->position.row) +
                              std::abs(node.position.column - center->position.column);
        if (manhattan <= distance) {
            result.emplace(id);
        }
    }
    return result;
}

bool MapSnapshot::has_valid_transfer_pair(NodeId id) const noexcept
{
    const Node* first = find_node(id);
    if (first == nullptr || !is_transfer_node(first->type) || !first->transfer_target.has_value() ||
        *first->transfer_target == id) {
        return false;
    }
    const Node* second = find_node(*first->transfer_target);
    return second != nullptr && is_transfer_node(second->type) && second->transfer_target == id;
}

bool MapSnapshot::validate(std::string* error) const
{
    std::set<std::tuple<int, int, int>> positions;
    for (const auto& [id, node] : m_nodes) {
        const auto expected = make_stable_node_id(node.floor, node.position);
        if (id == InvalidNodeId || node.id != id || !expected.has_value() || *expected != id) {
            if (error != nullptr) {
                *error = "node id is invalid or does not match floor and grid position";
            }
            return false;
        }
        if (!positions.emplace(node.floor, node.position.row, node.position.column).second) {
            if (error != nullptr) {
                *error = "multiple nodes occupy the same floor and grid position";
            }
            return false;
        }
        if (node.transfer_target == id) {
            if (error != nullptr) {
                *error = "transfer pairing must be non-self";
            }
            return false;
        }
    }
    std::set<std::pair<NodeId, NodeId>> edge_keys;
    for (const auto& edge_value : m_edges) {
        const Edge edge = normalized_edge(edge_value);
        if (edge.first == edge.second || !m_nodes.contains(edge.first) || !m_nodes.contains(edge.second)) {
            if (error != nullptr) {
                *error = "edge has an invalid endpoint";
            }
            return false;
        }
        if (!edge_keys.emplace(edge.first, edge.second).second) {
            if (error != nullptr) {
                *error = "duplicate edge";
            }
            return false;
        }
        if (edge.knowledge == EdgeKnowledge::Confirmed && edge.evidence.forced_by_connectivity_constraint) {
            if (error != nullptr) {
                *error = "connectivity-constraint edge cannot be confirmed";
            }
            return false;
        }
    }
    return true;
}

std::optional<LinkedEncounterReturnResolution> resolve_linked_encounter_return(
    const MapSnapshot& before,
    const MapSnapshot& after,
    NodeId known_event_node,
    NodeId current_node,
    NodeType linked_type,
    const std::vector<NodeId>& target_hypotheses) noexcept
{
    if (current_node == InvalidNodeId || linked_type == NodeType::Unknown ||
        std::ranges::find(target_hypotheses, current_node) == target_hypotheses.end() ||
        after.find_node(current_node) == nullptr) {
        return std::nullopt;
    }

    const auto became_empty = [&](NodeId id) {
        const Node* previous = before.find_node(id);
        const Node* current = after.find_node(id);
        return id != current_node && previous != nullptr && current != nullptr &&
               previous->type != NodeType::Empty && previous->progress == NodeProgress::Active &&
               current->type == NodeType::Empty;
    };

    NodeId event_node = known_event_node;
    if (event_node != InvalidNodeId) {
        // 可控移动在进页前已经由事务锁定原事件格；回图 OCR 是否及时把它画成空地
        // 不影响身份归属，页面完成阶段会负责把当前地图语义补成空地。
        if (event_node == current_node || before.find_node(event_node) == nullptr) {
            return std::nullopt;
        }
    }
    else {
        for (const auto& [id, node] : before.nodes()) {
            (void)node;
            if (!became_empty(id)) {
                continue;
            }
            if (event_node != InvalidNodeId) {
                // 小八界没有传送前落点；多于一个节点同时变为空地时不能猜。
                return std::nullopt;
            }
            event_node = id;
        }
        if (event_node == InvalidNodeId) {
            return std::nullopt;
        }
    }

    return LinkedEncounterReturnResolution {
        .event_node = event_node,
        .linked_node = current_node,
        .linked_type = linked_type,
    };
}

bool NormalizedMap::merge(const MapObservationBatch& batch, std::string* error)
{
    return merge(batch, MapMergePurpose::CurrentObservation, error);
}

bool NormalizedMap::merge(const MapObservationBatch& batch, MapMergePurpose purpose, std::string* error)
{
    if (batch.floor < 1) {
        if (error != nullptr) {
            *error = "observation floor must be positive";
        }
        return false;
    }
    NormalizedMap working = *this;
    if (working.m_floor != 0 && working.m_floor != batch.floor) {
        working.reset();
    }
    working.m_floor = batch.floor;

    std::unordered_set<GridPosition, GridPositionHash> observed_positions;
    for (const auto& observed : batch.nodes) {
        const auto id = make_stable_node_id(batch.floor, observed.position);
        if (!id.has_value() || !observed_positions.emplace(observed.position).second) {
            if (error != nullptr) {
                *error = "observation contains an invalid or duplicate grid position";
            }
            return false;
        }

        const Node* current = working.m_snapshot.find_node(*id);
        const bool preserve_door_identity = current != nullptr && current->type == NodeType::Door &&
                                            observed.type.has_value() && *observed.type != NodeType::Door;
        const bool topology_empty_fallback =
            observed.identity_source.has_value() && *observed.identity_source == "map_topology_no_ocr_empty";
        const bool preserve_reliable_identity =
            purpose == MapMergePurpose::ExplorationNotebook && current != nullptr &&
            current->type != NodeType::Unknown && current->type != NodeType::Empty && observed.type.has_value() &&
            (*observed.type == NodeType::Unknown || *observed.type == NodeType::Empty ||
             (topology_empty_fallback && current->type != NodeType::Empty));
        // 战斗情报探查或战斗插件得到的是具体关卡名；后续地图 OCR/模板只能再次看到泛型
        // “作战/紧急作战/险路恶敌”。探索笔记不得让同类型的弱观测降级覆盖具体关卡名。
        const bool preserve_battle_stage_name =
            purpose == MapMergePurpose::ExplorationNotebook && current != nullptr &&
            !battle_stage_name(*current).empty() && observed.type.has_value() && *observed.type == current->type &&
            (!observed.name.has_value() || is_generic_battle_name(*observed.type, *observed.name));
        // 当前地图的一帧节点文字 OCR 偶发漏检时，拓扑兜底只能证明“这里有一个节点”，
        // 不能把此前由现场 OCR 已确认、仍处于 Active 的非空节点抹成林间空地。节点完成后
        // progress 会先由事件流改为 Completed，此时再接受现场空地观测。
        const bool current_has_active_observed_identity =
            purpose == MapMergePurpose::CurrentObservation && current != nullptr &&
            current->progress == NodeProgress::Active && current->type != NodeType::Unknown &&
            current->type != NodeType::Empty && current->identity_revealed && topology_empty_fallback &&
            (current->identity_source == "ocr" || current->identity_source == "move_preview_ocr" ||
             current->identity_source == "event_name" || current->identity_source == "entered_page");
        // 模板固定身份描述的是未完成时的原始地图。非重复节点结算成林间空地后，
        // 后续观测若仍因模板把它标回作战/商店，当前观测地图必须保留结算后的空地语义。
        const bool preserve_completed_empty =
            purpose == MapMergePurpose::CurrentObservation && current != nullptr &&
            current->type == NodeType::Empty && current->progress == NodeProgress::Completed &&
            current->identity_source == "node_resolution_becomes_empty" && observed.identity_source.has_value() &&
            (*observed.identity_source == "map_template_fixed_identity" ||
             *observed.identity_source == "ideal_source_emergency_prediction" ||
             *observed.identity_source == "initial_roaming_resident_prediction");
        // 移动预览 OCR 已经看到了节点的实际标题。关闭预览后的下一次地图重建仍会再次套用模板初始身份，
        // 这里必须保留预览纠正，直到地图 OCR 或节点结算给出更新的现场事实。
        const bool preserve_preview_identity =
            purpose == MapMergePurpose::CurrentObservation && current != nullptr &&
            current->identity_source == "move_preview_ocr" && observed.identity_source.has_value() &&
            *observed.identity_source == "map_template_fixed_identity";
        const bool preserve_identity =
            preserve_door_identity || preserve_reliable_identity || current_has_active_observed_identity ||
            preserve_completed_empty || preserve_preview_identity || preserve_battle_stage_name;
        Node node = current == nullptr ? Node {} : *current;
        if (current == nullptr) {
            node.id = *id;
            node.floor = batch.floor;
            node.position = observed.position;
            node.traversal = default_traversal_for(NodeType::Unknown);
        }
        if (observed.type.has_value() && !preserve_identity) {
            const bool newly_classified = node.type == NodeType::Unknown && *observed.type != NodeType::Unknown;
            node.type = *observed.type;
            if (!observed.traversal.has_value() &&
                (current == nullptr || newly_classified || current->type != *observed.type)) {
                node.traversal = default_traversal_for(node.type);
            }
        }
        if (observed.name.has_value() && !preserve_identity) {
            node.name = *observed.name;
        }
        if (observed.fate_event.has_value() && !preserve_identity) {
            node.fate_event = *observed.fate_event;
        }
        if (observed.progress.has_value()) {
            node.progress = *observed.progress;
        }
        if (observed.traversal.has_value() && !preserve_identity) {
            node.traversal = *observed.traversal;
        }
        if (observed.identity_state.has_value() && !preserve_identity) {
            node.identity_state = *observed.identity_state;
        }
        if (observed.identity_revealed.has_value() && !preserve_identity) {
            node.identity_revealed = *observed.identity_revealed;
        }
        if (observed.visually_hidden.has_value() && !preserve_identity) {
            node.visually_hidden = *observed.visually_hidden;
        }
        if (observed.identity_from_topology.has_value() && !preserve_identity) {
            node.identity_from_topology = *observed.identity_from_topology;
        }
        if (observed.identity_from_prediction.has_value() && !preserve_identity) {
            node.identity_from_prediction = *observed.identity_from_prediction;
        }
        if (observed.prediction_rule.has_value() && !preserve_identity) {
            node.prediction_rule = *observed.prediction_rule;
        }
        if (observed.natural_reveal_suppressed.has_value()) {
            node.natural_reveal_suppressed = *observed.natural_reveal_suppressed;
        }
        if (observed.existence_source.has_value()) {
            node.existence_source = *observed.existence_source;
        }
        if (observed.identity_source.has_value() && !preserve_identity) {
            node.identity_source = *observed.identity_source;
        }
        if (observed.detected_by_vision.has_value()) {
            node.detected_by_vision = *observed.detected_by_vision;
        }
        if (observed.confirmed_by_topology.has_value()) {
            node.confirmed_by_topology = *observed.confirmed_by_topology;
        }
        if (purpose == MapMergePurpose::CurrentObservation && observed.marker_type.has_value()) {
            node.marker_type = *observed.marker_type;
        }
        if (purpose == MapMergePurpose::CurrentObservation && observed.marker_display_name.has_value()) {
            node.marker_display_name = *observed.marker_display_name;
        }
        if (purpose == MapMergePurpose::CurrentObservation && observed.marker_score.has_value()) {
            node.marker_score = *observed.marker_score;
        }
        if (purpose == MapMergePurpose::CurrentObservation &&
            observed.marker_resident_overlap_possible.has_value()) {
            node.marker_resident_overlap_possible = *observed.marker_resident_overlap_possible;
        }
        if (observed.badged.has_value() && !preserve_identity) {
            node.badged = *observed.badged;
        }
        if (observed.transfer_target.has_value() && observed.transfer_target->has_value()) {
            const auto transfer_target = make_stable_node_id(batch.floor, **observed.transfer_target);
            if (!transfer_target.has_value()) {
                if (error != nullptr) {
                    *error = "observation contains an invalid transfer target";
                }
                return false;
            }
            if (!node.transfer_target.has_value()) {
                node.transfer_target = *transfer_target;
            }
        }
        if (!working.m_snapshot.upsert_node(std::move(node))) {
            if (error != nullptr) {
                *error = "failed to merge observed node";
            }
            return false;
        }
    }

    std::unordered_set<GridPosition, GridPositionHash> covered(
        batch.covered_positions.begin(),
        batch.covered_positions.end());
    if (batch.coverage == ObservationCoverage::FullMap && covered.empty()) {
        for (const auto& [id, node] : working.m_snapshot.nodes()) {
            (void)id;
            if (node.floor == batch.floor) {
                covered.emplace(node.position);
            }
        }
    }
    for (const GridPosition& position : covered) {
        if (observed_positions.contains(position)) {
            continue;
        }
        const auto id = make_stable_node_id(batch.floor, position);
        if (!id.has_value()) {
            if (error != nullptr) {
                *error = "covered observation contains an invalid grid position";
            }
            return false;
        }
        const Node* current = working.m_snapshot.find_node(*id);
        if (current == nullptr || current->type == NodeType::Door) {
            continue;
        }
        // 探索笔记记录“这个位置曾经是什么”。完整地图里未再出现的已知节点通常只是
        // 结算后变成空地，不能据此抹掉已经记录的事件/关卡身份。
        if (purpose == MapMergePurpose::ExplorationNotebook && current->type != NodeType::Unknown &&
            current->type != NodeType::Empty) {
            continue;
        }
        Node empty = *current;
        empty.type = NodeType::Empty;
        empty.name = EmptyNodeName;
        empty.progress = NodeProgress::Active;
        empty.traversal = default_traversal_for(NodeType::Empty);
        empty.identity_state = NodeIdentityState::Classified;
        empty.identity_revealed = true;
        empty.fate_event = false;
        empty.visually_hidden = false;
        empty.identity_from_topology = false;
        empty.identity_from_prediction = false;
        empty.prediction_rule.clear();
        empty.natural_reveal_suppressed = false;
        empty.existence_source = "covered_position_without_observed_node";
        empty.identity_source = "covered_position_without_observed_node";
        empty.detected_by_vision = false;
        empty.confirmed_by_topology = false;
        empty.marker_type.clear();
        empty.marker_display_name.clear();
        empty.marker_score = 0.0;
        empty.marker_resident_overlap_possible = false;
        empty.badged = false;

        working.m_snapshot.upsert_node(std::move(empty));
    }

    std::vector<NodeId> door_ids;
    for (const auto& [id, node] : working.m_snapshot.nodes()) {
        if (node.floor == batch.floor && node.type == NodeType::Door) {
            door_ids.emplace_back(id);
        }
    }
    if (door_ids.size() == 2) {
        Node first = *working.m_snapshot.find_node(door_ids[0]);
        Node second = *working.m_snapshot.find_node(door_ids[1]);
        const bool first_compatible = !first.transfer_target.has_value() || first.transfer_target == second.id;
        const bool second_compatible = !second.transfer_target.has_value() || second.transfer_target == first.id;
        if (first_compatible && second_compatible) {
            if (!first.transfer_target.has_value()) {
                first.transfer_target = second.id;
                working.m_snapshot.upsert_node(std::move(first));
            }
            if (!second.transfer_target.has_value()) {
                second.transfer_target = door_ids[0];
                working.m_snapshot.upsert_node(std::move(second));
            }
        }
    }

    std::set<std::pair<NodeId, NodeId>> observed_edges;
    for (const auto& observed : batch.edges) {
        const auto first = make_stable_node_id(batch.floor, observed.first);
        const auto second = make_stable_node_id(batch.floor, observed.second);
        if (!first.has_value() || !second.has_value()) {
            if (error != nullptr) {
                *error = "observation edge contains an invalid grid position";
            }
            return false;
        }
        const Edge normalized = normalized_edge({ *first, *second, observed.knowledge, observed.evidence });
        if (!observed_edges.emplace(normalized.first, normalized.second).second ||
            !working.m_snapshot.upsert_edge(normalized)) {
            if (error != nullptr) {
                *error = "observation edge endpoints must reference merged nodes and be unique";
            }
            return false;
        }
    }

    std::vector<Edge> missing_edges;
    for (const auto& edge : working.m_snapshot.edges()) {
        if (observed_edges.contains({ edge.first, edge.second })) {
            continue;
        }
        const Node* first = working.m_snapshot.find_node(edge.first);
        const Node* second = working.m_snapshot.find_node(edge.second);
        const bool edge_covered = batch.coverage == ObservationCoverage::FullMap ||
                                  (first != nullptr && second != nullptr && covered.contains(first->position) &&
                                   covered.contains(second->position));
        const bool persistent_observed_extra =
            edge.knowledge == EdgeKnowledge::Confirmed && edge.evidence.decision_source == "observed_extra_edge";
        if (edge_covered && edge.knowledge != EdgeKnowledge::Absent &&
            !edge.evidence.forced_by_connectivity_constraint && !persistent_observed_extra) {
            Edge absent = edge;
            absent.knowledge = EdgeKnowledge::Absent;
            missing_edges.emplace_back(std::move(absent));
        }
    }
    for (const Edge& edge : missing_edges) {
        working.m_snapshot.upsert_edge(edge);
    }
    *this = std::move(working);
    return true;
}

void NormalizedMap::reset()
{
    m_floor = 0;
    m_snapshot = MapSnapshot {};
}

void ViewportObservation::replace(
    std::vector<NodeObservation> observations,
    std::uint64_t map_revision,
    std::uint64_t viewport_revision)
{
    m_nodes.clear();
    for (auto& observation : observations) {
        if (observation.node != InvalidNodeId && !observation.icon_rect.empty()) {
            m_nodes.insert_or_assign(observation.node, std::move(observation));
        }
    }
    m_map_revision = map_revision;
    m_viewport_revision = viewport_revision;
}

void ViewportObservation::clear(std::uint64_t map_revision, std::uint64_t viewport_revision)
{
    m_nodes.clear();
    m_map_revision = map_revision;
    m_viewport_revision = viewport_revision;
}

const NodeObservation* ViewportObservation::find(NodeId node) const noexcept
{
    const auto iter = m_nodes.find(node);
    return iter == m_nodes.end() ? nullptr : &iter->second;
}

std::optional<Rect> ViewportObservation::clickable_rect(
    NodeId node,
    std::uint64_t expected_map_revision,
    std::uint64_t expected_viewport_revision) const
{
    if (expected_map_revision != m_map_revision || expected_viewport_revision != m_viewport_revision) {
        return std::nullopt;
    }
    const NodeObservation* observation = find(node);
    return observation == nullptr ? std::nullopt : std::optional<Rect>(observation->icon_rect);
}

bool ViewportObservation::rebind_map_revision_after_semantic_update(
    std::uint64_t expected_previous_revision,
    std::uint64_t updated_revision) noexcept
{
    if (m_map_revision != expected_previous_revision || updated_revision < expected_previous_revision) {
        return false;
    }
    m_map_revision = updated_revision;
    return true;
}

NodeTraversal default_traversal_for(NodeType type) noexcept
{
    switch (type) {
    case NodeType::Empty:
        // 林间空地可以成为一次移动的落点，也可以重复进入；它仍然对徒步路径透明且不触发页面。
        return { false, false, true, true };
    case NodeType::Door:
        return { false, false, true, true };
    case NodeType::Shop:
    case NodeType::ScrapShop:
        return { true, true, true, true };
    default:
        return {};
    }
}

bool completed_node_becomes_empty(bool repeatable, std::optional<bool> explicit_becomes_empty) noexcept
{
    return explicit_becomes_empty.value_or(!repeatable);
}

bool should_apply_revealed_preview_identity(const Node& current, const MovePreview& preview) noexcept
{
    return preview.identity_revealed && preview.displayed_type != NodeType::Unknown &&
           (!current.identity_revealed || current.type != preview.displayed_type ||
            !preview_identity_names_equal(current.name, preview.displayed_name));
}

bool preview_confirms_roaming_resident(const Node& current, const MovePreview& preview) noexcept
{
    if (!current.identity_revealed || is_route_battle_node_type(current.type) ||
        preview.displayed_type != NodeType::BattleNormal || !preview.identity_revealed) {
        return false;
    }
    // 只信任已经由现场文字/页面确认的节点身份。模板或规则身份与预览冲突时仍应走
    // 原来的身份纠正流程，不能一概解释成流窜“居民”。
    const bool reliable_non_battle_identity =
        current.identity_source == "ocr" || current.identity_source == "move_preview_ocr" ||
        current.identity_source == "event_name" || current.identity_source == "entered_page";
    return reliable_non_battle_identity;
}

bool is_transfer_node(NodeType type) noexcept
{
    return type == NodeType::Door;
}

PageContentEffect classify_page_content_effect(std::string_view source, std::string_view content) noexcept
{
    if (source != "RoguelikeEvent") {
        return {};
    }

    static constexpr std::array<std::string_view, 28> IncidentEvents = {
        "桑尼的邀请",
        "色味不同源",
        "货从口出",
        "沉重的契约",
        "敲动杠杆",
        "血衣之下",
        "擒与缚",
        "沉寂之屋",
        "黑诞",
        "呼吸的红苔",
        "被歌颂的影子",
        "愈创之心",
        "思乡心切",
        "划算买卖",
        "鸭托邦",
        "传奇团伙",
        "湖中仙女",
        "洞中宝",
        "临时中介所",
        "和平守卫者",
        "和平守卫者-2",
        "独活",
        "独活-2",
        "线人",
        "泪之聚落",
        "好奇心之死",
        "窥视箱中",
        "调谐仪式",
    };
    if (std::ranges::find(IncidentEvents, content) != IncidentEvents.end()) {
        return { NodeType::Incident, false, true };
    }
    if (content == "未涉足之树") {
        return { NodeType::Expedition, false, true };
    }
    if (content == "回滚文明") {
        return { NodeType::Sacrifice, false, true };
    }
    if (content == "无人商店" || content == "无人商店-2") {
        return { NodeType::Wish, false, true };
    }
    if (content == "溯源") {
        return { NodeType::Portal, false, true };
    }
    if (content == "原始娱乐" || content == "掠夺成性") {
        return { NodeType::Duel, false, true };
    }
    if (content == "金色凝滞") {
        return { NodeType::Rest, false, true };
    }
    if (content == "三重身") {
        return { NodeType::Evacuate, true, true };
    }
    if (content == "险路尽头") {
        return { NodeType::Final, false, true };
    }
    if (content == "安眠一隅") {
        return { std::nullopt, true, false };
    }
    return {};
}

bool is_combat_node_type(NodeType type) noexcept
{
    return is_route_battle_node_type(type);
}

// 能离开当前区域进入下一层的三种节点。险路小径通常亏损，一般不走，
// 但它会阻断通往险路尽头的路径，所以必须算作出口，否则阻断时会被判定成无路可走。
// 走不走由倾向决定，这里只回答能不能走。
bool is_exit_node_type(NodeType type) noexcept
{
    return type == NodeType::Final || type == NodeType::BattleBoss || type == NodeType::Evacuate;
}

std::optional<NodeType> node_type_from_string(std::string_view value) noexcept
{
    static const std::unordered_map<std::string_view, NodeType> Mapping = {
        { "unclassified", NodeType::Unknown },
        { "battle_elite", NodeType::BattleElite },
        { "battle_normal", NodeType::BattleNormal },
        { "battle_savage", NodeType::BattleSavage },
        { "duel", NodeType::Duel },
        { "door", NodeType::Door },
        { "employ", NodeType::Employ },
        { "expedition", NodeType::Expedition },
        { "hide_battle", NodeType::HideBattle },
        { "hide_invisible", NodeType::HideInvisible },
        { "incident", NodeType::Incident },
        { "light", NodeType::Light },
        { "portal", NodeType::Portal },
        { "rest", NodeType::Rest },
        { "sacrifice", NodeType::Sacrifice },
        { "scrap_shop", NodeType::ScrapShop },
        { "shop", NodeType::Shop },
        { "wish", NodeType::Wish },
        { "empty", NodeType::Empty },
        { "evacuate", NodeType::Evacuate },
        { "final", NodeType::Final },
        { "battle_boss", NodeType::BattleBoss },
    };
    const auto found = Mapping.find(value);
    return found == Mapping.end() ? std::nullopt : std::optional<NodeType>(found->second);
}

bool node_type_allowed(const MovementSpec& movement, NodeType type) noexcept
{
    return std::ranges::find(movement.target_types, type) != movement.target_types.end();
}

const std::vector<MovementSpec>& movement_specs()
{
    static const std::vector<MovementSpec> Specs = {
        { MovementKind::Walk, "walk", "徒步跋涉", MovementRange::WalkEdges, AllTargetTypes, 1, -1, false, false, {} },
        { MovementKind::M01,
          "rogue_6_scrap_M_01",
          "报废轮子",
          MovementRange::OrthogonalTwo,
          AllTargetTypes,
          1,
          1,
          false,
          false,
          {} },
        { MovementKind::M02,
          "rogue_6_scrap_M_02",
          "报废假肢",
          MovementRange::SurroundingEight,
          AllTargetTypes,
          1,
          1,
          false,
          false,
          {} },
        { MovementKind::M03,
          "rogue_6_scrap_M_03",
          "标准引擎",
          MovementRange::SurroundingEight,
          AllTargetTypes,
          1,
          3,
          false,
          false,
          {} },
        { MovementKind::M04,
          "rogue_6_scrap_M_04",
          "重弹簧",
          MovementRange::ManhattanTwo,
          AllTargetTypes,
          0,
          1,
          false,
          true,
          {} },
        { MovementKind::M05,
          "rogue_6_scrap_M_05",
          "气垫底座",
          MovementRange::ManhattanTwo,
          AllTargetTypes,
          1,
          2,
          false,
          false,
          {} },
        { MovementKind::M06,
          "rogue_6_scrap_M_06",
          "试作外骨骼",
          MovementRange::OrthogonalThree,
          AllTargetTypes,
          1,
          2,
          false,
          false,
          {} },
        { MovementKind::M07,
          "rogue_6_scrap_M_07",
          "小八界",
          MovementRange::FullMap,
          NonCombatTargetTypes,
          0,
          1,
          true,
          true,
          {} },
        { MovementKind::M08,
          "rogue_6_scrap_M_08",
          "一次性喷气背包",
          MovementRange::FullMap,
          AllTargetTypes,
          0,
          1,
          false,
          false,
          {} },
        { MovementKind::M09,
          "rogue_6_scrap_M_09",
          "老妈妈的融雪",
          MovementRange::FullMap,
          NonCombatTargetTypes,
          1,
          1,
          false,
          false,
          { 0, 2, 0 } },
        { MovementKind::M10,
          "rogue_6_scrap_M_10",
          "坎诺特的触须",
          MovementRange::FullMap,
          ShopTargetTypes,
          1,
          2,
          false,
          false,
          { 0, 0, 4 } },
        { MovementKind::M11,
          "rogue_6_scrap_M_11",
          "结构性原理",
          MovementRange::FullMap,
          AllTargetTypes,
          1,
          3,
          false,
          false,
          {} },
        { MovementKind::M12,
          "rogue_6_scrap_M_12",
          "简易遥控器",
          MovementRange::SurroundingEight,
          AllTargetTypes,
          0,
          1,
          false,
          false,
          { 3, 0, 0 } },
    };
    return Specs;
}

int action_points_after(int current, int cost, int gain) noexcept
{
    const std::int64_t result = static_cast<std::int64_t>(current) - cost + gain;
    return static_cast<int>(
        std::clamp<std::int64_t>(result, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

const MovementSpec* find_movement_spec(MovementKind kind) noexcept
{
    const auto& specs = movement_specs();
    const auto iter = std::ranges::find_if(specs, [&](const MovementSpec& spec) { return spec.kind == kind; });
    return iter == specs.end() ? nullptr : &*iter;
}

int DynamicCostModel::movement_cost(const MovementSpec& movement, std::size_t walked_edges) const noexcept
{
    if (movement.kind == MovementKind::Walk) {
        const std::int64_t cost = static_cast<std::int64_t>(walk_cost_per_edge) * walked_edges;
        return static_cast<int>(std::clamp<std::int64_t>(cost, 0, std::numeric_limits<int>::max()));
    }
    const auto override_value = movement_cost_overrides.find(movement.kind);
    return override_value == movement_cost_overrides.end() ? movement.action_point_cost : override_value->second;
}

int DynamicCostModel::action_cost(std::string_view action_id, int fallback) const noexcept
{
    const auto found = action_cost_overrides.find(std::string(action_id));
    return found == action_cost_overrides.end() ? fallback : found->second;
}

bool DynamicCostModel::clear_action_cost_overrides() noexcept
{
    if (action_cost_overrides.empty()) {
        return false;
    }
    action_cost_overrides.clear();
    ++revision;
    return true;
}

bool DynamicCostModel::validate(std::string* error) const
{
    if (walk_cost_per_edge < 0) {
        if (error != nullptr) {
            *error = "walk edge cost must be non-negative";
        }
        return false;
    }
    for (const auto& [movement, cost] : movement_cost_overrides) {
        if (movement == MovementKind::Walk || find_movement_spec(movement) == nullptr || cost < 0) {
            if (error != nullptr) {
                *error = "movement cost override is invalid";
            }
            return false;
        }
    }
    for (const auto& [action_id, cost] : action_cost_overrides) {
        if (action_id.empty() || cost < 0) {
            if (error != nullptr) {
                *error = "action cost override is invalid";
            }
            return false;
        }
    }
    return true;
}

bool is_in_geometric_range(
    const MapSnapshot& map,
    NodeId source,
    NodeId target,
    const MovementSpec& movement,
    GraphLayer layer)
{
    const Node* source_node = map.find_node(source);
    const Node* target_node = map.find_node(target);
    if (source_node == nullptr || target_node == nullptr || target_node->progress == NodeProgress::Removed ||
        source_node->floor != target_node->floor) {
        return false;
    }
    // 地图重建后，只要当前位置仍显示为非空节点，它也是合法的下一次选择目标。
    // 徒步的原地动作由动作枚举器单独生成；所有加工品都把原地视为几何范围内。
    if (source == target) {
        return movement.kind != MovementKind::Walk && target_node->type != NodeType::Empty;
    }
    const int row_delta = std::abs(target_node->position.row - source_node->position.row);
    const int column_delta = std::abs(target_node->position.column - source_node->position.column);
    switch (movement.range) {
    case MovementRange::WalkEdges: {
        const Edge* edge = map.find_edge(source, target);
        return edge != nullptr && edge_visible_in_layer(*edge, layer);
    }
    case MovementRange::OrthogonalTwo:
        return (row_delta == 0 || column_delta == 0) && row_delta + column_delta <= 2;
    case MovementRange::SurroundingEight:
        return row_delta <= 1 && column_delta <= 1;
    case MovementRange::ManhattanTwo:
        return row_delta + column_delta <= 2;
    case MovementRange::OrthogonalThree:
        return (row_delta == 0 || column_delta == 0) && row_delta + column_delta <= 3;
    case MovementRange::FullMap:
        return true;
    }
    return false;
}

std::vector<NodeId>
    enumerate_geometric_targets(const MapSnapshot& map, NodeId source, const MovementSpec& movement, GraphLayer layer)
{
    const RunState empty_state;
    std::vector<NodeId> result;
    for (const auto& [id, node] : map.nodes()) {
        if (is_targetable(node, empty_state) && is_in_geometric_range(map, source, id, movement, layer)) {
            result.emplace_back(id);
        }
    }
    std::ranges::sort(result);
    return result;
}

NodeId resolve_landing(const MapSnapshot& map, NodeId target) noexcept
{
    const Node* node = map.find_node(target);
    if (node == nullptr || !is_transfer_node(node->type)) {
        return target;
    }
    return map.has_valid_transfer_pair(target) ? *node->transfer_target : InvalidNodeId;
}

// 注意：BlackFlowCompactStateSpace::actions 是本函数的位掩码紧凑翻版，两份必须保持语义一致，
// 否则按需图与紧凑空间对同一局面会给出不同的动作集，安全值与路线互相矛盾。
// 已知差异：那边以 opened_blockers 判定 walk 穿行（这里用 visited_nodes）、在生成期过滤
// forbidden 动作（这里由 OnDemandStateGraph 展开期过滤）。改动任一份时同步检查另一份。
std::vector<MoveAction> enumerate_move_actions(const MapSnapshot& map, const RunState& state, GraphLayer layer)
{
    std::vector<MoveAction> result;
    if (state.resources.action_points < 1 || map.find_node(state.current_node) == nullptr) {
        return result;
    }

    const MovementSpec* walk = find_movement_spec(MovementKind::Walk);
    if (walk != nullptr) {
        struct WalkFrontier
        {
            NodeId node = InvalidNodeId;
            std::vector<NodeId> path;
        };

        std::deque<WalkFrontier> queue;
        std::unordered_map<NodeId, std::size_t> walk_action_indices;
        std::unordered_set<NodeId> expanded;
        const Node* source_node = map.find_node(state.current_node);
        // 非空可重复节点允许原地再次进入；是否值得这么做由路线排序决定，不能在动作
        // 枚举层把游戏中的合法动作删掉。
        if (source_node != nullptr && source_node->type != NodeType::Empty &&
            is_targetable(*source_node, state) && node_type_allowed(*walk, source_node->type)) {
            MoveAction action;
            action.candidate.action_id =
                "walk:" + std::to_string(state.current_node) + ":" + std::to_string(state.current_node);
            action.candidate.movement = MovementKind::Walk;
            action.candidate.source = state.current_node;
            action.candidate.target = state.current_node;
            action.candidate.landing = resolve_landing(map, state.current_node);
            action.candidate.graph_layer = layer;
            if (action.candidate.landing != InvalidNodeId) {
                // 路径保留一个当前位置元素，使原地选择仍按一次徒步计费，并在诊断中可见。
                action.candidate.path = { state.current_node };
                if (source_node->identity_state == NodeIdentityState::Unclassified) {
                    action.candidate.first_unclassified = state.current_node;
                }
                action.candidate.predicted_action_point_cost = state.costs.action_cost(
                    action.candidate.action_id,
                    state.costs.movement_cost(*walk, action.candidate.path.size()));
                action.candidate.predicted_action_point_gain = predicted_node_gain(*source_node, state);
                action.candidate.possible_landings.emplace_back(action.candidate.landing);
                action.candidate.landing_node_types.emplace(action.candidate.landing, source_node->type);
                action.candidate.landing_action_point_gains.emplace(
                    action.candidate.landing,
                    action.candidate.predicted_action_point_gain);
                action.candidate.terminal_on_completion = is_exit_node_type(source_node->type);
                action.possible_landings.emplace_back(action.candidate.landing);
                walk_action_indices.emplace(state.current_node, result.size());
                result.emplace_back(std::move(action));
            }
        }
        queue.push_back({ state.current_node, {} });
        expanded.emplace(state.current_node);
        while (!queue.empty()) {
            WalkFrontier current = std::move(queue.front());
            queue.pop_front();
            for (const NodeId neighbor : map.neighbors(current.node, layer)) {
                if (std::ranges::find(current.path, neighbor) != current.path.end() || neighbor == state.current_node) {
                    continue;
                }
                const Node* node = map.find_node(neighbor);
                if (node == nullptr || effective_progress(*node, state) == NodeProgress::Removed) {
                    continue;
                }
                auto path = current.path;
                path.emplace_back(neighbor);
                if (is_targetable(*node, state) && node_type_allowed(*walk, node->type)) {
                    MoveAction action;
                    action.candidate.action_id =
                        "walk:" + std::to_string(state.current_node) + ":" + std::to_string(neighbor);
                    action.candidate.movement = MovementKind::Walk;
                    action.candidate.source = state.current_node;
                    action.candidate.target = neighbor;
                    action.candidate.landing = resolve_landing(map, neighbor);
                    action.candidate.graph_layer = layer;
                    if (action.candidate.landing != InvalidNodeId) {
                        action.candidate.path = path;
                        NodeId previous = state.current_node;
                        for (const NodeId step : path) {
                            const Edge* path_edge = map.find_edge(previous, step);
                            if (path_edge != nullptr) {
                                action.candidate.uses_unconfirmed_edge =
                                    action.candidate.uses_unconfirmed_edge ||
                                    path_edge->knowledge != EdgeKnowledge::Confirmed ||
                                    path_edge->evidence.forced_by_connectivity_constraint;
                                action.candidate.uses_inferred_edge =
                                    action.candidate.uses_inferred_edge ||
                                    path_edge->evidence.forced_by_connectivity_constraint;
                            }
                            const Node* path_node = map.find_node(step);
                            if (!action.candidate.first_unclassified.has_value() && path_node != nullptr &&
                                path_node->identity_state == NodeIdentityState::Unclassified) {
                                action.candidate.first_unclassified = step;
                            }
                            previous = step;
                        }
                        action.candidate.predicted_action_point_cost = state.costs.action_cost(
                            action.candidate.action_id,
                            state.costs.movement_cost(*walk, path.size()));
                        action.candidate.predicted_action_point_gain = predicted_node_gain(*node, state);
                        action.candidate.possible_landings.emplace_back(action.candidate.landing);
                        action.candidate.landing_node_types.emplace(action.candidate.landing, node->type);
                        action.candidate.landing_action_point_gains.emplace(
                            action.candidate.landing,
                            action.candidate.predicted_action_point_gain);
                        action.candidate.terminal_on_completion = is_exit_node_type(node->type);
                        action.possible_landings.emplace_back(action.candidate.landing);
                        const auto existing_action = walk_action_indices.find(neighbor);
                        if (existing_action == walk_action_indices.end()) {
                            walk_action_indices.emplace(neighbor, result.size());
                            result.emplace_back(std::move(action));
                        }
                        else if (action.candidate.path.size() < result[existing_action->second].candidate.path.size()) {
                            result[existing_action->second] = std::move(action);
                        }
                    }
                }
                if (is_walk_transparent(*node, state, layer) && expanded.emplace(neighbor).second) {
                    queue.push_back({ neighbor, std::move(path) });
                }
            }
        }
    }

    for (const auto& movement : movement_specs()) {
        if (movement.kind == MovementKind::Walk) {
            continue;
        }
        const auto charge = state.resources.movement_charges.find(movement.kind);
        if (charge == state.resources.movement_charges.end() || charge->second <= 0 ||
            state.cross_floor_expired.contains(movement.kind)) {
            continue;
        }

        std::vector<NodeId> targets;
        std::vector<NodeId> hidden_noncombat_targets;
        for (const auto& [id, node] : map.nodes()) {
            const NodeType semantic_type = effective_node_type(node, state);
            const NodeType visible_type = movement_visible_node_type(semantic_type, node.visually_hidden);
            const bool targetable = semantic_type == NodeType::Empty
                                        ? effective_progress(node, state) != NodeProgress::Removed
                                        : is_targetable(node, state);
            if (!targetable || !node_type_allowed(movement, visible_type) ||
                !movement_target_is_currently_selectable(movement.kind, node.visually_hidden) ||
                (movement.random_target && node_has_explicit_roaming_resident_marker(node)) ||
                !is_in_geometric_range(map, state.current_node, id, movement, GraphLayer::Confirmed)) {
                continue;
            }
            targets.emplace_back(id);
            if (movement.random_target && visible_type == NodeType::HideInvisible) {
                hidden_noncombat_targets.emplace_back(id);
            }
        }
        // 小八界优先随机到仍显示为“未知的诡秘”的非作战节点；只有不存在这种候选时，
        // 才退化为全部合法非作战节点。明确带流窜居民标记的节点在建立这两个候选池前
        // 已经排除，可能重叠但未确认的标记仍须保留。
        if (movement.random_target && !hidden_noncombat_targets.empty()) {
            targets = std::move(hidden_noncombat_targets);
        }
        std::ranges::sort(targets);
        if (targets.empty()) {
            continue;
        }

        if (movement.random_target) {
            MoveAction action;
            action.candidate.action_id = std::string(movement.id) + ":random:" + std::to_string(state.current_node);
            action.candidate.movement = movement.kind;
            action.candidate.source = state.current_node;
            // 随机移动仍要在地图上点击一个受该移动方式高亮的节点来打开预览；这个节点只负责
            // 触发 UI，实际落点继续由 possible_landings 的完整随机池描述。
            action.candidate.target = targets.front();
            action.candidate.predicted_action_point_cost =
                state.costs.action_cost(action.candidate.action_id, state.costs.movement_cost(movement));
            action.candidate.predicted_action_point_gain = movement.effect.action_point_gain;
            action.candidate.controllable = false;
            for (const NodeId target : targets) {
                const NodeId landing = resolve_landing(map, target);
                const Node* target_node = map.find_node(target);
                if (landing != InvalidNodeId && target_node != nullptr) {
                    action.possible_landings.emplace_back(landing);
                    action.candidate.landing_node_types.insert_or_assign(
                        landing,
                        effective_node_type(*target_node, state));
                    action.candidate.landing_action_point_gains.insert_or_assign(
                        landing,
                        movement.effect.action_point_gain + predicted_node_gain(*target_node, state));
                }
            }
            std::ranges::sort(action.possible_landings);
            action.possible_landings.erase(
                std::unique(action.possible_landings.begin(), action.possible_landings.end()),
                action.possible_landings.end());
            if (!action.possible_landings.empty()) {
                action.candidate.possible_landings = action.possible_landings;
                action.candidate.terminal_on_completion = std::ranges::all_of(
                    action.possible_landings,
                    [&](NodeId landing) { return move_landing_is_terminal(action.candidate, landing); });
                result.emplace_back(std::move(action));
            }
            continue;
        }

        for (const NodeId target : targets) {
            const Node* target_node = map.find_node(target);
            if (target_node == nullptr) {
                continue;
            }
            const NodeId landing = resolve_landing(map, target);
            if (landing == InvalidNodeId) {
                continue;
            }
            MoveAction action;
            action.candidate.action_id = std::string(movement.id) + ":" + std::to_string(state.current_node) + ":" +
                                         std::to_string(target) + ":" + std::to_string(landing);
            action.candidate.movement = movement.kind;
            action.candidate.source = state.current_node;
            action.candidate.target = target;
            action.candidate.landing = landing;
            action.candidate.path = { target };
            action.candidate.predicted_action_point_cost =
                state.costs.action_cost(action.candidate.action_id, state.costs.movement_cost(movement));
            action.candidate.predicted_action_point_gain =
                movement.effect.action_point_gain + predicted_node_gain(*target_node, state);
            action.candidate.possible_landings.emplace_back(landing);
            action.candidate.landing_node_types.emplace(landing, effective_node_type(*target_node, state));
            action.candidate.landing_action_point_gains.emplace(landing, action.candidate.predicted_action_point_gain);
            action.candidate.controllable = true;
            action.candidate.terminal_on_completion = is_exit_node_type(target_node->type);
            action.possible_landings.emplace_back(landing);
            result.emplace_back(std::move(action));
        }
    }

    // “险路尽头”同一次地图移动有两种页面结算：进入下一层，或选择第三项路过并
    // 回到本层。规划器必须看到两个语义不同、成本相同的动作，才能决定页面选项。
    const std::size_t physical_action_count = result.size();
    for (std::size_t index = 0; index < physical_action_count; ++index) {
        const bool can_land_on_final = std::ranges::any_of(
            result[index].candidate.possible_landings,
            [&](NodeId landing) { return move_landing_type(result[index].candidate, landing) == NodeType::Final; });
        if (!can_land_on_final) {
            continue;
        }
        MoveAction bypass = result[index];
        bypass.candidate.action_id += ":bypass-final";
        bypass.candidate.terminal_on_completion = false;
        bypass.candidate.bypass_final_on_completion = true;
        result.emplace_back(std::move(bypass));
    }

    struct ConfirmedWalk
    {
        NodeId target = InvalidNodeId;
        NodeId landing = InvalidNodeId;
        int action_point_cost = 0;
        int action_point_gain = 0;
    };

    std::vector<ConfirmedWalk> confirmed_walks;
    for (const MoveAction& action : result) {
        if (action.candidate.movement != MovementKind::Walk || action.candidate.path.size() != 1 ||
            action.candidate.uses_unconfirmed_edge || action.candidate.uses_inferred_edge) {
            continue;
        }
        const Node* source = map.find_node(action.candidate.source);
        const Node* target = map.find_node(action.candidate.target);
        if (source == nullptr || target == nullptr) {
            continue;
        }
        const bool self_walk = action.candidate.source == action.candidate.target;
        if (!self_walk) {
            const Edge* edge = map.find_edge(action.candidate.source, action.candidate.target);
            if (edge == nullptr || !edge_visible_in_layer(*edge, GraphLayer::Confirmed) ||
                std::abs(source->position.row - target->position.row) +
                        std::abs(source->position.column - target->position.column) !=
                    1) {
                continue;
            }
        }
        confirmed_walks.emplace_back(
            ConfirmedWalk {
                action.candidate.target,
                action.candidate.landing,
                action.candidate.predicted_action_point_cost,
                action.candidate.predicted_action_point_gain,
            });
    }
    std::erase_if(result, [&](const MoveAction& action) {
        const MovementKind kind = action.candidate.movement;
        if (kind == MovementKind::Walk || kind == MovementKind::M07 || kind == MovementKind::M12 ||
            !action.candidate.controllable || action.candidate.predicted_action_point_cost <= 0) {
            return false;
        }
        const MovementSpec* movement = find_movement_spec(kind);
        if (movement == nullptr || movement->effect.action_point_gain != 0 || movement->effect.hope_gain != 0 ||
            movement->effect.ingot_gain != 0) {
            return false;
        }
        return std::ranges::any_of(confirmed_walks, [&](const ConfirmedWalk& walk_action) {
            return walk_action.target == action.candidate.target && walk_action.landing == action.candidate.landing &&
                   walk_action.action_point_cost <= action.candidate.predicted_action_point_cost &&
                   walk_action.action_point_gain >= action.candidate.predicted_action_point_gain;
        });
    });
    return result;
}

std::optional<MoveTransaction> MoveTransaction::propose(
    MoveCandidate proposal,
    const MapSnapshot& map,
    const ViewportObservation& viewport,
    std::string* error)
{
    if (proposal.source == InvalidNodeId || map.find_node(proposal.source) == nullptr) {
        if (error != nullptr) {
            *error = "move proposal references an invalid source node";
        }
        return std::nullopt;
    }
    if (proposal.target == InvalidNodeId || map.find_node(proposal.target) == nullptr) {
        if (error != nullptr) {
            *error = "move proposal references an invalid preview target node";
        }
        return std::nullopt;
    }
    if (proposal.controllable && proposal.landing == InvalidNodeId) {
        if (error != nullptr) {
            *error = "controllable move proposal references an invalid target node";
        }
        return std::nullopt;
    }
    if (!proposal.controllable && proposal.possible_landings.empty()) {
        if (error != nullptr) {
            *error = "uncontrollable move proposal has no possible landing";
        }
        return std::nullopt;
    }
    if (!viewport.clickable_rect(proposal.target, map.revision, viewport.viewport_revision()).has_value()) {
        if (error != nullptr) {
            *error = "move proposal has no current viewport coordinate";
        }
        return std::nullopt;
    }
    MoveTransaction transaction;
    transaction.m_source_floor = map.find_node(proposal.source)->floor;
    if (proposal.controllable) {
        const NodeId destination = proposal.landing == InvalidNodeId ? proposal.target : proposal.landing;
        if (const Node* target = map.find_node(destination); target != nullptr) {
            transaction.m_target_type = target->type;
        }
    }
    transaction.m_proposal = std::move(proposal);
    transaction.m_map_revision = map.revision;
    transaction.m_viewport_revision = viewport.viewport_revision();
    return transaction;
}

bool MoveTransaction::record_preview(MovePreview preview, std::string* error)
{
    if (m_stage != MoveTransactionStage::Proposed || preview.exact_action_point_cost < 0) {
        if (error != nullptr) {
            *error = "preview is invalid for the current transaction stage";
        }
        return false;
    }
    if (preview.reachability == PreviewReachability::Unknown) {
        if (error != nullptr) {
            *error = "preview did not determine reachability";
        }
        return false;
    }
    m_preview = std::move(preview);
    if (m_preview->reachability != PreviewReachability::Reachable) {
        m_stage = MoveTransactionStage::Cancelled;
        return true;
    }
    m_stage = MoveTransactionStage::Previewed;
    return true;
}

bool MoveTransaction::commit(
    std::uint64_t current_map_revision,
    std::uint64_t current_viewport_revision,
    std::string* error)
{
    if (m_stage != MoveTransactionStage::Previewed || !m_preview.has_value() ||
        m_preview->reachability != PreviewReachability::Reachable) {
        if (error != nullptr) {
            *error = "only a reachable previewed transaction can be committed";
        }
        return false;
    }
    if (current_map_revision != m_map_revision || current_viewport_revision != m_viewport_revision) {
        m_stage = MoveTransactionStage::Invalidated;
        if (error != nullptr) {
            *error = "map or viewport revision changed before commit";
        }
        return false;
    }
    m_stage = MoveTransactionStage::Committed;
    return true;
}

bool MoveTransaction::mark_page_resolved(std::string* error)
{
    if (m_stage == MoveTransactionStage::PageResolved) {
        return true;
    }
    if (m_stage != MoveTransactionStage::Committed) {
        if (error != nullptr) {
            *error = "node page can only resolve a committed transaction";
        }
        return false;
    }
    m_stage = MoveTransactionStage::PageResolved;
    return true;
}

// 确认这次移动是否真的落到了预期的地方。两种都算成功：
// 留在本层且落点符合预期，或者走的是出口、观察到已经身处下一层。
bool MoveTransaction::observe(MoveObservation observation, std::string* error)
{
    const bool returned_to_same_floor = observation.floor == m_source_floor;
    const bool landing_matches = m_proposal.controllable
                                     ? observation.current_node == m_proposal.landing
                                     : std::ranges::find(m_proposal.possible_landings, observation.current_node) !=
                                           m_proposal.possible_landings.end();
    const bool linked_origin_matches = observation.linked_encounter_origin_node != InvalidNodeId &&
                                       observation.linked_encounter_origin_node != observation.current_node &&
                                       (m_proposal.controllable
                                            ? observation.linked_encounter_origin_node == m_proposal.landing
                                            : std::ranges::find(
                                                  m_proposal.possible_landings,
                                                  observation.linked_encounter_origin_node) !=
                                                  m_proposal.possible_landings.end());
    const bool relocated_by_resolved_linked_encounter = m_stage == MoveTransactionStage::PageResolved &&
                                                        returned_to_same_floor && linked_origin_matches;
    // 走出口之后落点在下一层，本层的 landing 无从比对。定向移动可直接使用 target 类型；
    // 小八界则由已经接管的节点页面把实际落点语义写入 landed_type。
    const bool observed_terminal = is_exit_node_type(m_target_type) || is_exit_node_type(observation.landed_type);
    const bool advanced_after_terminal = observation.floor == m_source_floor + 1 && observed_terminal;
    // 第三层追猎既可能发生在节点页面结算之后，也可能在最后一点行动力耗尽时抢先于落点页面触发。
    // 胜利时不会再回到第三层地图，所以最后一次非出口移动只能用经生命周期确认的四层首观测来收尾；
    // 没有显式确认的普通跨层仍按不匹配处理。
    const bool advanced_after_adapted_pursuit = observation.floor == m_source_floor + 1 &&
                                                 observation.advanced_via_adapted_pursuit;
    // 未揭示的诡秘在事务创建时还不是出口类型，但页面结算可能把探索直接送进下一层。
    // 只有会话同时确认“页面已结算”和“NextLevel 恰好推进一层”才会设置该信号。
    const bool advanced_after_resolved_page = m_stage == MoveTransactionStage::PageResolved &&
                                              observation.floor == m_source_floor + 1 &&
                                              observation.advanced_via_resolved_page;
    // “追忆”会让四层终点进入另一张四层地图。楼层不变且新图入口不可能等于旧图终点，
    // 因而只能由会话根据刚完成的 NextLevel 生命周期显式授权。
    const bool renewed_same_floor_after_terminal = m_stage == MoveTransactionStage::PageResolved &&
                                                   observation.floor == m_source_floor &&
                                                   observed_terminal &&
                                                   observation.renewed_same_floor_after_terminal;
    const bool stage_accepts_observation =
        m_stage == MoveTransactionStage::Committed || m_stage == MoveTransactionStage::PageResolved;
    if (!stage_accepts_observation || observation.viewport_revision <= m_viewport_revision ||
        !((returned_to_same_floor && landing_matches) || advanced_after_terminal ||
          advanced_after_adapted_pursuit || advanced_after_resolved_page || renewed_same_floor_after_terminal)) {
        if (relocated_by_resolved_linked_encounter && stage_accepts_observation &&
            observation.viewport_revision > m_viewport_revision) {
            m_observation = std::move(observation);
            m_stage = MoveTransactionStage::Observed;
            return true;
        }
        if (error != nullptr) {
            *error = "next map observation does not match the committed move";
        }
        return false;
    }
    m_observation = std::move(observation);
    m_stage = MoveTransactionStage::Observed;
    return true;
}

int MoveTransaction::authoritative_cost() const noexcept
{
    return m_preview.has_value() && m_preview->reachability == PreviewReachability::Reachable
               ? m_preview->exact_action_point_cost
               : m_proposal.predicted_action_point_cost;
}

bool MoveTransaction::apply(RunState& state, std::string* error)
{
    const RunResources resources_before = state.resources;
    if (m_stage != MoveTransactionStage::Observed || !m_observation.has_value() ||
        state.current_node != m_proposal.source || state.resources.action_points < 1) {
        if (error != nullptr) {
            *error = "transaction cannot be applied to the current run state";
        }
        return false;
    }
    const int cost = authoritative_cost();
    if (cost < 0 || state.resources.action_points < cost) {
        if (error != nullptr) {
            *error = "authoritative move cost exceeds current action points";
        }
        return false;
    }
    const MovementSpec* movement = find_movement_spec(m_proposal.movement);
    if (movement == nullptr) {
        if (error != nullptr) {
            *error = "transaction references an unknown movement";
        }
        return false;
    }
    // 不可控移动留在本层时，当前节点必须仍属于规划时枚举出的随机落点；但如果节点页面已经
    // 通过受信任的生命周期推进到下一层，观测到的是新层起点，不可能出现在上一层的落点表中。
    // MoveTransaction::observe 已经校验过这类跨层必须带有终点/页面结算/适配追猎信号。
    const bool observes_same_map_landing = m_observation->floor == m_source_floor &&
                                            !m_observation->renewed_same_floor_after_terminal;
    const NodeId movement_landing = m_observation->linked_encounter_origin_node == InvalidNodeId
                                        ? m_observation->current_node
                                        : m_observation->linked_encounter_origin_node;
    if (!m_proposal.controllable && observes_same_map_landing &&
        !m_proposal.landing_action_point_gains.contains(movement_landing)) {
        if (error != nullptr) {
            *error = "uncontrollable move observation has no matching action-point outcome";
        }
        return false;
    }
    if (m_observation->action_points < 0 || m_observation->action_points > 64) {
        if (error != nullptr) {
            *error = "observed action points must be between 0 and 64";
        }
        return false;
    }

    if (m_proposal.movement != MovementKind::Walk) {
        auto charge = state.resources.movement_charges.find(m_proposal.movement);
        if (charge == state.resources.movement_charges.end() || charge->second <= 0) {
            if (error != nullptr) {
                *error = "movement charge was exhausted before transaction application";
            }
            return false;
        }
        // 剩余次数由下一次零件箱星星观测重建。这里不猜测具体消耗了哪一件同类实例，
        // 也不再用事务结算修改库存事实；规划器自己的搜索状态仍会逐步扣除聚合次数。
    }
    state.resources.action_points = m_observation->action_points;
    state.resources.hope += movement->effect.hope_gain;
    state.resources.ingots += movement->effect.ingot_gain;
    state.floor = m_observation->floor;
    state.current_node = m_observation->current_node;

    const bool processing_move = m_proposal.movement != MovementKind::Walk;
    if (processing_move && is_route_battle_node_type(m_observation->landed_type) &&
        state.resources.white_model_birds > 0) {
        --state.resources.white_model_birds;
    }
    if (state.resources != resources_before) {
        ++state.resources_revision;
    }
    m_stage = MoveTransactionStage::Applied;
    return true;
}

void MoveTransaction::cancel() noexcept
{
    if (m_stage != MoveTransactionStage::Applied) {
        m_stage = MoveTransactionStage::Cancelled;
    }
}

void MoveTransaction::invalidate() noexcept
{
    if (m_stage != MoveTransactionStage::Applied) {
        m_stage = MoveTransactionStage::Invalidated;
    }
}

std::string_view to_string(NodeType type) noexcept
{
    switch (type) {
    case NodeType::Unknown:
        return "unclassified";
    case NodeType::BattleElite:
        return "battle_elite";
    case NodeType::BattleNormal:
        return "battle_normal";
    case NodeType::BattleSavage:
        return "battle_savage";
    case NodeType::Duel:
        return "duel";
    case NodeType::Door:
        return "door";
    case NodeType::Employ:
        return "employ";
    case NodeType::Expedition:
        return "expedition";
    case NodeType::HideBattle:
        return "hide_battle";
    case NodeType::HideInvisible:
        return "hide_invisible";
    case NodeType::Incident:
        return "incident";
    case NodeType::Light:
        return "light";
    case NodeType::Portal:
        return "portal";
    case NodeType::Rest:
        return "rest";
    case NodeType::Sacrifice:
        return "sacrifice";
    case NodeType::ScrapShop:
        return "scrap_shop";
    case NodeType::Shop:
        return "shop";
    case NodeType::Wish:
        return "wish";
    case NodeType::Empty:
        return "empty";
    case NodeType::Evacuate:
        return "evacuate";
    case NodeType::Final:
        return "final";
    case NodeType::BattleBoss:
        return "battle_boss";
    }
    return "unclassified";
}

std::string_view to_string(MovementKind kind) noexcept
{
    const MovementSpec* spec = find_movement_spec(kind);
    return spec == nullptr ? std::string_view("unknown") : spec->id;
}
} // namespace asst::blackflow
