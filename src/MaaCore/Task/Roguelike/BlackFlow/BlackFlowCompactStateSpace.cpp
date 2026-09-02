#include "BlackFlowCompactStateSpace.h"

#include "BlackFlowPlannerRules.h"
#include "BlackFlowRevealSemantics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <ranges>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace asst::blackflow
{
namespace
{
constexpr std::size_t MovementKindCount = 13;
constexpr std::size_t NodeTypeCount = 21;

std::size_t movement_index(MovementKind movement) noexcept
{
    return static_cast<std::size_t>(movement);
}

std::size_t node_type_index(NodeType type) noexcept
{
    return static_cast<std::size_t>(type);
}

bool geometry_matches(const GridPosition& source, const GridPosition& target, MovementRange range) noexcept
{
    const int row_delta = std::abs(target.row - source.row);
    const int column_delta = std::abs(target.column - source.column);
    switch (range) {
    case MovementRange::WalkEdges:
        return false;
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

bool planner_state_less(const PlannerState& lhs, const PlannerState& rhs) noexcept
{
    return std::tie(
               lhs.node,
               lhs.movement_charges,
               lhs.completed_nodes,
               lhs.opened_blockers,
               lhs.consumed_lights,
               lhs.revealed_hidden_battles,
               lhs.goal_progress_id,
               lhs.current_final_bypassed,
               lhs.terminal) <
           std::tie(
               rhs.node,
               rhs.movement_charges,
               rhs.completed_nodes,
               rhs.opened_blockers,
               rhs.consumed_lights,
               rhs.revealed_hidden_battles,
               rhs.goal_progress_id,
               rhs.current_final_bypassed,
               rhs.terminal);
}
} // namespace

bool BlackFlowCompactStateSpace::initialize(
    const MapSnapshot& map,
    const RunState& run,
    StateExpansionOptions options,
    std::string* error)
{
    m_run = nullptr;
    m_options = std::move(options);
    m_initial_state = {};
    m_indexed_nodes.clear();
    m_node_indices.clear();
    m_nodes.clear();
    m_confirmed_adjacency.clear();
    m_relaxed_adjacency.clear();
    m_endpoint_hidden_battle_reveals.clear();
    for (auto& by_source : m_geometric_targets) {
        by_source.clear();
    }
    m_type_nodes.fill(0);
    m_blocking_nodes = 0;
    m_transfer_nodes = 0;

    if (map.find_node(run.current_node) == nullptr || m_options.maximum_states == 0) {
        if (error != nullptr) {
            *error = "compact state space requires a valid current node";
        }
        return false;
    }
    std::string validation_error;
    if (!run.costs.validate(&validation_error)) {
        if (error != nullptr) {
            *error = validation_error;
        }
        return false;
    }

    for (const auto& [node_id, node] : map.nodes()) {
        if (node.floor == run.floor) {
            m_indexed_nodes.emplace_back(node_id);
        }
    }
    std::ranges::sort(m_indexed_nodes);
    if (m_indexed_nodes.size() > 64) {
        if (error != nullptr) {
            *error = "compact state space supports at most 64 nodes on one floor";
        }
        return false;
    }

    m_node_indices.reserve(m_indexed_nodes.size());
    m_nodes.reserve(m_indexed_nodes.size());
    for (std::size_t index = 0; index < m_indexed_nodes.size(); ++index) {
        const NodeId id = m_indexed_nodes[index];
        const Node* node = map.find_node(id);
        if (node == nullptr) {
            if (error != nullptr) {
                *error = "compact state space node index references a missing node";
            }
            return false;
        }
        const auto compact_index = static_cast<std::uint8_t>(index);
        m_node_indices.emplace(id, compact_index);
        m_nodes.emplace_back(
            StaticNode {
                id,
                node->position,
                node->type,
                node->progress,
                node->traversal,
                node->identity_state,
                node->visually_hidden,
                node_has_explicit_roaming_resident_marker(*node),
                std::nullopt,
            });
        const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << index;
        const std::size_t type_index = node_type_index(node->type);
        if (type_index < NodeTypeCount) {
            m_type_nodes[type_index] |= node_mask;
        }
        if (node->traversal.blocks_walk) {
            m_blocking_nodes |= node_mask;
        }
        if (is_transfer_node(node->type)) {
            m_transfer_nodes |= node_mask;
        }
    }

    for (std::size_t index = 0; index < m_nodes.size(); ++index) {
        const Node* node = map.find_node(m_nodes[index].id);
        if (node == nullptr || !is_transfer_node(node->type) || !map.has_valid_transfer_pair(node->id) ||
            !node->transfer_target.has_value()) {
            continue;
        }
        if (const auto found = m_node_indices.find(*node->transfer_target); found != m_node_indices.end()) {
            m_nodes[index].transfer_landing = found->second;
        }
    }

    precompute_adjacency(map);
    precompute_endpoint_hidden_battle_reveals(map);
    precompute_geometry();

    m_initial_state.node = run.current_node;
    for (const MovementSpec& movement : movement_specs()) {
        if (movement.kind == MovementKind::Walk) {
            continue;
        }
        int charges = 0;
        if (const auto found = run.resources.movement_charges.find(movement.kind);
            found != run.resources.movement_charges.end() && !run.cross_floor_expired.contains(movement.kind)) {
            charges = std::clamp(found->second, 0, 255);
        }
        m_initial_state.movement_charges[movement_index(movement.kind)] = static_cast<std::uint8_t>(charges);
    }
    for (const auto& [node, progress] : run.node_progress) {
        const auto node_mask = bit(node);
        if (progress == NodeProgress::Completed && node_mask.has_value()) {
            m_initial_state.completed_nodes |= *node_mask;
        }
    }
    for (const NodeId node_id : run.visited_nodes) {
        const auto found = m_node_indices.find(node_id);
        if (found == m_node_indices.end()) {
            continue;
        }
        const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << found->second;
        if (m_nodes[found->second].traversal.blocks_walk && (m_initial_state.completed_nodes & node_mask) == 0) {
            m_initial_state.opened_blockers |= node_mask;
        }
    }
    for (const NodeId node_id : run.consumed_one_time_nodes) {
        const auto found = m_node_indices.find(node_id);
        if (found != m_node_indices.end() && m_nodes[found->second].type == NodeType::Light) {
            m_initial_state.consumed_lights |= PlannerNodeMask { 1 } << found->second;
        }
    }
    for (std::size_t index = 0; index < m_nodes.size(); ++index) {
        const Node* node = map.find_node(m_nodes[index].id);
        if (node != nullptr && node->type == NodeType::HideBattle &&
            (node->identity_revealed || run.revealed_nodes.contains(node->id))) {
            m_initial_state.revealed_hidden_battles |= PlannerNodeMask { 1 } << index;
        }
    }
    if (const auto current = node_index(run.current_node);
        current.has_value() && m_nodes[*current].type == NodeType::Final) {
        m_initial_state.current_final_bypassed = true;
    }
    m_initial_state.terminal = is_endpoint(m_initial_state);
    m_run = &run;
    return true;
}

void BlackFlowCompactStateSpace::precompute_adjacency(const MapSnapshot& map)
{
    m_confirmed_adjacency.assign(m_nodes.size(), {});
    m_relaxed_adjacency.assign(m_nodes.size(), {});
    for (const Edge& edge : map.edges()) {
        const auto first = m_node_indices.find(edge.first);
        const auto second = m_node_indices.find(edge.second);
        if (first == m_node_indices.end() || second == m_node_indices.end() ||
            edge.knowledge == EdgeKnowledge::Absent) {
            continue;
        }
        const bool unconfirmed =
            edge.knowledge != EdgeKnowledge::Confirmed || edge.evidence.forced_by_connectivity_constraint;
        const bool inferred = edge.evidence.forced_by_connectivity_constraint;
        m_relaxed_adjacency[first->second].emplace_back(CompactNeighbor { second->second, unconfirmed, inferred });
        m_relaxed_adjacency[second->second].emplace_back(CompactNeighbor { first->second, unconfirmed, inferred });
        if (edge.knowledge == EdgeKnowledge::Confirmed && !edge.evidence.forced_by_connectivity_constraint) {
            m_confirmed_adjacency[first->second].emplace_back(CompactNeighbor { second->second, false, false });
            m_confirmed_adjacency[second->second].emplace_back(CompactNeighbor { first->second, false, false });
        }
    }
    const auto sort_neighbors = [&](std::vector<std::vector<CompactNeighbor>>& adjacency) {
        for (auto& neighbors : adjacency) {
            std::ranges::sort(neighbors, [&](const CompactNeighbor& lhs, const CompactNeighbor& rhs) {
                return m_nodes[lhs.index].id < m_nodes[rhs.index].id;
            });
            neighbors.erase(
                std::unique(
                    neighbors.begin(),
                    neighbors.end(),
                    [](const CompactNeighbor& lhs, const CompactNeighbor& rhs) { return lhs.index == rhs.index; }),
                neighbors.end());
        }
    };
    sort_neighbors(m_confirmed_adjacency);
    sort_neighbors(m_relaxed_adjacency);
}

void BlackFlowCompactStateSpace::precompute_endpoint_hidden_battle_reveals(const MapSnapshot& map)
{
    m_endpoint_hidden_battle_reveals.assign(m_nodes.size(), 0);
    for (std::size_t origin = 0; origin < m_nodes.size(); ++origin) {
        auto revealed = map.reveal_through_transparent_nodes(m_nodes[origin].id);
        if (m_nodes[origin].type == NodeType::Light) {
            const auto light_revealed = map.nodes_within_manhattan(m_nodes[origin].id, LightRevealRadius);
            for (const NodeId id : light_revealed) {
                revealed.emplace(id);
            }
        }
        for (const NodeId id : revealed) {
            const auto found = m_node_indices.find(id);
            if (found != m_node_indices.end() && m_nodes[found->second].type == NodeType::HideBattle) {
                m_endpoint_hidden_battle_reveals[origin] |= PlannerNodeMask { 1 } << found->second;
            }
        }
    }
}

void BlackFlowCompactStateSpace::precompute_geometry()
{
    for (const MovementSpec& movement : movement_specs()) {
        auto& by_source = m_geometric_targets[movement_index(movement.kind)];
        by_source.assign(m_nodes.size(), {});
        if (movement.kind == MovementKind::Walk) {
            continue;
        }
        for (std::size_t source = 0; source < m_nodes.size(); ++source) {
            for (std::size_t target = 0; target < m_nodes.size(); ++target) {
                if ((source == target && m_nodes[target].type == NodeType::Empty) ||
                    m_nodes[target].progress == NodeProgress::Removed ||
                    !geometry_matches(m_nodes[source].position, m_nodes[target].position, movement.range)) {
                    continue;
                }
                by_source[source].emplace_back(static_cast<std::uint8_t>(target));
            }
        }
    }
}

std::optional<std::uint8_t> BlackFlowCompactStateSpace::node_index(NodeId node) const noexcept
{
    const auto found = m_node_indices.find(node);
    return found == m_node_indices.end() ? std::nullopt : std::optional<std::uint8_t>(found->second);
}

std::optional<PlannerNodeMask> BlackFlowCompactStateSpace::bit(NodeId node) const noexcept
{
    const auto index = node_index(node);
    return index.has_value() ? std::optional<PlannerNodeMask>(PlannerNodeMask { 1 } << *index) : std::nullopt;
}

bool BlackFlowCompactStateSpace::mask_contains(PlannerNodeMask mask, NodeId node) const noexcept
{
    const auto node_mask = bit(node);
    return node_mask.has_value() && (mask & *node_mask) != 0;
}

PlannerNodeMask BlackFlowCompactStateSpace::nodes_of_type(NodeType type) const noexcept
{
    const std::size_t index = node_type_index(type);
    return index < NodeTypeCount ? m_type_nodes[index] : 0;
}

const std::vector<CompactNeighbor>& BlackFlowCompactStateSpace::adjacency(GraphLayer layer, std::uint8_t source) const
{
    static const std::vector<CompactNeighbor> Empty;
    const auto& selected = layer == GraphLayer::Confirmed ? m_confirmed_adjacency : m_relaxed_adjacency;
    return source < selected.size() ? selected[source] : Empty;
}

const std::vector<std::uint8_t>&
    BlackFlowCompactStateSpace::geometric_targets(MovementKind movement, std::uint8_t source) const
{
    static const std::vector<std::uint8_t> Empty;
    const std::size_t movement_id = movement_index(movement);
    if (movement_id >= MovementKindCount || source >= m_geometric_targets[movement_id].size()) {
        return Empty;
    }
    return m_geometric_targets[movement_id][source];
}

NodeProgress BlackFlowCompactStateSpace::effective_progress(const PlannerState& state, std::uint8_t node) const noexcept
{
    const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << node;
    return (state.completed_nodes & node_mask) != 0 ? NodeProgress::Completed : m_nodes[node].progress;
}

NodeType BlackFlowCompactStateSpace::effective_type(const PlannerState& state, std::uint8_t node) const noexcept
{
    const StaticNode& stored = m_nodes[node];
    if (effective_progress(state, node) == NodeProgress::Completed && !stored.traversal.repeatable &&
        stored.type != NodeType::Empty) {
        return NodeType::Empty;
    }
    return stored.type;
}

bool BlackFlowCompactStateSpace::targetable_for_walk(const PlannerState& state, std::uint8_t node) const noexcept
{
    const StaticNode& stored = m_nodes[node];
    const NodeProgress progress = effective_progress(state, node);
    if (!stored.traversal.enterable || progress == NodeProgress::Removed) {
        return false;
    }
    return progress != NodeProgress::Completed || stored.traversal.repeatable;
}

bool BlackFlowCompactStateSpace::walk_transparent(const PlannerState& state, std::uint8_t node, GraphLayer layer)
    const noexcept
{
    const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << node;
    return !m_nodes[node].traversal.blocks_walk || (state.opened_blockers & node_mask) != 0 ||
           effective_progress(state, node) == NodeProgress::Completed ||
           (layer == GraphLayer::Relaxed && m_nodes[node].identity_state == NodeIdentityState::Unclassified);
}

int BlackFlowCompactStateSpace::node_action_point_gain(const PlannerState& state, std::uint8_t node) const noexcept
{
    const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << node;
    return m_nodes[node].type == NodeType::Light && (state.consumed_lights & node_mask) == 0 ? 1 : 0;
}

NodeId BlackFlowCompactStateSpace::resolve_landing(std::uint8_t target) const noexcept
{
    if (target >= m_nodes.size()) {
        return InvalidNodeId;
    }
    if (!is_transfer_node(m_nodes[target].type)) {
        return m_nodes[target].id;
    }
    return m_nodes[target].transfer_landing.has_value() ? m_nodes[*m_nodes[target].transfer_landing].id : InvalidNodeId;
}

// 端点只表示「走到这里就没有后继了」，因此只认物理出口。策略终点是可以再走开的收工点，
// 不能掐掉它的后继。成功与否由 OnDemandStateGraph::intern 写入状态时统一判定。
bool BlackFlowCompactStateSpace::is_endpoint(const PlannerState& state) const noexcept
{
    const auto index = node_index(state.node);
    return index.has_value() && m_options.final_is_terminal && is_exit_node_type(m_nodes[*index].type) &&
           !(m_nodes[*index].type == NodeType::Final && state.current_final_bypassed);
}

bool BlackFlowCompactStateSpace::is_terminal(const PlannerState& state) const noexcept
{
    return state.terminal;
}

bool BlackFlowCompactStateSpace::unavailable_target(const PlannerState& source, NodeId target) const noexcept
{
    const auto index = node_index(target);
    if (!index.has_value()) {
        return true;
    }
    const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << *index;
    return (source.completed_nodes & node_mask) != 0 && !m_nodes[*index].traversal.repeatable;
}

PlannerState BlackFlowCompactStateSpace::transition(
    const PlannerState& source,
    const MoveCandidate& candidate,
    NodeId landing) const
{
    PlannerState successor = source;
    successor.node = landing;
    successor.current_final_bypassed = candidate.bypass_final_on_completion;
    if (candidate.movement != MovementKind::Walk) {
        auto& charge = successor.movement_charges[movement_index(candidate.movement)];
        if (charge > 0) {
            --charge;
        }
    }

    // 不可控移动的 target 只是打开预览的激活格；完成、点亮与节点收益都属于实际落点。
    const NodeId completed = candidate.controllable ? candidate.target : landing;
    const auto index = node_index(completed);
    if (index.has_value()) {
        const PlannerNodeMask node_mask = PlannerNodeMask { 1 } << *index;
        const StaticNode& node = m_nodes[*index];
        if (!candidate.bypass_final_on_completion && !node.traversal.repeatable && node.type != NodeType::Empty) {
            successor.completed_nodes |= node_mask;
            successor.opened_blockers &= ~node_mask;
        }
        else if (node.traversal.blocks_walk) {
            successor.opened_blockers |= node_mask;
        }
        if (node.type == NodeType::Light) {
            successor.consumed_lights |= node_mask;
        }
    }
    if (!candidate.terminal_on_completion) {
        const auto landing_index = node_index(landing);
        if (landing_index.has_value() && *landing_index < m_endpoint_hidden_battle_reveals.size()) {
            successor.revealed_hidden_battles |= m_endpoint_hidden_battle_reveals[*landing_index];
        }
    }
    successor.terminal = is_endpoint(successor);
    return successor;
}

int BlackFlowCompactStateSpace::outcome_gain(const PlannerState& source, const MoveCandidate& candidate, NodeId landing)
    const noexcept
{
    const MovementSpec* movement = find_movement_spec(candidate.movement);
    int gain = movement == nullptr ? 0 : movement->effect.action_point_gain;
    const NodeId effect_node = candidate.controllable ? candidate.target : landing;
    const auto index = node_index(effect_node);
    if (index.has_value()) {
        gain += node_action_point_gain(source, *index);
    }
    return gain;
}

// 注意：这是 enumerate_move_actions（BlackFlowModel）的位掩码紧凑翻版，两份必须保持语义一致，
// 否则按需图与紧凑空间对同一局面会给出不同的动作集，安全值与路线互相矛盾。
// 已知差异：这边以 opened_blockers 判定 walk 穿行（Model 版用 visited_nodes）、在生成期过滤
// forbidden 动作（Model 版由 OnDemandStateGraph 展开期过滤）。改动任一份时同步检查另一份。
std::optional<std::vector<CompactMoveAction>>
    BlackFlowCompactStateSpace::actions(const PlannerState& source, std::string* error) const
{
    if (m_run == nullptr) {
        if (error != nullptr) {
            *error = "compact state space is not initialized";
        }
        return std::nullopt;
    }
    const auto source_index_value = node_index(source.node);
    if (!source_index_value.has_value()) {
        if (error != nullptr) {
            *error = "compact action request references an unknown source node";
        }
        return std::nullopt;
    }
    if (is_endpoint(source)) {
        return std::vector<CompactMoveAction> {};
    }
    const std::uint8_t source_index = *source_index_value;
    std::vector<MoveAction> generated;

    const MovementSpec* walk = find_movement_spec(MovementKind::Walk);
    if (walk != nullptr) {
        struct WalkFrontier
        {
            std::uint8_t node = 0;
            std::vector<std::uint8_t> path;
        };

        std::deque<WalkFrontier> pending;
        std::array<int, 64> walk_action_indices;
        walk_action_indices.fill(-1);
        std::array<bool, 64> expanded {};
        const StaticNode& source_node = m_nodes[source_index];
        // 实际根状态允许重新进入当前可重复节点。模拟后缀不再展开自移动，既保留游戏
        // 的合法根动作，也避免单份规划在同一节点上构造无界循环。
        if (source == m_initial_state && source_node.type != NodeType::Empty &&
            targetable_for_walk(source, source_index) && node_type_allowed(*walk, source_node.type)) {
            MoveAction action;
            action.candidate.action_id =
                "walk:" + std::to_string(source.node) + ":" + std::to_string(source.node);
            action.candidate.movement = MovementKind::Walk;
            action.candidate.source = source.node;
            action.candidate.target = source.node;
            action.candidate.landing = resolve_landing(source_index);
            action.candidate.graph_layer = m_options.graph_layer;
            if (action.candidate.landing != InvalidNodeId) {
                action.candidate.path = { source.node };
                if (source_node.identity_state == NodeIdentityState::Unclassified) {
                    action.candidate.first_unclassified = source.node;
                }
                action.candidate.predicted_action_point_cost = m_run->costs.action_cost(
                    action.candidate.action_id,
                    m_run->costs.movement_cost(*walk, action.candidate.path.size()));
                action.candidate.predicted_action_point_gain = node_action_point_gain(source, source_index);
                action.candidate.possible_landings.emplace_back(action.candidate.landing);
                action.candidate.landing_node_types.emplace(action.candidate.landing, source_node.type);
                action.candidate.landing_action_point_gains.emplace(
                    action.candidate.landing,
                    action.candidate.predicted_action_point_gain);
                action.candidate.terminal_on_completion = is_exit_node_type(source_node.type);
                action.possible_landings.emplace_back(action.candidate.landing);
                walk_action_indices[source_index] = static_cast<int>(generated.size());
                generated.emplace_back(std::move(action));
            }
        }
        pending.push_back(WalkFrontier { source_index, {} });
        expanded[source_index] = true;
        while (!pending.empty()) {
            WalkFrontier current = std::move(pending.front());
            pending.pop_front();
            for (const CompactNeighbor& edge : adjacency(m_options.graph_layer, current.node)) {
                if (edge.index == source_index || std::ranges::find(current.path, edge.index) != current.path.end()) {
                    continue;
                }
                if (effective_progress(source, edge.index) == NodeProgress::Removed) {
                    continue;
                }
                auto path = current.path;
                path.emplace_back(edge.index);
                const StaticNode& node = m_nodes[edge.index];
                if (targetable_for_walk(source, edge.index) && node_type_allowed(*walk, node.type)) {
                    MoveAction action;
                    action.candidate.action_id = "walk:" + std::to_string(source.node) + ":" + std::to_string(node.id);
                    action.candidate.movement = MovementKind::Walk;
                    action.candidate.source = source.node;
                    action.candidate.target = node.id;
                    action.candidate.landing = resolve_landing(edge.index);
                    action.candidate.graph_layer = m_options.graph_layer;
                    if (action.candidate.landing != InvalidNodeId) {
                        action.candidate.path.reserve(path.size());
                        std::uint8_t previous = source_index;
                        for (const std::uint8_t step : path) {
                            action.candidate.path.emplace_back(m_nodes[step].id);
                            const auto& neighbors = adjacency(m_options.graph_layer, previous);
                            const auto path_edge = std::ranges::find(neighbors, step, &CompactNeighbor::index);
                            if (path_edge != neighbors.end()) {
                                action.candidate.uses_unconfirmed_edge =
                                    action.candidate.uses_unconfirmed_edge || path_edge->uses_unconfirmed_edge;
                                action.candidate.uses_inferred_edge =
                                    action.candidate.uses_inferred_edge || path_edge->uses_inferred_edge;
                            }
                            if (!action.candidate.first_unclassified.has_value() &&
                                m_nodes[step].identity_state == NodeIdentityState::Unclassified) {
                                action.candidate.first_unclassified = m_nodes[step].id;
                            }
                            previous = step;
                        }
                        action.candidate.predicted_action_point_cost = m_run->costs.action_cost(
                            action.candidate.action_id,
                            m_run->costs.movement_cost(*walk, path.size()));
                        action.candidate.predicted_action_point_gain = node_action_point_gain(source, edge.index);
                        action.candidate.possible_landings.emplace_back(action.candidate.landing);
                        action.candidate.landing_node_types.emplace(action.candidate.landing, node.type);
                        action.candidate.landing_action_point_gains.emplace(
                            action.candidate.landing,
                            action.candidate.predicted_action_point_gain);
                        action.candidate.terminal_on_completion = is_exit_node_type(node.type);
                        action.possible_landings.emplace_back(action.candidate.landing);
                        const int existing_index = walk_action_indices[edge.index];
                        if (existing_index < 0) {
                            walk_action_indices[edge.index] = static_cast<int>(generated.size());
                            generated.emplace_back(std::move(action));
                        }
                        else if (
                            action.candidate.path.size() <
                            generated[static_cast<std::size_t>(existing_index)].candidate.path.size()) {
                            generated[static_cast<std::size_t>(existing_index)] = std::move(action);
                        }
                    }
                }
                if (walk_transparent(source, edge.index, m_options.graph_layer) && !expanded[edge.index]) {
                    expanded[edge.index] = true;
                    pending.push_back(WalkFrontier { edge.index, std::move(path) });
                }
            }
        }
    }

    for (const MovementSpec& movement : movement_specs()) {
        if (movement.kind == MovementKind::Walk) {
            continue;
        }
        if (m_options.reserved_movement_kinds.contains(movement.kind)) {
            int reserved_charges = 0;
            for (const MovementKind reserved : m_options.reserved_movement_kinds) {
                reserved_charges += source.movement_charges[movement_index(reserved)];
            }
            if (reserved_charges <= m_options.reserved_movement_charges) {
                continue;
            }
        }
        const int charges = source.movement_charges[movement_index(movement.kind)];
        if (charges <= 0 || m_run->cross_floor_expired.contains(movement.kind)) {
            continue;
        }

        std::vector<std::uint8_t> targets;
        std::vector<std::uint8_t> hidden_noncombat_targets;
        for (const std::uint8_t target : geometric_targets(movement.kind, source_index)) {
            const NodeType semantic_type = effective_type(source, target);
            const NodeType visible_type = movement_visible_node_type(semantic_type, m_nodes[target].visually_hidden);
            const bool targetable = semantic_type == NodeType::Empty
                                        ? effective_progress(source, target) != NodeProgress::Removed
                                        : targetable_for_walk(source, target);
            if (!targetable || !node_type_allowed(movement, visible_type) ||
                !movement_target_is_currently_selectable(movement.kind, m_nodes[target].visually_hidden) ||
                (movement.random_target && m_nodes[target].explicit_roaming_resident_marker)) {
                continue;
            }
            targets.emplace_back(target);
            if (movement.random_target && visible_type == NodeType::HideInvisible) {
                hidden_noncombat_targets.emplace_back(target);
            }
        }
        // 与 enumerate_move_actions 保持一致：小八界先使用“未知的诡秘”候选池；只有
        // 该池为空时才使用全部合法非作战节点。明确流窜居民在入池前排除，疑似重叠保留。
        if (movement.random_target && !hidden_noncombat_targets.empty()) {
            targets = std::move(hidden_noncombat_targets);
        }
        if (targets.empty()) {
            continue;
        }

        if (movement.random_target) {
            MoveAction action;
            action.candidate.action_id = std::string(movement.id) + ":random:" + std::to_string(source.node);
            action.candidate.movement = movement.kind;
            action.candidate.source = source.node;
            // 与非压缩状态空间一致：target 是打开预览所点击的高亮节点，不代表随机实际落点。
            action.candidate.target = m_nodes[targets.front()].id;
            action.candidate.predicted_action_point_cost =
                m_run->costs.action_cost(action.candidate.action_id, m_run->costs.movement_cost(movement));
            action.candidate.predicted_action_point_gain = movement.effect.action_point_gain;
            action.candidate.controllable = false;
            for (const std::uint8_t target : targets) {
                const NodeId landing = resolve_landing(target);
                if (landing == InvalidNodeId) {
                    continue;
                }
                action.possible_landings.emplace_back(landing);
                action.candidate.landing_node_types.insert_or_assign(landing, effective_type(source, target));
                action.candidate.landing_action_point_gains.insert_or_assign(
                    landing,
                    movement.effect.action_point_gain + node_action_point_gain(source, target));
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
                generated.emplace_back(std::move(action));
            }
            continue;
        }

        for (const std::uint8_t target : targets) {
            const NodeId landing = resolve_landing(target);
            if (landing == InvalidNodeId) {
                continue;
            }
            MoveAction action;
            action.candidate.action_id = std::string(movement.id) + ":" + std::to_string(source.node) + ":" +
                                         std::to_string(m_nodes[target].id) + ":" + std::to_string(landing);
            action.candidate.movement = movement.kind;
            action.candidate.source = source.node;
            action.candidate.target = m_nodes[target].id;
            action.candidate.landing = landing;
            action.candidate.path = { m_nodes[target].id };
            action.candidate.predicted_action_point_cost =
                m_run->costs.action_cost(action.candidate.action_id, m_run->costs.movement_cost(movement));
            action.candidate.predicted_action_point_gain =
                movement.effect.action_point_gain + node_action_point_gain(source, target);
            action.candidate.possible_landings.emplace_back(landing);
            action.candidate.landing_node_types.emplace(landing, effective_type(source, target));
            action.candidate.landing_action_point_gains.emplace(landing, action.candidate.predicted_action_point_gain);
            action.candidate.controllable = true;
            action.candidate.terminal_on_completion = is_exit_node_type(m_nodes[target].type);
            action.possible_landings.emplace_back(landing);
            generated.emplace_back(std::move(action));
        }
    }

    const std::size_t physical_action_count = generated.size();
    for (std::size_t index = 0; index < physical_action_count; ++index) {
        const bool can_land_on_final = std::ranges::any_of(
            generated[index].candidate.possible_landings,
            [&](NodeId landing) { return move_landing_type(generated[index].candidate, landing) == NodeType::Final; });
        if (!can_land_on_final) {
            continue;
        }
        MoveAction bypass = generated[index];
        bypass.candidate.action_id += ":bypass-final";
        bypass.candidate.terminal_on_completion = false;
        bypass.candidate.bypass_final_on_completion = true;
        generated.emplace_back(std::move(bypass));
    }

    struct ConfirmedWalk
    {
        NodeId target = InvalidNodeId;
        NodeId landing = InvalidNodeId;
        int action_point_cost = 0;
        int action_point_gain = 0;
    };

    std::vector<ConfirmedWalk> confirmed_walks;
    for (const MoveAction& action : generated) {
        if (action.candidate.movement != MovementKind::Walk || action.candidate.path.size() != 1 ||
            action.candidate.uses_unconfirmed_edge || action.candidate.uses_inferred_edge) {
            continue;
        }
        const auto target_index = node_index(action.candidate.target);
        if (!target_index.has_value()) {
            continue;
        }
        const bool self_walk = source_index == *target_index;
        if (!self_walk) {
            const int manhattan = std::abs(m_nodes[source_index].position.row - m_nodes[*target_index].position.row) +
                                  std::abs(m_nodes[source_index].position.column - m_nodes[*target_index].position.column);
            const auto& confirmed_neighbors = adjacency(GraphLayer::Confirmed, source_index);
            if (manhattan != 1 || std::ranges::find(confirmed_neighbors, *target_index, &CompactNeighbor::index) ==
                                      confirmed_neighbors.end()) {
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
    std::erase_if(generated, [&](const MoveAction& action) {
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

    std::vector<CompactMoveAction> result;
    result.reserve(generated.size());
    for (MoveAction& move : generated) {
        if (m_options.forbidden_action_ids.contains(move.candidate.action_id)) {
            continue;
        }
        const auto forbidden_landing = [&](NodeId id) {
            const auto index = node_index(id);
            const bool revealed_before_landing = index.has_value() &&
                                                 (source.revealed_hidden_battles &
                                                  (PlannerNodeMask { 1 } << *index)) != 0;
            return index.has_value() && route_landing_is_forbidden(
                                            effective_type(source, *index),
                                            revealed_before_landing,
                                            m_options.allow_revealed_hidden_battle,
                                            m_options.forbidden_node_types);
        };
        if ((move.candidate.controllable && forbidden_landing(move.candidate.landing)) ||
            (!move.candidate.controllable && std::ranges::any_of(move.possible_landings, forbidden_landing))) {
            continue;
        }
        CompactMoveAction action;
        action.candidate = std::move(move.candidate);
        action.action_point_cost = action.candidate.predicted_action_point_cost;
        for (const NodeId landing : move.possible_landings) {
            const NodeId target = action.candidate.controllable ? action.candidate.target : landing;
            if (unavailable_target(source, target)) {
                continue;
            }
            const int gain = outcome_gain(source, action.candidate, landing);
            CompactActionOutcome outcome { transition(source, action.candidate, landing), gain };
            const bool duplicate = std::ranges::any_of(action.outcomes, [&](const CompactActionOutcome& existing) {
                return existing.successor == outcome.successor &&
                       existing.action_point_gain == outcome.action_point_gain;
            });
            if (!duplicate) {
                action.outcomes.emplace_back(std::move(outcome));
            }
        }
        std::ranges::sort(action.outcomes, [](const CompactActionOutcome& lhs, const CompactActionOutcome& rhs) {
            if (lhs.successor == rhs.successor) {
                return lhs.action_point_gain < rhs.action_point_gain;
            }
            return planner_state_less(lhs.successor, rhs.successor);
        });
        if (!action.outcomes.empty()) {
            result.emplace_back(std::move(action));
        }
    }
    std::ranges::sort(result, [](const CompactMoveAction& lhs, const CompactMoveAction& rhs) {
        return lhs.candidate.action_id < rhs.candidate.action_id;
    });
    return result;
}
} // namespace asst::blackflow
