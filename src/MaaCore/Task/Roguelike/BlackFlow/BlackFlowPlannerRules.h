#pragma once

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "BlackFlowModel.h"
#include "BlackFlowPolicy.h"

namespace asst::blackflow
{
// 路线提示描述的是“尚未执行”的后缀。只有事务真正 apply 成功后才消费首步；预览取消、
// 页面恢复和尚未提交的重规划都不得推进。用 action_id 对账，避免把另一份陈旧规划的首步删掉。
inline void advance_route_hint_after_applied_move(
    std::vector<PlannedRouteStep>& pending_steps,
    std::string_view applied_action_id)
{
    if (pending_steps.empty() || pending_steps.front().move.action_id != applied_action_id) {
        pending_steps.clear();
        return;
    }
    pending_steps.erase(pending_steps.begin());
}

[[nodiscard]] constexpr bool endpoint_fallback_candidate_is_safe(
    bool no_AP_is_terminal,
    int action_points_before,
    int action_point_cost,
    int action_point_gain,
    bool terminal_on_completion,
    bool direct_exhaustion) noexcept
{
    if (no_AP_is_terminal) {
        return true;
    }
    const int remaining = std::max(0, action_points_before - action_point_cost + action_point_gain);
    // 地图出口的 terminal_on_completion 是物理终点；“直接耗尽”只是追猎入口，只有本层
    // 本来就允许行动力归零终止时才能使用，不能借回退把二层伪装成可追猎层。
    return remaining > 0 || (terminal_on_completion && !direct_exhaustion);
}

// 后续路线沿用当前层的落点硬禁令；第一层策略不提供作战类禁令，二层起整条路线避战。
[[nodiscard]] inline std::unordered_set<NodeType>
    future_forbidden_landing_types(const std::unordered_set<NodeType>& immediate_forbidden)
{
    return immediate_forbidden;
}

[[nodiscard]] inline bool route_landing_is_forbidden(
    NodeType type,
    bool revealed_before_landing,
    bool allow_revealed_hidden_battle,
    const std::unordered_set<NodeType>& forbidden) noexcept
{
    if (!forbidden.contains(type)) {
        return false;
    }
    return type != NodeType::HideBattle || !allow_revealed_hidden_battle || !revealed_before_landing;
}

[[nodiscard]] inline bool move_lands_on_forbidden_node_type(
    const MapSnapshot& map,
    const MoveCandidate& move,
    const std::unordered_set<NodeType>& forbidden)
{
    const auto landing_is_forbidden = [&](NodeId id) {
        const Node* node = map.find_node(id);
        return node != nullptr && forbidden.contains(node->type);
    };

    if (move.controllable || move.possible_landings.empty()) {
        return move.landing != InvalidNodeId && landing_is_forbidden(move.landing);
    }
    return std::ranges::any_of(move.possible_landings, landing_is_forbidden);
}

// 流窜“居民”每次结算后可以留在原地，或沿一条已确认连线移动一步；但不会进入可重复节点、
// 命运所指、居民据点或已完成节点形成的林间空地。玩家刚落下的位置也已经被占用。
[[nodiscard]] inline bool roaming_resident_destination_is_protected(
    const Node& node,
    const std::unordered_set<NodeId>& completed_nodes) noexcept
{
    if (completed_nodes.contains(node.id) || node.name == "命运所指") {
        return true;
    }
    switch (node.type) {
    case NodeType::Shop:
    case NodeType::ScrapShop:
    case NodeType::Evacuate:
    case NodeType::Final:
    case NodeType::BattleBoss:
    case NodeType::Door:
    case NodeType::BattleSavage:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline std::vector<NodeId> roaming_resident_next_positions(
    const MapSnapshot& map,
    NodeId current,
    NodeId player_landing,
    const std::unordered_set<NodeId>& completed_nodes)
{
    // “留在原地”始终是一个独立结果，即使原位置的节点类型本来属于居民不会进入的种类。
    std::vector<NodeId> result { current };
    for (const NodeId neighbor : map.neighbors(current, GraphLayer::Confirmed)) {
        const Node* node = map.find_node(neighbor);
        if (neighbor == player_landing || node == nullptr ||
            roaming_resident_destination_is_protected(*node, completed_nodes)) {
            continue;
        }
        result.emplace_back(neighbor);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] inline bool
    move_intersects_nodes(const MoveCandidate& move, const std::unordered_set<NodeId>& occupied) noexcept
{
    const auto intersects = [&](NodeId node) {
        return node != InvalidNodeId && occupied.contains(node);
    };
    return intersects(move.target) || intersects(move.landing) ||
           std::ranges::any_of(move.path, intersects) || std::ranges::any_of(move.possible_landings, intersects);
}

// 这里刻意是“每种居民结果，各自存在一个应对动作”（∀居民结果 ∃玩家应对），而不是先把所有
// 可能位置取并集再要求同一个动作绕开它们。后者会把左右两条路分别可用的局面误判成无路可走。
template <typename ResponseAvailable>
[[nodiscard]] bool every_roaming_resident_outcome_has_response(
    const std::vector<std::vector<NodeId>>& per_resident_positions,
    ResponseAvailable&& response_available)
{
    std::unordered_set<NodeId> occupied;
    const auto visit = [&](const auto& self, std::size_t resident_index) -> bool {
        if (resident_index == per_resident_positions.size()) {
            return response_available(occupied);
        }
        for (const NodeId position : per_resident_positions[resident_index]) {
            const bool inserted = occupied.emplace(position).second;
            if (!self(self, resident_index + 1)) {
                return false;
            }
            if (inserted) {
                occupied.erase(position);
            }
        }
        return true;
    };
    return visit(visit, 0);
}

[[nodiscard]] inline bool is_exact_predicted_resident_settlement(const Node& node) noexcept
{
    return node.type == NodeType::BattleSavage && node.identity_from_prediction &&
           node.prediction_rule == "initial_roaming_resident_settlement";
}

[[nodiscard]] inline bool is_resident_settlement(const Node* node) noexcept
{
    return node != nullptr && node->type == NodeType::BattleSavage;
}

// 规则反推得到的“居民”据点语义是确定的。它在视野点亮前仍显示“未知的凶戾”，
// 但该视觉外观不能在预览安全层重新把它归入未知凶戾禁令。
[[nodiscard]] inline NodeType preview_landing_type_for_safety(
    const Node* mapped_landing,
    bool preview_is_landing,
    NodeType preview_type) noexcept
{
    if (mapped_landing != nullptr && is_exact_predicted_resident_settlement(*mapped_landing)) {
        return NodeType::BattleSavage;
    }
    if (preview_is_landing && preview_type != NodeType::Unknown) {
        return preview_type;
    }
    return mapped_landing == nullptr ? NodeType::Unknown : mapped_landing->type;
}

// 只有路线没有任何探明/有效节点收益、不会抵达物理终点，并且最终确实只是进入追猎时，
// 才能用 UI 的“直接耗尽”替代整条路线。路线搜索也可能已经显式附加了直接耗尽后缀；
// 这种情况下要把前面的无收益地图移动一并去掉。它是追猎入口，不是地图节点，也不消耗加工品。
[[nodiscard]] inline bool
    route_is_pure_action_point_exhaustion(const PolicyCandidateSummary& candidate, bool no_AP_is_terminal) noexcept
{
    if (!no_AP_is_terminal || candidate.revealed_node_count != 0 || candidate.effective_node_count != 0 ||
        candidate.planned_route_steps.empty() || candidate.planned_route_steps.back().action_points_after > 0) {
        return false;
    }
    const PlannedRouteStep& last = candidate.planned_route_steps.back();
    if (last.move.direct_exhaustion) {
        return std::ranges::none_of(
            candidate.planned_route_steps.begin(),
            std::prev(candidate.planned_route_steps.end()),
            [](const PlannedRouteStep& step) { return step.move.terminal_on_completion; });
    }
    return std::ranges::none_of(candidate.planned_route_steps, [](const PlannedRouteStep& step) {
        return step.move.terminal_on_completion;
    });
}

// 路线搜索的状态图会逐步扣除加工品次数；这里再对最终路线做一次独立资源守恒校验，
// 防止预演/提示复用把不属于当前资源状态的动作串进候选并用虚构收益参与排序。
[[nodiscard]] inline bool planned_route_fits_movement_inventory(
    const std::vector<PlannedRouteStep>& steps,
    const std::unordered_map<MovementKind, int>& available_charges) noexcept
{
    std::array<int, ProcessingMovementSlotCount> used {};
    for (const PlannedRouteStep& step : steps) {
        const MovementKind movement = step.move.movement;
        if (movement == MovementKind::Walk) {
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(movement);
        if (index >= used.size()) {
            return false;
        }
        const auto available = available_charges.find(movement);
        if (available == available_charges.end() || ++used[index] > std::max(available->second, 0)) {
            return false;
        }
    }
    return true;
}
} // namespace asst::blackflow
