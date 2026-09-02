#include "BlackFlowPlanner.h"

#include "BlackFlowPlannerRules.h"
#include "BlackFlowRevealSemantics.h"
#include "BlackFlowSafetyValue.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace asst::blackflow
{
namespace
{
std::size_t processing_movement_strength_rank(MovementKind movement) noexcept
{
    const auto found = std::ranges::find(ProcessingMovementStrengthOrder, movement);
    return found == ProcessingMovementStrengthOrder.end()
               ? ProcessingMovementStrengthOrder.size()
               : static_cast<std::size_t>(std::distance(ProcessingMovementStrengthOrder.begin(), found));
}

bool processing_movements_have_same_capability(const MovementSpec& lhs, const MovementSpec& rhs) noexcept
{
    return lhs.range == rhs.range && lhs.target_types == rhs.target_types &&
           lhs.action_point_cost == rhs.action_point_cost && lhs.random_target == rhs.random_target &&
           lhs.expires_on_floor_end == rhs.expires_on_floor_end &&
           lhs.effect.action_point_gain == rhs.effect.action_point_gain && lhs.effect.hope_gain == rhs.effect.hope_gain &&
           lhs.effect.ingot_gain == rhs.effect.ingot_gain;
}

int remaining_movement_charges(const RunState& run, MovementKind movement) noexcept
{
    const auto found = run.resources.movement_charges.find(movement);
    return found == run.resources.movement_charges.end() ? 0 : std::max(found->second, 0);
}

bool processing_movement_is_more_valuable(
    const RunState& run,
    MovementKind lhs,
    MovementKind rhs) noexcept
{
    const MovementSpec* lhs_spec = find_movement_spec(lhs);
    const MovementSpec* rhs_spec = find_movement_spec(rhs);
    if (lhs_spec != nullptr && rhs_spec != nullptr &&
        processing_movements_have_same_capability(*lhs_spec, *rhs_spec)) {
        const int lhs_charges = remaining_movement_charges(run, lhs);
        const int rhs_charges = remaining_movement_charges(run, rhs);
        if (lhs_charges != rhs_charges) {
            return lhs_charges > rhs_charges;
        }
    }
    return processing_movement_strength_rank(lhs) < processing_movement_strength_rank(rhs);
}

bool projected_route_states_are_equivalent(const RunState& lhs, const RunState& rhs) noexcept
{
    return lhs.floor == rhs.floor && lhs.current_node == rhs.current_node && lhs.resources == rhs.resources &&
           lhs.resources_revision == rhs.resources_revision && lhs.visited_nodes == rhs.visited_nodes &&
           lhs.consumed_one_time_nodes == rhs.consumed_one_time_nodes && lhs.revealed_nodes == rhs.revealed_nodes &&
           lhs.cross_floor_expired == rhs.cross_floor_expired && lhs.node_progress == rhs.node_progress &&
           lhs.strategy_terminal == rhs.strategy_terminal;
}

std::optional<std::pair<RunState, std::vector<PlannedRouteStep>>> replay_processing_order(
    const MapSnapshot& map,
    const RunState& initial,
    const std::vector<PlannedRouteStep>& source_steps,
    const std::vector<MovementKind>& order)
{
    if (source_steps.size() != order.size()) {
        return std::nullopt;
    }
    RunState current = initial;
    std::vector<PlannedRouteStep> rebuilt;
    rebuilt.reserve(source_steps.size());
    for (std::size_t index = 0; index < source_steps.size(); ++index) {
        const PlannedRouteStep& original = source_steps[index];
        if (original.move.direct_exhaustion || !original.move.controllable) {
            return std::nullopt;
        }
        const auto actions = enumerate_move_actions(map, current, original.move.graph_layer);
        const auto found = std::ranges::find_if(actions, [&](const MoveAction& action) {
            if (action.candidate.movement != order[index] || !action.candidate.controllable ||
                action.candidate.target != original.move.target || action.candidate.landing != original.move.landing ||
                action.candidate.bypass_final_on_completion != original.move.bypass_final_on_completion) {
                return false;
            }
            // 徒步路径上的中间节点也会影响揭示收益，不能只按相同终点替换成另一条路。
            return order[index] != MovementKind::Walk || action.candidate.path == original.move.path;
        });
        if (found == actions.end()) {
            return std::nullopt;
        }

        MoveCandidate move = found->candidate;
        move.action_point_requirement = original.move.action_point_requirement;
        move.requires_preview_verification = original.move.requires_preview_verification;
        const int before = current.resources.action_points;
        const int cost = move.predicted_action_point_cost;
        std::string projection_error;
        auto projected = project_move_outcome(map, current, move, cost, &projection_error);
        if (!projected.has_value()) {
            return std::nullopt;
        }
        rebuilt.emplace_back(
            PlannedRouteStep {
                std::move(move),
                before,
                cost,
                projected->action_point_gain,
                projected->run.resources.action_points,
            });
        current = std::move(projected->run);
    }
    return std::pair { std::move(current), std::move(rebuilt) };
}

bool graph_layers_have_identical_transitions(const MapSnapshot& map, const RunState& run) noexcept
{
    for (const Edge& edge : map.edges()) {
        const bool confirmed = edge.knowledge == EdgeKnowledge::Confirmed &&
                               !edge.evidence.forced_by_connectivity_constraint;
        const bool relaxed = edge.knowledge != EdgeKnowledge::Absent;
        if (confirmed != relaxed) {
            return false;
        }
    }
    for (const auto& [id, node] : map.nodes()) {
        if (!node.traversal.blocks_walk || node.identity_state != NodeIdentityState::Unclassified ||
            run.visited_nodes.contains(id)) {
            continue;
        }
        const auto progress = run.node_progress.find(id);
        const NodeProgress effective = progress == run.node_progress.end() ? node.progress : progress->second;
        if (effective != NodeProgress::Completed) {
            return false;
        }
    }
    return true;
}

bool prefer_less_valuable_exchangeable_processing_steps_impl(
    const MapSnapshot& map,
    const RunState& run,
    std::vector<PlannedRouteStep>& steps)
{
    if (steps.size() < 2) {
        return false;
    }
    std::vector<MovementKind> order;
    order.reserve(steps.size());
    for (const PlannedRouteStep& step : steps) {
        order.emplace_back(step.move.movement);
    }
    const auto baseline = replay_processing_order(map, run, steps, order);
    if (!baseline.has_value()) {
        return false;
    }

    bool changed = false;
    bool improved = true;
    while (improved) {
        improved = false;
        for (std::size_t earlier = 0; earlier + 1 < steps.size() && !improved; ++earlier) {
            if (order[earlier] == MovementKind::Walk) {
                continue;
            }
            std::vector<std::size_t> later_candidates;
            for (std::size_t later = earlier + 1; later < steps.size(); ++later) {
                if (order[later] != MovementKind::Walk &&
                    processing_movement_is_more_valuable(run, order[earlier], order[later])) {
                    later_candidates.emplace_back(later);
                }
            }
            std::ranges::sort(later_candidates, [&](std::size_t lhs, std::size_t rhs) {
                return processing_movement_is_more_valuable(run, order[rhs], order[lhs]);
            });
            for (const std::size_t later : later_candidates) {
                auto proposed_order = order;
                std::swap(proposed_order[earlier], proposed_order[later]);
                auto replayed = replay_processing_order(map, run, steps, proposed_order);
                if (!replayed.has_value() ||
                    !projected_route_states_are_equivalent(baseline->first, replayed->first)) {
                    continue;
                }
                order = std::move(proposed_order);
                steps = std::move(replayed->second);
                changed = true;
                improved = true;
                break;
            }
        }
    }
    return changed;
}

std::vector<std::string> sorted_types(const std::unordered_set<std::string>& types)
{
    std::vector<std::string> result(types.begin(), types.end());
    std::ranges::sort(result);
    return result;
}

struct ReachableFeatures
{
    std::unordered_set<std::string> node_types;
    bool has_badged = false;
    bool has_badged_incident = false;
};

bool revealed_by_consumed_light(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    SafetyStateId state,
    NodeId node)
{
    for (const auto& [light_id, light] : map.nodes()) {
        if (light.type != NodeType::Light || !graph.is_light_consumed(state, light_id)) {
            continue;
        }
        const auto revealed = map.nodes_within_manhattan(light_id, LightRevealRadius);
        if (std::ranges::find(revealed, node) != revealed.end()) {
            return true;
        }
    }
    return false;
}

int unknown_big_nodes_revealed(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    SafetyStateId state,
    NodeId light)
{
    const Node* light_node = map.find_node(light);
    if (light_node == nullptr || light_node->type != NodeType::Light) {
        return 0;
    }
    int count = 0;
    for (const NodeId id : map.nodes_within_manhattan(light, LightRevealRadius)) {
        const Node* candidate = map.find_node(id);
        if (candidate == nullptr || candidate->type == NodeType::Empty || candidate->identity_revealed ||
            graph.is_completed(state, id) || graph.source_run().revealed_nodes.contains(id) ||
            revealed_by_consumed_light(map, graph, state, id)) {
            continue;
        }
        ++count;
    }
    return count;
}

void add_newly_revealed_nodes(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    const RunState& run,
    const MoveCandidate& move,
    NodeId landing,
    bool endpoint_observation_available,
    PlannerNodeMask& revealed)
{
    const auto newly_revealed = expected_move_reveals(map, run, move, landing, endpoint_observation_available);
    for (const NodeId id : newly_revealed) {
        if (const auto bit = graph.node_mask(id); bit.has_value()) {
            revealed |= *bit;
        }
    }
}

bool revealed_superset(PlannerNodeMask superset, PlannerNodeMask subset) noexcept
{
    return (superset & subset) == subset;
}

[[nodiscard]] std::size_t node_mask_size(PlannerNodeMask mask) noexcept { return std::popcount(mask); }

[[nodiscard]] bool
    node_mask_contains(const OnDemandStateGraph& graph, PlannerNodeMask mask, NodeId node) noexcept
{
    const auto bit = graph.node_mask(node);
    return bit.has_value() && (mask & *bit) != 0;
}

std::vector<NodeId>
    nodes_from_mask(const MapSnapshot& map, const OnDemandStateGraph& graph, PlannerNodeMask mask)
{
    std::vector<NodeId> result;
    result.reserve(node_mask_size(mask));
    for (const auto& [id, _] : map.nodes()) {
        if (const auto bit = graph.node_mask(id); bit.has_value() && (mask & *bit) != 0) {
            result.emplace_back(id);
        }
    }
    std::ranges::sort(result);
    return result;
}

int revealable_node_count(const MapSnapshot& map, const RunState& run)
{
    return static_cast<int>(std::ranges::count_if(map.nodes(), [&](const auto& entry) {
        return initially_unknown_for_reveal(map, run, entry.first);
    }));
}

int intermediate_interaction_cost(NodeType type) noexcept
{
    return type == NodeType::Empty || type == NodeType::ScrapShop ? 0 : 1;
}

struct RouteMetric
{
    int battles = 0;
    int processing_moves = 0;
    // 跨层保留的加工品有正残值；层末失效的 M04、M07 有负残值，本层不用就是浪费。
    // 总使用数与跨层使用数配合即可推出层末失效使用数。
    int persistent_processing_moves = 0;
    std::array<int, ProcessingMovementSlotCount> processing_move_counts {};
    int route_length = 0;
};

struct RouteRankingOptions
{
    bool minimize_intermediate_interactions = false;
    bool maximize_revealed_nodes = false;
    bool maximize_effective_nodes = false;
    // 自动化收集第一层允许作战，后续层由硬约束彻底避战；两种情况都不需要再按作战数排序。
    bool ignore_battle_tiebreaks = false;
    // 同等收益下先少用可跨层保留的加工品；在跨层用量相同时，多用层末失效加工品，避免浪费。
    bool optimize_processing_moves = false;
};

RouteMetric add_metric(RouteMetric lhs, const RouteMetric& rhs) noexcept
{
    const auto add = [](int first, int second) {
        return static_cast<int>(
            std::min<std::int64_t>(static_cast<std::int64_t>(first) + second, std::numeric_limits<int>::max()));
    };
    lhs.battles = add(lhs.battles, rhs.battles);
    lhs.processing_moves = add(lhs.processing_moves, rhs.processing_moves);
    lhs.persistent_processing_moves = add(lhs.persistent_processing_moves, rhs.persistent_processing_moves);
    for (std::size_t index = 0; index < lhs.processing_move_counts.size(); ++index) {
        lhs.processing_move_counts[index] = add(lhs.processing_move_counts[index], rhs.processing_move_counts[index]);
    }
    lhs.route_length = add(lhs.route_length, rhs.route_length);
    return lhs;
}

std::int64_t route_penalty(const RouteMetric& metric, const RouteRankingOptions& options) noexcept
{
    return static_cast<std::int64_t>(options.ignore_battle_tiebreaks ? 0 : metric.battles) +
           metric.processing_moves + metric.route_length;
}

std::vector<int> route_metric_tie(
    const RouteMetric& metric,
    const RouteRankingOptions& options,
    std::optional<int> intermediate_interactions = std::nullopt)
{
    std::vector<int> result {
        options.ignore_battle_tiebreaks ? 0 : metric.battles,
    };
    if (intermediate_interactions.has_value()) {
        result.emplace_back(*intermediate_interactions);
    }
    result.emplace_back(metric.persistent_processing_moves);
    if (options.optimize_processing_moves) {
        // 跨层用量已经相同后，总用量越大就等价于层末失效用量越大。
        result.emplace_back(-metric.processing_moves);
        for (const MovementKind movement : ProcessingMovementStrengthOrder) {
            const int count = metric.processing_move_counts[static_cast<std::size_t>(movement)];
            // 从强到弱逐项少用，等价于在同类加工品之间优先消耗低价值能力。
            result.emplace_back(count);
        }
    }
    result.emplace_back(metric.route_length);
    return result;
}

// 字典序先少用跨层加工品；跨层用量相同时，总用量越大，等价于层末失效加工品用得越多。
bool route_metric_weakly_better(
    const RouteMetric& lhs,
    const RouteMetric& rhs,
    const RouteRankingOptions& options) noexcept
{
    return route_penalty(lhs, options) <= route_penalty(rhs, options) &&
           route_metric_tie(lhs, options) <= route_metric_tie(rhs, options);
}

struct RouteMilestone
{
    const Milestone* definition = nullptr;
    int initial_progress = 0;
    // 本轮被可行性阶梯锁定。锁定的目标排在字典序最高位，降级的回到软层按 kind 计分。
    bool binding = false;
};

std::vector<RouteMilestone> route_milestones(
    const ResolvedPolicy& policy,
    const MissionState& mission,
    int floor,
    const FactStore& facts,
    const std::unordered_set<std::string>& binding_milestone_ids)
{
    std::vector<RouteMilestone> result;
    for (const Milestone& milestone : policy.milestones) {
        const MilestoneStatus status = mission.status(milestone.id);
        if (floor < milestone.floor_begin || floor > milestone.floor_end || status == MilestoneStatus::Satisfied ||
            status == MilestoneStatus::Missed || status == MilestoneStatus::Impossible ||
            !milestone.active_if.evaluate(facts) || milestone.selector.empty()) {
            continue;
        }
        result.emplace_back(
            RouteMilestone {
                &milestone,
                mission.progress(milestone.id),
                binding_milestone_ids.contains(milestone.id),
            });
    }
    std::ranges::sort(result, [](const RouteMilestone& lhs, const RouteMilestone& rhs) {
        if (lhs.binding != rhs.binding) {
            return lhs.binding;
        }
        return std::tie(lhs.definition->kind, lhs.definition->rank, lhs.definition->id) <
               std::tie(rhs.definition->kind, rhs.definition->rank, rhs.definition->id);
    });
    return result;
}

bool simulated_prerequisites_satisfied(
    const Milestone& milestone,
    const std::vector<RouteMilestone>& milestones,
    const std::vector<int>& progress,
    const MissionState& mission)
{
    return std::ranges::all_of(milestone.prerequisites, [&](const std::string& prerequisite) {
        if (mission.status(prerequisite) == MilestoneStatus::Satisfied) {
            return true;
        }
        for (std::size_t index = 0; index < milestones.size(); ++index) {
            if (milestones[index].definition->id == prerequisite) {
                return progress[index] >= milestones[index].definition->required_count;
            }
        }
        return false;
    });
}

using CountedNodes = std::vector<PlannerNodeMask>;

std::vector<std::string> advance_milestones(
    const Node& node,
    const OnDemandStateGraph& graph,
    const std::vector<RouteMilestone>& milestones,
    const MissionState& mission,
    std::vector<int>& progress,
    CountedNodes& counted,
    int unknown_nodes_revealed = 0)
{
    std::vector<std::string> advanced;
    const std::vector<int> before = progress;
    for (std::size_t index = 0; index < milestones.size(); ++index) {
        const Milestone& milestone = *milestones[index].definition;
        const auto node_bit = graph.node_mask(node.id);
        if (before[index] >= milestone.required_count || !milestone_matches_node(milestone, node) ||
            milestone.minimum_unknown_nodes_revealed > unknown_nodes_revealed ||
            (node_bit.has_value() && (counted[index] & *node_bit) != 0) ||
            !simulated_prerequisites_satisfied(milestone, milestones, before, mission)) {
            continue;
        }
        progress[index] = std::min(milestone.required_count, before[index] + 1);
        if (node_bit.has_value()) {
            counted[index] |= *node_bit;
        }
        advanced.emplace_back(milestone.id);
    }
    return advanced;
}

// 已锁定目标占字典序最高位。锁定过的目标一定排在 milestones 的前缀，所以这里从头扫到第一个
// 非锁定项为止即可。
std::vector<int> binding_progress_score(const std::vector<RouteMilestone>& milestones, const std::vector<int>& progress)
{
    std::vector<int> score;
    std::size_t begin = 0;
    while (begin < milestones.size() && milestones[begin].binding) {
        const int rank = milestones[begin].definition->rank;
        int completed = 0;
        int sum = 0;
        std::size_t end = begin;
        while (end < milestones.size() && milestones[end].binding && milestones[end].definition->rank == rank) {
            completed += progress[end] >= milestones[end].definition->required_count ? 1 : 0;
            sum += milestones[end].definition->weight *
                   std::min(progress[end], milestones[end].definition->required_count);
            ++end;
        }
        score.emplace_back(completed);
        score.emplace_back(sum);
        begin = end;
    }
    return score;
}

std::vector<std::int64_t>
    preferred_progress_score(const std::vector<RouteMilestone>& milestones, const std::vector<int>& progress)
{
    std::vector<std::int64_t> score;
    for (const MilestoneKind kind : { MilestoneKind::Preferred, MilestoneKind::Opportunistic }) {
        std::size_t begin = 0;
        while (begin < milestones.size()) {
            while (begin < milestones.size() &&
                   (milestones[begin].binding || milestones[begin].definition->kind != kind)) {
                ++begin;
            }
            if (begin == milestones.size()) {
                break;
            }
            const int rank = milestones[begin].definition->rank;
            std::int64_t reward = 0;
            std::size_t end = begin;
            while (end < milestones.size() && !milestones[end].binding && milestones[end].definition->kind == kind &&
                   milestones[end].definition->rank == rank) {
                const Milestone& milestone = *milestones[end].definition;
                const int gained = std::max(
                    0,
                    std::min(progress[end], milestone.required_count) -
                        std::min(milestones[end].initial_progress, milestone.required_count));
                reward += static_cast<std::int64_t>(milestone.weight) * gained;
                ++end;
            }
            score.emplace_back(reward);
            begin = end;
        }
    }
    return score;
}

bool score_greater(const std::vector<int>& lhs, const std::vector<int>& rhs)
{
    return std::lexicographical_compare(rhs.begin(), rhs.end(), lhs.begin(), lhs.end());
}

using RouteTraceId = std::size_t;
inline constexpr RouteTraceId InvalidRouteTraceId = std::numeric_limits<RouteTraceId>::max();

struct RouteTraceNode
{
    RouteTraceId parent = InvalidRouteTraceId;
    NodeId entered_node = InvalidNodeId;
    PlannedRouteStep step;
};

class RouteTraceArena
{
public:
    RouteTraceArena() { m_nodes.reserve(4096); }

    RouteTraceId append(RouteTraceId parent, NodeId entered_node, PlannedRouteStep step)
    {
        const RouteTraceId id = m_nodes.size();
        m_nodes.emplace_back(RouteTraceNode { parent, entered_node, std::move(step) });
        return id;
    }

    [[nodiscard]] std::vector<PlannedRouteStep> steps(RouteTraceId id) const
    {
        std::vector<PlannedRouteStep> result;
        while (id != InvalidRouteTraceId) {
            result.emplace_back(m_nodes[id].step);
            id = m_nodes[id].parent;
        }
        std::ranges::reverse(result);
        return result;
    }

    [[nodiscard]] std::vector<NodeId> route(RouteTraceId id) const
    {
        std::vector<NodeId> result;
        while (id != InvalidRouteTraceId) {
            if (m_nodes[id].entered_node != InvalidNodeId) {
                result.emplace_back(m_nodes[id].entered_node);
            }
            id = m_nodes[id].parent;
        }
        std::ranges::reverse(result);
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_nodes.size(); }

private:
    std::vector<RouteTraceNode> m_nodes;
};

struct RouteLabel
{
    SafetyStateId state = 0;
    int action_points = 0;
    std::vector<int> progress;
    CountedNodes counted;
    RouteMetric metric;
    int intermediate_interactions = 0;
    PlannerNodeMask revealed_nodes = 0;
    // 实际落点中的有效节点，按节点去重；徒步路径中间节点不会写入。
    PlannerNodeMask effective_nodes = 0;
    // 具体基础分随节点类型和楼层决定；藏果地和坎诺特的触须落点各额外 +1。
    // 重复落点不重复计分。
    int effective_node_score = 0;
    std::vector<std::string> immediate_milestone_ids;
    RouteTraceId trace = InvalidRouteTraceId;
    // 搜索过程中保持为空；只对最终胜出的标签从 trace 一次性还原。
    std::vector<NodeId> route;
    std::vector<PlannedRouteStep> steps;
};

RouteLabel with_direct_exhaustion(RouteLabel route, NodeId current_node, RouteTraceArena& traces)
{
    MoveCandidate direct;
    direct.action_id = "direct_exhaustion";
    direct.movement = MovementKind::Walk;
    direct.source = current_node;
    direct.target = current_node;
    direct.landing = current_node;
    direct.predicted_action_point_cost = std::max(route.action_points, 0);
    direct.action_point_requirement = 0;
    direct.direct_exhaustion = true;
    direct.terminal_on_completion = true;

    route.metric.route_length =
        add_metric(route.metric, RouteMetric { .route_length = 1 }).route_length;
    route.trace = traces.append(
        route.trace,
        InvalidNodeId,
        PlannedRouteStep {
            std::move(direct),
            route.action_points,
            std::max(route.action_points, 0),
            0,
            0,
        });
    route.action_points = 0;
    return route;
}

bool route_label_better(
    const RouteLabel& lhs,
    const RouteLabel& rhs,
    const std::vector<RouteMilestone>& milestones,
    const RouteRankingOptions& options)
{
    const auto lhs_end = binding_progress_score(milestones, lhs.progress);
    const auto rhs_end = binding_progress_score(milestones, rhs.progress);
    if (score_greater(lhs_end, rhs_end)) {
        return true;
    }
    if (score_greater(rhs_end, lhs_end)) {
        return false;
    }
    if (options.maximize_revealed_nodes && node_mask_size(lhs.revealed_nodes) != node_mask_size(rhs.revealed_nodes)) {
        return node_mask_size(lhs.revealed_nodes) > node_mask_size(rhs.revealed_nodes);
    }
    if (options.maximize_effective_nodes && lhs.effective_node_score != rhs.effective_node_score) {
        return lhs.effective_node_score > rhs.effective_node_score;
    }
    const auto lhs_preferred = preferred_progress_score(milestones, lhs.progress);
    const auto rhs_preferred = preferred_progress_score(milestones, rhs.progress);
    for (std::size_t index = 0; index < std::min(lhs_preferred.size(), rhs_preferred.size()); ++index) {
        if (lhs_preferred[index] == rhs_preferred[index]) {
            continue;
        }
        const std::int64_t lhs_utility = lhs_preferred[index] - route_penalty(lhs.metric, options) -
                                         (options.minimize_intermediate_interactions ? lhs.intermediate_interactions : 0);
        const std::int64_t rhs_utility = rhs_preferred[index] - route_penalty(rhs.metric, options) -
                                         (options.minimize_intermediate_interactions ? rhs.intermediate_interactions : 0);
        if (lhs_utility != rhs_utility) {
            return lhs_utility > rhs_utility;
        }
        return lhs_preferred[index] > rhs_preferred[index];
    }
    if (options.minimize_intermediate_interactions) {
        const auto lhs_metric = route_metric_tie(lhs.metric, options, lhs.intermediate_interactions);
        const auto rhs_metric = route_metric_tie(rhs.metric, options, rhs.intermediate_interactions);
        if (lhs_metric != rhs_metric) {
            return lhs_metric < rhs_metric;
        }
    }
    else if (route_metric_tie(lhs.metric, options) != route_metric_tie(rhs.metric, options)) {
        return route_metric_tie(lhs.metric, options) < route_metric_tie(rhs.metric, options);
    }
    return lhs.action_points > rhs.action_points;
}

bool boolean_fact(const FactStore& facts, std::string_view name)
{
    const FactValue* value = facts.find(name);
    return value != nullptr && std::holds_alternative<bool>(*value) && std::get<bool>(*value);
}

struct BudgetStateKey
{
    SafetyStateId state = 0;
    int action_points = 0;

    bool operator==(const BudgetStateKey&) const noexcept = default;
};

struct BudgetStateKeyHash
{
    std::size_t operator()(const BudgetStateKey& key) const noexcept
    {
        const std::size_t first = std::hash<SafetyStateId> {}(key.state);
        const std::size_t second = std::hash<int> {}(key.action_points);
        return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
    }
};

class OnDemandSafetyOracle
{
public:
    explicit OnDemandSafetyOracle(
        OnDemandStateGraph& graph,
        std::string instance_name,
        bool enable_resource_dominance = true) :
        m_graph(graph),
        m_solver([&graph, instance_name = std::move(instance_name), enable_resource_dominance]() mutable {
            auto problem = make_on_demand_safety_value_problem(graph, std::move(instance_name));
            if (!enable_resource_dominance) {
                problem.dominance_descriptor = {};
            }
            return problem;
        }())
    {
    }

    bool certifies(SafetyStateId state, int action_points)
    {
        const int required = exact_requirement(state, action_points);
        return required < UnreachableActionPointRequirement && action_points >= required;
    }

    bool action_certifies(const OnDemandSafetyAction& action, int action_points, std::size_t* proof_depth = nullptr)
    {
        const int required = exact_action_requirement(action, action_points);
        if (required >= UnreachableActionPointRequirement || action_points < required) {
            return false;
        }
        std::size_t depth = 0;
        for (const OnDemandSafetyOutcome& outcome : action.outcomes) {
            const int remaining =
                action_points_after(action_points, action.action_point_cost, outcome.action_point_gain);
            const auto successor_proof = proof(outcome.successor, remaining);
            if (!successor_proof.has_value()) {
                return false;
            }
            depth = std::max(depth, successor_proof->depth + 1);
        }
        if (proof_depth != nullptr) {
            *proof_depth = depth;
        }
        return true;
    }

    int requirement(SafetyStateId state, int maximum_action_points)
    {
        return exact_requirement(state, maximum_action_points);
    }

    int action_requirement(const OnDemandSafetyAction& action, int maximum_action_points)
    {
        return exact_action_requirement(action, maximum_action_points);
    }

    // 安全层关闭时没有可证的命题，证明深度与见证动作都不存在。求解器对这两问会按
    // 「未求解」报错，因此在这里一并短路。
    std::optional<std::size_t> cached_depth(SafetyStateId state, int action_points)
    {
        if (m_graph.exhaustion_terminates()) {
            return std::size_t { 0 };
        }
        std::string error;
        const auto depth = m_solver.bounded_proof_depth(state, action_points, &error);
        if (!error.empty()) {
            m_error = std::move(error);
        }
        return depth;
    }

    std::optional<std::string> lexicographic_first_action(SafetyStateId state, int action_points)
    {
        if (m_graph.exhaustion_terminates()) {
            return std::nullopt;
        }
        std::string error;
        const auto action = m_solver.bounded_witness(state, action_points, &error);
        if (!error.empty()) {
            m_error = std::move(error);
        }
        return action;
    }

    std::optional<std::string> first_action(SafetyStateId state, int action_points)
    {
        return lexicographic_first_action(state, action_points);
    }

    const std::string& error() const noexcept { return m_error; }

private:
    struct Proof
    {
        std::size_t depth = 0;
        std::string action_id;
    };

    // 锁定目标为空且策略允许耗尽收工时，任何走法都能合法收场，安全层没有可证的命题。
    // 直接返回零比让求解器把整个闭包展开一遍再得出同一结论便宜得多。
    int exact_requirement(SafetyStateId state, int maximum_action_points)
    {
        if (m_graph.exhaustion_terminates()) {
            return 0;
        }
        std::string error;
        const int required = m_solver.N_bounded(state, maximum_action_points, &error);
        if (!error.empty()) {
            m_error = std::move(error);
        }
        return required;
    }

    int exact_action_requirement(const OnDemandSafetyAction& action, int maximum_action_points)
    {
        const int first_budget = std::max(action.minimum_action_points_to_start, action.action_point_cost);
        // 下面的扫描在预算不足时一次都不进循环，因而「付不起」与「不可达」共用同一个返回值，
        // 调用方只检查这一个。短路必须保持同一约定，否则付不起的动作会被当成可行。
        //
        // 这条短路同时跳过了对全部结果的遍历，随机落点的动作（目前只有 M07 小八界）因此
        // 不再按最坏落点定价。襁褓动物在最后一层可以接受——落到哪里都算收工，而 M07 本层
        // 不用就作废。换到「走不出去有代价」的层或策略上复用时，这一条必须重新评估。
        if (m_graph.exhaustion_terminates()) {
            return first_budget <= maximum_action_points ? first_budget : UnreachableActionPointRequirement;
        }
        for (int action_points = first_budget; action_points <= maximum_action_points; ++action_points) {
            bool safe = true;
            for (const OnDemandSafetyOutcome& outcome : action.outcomes) {
                const int remaining =
                    action_points_after(action_points, action.action_point_cost, outcome.action_point_gain);
                const int successor = exact_requirement(outcome.successor, remaining);
                if (successor >= UnreachableActionPointRequirement || successor > remaining) {
                    safe = false;
                    break;
                }
            }
            if (safe) {
                return action_points;
            }
        }
        return UnreachableActionPointRequirement;
    }

    std::optional<Proof> proof(SafetyStateId state, int action_points)
    {
        if (m_graph.is_terminal(state) || m_graph.exhaustion_terminates()) {
            return Proof {};
        }
        const BudgetStateKey key { state, action_points };
        if (const auto found = m_proofs.find(key); found != m_proofs.end()) {
            return found->second;
        }
        std::unordered_set<BudgetStateKey, BudgetStateKeyHash> visiting;
        return build_proof(state, action_points, visiting);
    }

    std::optional<Proof> build_proof(
        SafetyStateId state,
        int action_points,
        std::unordered_set<BudgetStateKey, BudgetStateKeyHash>& visiting)
    {
        if (m_graph.is_terminal(state) || m_graph.exhaustion_terminates()) {
            return Proof {};
        }
        const BudgetStateKey key { state, action_points };
        if (const auto found = m_proofs.find(key); found != m_proofs.end()) {
            return found->second;
        }
        if (!visiting.emplace(key).second) {
            return std::nullopt;
        }
        const int state_requirement = exact_requirement(state, action_points);
        const auto* actions = m_graph.actions(state, &m_error);
        if (actions == nullptr) {
            visiting.erase(key);
            return std::nullopt;
        }
        const auto attempt = [&](const OnDemandSafetyAction& action) -> std::optional<Proof> {
            if (exact_action_requirement(action, action_points) != state_requirement) {
                return std::nullopt;
            }
            std::size_t depth = 0;
            for (const OnDemandSafetyOutcome& outcome : action.outcomes) {
                const int remaining =
                    action_points_after(action_points, action.action_point_cost, outcome.action_point_gain);
                const auto successor = build_proof(outcome.successor, remaining, visiting);
                if (!successor.has_value()) {
                    return std::nullopt;
                }
                depth = std::max(depth, successor->depth + 1);
            }
            return Proof { depth, action.candidate.action_id };
        };
        std::optional<Proof> selected;
        for (const OnDemandSafetyAction& action : *actions) {
            const bool direct_terminal =
                std::ranges::all_of(action.outcomes, [&](const OnDemandSafetyOutcome& outcome) {
                    return m_graph.is_terminal(outcome.successor);
                });
            if (direct_terminal && (selected = attempt(action)).has_value()) {
                break;
            }
        }
        if (!selected.has_value()) {
            for (const OnDemandSafetyAction& action : *actions) {
                const bool direct_terminal =
                    std::ranges::all_of(action.outcomes, [&](const OnDemandSafetyOutcome& outcome) {
                        return m_graph.is_terminal(outcome.successor);
                    });
                if (!direct_terminal && (selected = attempt(action)).has_value()) {
                    break;
                }
            }
        }
        visiting.erase(key);
        if (selected.has_value()) {
            m_proofs.insert_or_assign(key, *selected);
        }
        return selected;
    }

    OnDemandStateGraph& m_graph;
    SafetyValueSolver m_solver;
    std::unordered_map<BudgetStateKey, Proof, BudgetStateKeyHash> m_proofs;
    std::string m_error;
};

struct FloorThreeBossApproach
{
    std::vector<NodeId> route;
    std::vector<PlannedRouteStep> steps;
    int processing_moves = 0;
    int persistent_processing_moves = 0;
    std::array<int, ProcessingMovementSlotCount> processing_move_counts {};
    int route_length = 0;
};

NodeType route_node_type(const MapSnapshot& map, const OnDemandStateGraph& graph, SafetyStateId source, NodeId node);

struct FloorThreeBossSearchKey
{
    SafetyStateId state = 0;
    int action_points = 0;
    std::uint8_t processing_moves = 0;

    bool operator==(const FloorThreeBossSearchKey&) const noexcept = default;
};

struct FloorThreeBossSearchKeyHash
{
    std::size_t operator()(const FloorThreeBossSearchKey& key) const noexcept
    {
        std::size_t seed = std::hash<SafetyStateId> {}(key.state);
        seed ^= std::hash<int> {}(key.action_points) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<std::uint8_t> {}(key.processing_moves) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
                (seed >> 2U);
        return seed;
    }
};

std::optional<FloorThreeBossApproach> find_floor_three_boss_before_direct_exhaustion(
    const MapSnapshot& map,
    OnDemandStateGraph& graph,
    OnDemandSafetyOracle& oracle,
    int initial_action_points,
    std::string* error)
{
    struct Pending
    {
        SafetyStateId state = 0;
        int action_points = 0;
        FloorThreeBossApproach approach;
    };
    const auto lower_priority = [](const Pending& lhs, const Pending& rhs) {
        return std::tuple {
                   lhs.approach.processing_moves,
                   lhs.approach.route_length,
                   lhs.approach.steps.size(),
               } >
               std::tuple {
                   rhs.approach.processing_moves,
                   rhs.approach.route_length,
                   rhs.approach.steps.size(),
               };
    };

    std::priority_queue<Pending, std::vector<Pending>, decltype(lower_priority)> pending(lower_priority);
    pending.emplace(Pending { graph.initial_state(), initial_action_points, {} });
    std::unordered_set<FloorThreeBossSearchKey, FloorThreeBossSearchKeyHash> visited;

    while (!pending.empty()) {
        Pending current = pending.top();
        pending.pop();
        const FloorThreeBossSearchKey key {
            current.state,
            current.action_points,
            static_cast<std::uint8_t>(current.approach.processing_moves),
        };
        if (!visited.emplace(key).second) {
            continue;
        }

        const auto* actions = graph.actions(current.state, error);
        if (actions == nullptr) {
            return std::nullopt;
        }
        for (const OnDemandSafetyAction& action : *actions) {
            if (action.candidate.direct_exhaustion || action.outcomes.size() != 1) {
                continue;
            }
            const MovementSpec* movement = find_movement_spec(action.candidate.movement);
            const bool processing = action.candidate.movement != MovementKind::Walk;
            if ((processing && (movement == nullptr || movement->range == MovementRange::FullMap)) ||
                current.approach.processing_moves + (processing ? 1 : 0) > 1) {
                continue;
            }

            const int requirement = oracle.action_requirement(action, current.action_points);
            if (requirement >= UnreachableActionPointRequirement || current.action_points < requirement) {
                continue;
            }
            const OnDemandSafetyOutcome& outcome = action.outcomes.front();
            const int remaining =
                action_points_after(current.action_points, action.action_point_cost, outcome.action_point_gain);
            const NodeId landing = graph.state(outcome.successor).node;

            Pending next = current;
            next.state = outcome.successor;
            next.action_points = remaining;
            MoveCandidate planned_move = action.candidate;
            planned_move.graph_layer = GraphLayer::Confirmed;
            planned_move.action_point_requirement = requirement;
            next.approach.steps.emplace_back(
                PlannedRouteStep {
                    std::move(planned_move),
                    current.action_points,
                    action.action_point_cost,
                    outcome.action_point_gain,
                    remaining,
                });
            next.approach.route.emplace_back(landing);
            const int step_length = processing ? 1 : std::max(1, static_cast<int>(action.candidate.path.size()));
            next.approach.route_length += step_length;
            if (processing) {
                ++next.approach.processing_moves;
                ++next.approach.processing_move_counts[static_cast<std::size_t>(action.candidate.movement)];
                if (movement != nullptr && !movement->expires_on_floor_end) {
                    ++next.approach.persistent_processing_moves;
                }
            }

            const NodeType landing_type = route_node_type(map, graph, current.state, landing);
            const Node* landing_node = map.find_node(landing);
            if (landing_type == NodeType::BattleBoss && landing_node != nullptr && landing_node->floor == 3) {
                return next.approach;
            }
            if (graph.is_terminal(outcome.successor) ||
                (graph.exhaustion_terminates() && remaining <= 0)) {
                continue;
            }
            pending.emplace(std::move(next));
        }
    }
    if (!oracle.error().empty() && error != nullptr) {
        *error = oracle.error();
    }
    return std::nullopt;
}

struct MobileMarkerLookaheadResult
{
    bool robust = true;
    std::size_t outcomes_checked = 0;
    std::string error;
};

bool node_matches_mobile_marker_types(
    const Node& node,
    const std::unordered_set<std::string>& mobile_marker_types) noexcept
{
    return mobile_marker_types.contains(node.marker_type) ||
           (node.marker_resident_overlap_possible && mobile_marker_types.contains("savage"));
}

MobileMarkerLookaheadResult assess_mobile_marker_lookahead(
    const MapSnapshot& map,
    OnDemandStateGraph& graph,
    OnDemandSafetyOracle& oracle,
    const OnDemandSafetyAction& root_action,
    int current_action_points,
    const std::unordered_set<std::string>& mobile_marker_types)
{
    MobileMarkerLookaheadResult result;
    std::vector<NodeId> residents;
    for (const auto& [id, node] : map.nodes()) {
        if (node_matches_mobile_marker_types(node, mobile_marker_types)) {
            residents.emplace_back(id);
        }
    }
    std::ranges::sort(residents);
    if (residents.empty()) {
        return result;
    }

    for (const OnDemandSafetyOutcome& root_outcome : root_action.outcomes) {
        const int remaining = action_points_after(
            current_action_points,
            root_action.action_point_cost,
            root_outcome.action_point_gain);
        if (graph.is_terminal(root_outcome.successor) ||
            (remaining <= 0 && graph.exhaustion_terminates())) {
            continue;
        }

        std::unordered_set<NodeId> completed_nodes;
        for (const auto& [id, node] : map.nodes()) {
            (void)node;
            if (graph.is_completed(root_outcome.successor, id)) {
                completed_nodes.emplace(id);
            }
        }
        const NodeId player_landing = graph.state(root_outcome.successor).node;
        // “居民”据点在落点结算时先被清除，随后所有流窜居民一并消失；这一分支不再有居民回合。
        if (is_resident_settlement(map.find_node(player_landing))) {
            continue;
        }
        std::vector<std::vector<NodeId>> per_resident_positions;
        per_resident_positions.reserve(residents.size());
        for (const NodeId resident : residents) {
            per_resident_positions.emplace_back(
                roaming_resident_next_positions(map, resident, player_landing, completed_nodes));
        }

        std::string action_error;
        const auto* responses = graph.actions(root_outcome.successor, &action_error);
        if (responses == nullptr || !action_error.empty()) {
            result.error = action_error.empty() ? "failed to enumerate resident-response actions" : action_error;
            result.robust = false;
            return result;
        }
        const bool every_outcome_has_response = every_roaming_resident_outcome_has_response(
            per_resident_positions,
            [&](const std::unordered_set<NodeId>& occupied) {
                ++result.outcomes_checked;
                for (const OnDemandSafetyAction& response : *responses) {
                    if (move_intersects_nodes(response.candidate, occupied)) {
                        continue;
                    }
                    const int requirement = oracle.action_requirement(response, remaining);
                    if (requirement < UnreachableActionPointRequirement && requirement <= remaining) {
                        return true;
                    }
                }
                return false;
            });
        if (!oracle.error().empty()) {
            result.error = oracle.error();
            result.robust = false;
            return result;
        }
        if (!every_outcome_has_response) {
            result.robust = false;
            return result;
        }
    }
    return result;
}

NodeType route_node_type(const MapSnapshot& map, const OnDemandStateGraph& graph, SafetyStateId source, NodeId node)
{
    const Node* target = map.find_node(node);
    if (target == nullptr) {
        return NodeType::Unknown;
    }
    if (!target->traversal.repeatable && graph.is_completed(source, node)) {
        return NodeType::Empty;
    }
    return target->type;
}

RouteMetric move_metric(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    SafetyStateId source,
    const MoveCandidate& move,
    NodeId landing = InvalidNodeId)
{
    const NodeId entered = !move.controllable && landing != InvalidNodeId ? landing : move.target;
    const bool battle = is_route_battle_node_type(route_node_type(map, graph, source, entered));
    const MovementSpec* spec = find_movement_spec(move.movement);
    const bool processing = move.movement != MovementKind::Walk;
    const bool persistent = processing && spec != nullptr && !spec->expires_on_floor_end;
    RouteMetric result;
    result.battles = battle ? 1 : 0;
    result.processing_moves = processing ? 1 : 0;
    result.persistent_processing_moves = persistent ? 1 : 0;
    if (processing) {
        result.processing_move_counts[static_cast<std::size_t>(move.movement)] = 1;
    }
    result.route_length = processing ? 1 : std::max(1, static_cast<int>(move.path.size()));
    return result;
}

void add_effective_landing(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    SafetyStateId source,
    NodeId landing,
    MovementKind movement,
    int& effective_node_score,
    PlannerNodeMask& effective_nodes)
{
    const Node* landing_node = map.find_node(landing);
    const auto bit = graph.node_mask(landing);
    if (landing == InvalidNodeId || landing == graph.source_run().current_node || landing_node == nullptr ||
        !bit.has_value() || (effective_nodes & *bit) != 0 || graph.source_run().visited_nodes.contains(landing)) {
        return;
    }
    const int weight = effective_node_weight_at_floor(
        route_node_type(map, graph, source, landing),
        landing_node->floor,
        landing_node->marker_type,
        movement);
    if (weight <= 0) {
        return;
    }
    effective_nodes |= *bit;
    effective_node_score += weight;
}

int maximum_future_entries(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    SafetyStateId state_id,
    int action_points)
{
    const PlannerState& state = graph.state(state_id);
    int remaining_nodes = 0;
    int possible_light_gains = 0;
    for (const auto& [id, node] : map.nodes()) {
        if (node.progress == NodeProgress::Removed || route_node_type(map, graph, state_id, id) == NodeType::Empty) {
            continue;
        }
        ++remaining_nodes;
        if (node.type == NodeType::Light) {
            ++possible_light_gains;
        }
    }
    if (remaining_nodes == 0) {
        return 0;
    }

    const DynamicCostModel& costs = graph.source_run().costs;
    if (costs.walk_cost_per_edge == 0 ||
        std::ranges::any_of(costs.action_cost_overrides, [](const auto& value) { return value.second == 0; })) {
        return remaining_nodes;
    }

    std::int64_t entries = std::max(action_points, 0) + possible_light_gains;
    for (const MovementSpec& movement : movement_specs()) {
        if (movement.kind == MovementKind::Walk) {
            continue;
        }
        const int charges = state.movement_charges[static_cast<std::size_t>(movement.kind)];
        if (charges <= 0) {
            continue;
        }
        const int cost = costs.movement_cost(movement);
        if (cost == 0) {
            entries += charges;
        }
        entries += static_cast<std::int64_t>(charges) * std::max(movement.effect.action_point_gain, 0);
    }
    return static_cast<int>(std::min<std::int64_t>(remaining_nodes, entries));
}

int maximum_effective_node_score(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    const RouteLabel& current,
    int entry_limit)
{
    std::vector<int> remaining_weights;
    remaining_weights.reserve(map.nodes().size());
    for (const auto& [id, node] : map.nodes()) {
        if (node.progress == NodeProgress::Removed || id == graph.source_run().current_node ||
            graph.source_run().visited_nodes.contains(id) || node_mask_contains(graph, current.effective_nodes, id)) {
            continue;
        }
        remaining_weights.emplace_back(
            effective_node_weight_at_floor(
                route_node_type(map, graph, current.state, id),
                node.floor,
                node.marker_type));
    }
    std::ranges::sort(remaining_weights, std::greater {});
    const std::size_t possible_entries =
        std::min<std::size_t>(remaining_weights.size(), static_cast<std::size_t>(std::max(entry_limit, 0)));
    int upper = current.effective_node_score;
    for (std::size_t index = 0; index < possible_entries; ++index) {
        upper += remaining_weights[index];
    }

    // 坎诺特的触须可以让实际落点额外计 1。忽略其目标类型限制只会放宽上界，
    // 但次数必须受当前状态中的真实剩余次数约束。
    const PlannerState& state = graph.state(current.state);
    const int tentacle_charges = state.movement_charges[static_cast<std::size_t>(MovementKind::M10)];
    upper += std::min<int>(std::max(tentacle_charges, 0), static_cast<int>(possible_entries));
    return upper;
}

bool route_may_beat(
    const MapSnapshot& map,
    const OnDemandStateGraph& graph,
    const std::vector<RouteMilestone>& milestones,
    const RouteLabel& current,
    const RouteLabel& incumbent,
    const RouteRankingOptions& options)
{
    std::vector<int> upper_progress = current.progress;
    const int entry_limit = maximum_future_entries(map, graph, current.state, current.action_points);
    const int nonterminal_entry_limit = std::max(entry_limit - 1, 0);
    for (std::size_t index = 0; index < milestones.size(); ++index) {
        const Milestone& milestone = *milestones[index].definition;
        int matching_nonterminals = 0;
        bool matching_exact_terminal = false;
        for (const auto& [id, stored] : map.nodes()) {
            if (stored.progress == NodeProgress::Removed || node_mask_contains(graph, current.counted[index], id)) {
                continue;
            }
            Node node = stored;
            node.type = route_node_type(map, graph, current.state, id);
            if (node.type == NodeType::Empty) {
                node.name = EmptyNodeName;
            }
            if (!milestone_matches_node(milestone, node)) {
                continue;
            }
            if (graph.is_terminal_node(id)) {
                matching_exact_terminal = true;
            }
            else {
                ++matching_nonterminals;
            }
        }
        const int possible_increment = std::min(matching_nonterminals, nonterminal_entry_limit) +
                                       (entry_limit > 0 && matching_exact_terminal ? 1 : 0);
        upper_progress[index] = std::min(milestone.required_count, current.progress[index] + possible_increment);
    }

    const auto upper_end = binding_progress_score(milestones, upper_progress);
    const auto incumbent_end = binding_progress_score(milestones, incumbent.progress);
    if (score_greater(upper_end, incumbent_end)) {
        return true;
    }
    if (score_greater(incumbent_end, upper_end)) {
        return false;
    }

    // 只要还有未知节点没有被当前最优路线覆盖，就不能用常规的路程下界剪枝：多走一步
    // 可能正好打开一整片视野。全部揭示以后才回到原有的成本比较。
    if (options.maximize_revealed_nodes &&
        static_cast<int>(node_mask_size(incumbent.revealed_nodes)) < revealable_node_count(map, graph.source_run())) {
        return true;
    }
    if (options.maximize_effective_nodes) {
        const int upper_effective_score = maximum_effective_node_score(map, graph, current, entry_limit);
        if (upper_effective_score > incumbent.effective_node_score) {
            return true;
        }
        if (upper_effective_score < incumbent.effective_node_score) {
            return false;
        }
    }

    RouteMetric minimum_metric = current.metric;
    if (!graph.is_terminal(current.state)) {
        minimum_metric.route_length =
            add_metric(minimum_metric, RouteMetric { .route_length = 1 }).route_length;
        bool has_terminal = false;
        bool every_terminal_is_combat = true;
        for (const auto& [id, node] : map.nodes()) {
            if (node.progress == NodeProgress::Removed || !graph.is_terminal_node(id)) {
                continue;
            }
            has_terminal = true;
            every_terminal_is_combat = every_terminal_is_combat && is_route_battle_node_type(node.type);
        }
        if (!options.ignore_battle_tiebreaks && has_terminal && every_terminal_is_combat) {
            minimum_metric.battles = add_metric(minimum_metric, RouteMetric { .battles = 1 }).battles;
        }
    }

    const auto lower_preferred = preferred_progress_score(milestones, current.progress);
    std::vector<std::int64_t> bounded_upper_preferred = preferred_progress_score(milestones, upper_progress);
    const auto incumbent_preferred = preferred_progress_score(milestones, incumbent.progress);
    const std::int64_t current_penalty =
        route_penalty(current.metric, options) +
        (options.minimize_intermediate_interactions ? current.intermediate_interactions : 0);
    const std::int64_t minimum_penalty =
        route_penalty(minimum_metric, options) +
        (options.minimize_intermediate_interactions ? current.intermediate_interactions : 0);
    const std::int64_t incumbent_penalty =
        route_penalty(incumbent.metric, options) +
        (options.minimize_intermediate_interactions ? incumbent.intermediate_interactions : 0);
    std::vector<std::int64_t> upper_utilities;
    upper_utilities.reserve(bounded_upper_preferred.size());
    std::size_t score_index = 0;
    for (const MilestoneKind kind : { MilestoneKind::Preferred, MilestoneKind::Opportunistic }) {
        std::size_t begin = 0;
        while (begin < milestones.size()) {
            while (begin < milestones.size() &&
                   (milestones[begin].binding || milestones[begin].definition->kind != kind)) {
                ++begin;
            }
            if (begin == milestones.size()) {
                break;
            }
            const int rank = milestones[begin].definition->rank;
            std::size_t end = begin;
            while (end < milestones.size() && !milestones[end].binding && milestones[end].definition->kind == kind &&
                   milestones[end].definition->rank == rank) {
                ++end;
            }

            int best_terminal_reward = 0;
            int best_terminal_net = std::numeric_limits<int>::min();
            bool has_terminal = false;
            std::vector<int> nonterminal_rewards;
            std::vector<int> nonterminal_nets;
            for (const auto& [id, stored] : map.nodes()) {
                if (stored.progress == NodeProgress::Removed) {
                    continue;
                }
                Node node = stored;
                node.type = route_node_type(map, graph, current.state, id);
                if (node.type == NodeType::Empty) {
                    node.name = EmptyNodeName;
                }
                int reward = 0;
                for (std::size_t milestone_index = begin; milestone_index < end; ++milestone_index) {
                    const Milestone& milestone = *milestones[milestone_index].definition;
                    if (current.progress[milestone_index] >= milestone.required_count ||
                        node_mask_contains(graph, current.counted[milestone_index], id) ||
                        !milestone_matches_node(milestone, node)) {
                        continue;
                    }
                    reward += milestone.weight;
                }
                const bool terminal = graph.is_terminal_node(id);
                const int interaction_penalty =
                    options.minimize_intermediate_interactions ? (terminal ? 0 : intermediate_interaction_cost(node.type))
                                                               : 1;
                const int net = reward - interaction_penalty -
                                (!options.ignore_battle_tiebreaks && is_route_battle_node_type(node.type) ? 1 : 0);
                if (terminal) {
                    has_terminal = true;
                    best_terminal_reward = std::max(best_terminal_reward, reward);
                    best_terminal_net = std::max(best_terminal_net, net);
                }
                else if (reward > 0) {
                    nonterminal_rewards.emplace_back(reward);
                    nonterminal_nets.emplace_back(net);
                }
            }
            if (!has_terminal || score_index >= bounded_upper_preferred.size()) {
                return true;
            }
            std::ranges::sort(nonterminal_rewards, std::greater {});
            std::ranges::sort(nonterminal_nets, std::greater {});
            const std::size_t optional_slots =
                std::min<std::size_t>(nonterminal_rewards.size(), static_cast<std::size_t>(nonterminal_entry_limit));
            std::int64_t additive_reward_upper = lower_preferred[score_index] + best_terminal_reward;
            for (std::size_t position = 0; position < optional_slots; ++position) {
                additive_reward_upper += nonterminal_rewards[position];
            }
            bounded_upper_preferred[score_index] =
                std::min(bounded_upper_preferred[score_index], additive_reward_upper);

            std::int64_t additive_utility_upper = lower_preferred[score_index] - current_penalty + best_terminal_net;
            const std::size_t net_slots =
                std::min<std::size_t>(nonterminal_nets.size(), static_cast<std::size_t>(nonterminal_entry_limit));
            for (std::size_t position = 0; position < net_slots && nonterminal_nets[position] > 0; ++position) {
                additive_utility_upper += nonterminal_nets[position];
            }
            upper_utilities.emplace_back(
                std::min(additive_utility_upper, bounded_upper_preferred[score_index] - minimum_penalty));
            ++score_index;
            begin = end;
        }
    }
    if (score_index != bounded_upper_preferred.size()) {
        return true;
    }

    for (std::size_t index = 0; index < bounded_upper_preferred.size(); ++index) {
        const std::int64_t lower_reward = lower_preferred[index];
        const std::int64_t upper_reward = bounded_upper_preferred[index];
        const std::int64_t incumbent_reward = incumbent_preferred[index];
        const std::int64_t upper_utility = upper_utilities[index];
        const std::int64_t incumbent_utility = incumbent_reward - incumbent_penalty;
        if (upper_utility > incumbent_utility ||
            (upper_utility == incumbent_utility && upper_reward > incumbent_reward)) {
            return true;
        }
        if (incumbent_reward < lower_reward || incumbent_reward > upper_reward) {
            return false;
        }
    }

    if (options.minimize_intermediate_interactions) {
        const auto minimum_tie = route_metric_tie(
            minimum_metric,
            options,
            current.intermediate_interactions);
        const auto incumbent_tie = route_metric_tie(
            incumbent.metric,
            options,
            incumbent.intermediate_interactions);
        if (minimum_tie != incumbent_tie) {
            return minimum_tie < incumbent_tie;
        }
    }
    else {
        const auto minimum_tie = route_metric_tie(minimum_metric, options);
        const auto incumbent_tie = route_metric_tie(incumbent.metric, options);
        if (minimum_tie != incumbent_tie) {
            return minimum_tie < incumbent_tie;
        }
    }
    return true;
}

struct RouteSearchBudget
{
    std::chrono::steady_clock::time_point deadline;
    std::size_t remaining_expansions = 0;
    std::size_t initial_expansions = 0;

    [[nodiscard]] bool exhausted() const noexcept
    {
        return remaining_expansions == 0 || std::chrono::steady_clock::now() >= deadline;
    }

    [[nodiscard]] std::size_t consumed_expansions() const noexcept { return initial_expansions - remaining_expansions; }

    [[nodiscard]] bool time_exhausted() const noexcept { return std::chrono::steady_clock::now() >= deadline; }

    bool consume() noexcept
    {
        if (exhausted()) {
            return false;
        }
        --remaining_expansions;
        return true;
    }
};

struct RouteSearchStatistics
{
    std::size_t labels_generated = 0;
    std::size_t labels_dominated = 0;
    std::size_t labels_retained_peak = 0;
    std::size_t trace_nodes = 0;
};

RouteLabel best_route_after_outcome(
    const MapSnapshot& map,
    OnDemandStateGraph& graph,
    OnDemandSafetyOracle& oracle,
    const std::vector<RouteMilestone>& milestones,
    const MissionState& mission,
    const MoveCandidate& root_move,
    const OnDemandSafetyAction& root_action,
    const OnDemandSafetyOutcome& root_outcome,
    int initial_action_points,
    int remaining_action_points,
    const std::vector<std::string>* route_hint_action_ids,
    std::size_t* route_hint_replayed_steps,
    const RouteSearchOptions& search_options,
    RouteSearchBudget& search_budget,
    const RouteRankingOptions& ranking_options,
    RouteSearchStatistics* statistics,
    std::string* error)
{
    RouteTraceArena traces;
    RouteLabel initial;
    initial.state = root_outcome.successor;
    initial.action_points = remaining_action_points;
    initial.progress.reserve(milestones.size());
    initial.counted.resize(milestones.size());
    for (std::size_t index = 0; index < milestones.size(); ++index) {
        const RouteMilestone& milestone = milestones[index];
        initial.progress.emplace_back(milestone.initial_progress);
        const auto counted = mission.milestone_nodes.find(milestone.definition->id);
        if (counted != mission.milestone_nodes.end()) {
            for (const NodeId id : counted->second) {
                if (const auto bit = graph.node_mask(id); bit.has_value()) {
                    initial.counted[index] |= *bit;
                }
            }
        }
    }
    initial.metric = move_metric(
        map,
        graph,
        graph.initial_state(),
        root_move,
        graph.state(root_outcome.successor).node);
    add_effective_landing(
        map,
        graph,
        graph.initial_state(),
        graph.state(root_outcome.successor).node,
        root_move.movement,
        initial.effective_node_score,
        initial.effective_nodes);
    PlannedRouteStep initial_step {
            root_move,
            initial_action_points,
            root_action.action_point_cost,
            root_outcome.action_point_gain,
            remaining_action_points,
    };
    const NodeId entered_node =
        root_move.controllable ? root_move.target : graph.state(root_outcome.successor).node;
    if (ranking_options.minimize_intermediate_interactions && !graph.is_terminal(root_outcome.successor)) {
        initial.intermediate_interactions =
            intermediate_interaction_cost(route_node_type(map, graph, graph.initial_state(), entered_node));
    }
    if (const Node* target = map.find_node(entered_node); target != nullptr) {
        const bool endpoint_observation_available =
            move_endpoint_observation_available(root_move.terminal_on_completion, remaining_action_points);
        Node entered = *target;
        entered.type = route_node_type(map, graph, graph.initial_state(), target->id);
        if (entered.type == NodeType::Empty) {
            entered.name = EmptyNodeName;
        }
        initial.immediate_milestone_ids = advance_milestones(
            entered,
            graph,
            milestones,
            mission,
            initial.progress,
            initial.counted,
            endpoint_observation_available
                ? unknown_big_nodes_revealed(map, graph, graph.initial_state(), entered.id)
                : 0);
        initial.trace = traces.append(InvalidRouteTraceId, target->id, std::move(initial_step));
        add_newly_revealed_nodes(
            map,
            graph,
            graph.source_run(),
            root_move,
            graph.state(root_outcome.successor).node,
            endpoint_observation_available,
            initial.revealed_nodes);
    }
    else {
        initial.trace = traces.append(InvalidRouteTraceId, InvalidNodeId, std::move(initial_step));
    }

    const auto materialize = [&](RouteLabel route) {
        route.route = traces.route(route.trace);
        route.steps = traces.steps(route.trace);
        if (statistics != nullptr) {
            statistics->trace_nodes += traces.size();
        }
        return route;
    };
    const auto fits_inventory = [&](const RouteLabel& route) {
        for (std::size_t index = 1; index < route.metric.processing_move_counts.size(); ++index) {
            const MovementKind movement = static_cast<MovementKind>(index);
            const auto available = graph.source_run().resources.movement_charges.find(movement);
            const int charges = available == graph.source_run().resources.movement_charges.end()
                                    ? 0
                                    : std::max(available->second, 0);
            if (route.metric.processing_move_counts[index] > charges) {
                return false;
            }
        }
        return true;
    };

    std::unordered_map<SafetyStateId, std::vector<RouteLabel>> labels;
    labels[initial.state].emplace_back(initial);
    std::size_t retained_labels = 1;
    if (statistics != nullptr) {
        ++statistics->labels_generated;
        statistics->labels_retained_peak = std::max(statistics->labels_retained_peak, retained_labels);
    }

    struct PendingRoute
    {
        RouteLabel route;
        std::size_t terminal_depth = std::numeric_limits<std::size_t>::max();
    };

    const auto make_pending = [&](RouteLabel route) {
        return PendingRoute {
            route,
            oracle.cached_depth(route.state, route.action_points).value_or(std::numeric_limits<std::size_t>::max()),
        };
    };
    const auto lower_priority = [&](const PendingRoute& lhs, const PendingRoute& rhs) {
        if (route_label_better(
                lhs.route,
                rhs.route,
                milestones,
                ranking_options)) {
            return false;
        }
        if (route_label_better(
                rhs.route,
                lhs.route,
                milestones,
                ranking_options)) {
            return true;
        }
        return lhs.terminal_depth > rhs.terminal_depth;
    };
    std::priority_queue<PendingRoute, std::vector<PendingRoute>, decltype(lower_priority)> pending(lower_priority);
    pending.emplace(make_pending(initial));
    std::optional<RouteLabel> best;
    const std::size_t route_expansion_budget_per_root = search_options.expansions_per_root;
    std::size_t expanded_routes = 0;

    const auto action_matches_current_resources = [&](SafetyStateId source_id, const OnDemandSafetyAction& action) {
        if (action.outcomes.size() != 1) {
            return false;
        }
        const PlannerState& source = graph.state(source_id);
        if (action.candidate.source != source.node) {
            return false;
        }
        if (action.candidate.movement == MovementKind::Walk) {
            return true;
        }
        const std::size_t movement = static_cast<std::size_t>(action.candidate.movement);
        if (movement >= source.movement_charges.size() || source.movement_charges[movement] == 0) {
            return false;
        }
        const PlannerState& successor = graph.state(action.outcomes.front().successor);
        return successor.movement_charges[movement] + 1 == source.movement_charges[movement];
    };

    const auto build_next = [&](const RouteLabel& current,
                                const OnDemandSafetyAction& action,
                                bool verify_safety = true) -> std::optional<RouteLabel> {
        if (!action_matches_current_resources(current.state, action)) {
            return std::nullopt;
        }
        const int requirement = verify_safety
                                    ? oracle.action_requirement(action, current.action_points)
                                    : std::max(action.minimum_action_points_to_start, action.action_point_cost);
        if (requirement >= UnreachableActionPointRequirement || current.action_points < requirement) {
            return std::nullopt;
        }
        const OnDemandSafetyOutcome& outcome = action.outcomes.front();
        const int remaining =
            action_points_after(current.action_points, action.action_point_cost, outcome.action_point_gain);
        RouteLabel next = current;
        next.state = outcome.successor;
        next.action_points = remaining;
        next.metric = add_metric(
            next.metric,
            move_metric(
                map,
                graph,
                current.state,
                action.candidate,
                graph.state(outcome.successor).node));
        add_effective_landing(
            map,
            graph,
            current.state,
            graph.state(outcome.successor).node,
            action.candidate.movement,
            next.effective_node_score,
            next.effective_nodes);
        if (ranking_options.minimize_intermediate_interactions && !graph.is_terminal(outcome.successor)) {
            next.intermediate_interactions +=
                intermediate_interaction_cost(route_node_type(
                    map,
                    graph,
                    current.state,
                    action.candidate.controllable ? action.candidate.target : graph.state(outcome.successor).node));
        }
        MoveCandidate planned_move = action.candidate;
        planned_move.action_point_requirement = requirement;
        PlannedRouteStep next_step {
                std::move(planned_move),
                current.action_points,
                action.action_point_cost,
                outcome.action_point_gain,
                remaining,
        };
        const NodeId target_id =
            action.candidate.controllable ? action.candidate.target : graph.state(outcome.successor).node;
        NodeId entered_trace_node = InvalidNodeId;
        if (const Node* target = map.find_node(target_id); target != nullptr) {
            const bool endpoint_observation_available =
                move_endpoint_observation_available(action.candidate.terminal_on_completion, remaining);
            Node entered = *target;
            entered.type = route_node_type(map, graph, current.state, target->id);
            if (entered.type == NodeType::Empty) {
                entered.name = EmptyNodeName;
            }
            (void)advance_milestones(
                entered,
                graph,
                milestones,
                mission,
                next.progress,
                next.counted,
                endpoint_observation_available ? unknown_big_nodes_revealed(map, graph, current.state, entered.id) : 0);
            entered_trace_node = target->id;
            add_newly_revealed_nodes(
                map,
                graph,
                graph.source_run(),
                action.candidate,
                graph.state(outcome.successor).node,
                endpoint_observation_available,
                next.revealed_nodes);
        }
        next.trace = traces.append(current.trace, entered_trace_node, std::move(next_step));
        if (statistics != nullptr) {
            ++statistics->labels_generated;
        }
        return next;
    };

    // 贪心补全只在抵达地图端点，或已经没有付得起的地图移动时停下。允许追猎时，仍要继续
    // 搜索后续探明/有效节点；“直接耗尽”只是每个有收益前缀都可选的终止后缀，不能让搜索
    // 一开始就停住。
    const auto route_cannot_continue = [&](const RouteLabel& route) {
        if (graph.is_terminal(route.state)) {
            return true;
        }
        if (!graph.exhaustion_terminates()) {
            return false;
        }
        const auto* actions = graph.actions(route.state, error);
        if (actions == nullptr) {
            return true;
        }
        return std::ranges::none_of(*actions, [&](const OnDemandSafetyAction& action) {
            return route.action_points >= std::max(action.minimum_action_points_to_start, action.action_point_cost);
        });
    };
    const auto completed_route = [&](const RouteLabel& route) -> std::optional<RouteLabel> {
        if (graph.is_terminal(route.state) || (graph.exhaustion_terminates() && route.action_points <= 0)) {
            return route;
        }
        if (!graph.exhaustion_terminates()) {
            return std::nullopt;
        }
        return with_direct_exhaustion(route, graph.state(route.state).node, traces);
    };

    const auto flexibility_score = [&](const RouteLabel& route) {
        const PlannerState& state = graph.state(route.state);
        int score = 0;
        for (const MovementSpec& movement : movement_specs()) {
            if (movement.kind == MovementKind::Walk) {
                continue;
            }
            int range_weight = 1;
            switch (movement.range) {
            case MovementRange::WalkEdges:
                range_weight = 1;
                break;
            case MovementRange::SurroundingEight:
                range_weight = 4;
                break;
            case MovementRange::OrthogonalTwo:
            case MovementRange::ManhattanTwo:
                range_weight = 3;
                break;
            case MovementRange::OrthogonalThree:
                range_weight = 4;
                break;
            case MovementRange::FullMap:
                range_weight = 8;
                break;
            }
            const int charges = state.movement_charges[static_cast<std::size_t>(movement.kind)];
            const int residual_value =
                ranking_options.optimize_processing_moves && movement.expires_on_floor_end ? -1 : 1;
            score += residual_value * range_weight * charges;
            if (charges > 0) {
                score += residual_value * 4;
            }
        }
        return score;
    };
    const auto heuristic_better = [&](const RouteLabel& lhs, const RouteLabel& rhs) {
        const auto lhs_end = binding_progress_score(milestones, lhs.progress);
        const auto rhs_end = binding_progress_score(milestones, rhs.progress);
        if (score_greater(lhs_end, rhs_end)) {
            return true;
        }
        if (score_greater(rhs_end, lhs_end)) {
            return false;
        }
        if (ranking_options.maximize_revealed_nodes &&
            node_mask_size(lhs.revealed_nodes) != node_mask_size(rhs.revealed_nodes)) {
            return node_mask_size(lhs.revealed_nodes) > node_mask_size(rhs.revealed_nodes);
        }
        if (ranking_options.maximize_effective_nodes && lhs.effective_node_score != rhs.effective_node_score) {
            return lhs.effective_node_score > rhs.effective_node_score;
        }
        const auto lhs_preferred = preferred_progress_score(milestones, lhs.progress);
        const auto rhs_preferred = preferred_progress_score(milestones, rhs.progress);
        for (std::size_t index = 0; index < std::min(lhs_preferred.size(), rhs_preferred.size()); ++index) {
            const std::int64_t lhs_utility = lhs_preferred[index] - route_penalty(lhs.metric, ranking_options) -
                                             (ranking_options.minimize_intermediate_interactions
                                                  ? lhs.intermediate_interactions
                                                  : 0);
            const std::int64_t rhs_utility = rhs_preferred[index] - route_penalty(rhs.metric, ranking_options) -
                                             (ranking_options.minimize_intermediate_interactions
                                                  ? rhs.intermediate_interactions
                                                  : 0);
            if (lhs_utility != rhs_utility) {
                return lhs_utility > rhs_utility;
            }
            if (lhs_preferred[index] != rhs_preferred[index]) {
                return lhs_preferred[index] > rhs_preferred[index];
            }
        }
        const int lhs_flexibility = flexibility_score(lhs);
        const int rhs_flexibility = flexibility_score(rhs);
        if (lhs_flexibility != rhs_flexibility) {
            return lhs_flexibility > rhs_flexibility;
        }
        return route_label_better(lhs, rhs, milestones, ranking_options);
    };
    const auto complete_greedy = [&](RouteLabel greedy) -> std::optional<RouteLabel> {
        struct PreviewedAction
        {
            const OnDemandSafetyAction* action = nullptr;
            RouteLabel preview;
        };
        struct PreviewedPair
        {
            const OnDemandSafetyAction* first = nullptr;
            const OnDemandSafetyAction* second = nullptr;
            RouteLabel preview;
        };
        struct PreviewedSequence
        {
            std::vector<const OnDemandSafetyAction*> actions;
            RouteLabel preview;
        };

        std::unordered_set<BudgetStateKey, BudgetStateKeyHash> greedy_seen;
        while (!route_cannot_continue(greedy) &&
               greedy_seen.emplace(BudgetStateKey { greedy.state, greedy.action_points }).second) {
            if (search_budget.exhausted()) {
                if (graph.exhaustion_terminates()) {
                    return completed_route(greedy);
                }
                const auto witness = oracle.first_action(greedy.state, greedy.action_points);
                const auto* witness_actions = graph.actions(greedy.state, error);
                if (!witness.has_value() || witness_actions == nullptr) {
                    break;
                }
                const auto witness_action =
                    std::ranges::find_if(*witness_actions, [&](const OnDemandSafetyAction& action) {
                        return action.candidate.action_id == *witness;
                    });
                if (witness_action == witness_actions->end()) {
                    break;
                }
                auto next = build_next(greedy, *witness_action);
                if (!next.has_value()) {
                    break;
                }
                greedy = std::move(*next);
                continue;
            }
            const auto* actions = graph.actions(greedy.state, error);
            if (actions == nullptr) {
                break;
            }
            const auto witness = oracle.first_action(greedy.state, greedy.action_points);
            std::vector<PreviewedAction> previews;
            previews.reserve(actions->size());
            for (const OnDemandSafetyAction& action : *actions) {
                if (auto preview = build_next(greedy, action, false); preview.has_value()) {
                    previews.emplace_back(PreviewedAction { &action, std::move(*preview) });
                }
            }

            std::unordered_map<const OnDemandSafetyAction*, std::optional<RouteLabel>> verified;
            const auto verify_first = [&](const OnDemandSafetyAction* action) -> std::optional<RouteLabel> {
                if (const auto found = verified.find(action); found != verified.end()) {
                    return found->second;
                }
                if (!search_budget.consume()) {
                    return std::nullopt;
                }
                auto next = build_next(greedy, *action);
                verified.emplace(action, next);
                return next;
            };

            std::optional<RouteLabel> selected;
            if (root_move.movement == MovementKind::Walk && search_options.greedy_preview_depth > 2) {
                std::ranges::stable_sort(previews, [&](const PreviewedAction& lhs, const PreviewedAction& rhs) {
                    return heuristic_better(lhs.preview, rhs.preview);
                });
                if (previews.size() > search_options.greedy_preview_width) {
                    previews.resize(search_options.greedy_preview_width);
                }
                std::vector<PreviewedSequence> sequences;
                sequences.reserve(previews.size());
                for (const PreviewedAction& first : previews) {
                    PreviewedSequence sequence { { first.action }, first.preview };
                    std::unordered_set<BudgetStateKey, BudgetStateKeyHash> preview_seen;
                    preview_seen.emplace(BudgetStateKey { sequence.preview.state, sequence.preview.action_points });
                    while (static_cast<int>(sequence.actions.size()) < search_options.greedy_preview_depth &&
                           !graph.is_terminal(sequence.preview.state) && !search_budget.time_exhausted()) {
                        const auto* deeper_actions = graph.actions(sequence.preview.state, error);
                        if (deeper_actions == nullptr) {
                            break;
                        }
                        std::optional<PreviewedAction> best_next;
                        for (const OnDemandSafetyAction& deeper_action : *deeper_actions) {
                            auto next = build_next(sequence.preview, deeper_action, false);
                            if (!next.has_value()) {
                                continue;
                            }
                            if (!graph.is_terminal(next->state) && !heuristic_better(*next, sequence.preview)) {
                                continue;
                            }
                            if (!best_next.has_value() || heuristic_better(*next, best_next->preview)) {
                                best_next = PreviewedAction { &deeper_action, std::move(*next) };
                            }
                        }
                        if (!best_next.has_value() || !preview_seen
                                                           .emplace(
                                                               BudgetStateKey {
                                                                   best_next->preview.state,
                                                                   best_next->preview.action_points,
                                                               })
                                                           .second) {
                            break;
                        }
                        sequence.actions.emplace_back(best_next->action);
                        sequence.preview = std::move(best_next->preview);
                    }
                    sequences.emplace_back(std::move(sequence));
                }
                std::ranges::stable_sort(sequences, [&](const PreviewedSequence& lhs, const PreviewedSequence& rhs) {
                    return heuristic_better(lhs.preview, rhs.preview);
                });
                for (const PreviewedSequence& sequence : sequences) {
                    auto first = verify_first(sequence.actions.front());
                    if (!first.has_value()) {
                        if (search_budget.exhausted()) {
                            break;
                        }
                        continue;
                    }
                    RouteLabel verified_route = *first;
                    bool valid = true;
                    for (std::size_t index = 1; index < sequence.actions.size(); ++index) {
                        if (!search_budget.consume()) {
                            valid = false;
                            break;
                        }
                        auto next = build_next(verified_route, *sequence.actions[index]);
                        if (!next.has_value()) {
                            valid = false;
                            break;
                        }
                        verified_route = std::move(*next);
                    }
                    if (valid) {
                        selected = std::move(*first);
                        break;
                    }
                }
            }
            else if (root_move.movement == MovementKind::Walk && search_options.greedy_preview_depth <= 1) {
                std::ranges::stable_sort(previews, [&](const PreviewedAction& lhs, const PreviewedAction& rhs) {
                    return heuristic_better(lhs.preview, rhs.preview);
                });
                for (const PreviewedAction& preview : previews) {
                    if (auto first = verify_first(preview.action); first.has_value()) {
                        selected = std::move(*first);
                        break;
                    }
                    if (search_budget.exhausted()) {
                        break;
                    }
                }
            }
            else if (root_move.movement == MovementKind::Walk) {
                std::ranges::stable_sort(previews, [&](const PreviewedAction& lhs, const PreviewedAction& rhs) {
                    return heuristic_better(lhs.preview, rhs.preview);
                });
                if (previews.size() > search_options.greedy_preview_width) {
                    previews.resize(search_options.greedy_preview_width);
                }
                std::vector<PreviewedPair> pairs;
                for (const PreviewedAction& first : previews) {
                    pairs.emplace_back(PreviewedPair { first.action, nullptr, first.preview });
                    const auto* second_actions = graph.actions(first.preview.state, error);
                    if (second_actions == nullptr) {
                        continue;
                    }
                    for (const OnDemandSafetyAction& second_action : *second_actions) {
                        auto second = build_next(first.preview, second_action, false);
                        if (second.has_value() && heuristic_better(*second, first.preview)) {
                            pairs.emplace_back(
                                PreviewedPair {
                                    first.action,
                                    &second_action,
                                    std::move(*second),
                                });
                        }
                    }
                }
                std::ranges::stable_sort(pairs, [&](const PreviewedPair& lhs, const PreviewedPair& rhs) {
                    return heuristic_better(lhs.preview, rhs.preview);
                });
                for (const PreviewedPair& pair : pairs) {
                    auto first = verify_first(pair.first);
                    if (!first.has_value()) {
                        if (search_budget.exhausted()) {
                            break;
                        }
                        continue;
                    }
                    if (pair.second != nullptr) {
                        if (!search_budget.consume()) {
                            break;
                        }
                        if (!build_next(*first, *pair.second).has_value()) {
                            continue;
                        }
                    }
                    selected = std::move(*first);
                    break;
                }
            }
            else {
                std::ranges::stable_sort(previews, [&](const PreviewedAction& lhs, const PreviewedAction& rhs) {
                    return route_label_better(
                        lhs.preview,
                        rhs.preview,
                        milestones,
                        ranking_options);
                });
                const auto find_first_safe = [&](const auto& predicate) -> std::optional<RouteLabel> {
                    for (const PreviewedAction& preview : previews) {
                        if (!predicate(preview)) {
                            continue;
                        }
                        if (auto next = verify_first(preview.action); next.has_value()) {
                            return next;
                        }
                        if (search_budget.exhausted()) {
                            break;
                        }
                    }
                    return std::nullopt;
                };
                selected = find_first_safe(
                    [&](const PreviewedAction& preview) { return preview.preview.progress != greedy.progress; });
                if (!selected.has_value()) {
                    selected = find_first_safe(
                        [&](const PreviewedAction& preview) { return graph.is_terminal(preview.preview.state); });
                }
                if (!selected.has_value() && witness.has_value()) {
                    selected = find_first_safe([&](const PreviewedAction& preview) {
                        return preview.action->candidate.action_id == *witness;
                    });
                }
                if (!selected.has_value()) {
                    selected = find_first_safe([](const PreviewedAction&) { return true; });
                }
            }

            if (!selected.has_value() && witness.has_value()) {
                const auto witness_action = std::ranges::find_if(*actions, [&](const OnDemandSafetyAction& action) {
                    return action.candidate.action_id == *witness;
                });
                if (witness_action != actions->end()) {
                    selected = build_next(greedy, *witness_action);
                }
            }
            if (!selected.has_value()) {
                break;
            }
            greedy = std::move(*selected);
        }
        return route_cannot_continue(greedy) ? completed_route(greedy) : std::nullopt;
    };
    if (route_hint_action_ids != nullptr && route_hint_action_ids->size() > 1) {
        RouteLabel replayed = initial;
        std::size_t replayed_steps = 1;
        for (std::size_t index = 1; index < route_hint_action_ids->size(); ++index) {
            if (graph.is_terminal(replayed.state) ||
                (graph.exhaustion_terminates() && replayed.action_points <= 0)) {
                break;
            }
            if (route_hint_action_ids->at(index) == "direct_exhaustion") {
                if (auto completed = completed_route(replayed); completed.has_value()) {
                    replayed = std::move(*completed);
                    ++replayed_steps;
                }
                break;
            }
            if (!search_budget.consume()) {
                break;
            }
            const auto* actions = graph.actions(replayed.state, error);
            if (actions == nullptr) {
                break;
            }
            const auto action = std::ranges::find_if(*actions, [&](const OnDemandSafetyAction& candidate) {
                return candidate.candidate.action_id == route_hint_action_ids->at(index);
            });
            if (action == actions->end()) {
                break;
            }
            auto next = build_next(replayed, *action);
            if (!next.has_value()) {
                break;
            }
            replayed = std::move(*next);
            ++replayed_steps;
        }
        if (route_hint_replayed_steps != nullptr) {
            *route_hint_replayed_steps = replayed_steps;
        }
        if (auto completed = completed_route(replayed);
            completed.has_value() && fits_inventory(*completed)) {
            best = std::move(*completed);
        }
    }
    if (auto greedy = complete_greedy(initial);
        greedy.has_value() && fits_inventory(*greedy) &&
        (!best.has_value() || route_label_better(*greedy, *best, milestones, ranking_options))) {
        best = std::move(*greedy);
    }
    while (!pending.empty() && expanded_routes < route_expansion_budget_per_root && search_budget.consume()) {
        RouteLabel current = pending.top().route;
        pending.pop();
        ++expanded_routes;
        if (auto completed = completed_route(current); completed.has_value()) {
            if (fits_inventory(*completed) &&
                (!best.has_value() || route_label_better(
                                         *completed,
                                         *best,
                                         milestones,
                                         ranking_options))) {
                best = std::move(*completed);
            }
        }
        if (graph.is_terminal(current.state) ||
            (graph.exhaustion_terminates() && current.action_points <= 0)) {
            continue;
        }
        if (best.has_value() && !route_may_beat(
                                    map,
                                    graph,
                                    milestones,
                                    current,
                                    *best,
                                    ranking_options)) {
            continue;
        }

        const auto* actions = graph.actions(current.state, error);
        if (actions == nullptr) {
            return materialize(best.value_or(initial));
        }
        for (const OnDemandSafetyAction& action : *actions) {
            if (!action_matches_current_resources(current.state, action)) {
                continue;
            }
            const int requirement = oracle.action_requirement(action, current.action_points);
            if (requirement >= UnreachableActionPointRequirement) {
                continue;
            }
            const OnDemandSafetyOutcome& outcome = action.outcomes.front();
            const int remaining =
                action_points_after(current.action_points, action.action_point_cost, outcome.action_point_gain);
            RouteLabel next = current;
            next.state = outcome.successor;
            next.action_points = remaining;
            next.metric = add_metric(
                next.metric,
                move_metric(
                    map,
                    graph,
                    current.state,
                    action.candidate,
                    graph.state(outcome.successor).node));
            add_effective_landing(
                map,
                graph,
                current.state,
                graph.state(outcome.successor).node,
                action.candidate.movement,
                next.effective_node_score,
                next.effective_nodes);
            if (ranking_options.minimize_intermediate_interactions && !graph.is_terminal(outcome.successor)) {
                next.intermediate_interactions +=
                    intermediate_interaction_cost(route_node_type(
                        map,
                        graph,
                        current.state,
                        action.candidate.controllable ? action.candidate.target : graph.state(outcome.successor).node));
            }
            MoveCandidate planned_move = action.candidate;
            planned_move.action_point_requirement = requirement;
            PlannedRouteStep next_step {
                    std::move(planned_move),
                    current.action_points,
                    action.action_point_cost,
                    outcome.action_point_gain,
                    remaining,
            };
            const NodeId outcome_entered_node =
                action.candidate.controllable ? action.candidate.target : graph.state(outcome.successor).node;
            NodeId entered_trace_node = InvalidNodeId;
            if (const Node* target = map.find_node(outcome_entered_node); target != nullptr) {
                const bool endpoint_observation_available =
                    move_endpoint_observation_available(action.candidate.terminal_on_completion, remaining);
                Node entered = *target;
                entered.type = route_node_type(map, graph, current.state, target->id);
                if (entered.type == NodeType::Empty) {
                    entered.name = EmptyNodeName;
                }
                (void)advance_milestones(
                    entered,
                    graph,
                    milestones,
                    mission,
                    next.progress,
                    next.counted,
                    endpoint_observation_available
                        ? unknown_big_nodes_revealed(map, graph, current.state, entered.id)
                        : 0);
                entered_trace_node = target->id;
                add_newly_revealed_nodes(
                    map,
                    graph,
                    graph.source_run(),
                    action.candidate,
                    graph.state(outcome.successor).node,
                    endpoint_observation_available,
                    next.revealed_nodes);
            }
            next.trace = traces.append(current.trace, entered_trace_node, std::move(next_step));
            if (statistics != nullptr) {
                ++statistics->labels_generated;
            }
            auto& existing = labels[next.state];
            const bool dominated = std::ranges::any_of(existing, [&](const RouteLabel& value) {
                return value.progress == next.progress && value.counted == next.counted &&
                       value.action_points >= next.action_points &&
                       (!ranking_options.maximize_revealed_nodes ||
                        revealed_superset(value.revealed_nodes, next.revealed_nodes)) &&
                       (!ranking_options.maximize_effective_nodes ||
                        revealed_superset(value.effective_nodes, next.effective_nodes)) &&
                       (!ranking_options.minimize_intermediate_interactions ||
                        value.intermediate_interactions <= next.intermediate_interactions) &&
                       route_metric_weakly_better(value.metric, next.metric, ranking_options);
            });
            if (dominated) {
                if (statistics != nullptr) {
                    ++statistics->labels_dominated;
                }
                continue;
            }
            const std::size_t before_erase = existing.size();
            std::erase_if(existing, [&](const RouteLabel& value) {
                return value.progress == next.progress && value.counted == next.counted &&
                       next.action_points >= value.action_points &&
                       (!ranking_options.maximize_revealed_nodes ||
                        revealed_superset(next.revealed_nodes, value.revealed_nodes)) &&
                       (!ranking_options.maximize_effective_nodes ||
                        revealed_superset(next.effective_nodes, value.effective_nodes)) &&
                       (!ranking_options.minimize_intermediate_interactions ||
                        next.intermediate_interactions <= value.intermediate_interactions) &&
                       route_metric_weakly_better(next.metric, value.metric, ranking_options);
            });
            const std::size_t erased = before_erase - existing.size();
            retained_labels -= erased;
            if (statistics != nullptr) {
                statistics->labels_dominated += erased;
            }
            existing.emplace_back(next);
            ++retained_labels;
            if (statistics != nullptr) {
                statistics->labels_retained_peak = std::max(statistics->labels_retained_peak, retained_labels);
            }
            pending.emplace(make_pending(std::move(next)));
        }
    }
    return materialize(best.value_or(initial));
}

ReachableFeatures planned_route_features(const MapSnapshot& map, const std::vector<NodeId>& route)
{
    ReachableFeatures result;
    for (const NodeId id : route) {
        const Node* node = map.find_node(id);
        if (node == nullptr || node->type == NodeType::Empty || node->progress == NodeProgress::Removed) {
            continue;
        }
        result.node_types.emplace(to_string(node->type));
        if (node->type == NodeType::BattleBoss || node->type == NodeType::Final) {
            result.node_types.emplace("final");
        }
        result.has_badged = result.has_badged || node->badged;
        result.has_badged_incident = result.has_badged_incident || (node->badged && node->type == NodeType::Incident);
    }
    return result;
}

void merge_route_union(ReachableFeatures& destination, const ReachableFeatures& source)
{
    destination.node_types.insert(source.node_types.begin(), source.node_types.end());
    destination.has_badged = destination.has_badged || source.has_badged;
    destination.has_badged_incident = destination.has_badged_incident || source.has_badged_incident;
}

void intersect_route_features(ReachableFeatures& destination, const ReachableFeatures& source)
{
    std::erase_if(destination.node_types, [&](const std::string& type) { return !source.node_types.contains(type); });
    destination.has_badged = destination.has_badged && source.has_badged;
    destination.has_badged_incident = destination.has_badged_incident && source.has_badged_incident;
}

void set_route_feature_facts(FactStore& facts, const ReachableFeatures& possible, const ReachableFeatures& guaranteed)
{
    facts.set("candidate.route_node_types", sorted_types(possible.node_types));
    facts.set("candidate.guaranteed_route_node_types", sorted_types(guaranteed.node_types));
    facts.set("candidate.route_has_badged", possible.has_badged);
    facts.set("candidate.guaranteed_route_has_badged", guaranteed.has_badged);
    facts.set("candidate.route_has_badged_incident", possible.has_badged_incident);
    facts.set("candidate.guaranteed_route_has_badged_incident", guaranteed.has_badged_incident);
}

void set_first_move_facts(FactStore& facts, const RunState& run, const MoveCandidate& move)
{
    facts.set("candidate.movement", std::string(to_string(move.movement)));
    facts.set(
        "candidate.move_edges",
        static_cast<std::int64_t>(move.movement == MovementKind::Walk ? move.path.size() : 0));
    facts.set(
        "candidate.requires_movement_switch",
        !run.active_movement.has_value() || *run.active_movement != move.movement);
}

FactStore on_demand_candidate_facts(
    const MapSnapshot& map,
    OnDemandStateGraph& graph,
    const OnDemandSafetyAction& root_action,
    const RunState& run,
    bool strategy_end)
{
    FactStore facts;
    const MoveCandidate& move = root_action.candidate;
    const Node* target = map.find_node(move.target);
    facts.set("candidate.node_type", std::string(target == nullptr ? "unclassified" : to_string(target->type)));
    facts.set("candidate.node_name", target == nullptr ? std::string() : target->name);
    facts.set("candidate.badged", target != nullptr && target->badged);
    const bool combat = target != nullptr && is_route_battle_node_type(target->type);
    facts.set("candidate.combat", combat);
    facts.set("candidate.boss", combat && target->type == NodeType::BattleBoss);
    facts.set(
        "candidate.exit",
        target != nullptr && (target->type == NodeType::Final || target->type == NodeType::BattleBoss));
    facts.set("candidate.strategy_end", strategy_end);
    facts.set("candidate.uses_processing_item", move.movement != MovementKind::Walk);
    set_first_move_facts(facts, run, move);
    facts.set(
        "candidate.light_reveal_count",
        static_cast<std::int64_t>(
            target != nullptr && target->type == NodeType::Light
                ? unknown_big_nodes_revealed(map, graph, graph.initial_state(), target->id)
                : 0));

    set_route_feature_facts(facts, ReachableFeatures {}, ReachableFeatures {});
    return facts;
}

struct BindingResolution
{
    std::vector<std::string> locked;
    std::vector<std::string> demoted;
    std::string error;
};

// 可行性阶梯。候选按优先级从高到低给出，这里从全体开始，每次证不出「加上这些约束仍有安全解」
// 就把优先级最低的一个降级，直到证得出为止。
//
// 关键在于约束只在证明通过之后才施加：被拒绝的路线一定有一条已经证明存在的合规路线可以替代，
// 所以强制目标不会像无条件必达那样把整层判成无解。候选为空时一次求解都不做。
//
// 判定放在 Confirmed 层：它只认已确认的地图事实，因而是保守的。偶尔会因为通路只存在于推断边
// 而误判不可行，代价仅是这一轮降级成倾向，等地图确认更多之后重规划会重新锁上。
BindingResolution resolve_binding_milestones(const BlackFlowPlanRequest& request)
{
    BindingResolution resolution;
    resolution.locked = request.binding_milestone_candidates;
    const int current_action_points = request.run->resources.action_points;
    while (resolution.locked.size() > request.undemotable_binding_count) {
        const std::unordered_set<std::string> ids(resolution.locked.begin(), resolution.locked.end());
        std::string error;
        auto goal = SafetyGoalProgram::compile(*request.policy, *request.mission, *request.facts, ids, &error);
        if (!goal.has_value()) {
            resolution.error = "binding feasibility goal compilation failed: " + error;
            return resolution;
        }

        StateExpansionOptions options;
        options.forbidden_node_types = future_forbidden_landing_types(request.forbidden_node_types);
        options.allow_revealed_hidden_battle = request.run->floor == 1;
        options.reserved_movement_kinds = request.reserved_movement_kinds;
        options.reserved_movement_charges = request.reserved_movement_charges;
        options.strategy_terminal_nodes = request.strategy_terminal_nodes;
        options.no_AP_is_terminal = request.no_AP_is_terminal;
        options.graph_layer = GraphLayer::Confirmed;
        options.safety_goal = &*goal;
        options.safety_goal_facts = request.facts;
        options.maximum_states = request.maximum_states;
        if (request.forbidden_actions != nullptr) {
            options.forbidden_action_ids = *request.forbidden_actions;
        }

        OnDemandStateGraph graph;
        if (!graph.initialize(*request.map, *request.run, std::move(options), &error)) {
            resolution.error = "binding feasibility graph initialization failed: " + error;
            return resolution;
        }
        OnDemandSafetyOracle oracle(graph, "Binding feasibility", request.route_search.safety_resource_dominance);
        const int requirement = oracle.requirement(graph.initial_state(), current_action_points);
        if (!oracle.error().empty()) {
            resolution.error = "binding feasibility calculation failed: " + oracle.error();
            return resolution;
        }
        // N_bounded 以当前行动力为上界，取到有限值就等于「现在的行动力够」。
        if (requirement < UnreachableActionPointRequirement) {
            return resolution;
        }
        resolution.demoted.emplace_back(std::move(resolution.locked.back()));
        resolution.locked.pop_back();
    }
    return resolution;
}
} // namespace

bool prefer_less_valuable_exchangeable_processing_steps(
    const MapSnapshot& map,
    const RunState& run,
    std::vector<PlannedRouteStep>& steps)
{
    return prefer_less_valuable_exchangeable_processing_steps_impl(map, run, steps);
}

BlackFlowPlan BlackFlowPlanner::plan(const BlackFlowPlanRequest& request) const
{
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&](BlackFlowPlan result) {
        result.planning_elapsed_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        return result;
    };
    try {
        return finish(plan_impl(request));
    }
    catch (const std::exception& exception) {
        BlackFlowPlan result;
        result.no_AP_is_terminal = request.no_AP_is_terminal;
        result.error = std::string("planner exception: ") + exception.what();
        return finish(std::move(result));
    }
    catch (...) {
        BlackFlowPlan result;
        result.no_AP_is_terminal = request.no_AP_is_terminal;
        result.error = "planner exception: unknown error";
        return finish(std::move(result));
    }
}

PreviewSafetyVerification BlackFlowPlanner::verify_previewed_move(
    const BlackFlowPlanRequest& request,
    const MoveCandidate& move,
    int exact_action_point_cost) const
{
    try {
        return verify_previewed_move_impl(request, move, exact_action_point_cost);
    }
    catch (const std::exception& exception) {
        PreviewSafetyVerification result;
        result.error = std::string("planner exception: ") + exception.what();
        return result;
    }
    catch (...) {
        PreviewSafetyVerification result;
        result.error = "planner exception: unknown error";
        return result;
    }
}

PreviewSafetyVerification BlackFlowPlanner::verify_previewed_move_impl(
    const BlackFlowPlanRequest& request,
    const MoveCandidate& move,
    int exact_action_point_cost) const
{
    PreviewSafetyVerification result;
    if (request.map == nullptr || request.run == nullptr || request.policy == nullptr || request.facts == nullptr ||
        request.mission == nullptr) {
        result.error = "preview safety request is incomplete";
        return result;
    }
    if (move_lands_on_forbidden_node_type(*request.map, move, request.forbidden_node_types)) {
        result.error = "immediate landing uses a forbidden node type";
        return result;
    }

    auto projected = project_move_outcome(*request.map, *request.run, move, exact_action_point_cost, &result.error);
    if (!projected.has_value()) {
        return result;
    }
    result.action_points_after = projected->run.resources.action_points;

    // 预览验证沿用规划当轮已经定下的锁定集合，不再重跑可行性阶梯：这一步只回答
    // 「这一步落地之后还走得完」，锁定集合改变属于下一次规划的事。
    const std::unordered_set<std::string> binding_ids(
        request.binding_milestone_candidates.begin(),
        request.binding_milestone_candidates.end());
    std::string goal_error;
    auto safety_goal =
        SafetyGoalProgram::compile(*request.policy, *request.mission, *request.facts, binding_ids, &goal_error);
    if (!safety_goal.has_value()) {
        result.error = "strategy preview safety goal compilation failed: " + goal_error;
        return result;
    }
    SafetyGoalProgressId preview_goal_progress = safety_goal->initial_progress_id();
    if (const Node* entered = request.map->find_node(move.target); entered != nullptr) {
        int newly_revealed_unknown_big_nodes = 0;
        if (entered->type == NodeType::Light) {
            StateExpansionOptions source_options;
            source_options.forbidden_node_types = future_forbidden_landing_types(request.forbidden_node_types);
            source_options.allow_revealed_hidden_battle = request.run->floor == 1;
            source_options.reserved_movement_kinds = request.reserved_movement_kinds;
            source_options.reserved_movement_charges = request.reserved_movement_charges;
            source_options.strategy_terminal_nodes = request.strategy_terminal_nodes;
            source_options.no_AP_is_terminal = request.no_AP_is_terminal;
            source_options.graph_layer = GraphLayer::Confirmed;
            source_options.maximum_states = request.maximum_states;
            if (request.forbidden_actions != nullptr) {
                source_options.forbidden_action_ids = *request.forbidden_actions;
            }

            OnDemandStateGraph source_graph;
            if (!source_graph.initialize(*request.map, *request.run, std::move(source_options), &result.error)) {
                result.error = "confirmed source graph initialization failed: " + result.error;
                return result;
            }
            newly_revealed_unknown_big_nodes =
                unknown_big_nodes_revealed(*request.map, source_graph, source_graph.initial_state(), entered->id);
        }

        const auto advanced = safety_goal->advance_node(
            preview_goal_progress,
            *entered,
            newly_revealed_unknown_big_nodes,
            *request.facts,
            &goal_error);
        if (!advanced.has_value()) {
            result.error = "strategy preview safety goal transition failed: " + goal_error;
            return result;
        }
        preview_goal_progress = *advanced;
    }

    StateExpansionOptions options;
    options.forbidden_node_types = future_forbidden_landing_types(request.forbidden_node_types);
    options.allow_revealed_hidden_battle = request.run->floor == 1;
    options.reserved_movement_kinds = request.reserved_movement_kinds;
    options.reserved_movement_charges = request.reserved_movement_charges;
    options.strategy_terminal_nodes = request.strategy_terminal_nodes;
    options.no_AP_is_terminal = request.no_AP_is_terminal;
    options.graph_layer = GraphLayer::Confirmed;
    options.safety_goal = &*safety_goal;
    options.safety_goal_facts = request.facts;
    options.initial_goal_progress_id = preview_goal_progress;
    options.maximum_states = request.maximum_states;
    if (request.forbidden_actions != nullptr) {
        options.forbidden_action_ids = *request.forbidden_actions;
    }

    OnDemandStateGraph graph;
    if (!graph.initialize(*request.map, projected->run, std::move(options), &result.error)) {
        result.error = "confirmed successor graph initialization failed: " + result.error;
        return result;
    }
    OnDemandSafetyOracle oracle(graph, "Confirmed preview", request.route_search.safety_resource_dominance);
    result.required_action_points_after =
        oracle.requirement(graph.initial_state(), projected->run.resources.action_points);
    if (!oracle.error().empty()) {
        result.error = "confirmed successor safety calculation failed: " + oracle.error();
        return result;
    }
    result.safe = result.required_action_points_after < UnreachableActionPointRequirement;
    if (result.safe) {
        result.proof_depth = oracle.cached_depth(graph.initial_state(), result.required_action_points_after);
    }
    return result;
}

BlackFlowPlan BlackFlowPlanner::plan_impl(const BlackFlowPlanRequest& request) const
{
    BlackFlowPlan result;
    result.no_AP_is_terminal = request.no_AP_is_terminal;
    if (request.map == nullptr || request.run == nullptr || request.policy == nullptr || request.facts == nullptr ||
        request.mission == nullptr) {
        result.error = "BlackFlow planner request is incomplete";
        return result;
    }
    if (request.route_search.time_budget_ms <= 0 || request.route_search.total_expansions == 0 ||
        request.route_search.expansions_per_root == 0 || request.route_search.greedy_preview_depth <= 0 ||
        request.route_search.greedy_preview_width == 0) {
        result.error = "route search options must be positive";
        return result;
    }
    result.route_search_time_budget_ms = request.route_search.time_budget_ms;
    result.route_search_total_expansions = request.route_search.total_expansions;
    result.route_search_expansions_per_root = request.route_search.expansions_per_root;
    result.map_revision = request.map->revision;
    result.cost_revision = request.run->costs.revision;
    const RouteRankingOptions ranking_options {
        .minimize_intermediate_interactions =
            std::ranges::find(
                request.policy->route_preferences,
                RoutePreference::MinimizeIntermediateInteractions) != request.policy->route_preferences.end(),
        .maximize_revealed_nodes =
            std::ranges::find(request.policy->route_preferences, RoutePreference::MaximizeRevealedNodes) !=
            request.policy->route_preferences.end(),
        .maximize_effective_nodes =
            std::ranges::find(request.policy->route_preferences, RoutePreference::MaximizeEffectiveNodes) !=
            request.policy->route_preferences.end(),
        .ignore_battle_tiebreaks =
            std::ranges::find(request.policy->route_preferences, RoutePreference::IgnoreBattleTieBreaks) !=
            request.policy->route_preferences.end(),
        .optimize_processing_moves =
            std::ranges::find(request.policy->route_preferences, RoutePreference::OptimizeProcessingMoves) !=
            request.policy->route_preferences.end(),
    };
    std::string error;

    auto binding = resolve_binding_milestones(request);
    if (!binding.error.empty()) {
        result.error = std::move(binding.error);
        return result;
    }
    result.binding_milestone_ids.insert(binding.locked.begin(), binding.locked.end());
    result.demoted_milestone_ids = binding.demoted;

    // 已锁定目标的匹配节点。策略规则靠 candidate.strategy_end 区分「这个秘境行商是本轮的硬目标」
    // 和「只是顺路的秘境行商」，例如没资源时不进秘境行商的那条禁止规则就要放行硬目标。
    std::unordered_set<NodeId> binding_goal_nodes;
    for (const std::string& id : binding.locked) {
        const auto definition = std::ranges::find(request.policy->milestones, id, &Milestone::id);
        if (definition == request.policy->milestones.end()) {
            continue;
        }
        for (const auto& [node_id, node] : request.map->nodes()) {
            if (node.progress != NodeProgress::Removed && milestone_matches_node(*definition, node)) {
                binding_goal_nodes.emplace(node_id);
            }
        }
    }

    auto safety_goal = SafetyGoalProgram::compile(
        *request.policy,
        *request.mission,
        *request.facts,
        result.binding_milestone_ids,
        &error);
    if (!safety_goal.has_value()) {
        result.error = "strategy safety goal compilation failed: " + error;
        return result;
    }
    SafetyGoalProgram relaxed_safety_goal = *safety_goal;

    StateExpansionOptions confirmed_options;
    confirmed_options.forbidden_node_types = future_forbidden_landing_types(request.forbidden_node_types);
    confirmed_options.allow_revealed_hidden_battle = request.run->floor == 1;
    confirmed_options.reserved_movement_kinds = request.reserved_movement_kinds;
    confirmed_options.reserved_movement_charges = request.reserved_movement_charges;
    confirmed_options.strategy_terminal_nodes = request.strategy_terminal_nodes;
    confirmed_options.no_AP_is_terminal = request.no_AP_is_terminal;
    confirmed_options.graph_layer = GraphLayer::Confirmed;
    confirmed_options.safety_goal = &*safety_goal;
    confirmed_options.safety_goal_facts = request.facts;
    confirmed_options.maximum_states = request.maximum_states;
    if (request.forbidden_actions != nullptr) {
        confirmed_options.forbidden_action_ids = *request.forbidden_actions;
    }

    StateExpansionOptions relaxed_options = confirmed_options;
    relaxed_options.graph_layer = GraphLayer::Relaxed;
    relaxed_options.safety_goal = &relaxed_safety_goal;
    result.planning_graphs_shared = graph_layers_have_identical_transitions(*request.map, *request.run);
    const auto graph_initialization_started = std::chrono::steady_clock::now();
    OnDemandStateGraph relaxed_graph;
    if (!relaxed_graph.initialize(*request.map, *request.run, relaxed_options, &error)) {
        result.error = "relaxed on-demand graph initialization failed: " + error;
        return result;
    }
    std::unique_ptr<OnDemandStateGraph> confirmed_graph_storage;
    if (!result.planning_graphs_shared) {
        confirmed_graph_storage = std::make_unique<OnDemandStateGraph>();
        if (!confirmed_graph_storage->initialize(*request.map, *request.run, confirmed_options, &error)) {
            result.error = "confirmed on-demand graph initialization failed: " + error;
            return result;
        }
    }
    OnDemandStateGraph& confirmed_graph =
        result.planning_graphs_shared ? relaxed_graph : *confirmed_graph_storage;
    result.graph_initialization_elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - graph_initialization_started)
            .count());

    const int current_action_points = request.run->resources.action_points;
    OnDemandSafetyOracle relaxed_oracle(relaxed_graph, "Relaxed", request.route_search.safety_resource_dominance);
    std::unique_ptr<OnDemandSafetyOracle> confirmed_oracle_storage;
    if (!result.planning_graphs_shared) {
        confirmed_oracle_storage = std::make_unique<OnDemandSafetyOracle>(
            confirmed_graph,
            "Confirmed",
            request.route_search.safety_resource_dominance);
    }
    OnDemandSafetyOracle& confirmed_oracle =
        result.planning_graphs_shared ? relaxed_oracle : *confirmed_oracle_storage;
    const auto confirmed_safety_started = std::chrono::steady_clock::now();
    result.safety.required_action_points =
        confirmed_oracle.requirement(confirmed_graph.initial_state(), current_action_points);
    result.confirmed_safety_elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - confirmed_safety_started)
            .count());
    const auto relaxed_safety_started = std::chrono::steady_clock::now();
    result.relaxed_safety.required_action_points =
        relaxed_oracle.requirement(relaxed_graph.initial_state(), current_action_points);
    result.relaxed_safety_elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - relaxed_safety_started)
            .count());
    if (!confirmed_oracle.error().empty() || !relaxed_oracle.error().empty()) {
        result.error = "root safety calculation failed: " +
                       (!confirmed_oracle.error().empty() ? confirmed_oracle.error() : relaxed_oracle.error());
        return result;
    }
    if (result.safety.required_action_points < UnreachableActionPointRequirement) {
        result.safety.proof_depth =
            confirmed_oracle.cached_depth(confirmed_graph.initial_state(), result.safety.required_action_points);
        result.safety.first_action = confirmed_oracle.lexicographic_first_action(
            confirmed_graph.initial_state(),
            result.safety.required_action_points);
    }

    const auto* confirmed_root_action_list = confirmed_graph.actions(confirmed_graph.initial_state(), &error);
    if (confirmed_root_action_list == nullptr) {
        result.error = "confirmed root action generation failed: " + error;
        return result;
    }

    if (result.relaxed_safety.required_action_points < UnreachableActionPointRequirement) {
        result.relaxed_safety.proof_depth =
            relaxed_oracle.cached_depth(relaxed_graph.initial_state(), result.relaxed_safety.required_action_points);
        result.relaxed_safety.first_action = relaxed_oracle.lexicographic_first_action(
            relaxed_graph.initial_state(),
            result.relaxed_safety.required_action_points);
    }

    const auto* relaxed_root_action_list = relaxed_graph.actions(relaxed_graph.initial_state(), &error);
    if (relaxed_root_action_list == nullptr) {
        result.error = "relaxed root action generation failed: " + error;
        return result;
    }
    result.confirmed_state_count = confirmed_graph.state_count();
    result.relaxed_state_count = relaxed_graph.state_count();
    std::unordered_map<std::string, const OnDemandSafetyAction*> confirmed_root_actions;
    for (const OnDemandSafetyAction& action : *confirmed_root_action_list) {
        confirmed_root_actions.insert_or_assign(action.candidate.action_id, &action);
    }

    std::unordered_map<std::string, int> confirmed_root_requirements;
    std::unordered_map<std::string, int> relaxed_root_requirements;
    confirmed_root_requirements.reserve(confirmed_root_action_list->size());
    relaxed_root_requirements.reserve(relaxed_root_action_list->size());
    for (const OnDemandSafetyAction& action : *relaxed_root_action_list) {
        relaxed_root_requirements.emplace(
            action.candidate.action_id,
            relaxed_oracle.action_requirement(action, current_action_points));
    }
    for (const OnDemandSafetyAction& action : *confirmed_root_action_list) {
        if (!action.candidate.controllable) {
            confirmed_root_requirements.emplace(
                action.candidate.action_id,
                confirmed_oracle.action_requirement(action, current_action_points));
        }
    }
    if (!confirmed_oracle.error().empty() || !relaxed_oracle.error().empty()) {
        result.error = "batched root safety calculation failed: " +
                       (!confirmed_oracle.error().empty() ? confirmed_oracle.error() : relaxed_oracle.error());
        return result;
    }

    const auto milestones = route_milestones(
        *request.policy,
        *request.mission,
        request.run->floor,
        *request.facts,
        result.binding_milestone_ids);
    RouteSearchBudget route_search_budget {
        std::chrono::steady_clock::now() + std::chrono::milliseconds(request.route_search.time_budget_ms),
        request.route_search.total_expansions,
        request.route_search.total_expansions,
    };
    const auto route_search_started = std::chrono::steady_clock::now();
    RouteSearchStatistics route_search_statistics;
    result.route_root_action_count = relaxed_root_action_list->size();
    std::vector<PolicyCandidate> policy_candidates;
    result.mobile_marker_lookahead_active =
        request.robust_mobile_marker_lookahead && !request.root_forbidden_marker_types.empty() &&
        std::ranges::any_of(request.map->nodes(), [&](const auto& entry) {
            return node_matches_mobile_marker_types(entry.second, request.root_forbidden_marker_types);
        });
    std::vector<const OnDemandSafetyAction*> ordered_root_actions;
    ordered_root_actions.reserve(relaxed_root_action_list->size());
    for (const OnDemandSafetyAction& action : *relaxed_root_action_list) {
        ordered_root_actions.emplace_back(&action);
    }
    std::ranges::stable_sort(ordered_root_actions, [&](const auto* lhs, const auto* rhs) {
        const bool lhs_hinted = !request.route_hint_action_ids.empty() &&
                                lhs->candidate.action_id == request.route_hint_action_ids.front();
        const bool rhs_hinted = !request.route_hint_action_ids.empty() &&
                                rhs->candidate.action_id == request.route_hint_action_ids.front();
        if (lhs_hinted != rhs_hinted) {
            return lhs_hinted;
        }
        const int lhs_processing = lhs->candidate.movement == MovementKind::Walk ? 0 : 1;
        const int rhs_processing = rhs->candidate.movement == MovementKind::Walk ? 0 : 1;
        return lhs_processing < rhs_processing;
    });
    const auto root_uses_forbidden_marker = [&](const MoveCandidate& move) {
        const auto forbidden = [&](NodeId id) {
            const Node* node = request.map->find_node(id);
            return node != nullptr && node_matches_mobile_marker_types(*node, request.root_forbidden_marker_types);
        };
        if (std::ranges::any_of(move.path, forbidden)) {
            return true;
        }
        if (move.target != InvalidNodeId && forbidden(move.target)) {
            return true;
        }
        if (move.landing != InvalidNodeId && forbidden(move.landing)) {
            return true;
        }
        return std::ranges::any_of(move.possible_landings, forbidden);
    };
    for (const OnDemandSafetyAction* action_pointer : ordered_root_actions) {
        const OnDemandSafetyAction& action = *action_pointer;
        if (move_lands_on_forbidden_node_type(*request.map, action.candidate, request.forbidden_node_types) ||
            root_uses_forbidden_marker(action.candidate)) {
            continue;
        }
        PolicyCandidate candidate;
        candidate.move = action.candidate;
        const int relaxed_requirement = relaxed_root_requirements.at(action.candidate.action_id);
        candidate.move.action_point_requirement = relaxed_requirement;
        const bool relaxed_safe = relaxed_requirement < UnreachableActionPointRequirement;
        bool confirmed_safe = false;
        if (const auto confirmed = confirmed_root_actions.find(candidate.move.action_id);
            confirmed != confirmed_root_actions.end()) {
            const auto requirement = confirmed_root_requirements.find(candidate.move.action_id);
            if (requirement != confirmed_root_requirements.end() &&
                requirement->second < UnreachableActionPointRequirement) {
                candidate.move = confirmed->second->candidate;
                candidate.move.graph_layer = GraphLayer::Confirmed;
                candidate.move.action_point_requirement = requirement->second;
                confirmed_safe = true;
            }
        }
        candidate.safe = confirmed_safe || (relaxed_safe && candidate.move.controllable);
        const bool root_matches_hint = !request.route_hint_action_ids.empty() &&
                                       candidate.move.action_id == request.route_hint_action_ids.front();
        result.route_hint_root_matched = result.route_hint_root_matched || root_matches_hint;
        const bool probing_target = request.probe_target.has_value() && candidate.move.target == *request.probe_target;
        candidate.move.requires_preview_verification = candidate.safe && (!confirmed_safe || probing_target);
        candidate.facts = on_demand_candidate_facts(
            *request.map,
            relaxed_graph,
            action,
            *request.run,
            binding_goal_nodes.contains(action.candidate.target));
        if (!error.empty() || !relaxed_oracle.error().empty()) {
            result.error =
                "candidate reachability calculation failed: " + (!error.empty() ? error : relaxed_oracle.error());
            return result;
        }
        candidate.facts.set("candidate.preview_required", candidate.move.requires_preview_verification);
        const Node* target = request.map->find_node(candidate.move.target);
        candidate.battle_count = target != nullptr && is_route_battle_node_type(target->type) ? 1 : 0;
        candidate.route_length = candidate.move.movement == MovementKind::Walk
                                     ? std::max(1, static_cast<int>(candidate.move.path.size()))
                                     : 1;
        candidate.risk_score = target == nullptr || !target->identity_revealed
                                   ? 5
                                   : (target->type == NodeType::BattleElite || target->type == NodeType::BattleBoss
                                          ? 10
                                          : candidate.battle_count * 4);

        if (!candidate.safe) {
            candidate.facts.set("candidate.mobile_marker_robust", true);
            policy_candidates.emplace_back(std::move(candidate));
            continue;
        }

        if (result.mobile_marker_lookahead_active) {
            MobileMarkerLookaheadResult lookahead = assess_mobile_marker_lookahead(
                *request.map,
                relaxed_graph,
                relaxed_oracle,
                action,
                current_action_points,
                request.root_forbidden_marker_types);
            result.mobile_marker_outcomes_checked += lookahead.outcomes_checked;
            if (!lookahead.error.empty()) {
                result.error = "mobile marker lookahead failed: " + lookahead.error;
                return result;
            }
            candidate.facts.set("candidate.mobile_marker_robust", lookahead.robust);
        }
        else {
            candidate.facts.set("candidate.mobile_marker_robust", true);
        }

        bool first_outcome = true;
        ReachableFeatures possible_route_features;
        std::optional<ReachableFeatures> guaranteed_route_features;
        std::vector<int> guaranteed_progress;
        RouteMetric worst_metric;
        int worst_intermediate_interactions = 0;
        int guaranteed_revealed_nodes = std::numeric_limits<int>::max();
        int guaranteed_effective_nodes = std::numeric_limits<int>::max();
        for (const OnDemandSafetyOutcome& outcome : action.outcomes) {
            const int remaining =
                action_points_after(current_action_points, action.action_point_cost, outcome.action_point_gain);
            std::size_t replayed_hint_steps = root_matches_hint ? 1 : 0;
            RouteLabel route = best_route_after_outcome(
                *request.map,
                relaxed_graph,
                relaxed_oracle,
                milestones,
                *request.mission,
                candidate.move,
                action,
                outcome,
                current_action_points,
                remaining,
                root_matches_hint ? &request.route_hint_action_ids : nullptr,
                &replayed_hint_steps,
                request.route_search,
                route_search_budget,
                ranking_options,
                &route_search_statistics,
                &error);
            result.route_hint_replayed_steps = std::max(result.route_hint_replayed_steps, replayed_hint_steps);
            if (!error.empty() || !relaxed_oracle.error().empty()) {
                result.error =
                    "candidate route calculation failed: " + (!error.empty() ? error : relaxed_oracle.error());
                return result;
            }

            PolicyRouteOutcome route_outcome;
            route_outcome.revealed_node_count = static_cast<int>(node_mask_size(route.revealed_nodes));
            route_outcome.effective_node_count = route.effective_node_score;
            route_outcome.battle_count = route.metric.battles;
            route_outcome.intermediate_interaction_count = route.intermediate_interactions;
            route_outcome.processing_move_counts = route.metric.processing_move_counts;
            if (ranking_options.optimize_processing_moves) {
                for (std::size_t index = 1; index < route_outcome.processing_move_counts.size(); ++index) {
                    const int count = route_outcome.processing_move_counts[index];
                    route_outcome.processing_move_count += count;
                    const MovementSpec* spec = find_movement_spec(static_cast<MovementKind>(index));
                    if (spec != nullptr && !spec->expires_on_floor_end) {
                        route_outcome.persistent_processing_move_count += count;
                    }
                }
            }
            else {
                route_outcome.processing_move_count = route.metric.processing_moves;
                route_outcome.persistent_processing_move_count = route.metric.persistent_processing_moves;
            }
            route_outcome.route_length = route.metric.route_length;
            route_outcome.movement_action_count = static_cast<int>(route.steps.size());
            for (std::size_t index = 0; index < milestones.size() && index < route.progress.size(); ++index) {
                route_outcome.milestone_progress.emplace(milestones[index].definition->id, route.progress[index]);
            }
            candidate.route_outcomes.emplace_back(std::move(route_outcome));

            worst_intermediate_interactions =
                std::max(worst_intermediate_interactions, route.intermediate_interactions);
            guaranteed_revealed_nodes =
                std::min(guaranteed_revealed_nodes, static_cast<int>(node_mask_size(route.revealed_nodes)));
            guaranteed_effective_nodes = std::min(guaranteed_effective_nodes, route.effective_node_score);
            const ReachableFeatures outcome_features = planned_route_features(*request.map, route.route);
            merge_route_union(possible_route_features, outcome_features);
            if (!guaranteed_route_features.has_value()) {
                guaranteed_route_features = outcome_features;
            }
            else {
                intersect_route_features(*guaranteed_route_features, outcome_features);
            }
            if (first_outcome) {
                guaranteed_progress = route.progress;
                candidate.immediate_milestone_ids = route.immediate_milestone_ids;
                candidate.planned_route = route.route;
                candidate.planned_route_steps = route.steps;
                candidate.revealed_nodes = nodes_from_mask(*request.map, relaxed_graph, route.revealed_nodes);
                worst_metric = route.metric;
                first_outcome = false;
            }
            else {
                for (std::size_t index = 0; index < guaranteed_progress.size(); ++index) {
                    guaranteed_progress[index] = std::min(guaranteed_progress[index], route.progress[index]);
                }
                std::erase_if(candidate.immediate_milestone_ids, [&](const std::string& id) {
                    return std::ranges::find(route.immediate_milestone_ids, id) == route.immediate_milestone_ids.end();
                });
                worst_metric.battles = std::max(worst_metric.battles, route.metric.battles);
                worst_metric.processing_moves = std::max(worst_metric.processing_moves, route.metric.processing_moves);
                worst_metric.persistent_processing_moves =
                    std::max(worst_metric.persistent_processing_moves, route.metric.persistent_processing_moves);
                for (std::size_t index = 0; index < worst_metric.processing_move_counts.size(); ++index) {
                    const MovementKind movement = static_cast<MovementKind>(index);
                    const MovementSpec* spec = find_movement_spec(movement);
                    if (ranking_options.optimize_processing_moves && spec != nullptr && spec->expires_on_floor_end) {
                        worst_metric.processing_move_counts[index] = std::min(
                            worst_metric.processing_move_counts[index],
                            route.metric.processing_move_counts[index]);
                    }
                    else {
                        worst_metric.processing_move_counts[index] = std::max(
                            worst_metric.processing_move_counts[index],
                            route.metric.processing_move_counts[index]);
                    }
                }
                worst_metric.route_length = std::max(worst_metric.route_length, route.metric.route_length);
                candidate.planned_route.clear();
                candidate.planned_route_steps.clear();
                candidate.revealed_nodes.clear();
            }
        }
        set_route_feature_facts(
            candidate.facts,
            possible_route_features,
            guaranteed_route_features.value_or(ReachableFeatures {}));
        for (std::size_t index = 0; index < milestones.size(); ++index) {
            candidate.milestone_progress.emplace(milestones[index].definition->id, guaranteed_progress[index]);
        }
        candidate.battle_count = worst_metric.battles;
        candidate.intermediate_interaction_count = worst_intermediate_interactions;
        candidate.revealed_node_count =
            guaranteed_revealed_nodes == std::numeric_limits<int>::max() ? 0 : guaranteed_revealed_nodes;
        candidate.effective_node_count =
            guaranteed_effective_nodes == std::numeric_limits<int>::max() ? 0 : guaranteed_effective_nodes;
        candidate.processing_move_counts = worst_metric.processing_move_counts;
        if (ranking_options.optimize_processing_moves) {
            candidate.processing_move_count = 0;
            candidate.persistent_processing_move_count = 0;
            for (std::size_t index = 1; index < candidate.processing_move_counts.size(); ++index) {
                const int count = candidate.processing_move_counts[index];
                candidate.processing_move_count += count;
                const MovementSpec* spec = find_movement_spec(static_cast<MovementKind>(index));
                if (spec != nullptr && !spec->expires_on_floor_end) {
                    candidate.persistent_processing_move_count += count;
                }
            }
        }
        else {
            candidate.processing_move_count = worst_metric.processing_moves;
            candidate.persistent_processing_move_count = worst_metric.persistent_processing_moves;
        }
        candidate.route_length = worst_metric.route_length;
        candidate.development_score = 0;
        policy_candidates.emplace_back(std::move(candidate));
    }

    const bool has_safe_noncombat_alternative =
        std::ranges::any_of(policy_candidates, [](const PolicyCandidate& candidate) {
            return candidate.safe && !boolean_fact(candidate.facts, "candidate.combat");
        });
    for (auto& candidate : policy_candidates) {
        candidate.facts.set(
            "candidate.combat_is_optional",
            candidate.safe && boolean_fact(candidate.facts, "candidate.combat") && has_safe_noncombat_alternative);
    }

    bool has_safe_direct_exit = false;
    bool has_safe_non_exit = false;
    for (const auto& candidate : policy_candidates) {
        if (!candidate.safe) {
            continue;
        }
        if (boolean_fact(candidate.facts, "candidate.exit")) {
            has_safe_direct_exit = true;
        }
        else {
            has_safe_non_exit = true;
        }
    }
    FactStore policy_facts = *request.facts;
    policy_facts.set(
        "safety_margin_low",
        boolean_fact(*request.facts, "safety_margin_low") || (has_safe_direct_exit && !has_safe_non_exit));
    policy_facts.set(
        "floor_development_exhausted",
        boolean_fact(*request.facts, "floor_development_exhausted") || !has_safe_non_exit);

    ResourceRegistry resources;
    PolicyExecutor executor;
    const auto choose = [&](const std::vector<PolicyCandidate>& candidates) {
        PolicyDecision decision;
        if (request.probe_target.has_value()) {
            std::vector<PolicyCandidate> probe_candidates;
            std::ranges::copy_if(
                candidates,
                std::back_inserter(probe_candidates),
                [&](const PolicyCandidate& candidate) {
                    return candidate.safe && candidate.move.target == *request.probe_target;
                });
            if (!probe_candidates.empty()) {
                decision = executor.choose(
                    *request.policy,
                    policy_facts,
                    *request.mission,
                    *request.run,
                    resources,
                    result.binding_milestone_ids,
                    probe_candidates);
            }
        }
        if (!decision.selected.has_value()) {
            decision = executor.choose(
                *request.policy,
                policy_facts,
                *request.mission,
                *request.run,
                resources,
                result.binding_milestone_ids,
                candidates);
        }
        return decision;
    };

    if (result.mobile_marker_lookahead_active) {
        std::vector<PolicyCandidate> robust_candidates = policy_candidates;
        for (PolicyCandidate& candidate : robust_candidates) {
            if (candidate.safe && !boolean_fact(candidate.facts, "candidate.mobile_marker_robust")) {
                candidate.safe = false;
                ++result.mobile_marker_lookahead_rejected_candidates;
            }
        }
        result.decision = choose(robust_candidates);
        if (!result.decision.selected.has_value() && result.mobile_marker_lookahead_rejected_candidates > 0) {
            result.mobile_marker_lookahead_fallback_active = true;
            result.decision = choose(policy_candidates);
            result.decision.rejected.emplace_back(
                "流窜居民一拍最坏情形没有覆盖全部结果，已回退为仅避开当前第一步");
        }
    }
    else {
        result.decision = choose(policy_candidates);
    }
    if (!result.decision.selected.has_value()) {
        result.error = result.decision.reason;
    }
    else if (result.decision.selected->controllable) {
        const auto confirmed = confirmed_root_actions.find(result.decision.selected->action_id);
        if (confirmed != confirmed_root_actions.end()) {
            const int confirmed_requirement =
                confirmed_oracle.action_requirement(*confirmed->second, current_action_points);
            if (!confirmed_oracle.error().empty()) {
                result.error = "selected confirmed safety calculation failed: " + confirmed_oracle.error();
                return result;
            }
            if (confirmed_requirement < UnreachableActionPointRequirement) {
                MoveCandidate selected = confirmed->second->candidate;
                selected.graph_layer = GraphLayer::Confirmed;
                selected.action_point_requirement = confirmed_requirement;
                selected.requires_preview_verification =
                    request.probe_target.has_value() && selected.target == *request.probe_target;
                result.decision.selected = selected;
                if (!result.decision.planned_route_steps.empty() &&
                    result.decision.planned_route_steps.front().move.action_id == selected.action_id) {
                    result.decision.planned_route_steps.front().move = selected;
                }
            }
        }
    }
    if (result.decision.selected.has_value()) {
        const auto selected_summary = std::ranges::find(
            result.decision.candidate_summaries,
            result.decision.selected->action_id,
            [](const PolicyCandidateSummary& summary) { return summary.move.action_id; });
        const auto unproductive_floor_three_boss_route = [&]() {
            if (selected_summary == result.decision.candidate_summaries.end() || !request.no_AP_is_terminal ||
                request.run->floor != 3 || selected_summary->revealed_node_count != 0 ||
                selected_summary->effective_node_count != 0 || selected_summary->planned_route_steps.empty()) {
                return false;
            }
            const MoveCandidate& last = selected_summary->planned_route_steps.back().move;
            const NodeId landing = last.landing != InvalidNodeId ? last.landing : last.target;
            const Node* node = request.map->find_node(landing);
            const NodeType type = move_landing_type(last, landing);
            return (type == NodeType::BattleBoss || (type == NodeType::Unknown && node != nullptr &&
                                                     node->type == NodeType::BattleBoss));
        }();
        if (selected_summary != result.decision.candidate_summaries.end() &&
            (route_is_pure_action_point_exhaustion(*selected_summary, request.no_AP_is_terminal) ||
             unproductive_floor_three_boss_route)) {
            const MoveCandidate previous_selection = *result.decision.selected;
            std::string approach_error;
            std::optional<FloorThreeBossApproach> boss_approach;
            if (request.run->floor == 3) {
                boss_approach = find_floor_three_boss_before_direct_exhaustion(
                    *request.map,
                    confirmed_graph,
                    confirmed_oracle,
                    current_action_points,
                    &approach_error);
            }
            if (!approach_error.empty()) {
                result.error = "floor three boss approach calculation failed: " + approach_error;
                return result;
            }

            if (boss_approach.has_value() && !boss_approach->steps.empty()) {
                const MoveCandidate selected = boss_approach->steps.front().move;
                const auto matching_summary = std::ranges::find(
                    result.decision.candidate_summaries,
                    selected.action_id,
                    [](const PolicyCandidateSummary& summary) { return summary.move.action_id; });
                PolicyCandidateSummary approach_summary = matching_summary == result.decision.candidate_summaries.end()
                                                              ? *selected_summary
                                                              : *matching_summary;
                approach_summary.move = selected;
                approach_summary.revealed_node_count = 0;
                approach_summary.effective_node_count = 0;
                approach_summary.battle_count = 1;
                approach_summary.processing_move_count = boss_approach->processing_moves;
                approach_summary.persistent_processing_move_count = boss_approach->persistent_processing_moves;
                approach_summary.processing_move_counts = boss_approach->processing_move_counts;
                approach_summary.route_length = boss_approach->route_length;
                approach_summary.risk_score = 0;
                approach_summary.planned_route_steps = boss_approach->steps;
                approach_summary.revealed_nodes.clear();
                for (std::size_t index = 0; index < approach_summary.lexicographic_score_labels.size() &&
                                            index < approach_summary.lexicographic_score.size();
                     ++index) {
                    const std::string& label = approach_summary.lexicographic_score_labels[index];
                    if (label == "revealed_node_count" || label == "effective_node_count") {
                        approach_summary.lexicographic_score[index] = 0;
                    }
                    else if (label == "battle_count") {
                        approach_summary.lexicographic_score[index] = 1;
                    }
                    else if (label == "persistent_processing_move_count") {
                        approach_summary.lexicographic_score[index] = boss_approach->persistent_processing_moves;
                    }
                    else if (label == "processing_move_count") {
                        approach_summary.lexicographic_score[index] =
                            ranking_options.optimize_processing_moves ? -boss_approach->processing_moves
                                                                      : boss_approach->processing_moves;
                    }
                    else if (label == "route_length") {
                        approach_summary.lexicographic_score[index] = boss_approach->route_length;
                    }
                    else if (label == "movement_action_count") {
                        approach_summary.lexicographic_score[index] =
                            static_cast<int>(boss_approach->steps.size());
                    }
                    else if (label == "risk_score") {
                        approach_summary.lexicographic_score[index] = 0;
                    }
                    else if (label.starts_with("processing_semantic.")) {
                        for (std::size_t movement_index = 1;
                             movement_index < boss_approach->processing_move_counts.size();
                             ++movement_index) {
                            const MovementKind movement = static_cast<MovementKind>(movement_index);
                            if (label == "processing_semantic." + std::string(to_string(movement))) {
                                approach_summary.lexicographic_score[index] =
                                    boss_approach->processing_move_counts[movement_index];
                                break;
                            }
                        }
                    }
                }

                result.decision.selected = selected;
                result.decision.planned_route = boss_approach->route;
                result.decision.planned_route_steps = boss_approach->steps;
                result.decision.runners_up.insert(result.decision.runners_up.begin(), previous_selection);
                result.decision.candidate_summaries.insert(
                    result.decision.candidate_summaries.begin(),
                    std::move(approach_summary));
                ++result.decision.total_candidates;
                ++result.decision.eligible_candidates;
                result.decision.reason_category = DecisionReasonCategory::TieBreak;
                result.decision.decisive_rule_id = "floor_three_boss_before_direct_exhaustion";
                result.decision.reason =
                    "floor three boss is reachable before direct exhaustion with at most one non-full-map processing move";
            }
            else {
                MoveCandidate direct;
                direct.action_id = "direct_exhaustion";
                direct.movement = MovementKind::Walk;
                direct.source = request.run->current_node;
                direct.target = request.run->current_node;
                direct.landing = request.run->current_node;
                direct.predicted_action_point_cost = current_action_points;
                direct.action_point_requirement = 0;
                direct.direct_exhaustion = true;
                direct.terminal_on_completion = true;

                PlannedRouteStep direct_step;
                direct_step.move = direct;
                direct_step.action_points_before = current_action_points;
                direct_step.action_point_cost = current_action_points;
                direct_step.action_points_after = 0;

                PolicyCandidateSummary direct_summary = *selected_summary;
                direct_summary.move = direct;
                direct_summary.battle_count = 0;
                direct_summary.processing_move_count = 0;
                direct_summary.persistent_processing_move_count = 0;
                direct_summary.processing_move_counts.fill(0);
                direct_summary.route_length = 1;
                direct_summary.risk_score = 0;
                direct_summary.planned_route_steps = { direct_step };
                direct_summary.revealed_nodes.clear();
                for (std::size_t index = 0; index < direct_summary.lexicographic_score_labels.size() &&
                                            index < direct_summary.lexicographic_score.size();
                     ++index) {
                    const std::string& label = direct_summary.lexicographic_score_labels[index];
                    if (label == "route_length") {
                        direct_summary.lexicographic_score[index] = 1;
                    }
                    else if (label == "persistent_processing_move_count" || label == "processing_move_count" ||
                             label.starts_with("processing_semantic.")) {
                        direct_summary.lexicographic_score[index] = 0;
                    }
                }

                result.decision.selected = direct;
                result.decision.planned_route = { request.run->current_node };
                result.decision.planned_route_steps = { direct_step };
                result.decision.runners_up.insert(result.decision.runners_up.begin(), previous_selection);
                result.decision.candidate_summaries.insert(
                    result.decision.candidate_summaries.begin(),
                    std::move(direct_summary));
                ++result.decision.total_candidates;
                ++result.decision.eligible_candidates;
                result.decision.reason_category = DecisionReasonCategory::TieBreak;
                result.decision.decisive_rule_id = "direct_exhaustion";
                result.decision.reason = "no exploration gain remains; directly exhaust action points";
            }
        }
    }
    if (result.decision.selected.has_value() && !result.decision.selected->direct_exhaustion &&
        !result.decision.planned_route_steps.empty()) {
        const MoveCandidate previous_selection = *result.decision.selected;
        const std::vector<PlannedRouteStep> previous_steps = result.decision.planned_route_steps;
        if (prefer_less_valuable_exchangeable_processing_steps(
                *request.map,
                *request.run,
                result.decision.planned_route_steps)) {
            const MoveCandidate canonical_selection = result.decision.planned_route_steps.front().move;
            const auto old_summary = std::ranges::find(
                result.decision.candidate_summaries,
                previous_selection.action_id,
                [](const PolicyCandidateSummary& summary) { return summary.move.action_id; });
            const auto canonical_root_summary = std::ranges::find(
                result.decision.candidate_summaries,
                canonical_selection.action_id,
                [](const PolicyCandidateSummary& summary) { return summary.move.action_id; });
            if (old_summary != result.decision.candidate_summaries.end()) {
                const PolicyCandidateSummary previous_selected_summary = *old_summary;
                old_summary->move = canonical_selection;
                old_summary->planned_route_steps = result.decision.planned_route_steps;
                if (canonical_root_summary != result.decision.candidate_summaries.end() &&
                    canonical_root_summary != old_summary) {
                    // 保留原高价值首步作为候选，同时用规范化后的完整安全路线替换原本
                    // 搜索预算内较差的低价值根路线，避免诊断列表出现两个相同首动作。
                    *canonical_root_summary = previous_selected_summary;
                    canonical_root_summary->move = previous_selection;
                    canonical_root_summary->planned_route_steps = previous_steps;
                }
            }
            result.decision.selected = canonical_selection;
            std::erase_if(result.decision.runners_up, [&](const MoveCandidate& candidate) {
                return candidate.action_id == canonical_selection.action_id;
            });
            result.decision.runners_up.insert(result.decision.runners_up.begin(), previous_selection);
            result.decision.decisive_rule_id = "exchangeable_processing_value_order";
            result.decision.reason_category = DecisionReasonCategory::TieBreak;
            result.decision.reason =
                "节点路线和安全结局相同，优先使用可等价交换的低价值加工品";
        }
    }
    if (!result.planning_graphs_shared) {
        result.confirmed_state_count = confirmed_graph.state_count();
    }
    result.relaxed_state_count = relaxed_graph.state_count();
    result.route_search_expansions = route_search_budget.consumed_expansions();
    result.route_labels_generated = route_search_statistics.labels_generated;
    result.route_labels_dominated = route_search_statistics.labels_dominated;
    result.route_labels_retained_peak = route_search_statistics.labels_retained_peak;
    result.route_trace_nodes = route_search_statistics.trace_nodes;
    result.route_search_time_exhausted = route_search_budget.time_exhausted();
    result.route_search_expansions_exhausted = route_search_budget.remaining_expansions == 0;
    const auto planning_finished = std::chrono::steady_clock::now();
    result.route_search_elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(planning_finished - route_search_started).count());
    return result;
}
} // namespace asst::blackflow
