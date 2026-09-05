#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BlackFlowModel.h"
#include "BlackFlowSafetyGoal.h"

namespace asst::blackflow
{
class BlackFlowCompactStateSpace;

using SafetyStateId = std::uint32_t;
inline constexpr int UnreachableActionPointRequirement = std::numeric_limits<int>::max() / 4;

using PlannerNodeMask = std::uint64_t;

struct PlannerState
{
    NodeId node = InvalidNodeId;
    std::array<std::uint8_t, 13> movement_charges {};
    PlannerNodeMask completed_nodes = 0;
    PlannerNodeMask opened_blockers = 0;
    PlannerNodeMask consumed_lights = 0;
    // 凶戾揭示控制避战合法性；其他隐藏节点的揭示也会改变小八界和定向加工品的动作集。
    PlannerNodeMask revealed_hidden_battles = 0;
    PlannerNodeMask newly_revealed_nodes = 0;
    SafetyGoalProgressId goal_progress_id = InvalidSafetyGoalProgressId;
    // 只有当前站在“选择路过”的险路尽头时为 true；离开后自动清除，之后仍可重新
    // 进入同一尽头并选择真正下层。它不能写进 completed_nodes。
    bool current_final_bypassed = false;
    bool terminal = false;

    bool operator==(const PlannerState&) const noexcept = default;
};

struct PlannerStateHash
{
    std::size_t operator()(const PlannerState& state) const noexcept;
};

struct StateExpansionOptions
{
    // 策略声明「达成即收工」的目标节点。它与物理出口取并集构成端点集合，因此端点集合恒非空；
    // 目标做没做完由 safety_goal 的合取项回答，不会因为策略目标暂时不存在就让求解无终点。
    std::unordered_set<NodeId> strategy_terminal_nodes;
    std::unordered_set<std::string> forbidden_action_ids;
    std::unordered_set<NodeType> forbidden_node_types;
    // 调用方把未知凶戾列为禁区时，可选择豁免抵达前已被先前落点揭示的后续落点。
    bool allow_revealed_hidden_battle = false;
    std::unordered_set<MovementKind> reserved_movement_kinds;
    int reserved_movement_charges = 0;
    GraphLayer graph_layer = GraphLayer::Confirmed;
    bool final_is_terminal = true;
    // 行动力耗尽视为合法收工。走出本层与耗尽行动力结局相同的策略打开它，
    // 为出口预留行动力就不再有意义。
    bool no_AP_is_terminal = false;
    SafetyGoalProgram* safety_goal = nullptr;
    const FactStore* safety_goal_facts = nullptr;
    SafetyGoalProgressId initial_goal_progress_id = InvalidSafetyGoalProgressId;
    bool use_compact_actions = true;
    std::size_t maximum_states = 2'000'000;
};

struct OnDemandSafetyOutcome
{
    SafetyStateId successor = 0;
    int action_point_gain = 0;
};

struct OnDemandSafetyAction
{
    MoveCandidate candidate;
    int action_point_cost = 0;
    int minimum_action_points_to_start = 1;
    std::vector<OnDemandSafetyOutcome> outcomes;
};

class OnDemandStateGraph
{
public:
    OnDemandStateGraph();
    ~OnDemandStateGraph();
    OnDemandStateGraph(const OnDemandStateGraph&) = delete;
    OnDemandStateGraph& operator=(const OnDemandStateGraph&) = delete;
    [[nodiscard]] bool initialize(
        const MapSnapshot& map,
        const RunState& run,
        StateExpansionOptions options,
        std::string* error = nullptr);

    [[nodiscard]] SafetyStateId initial_state() const noexcept { return m_initial_state; }

    [[nodiscard]] const PlannerState& state(SafetyStateId id) const { return m_states.at(id); }

    [[nodiscard]] bool is_terminal(SafetyStateId id) const noexcept;
    [[nodiscard]] bool exhaustion_terminates() const noexcept;
    [[nodiscard]] bool is_terminal_node(NodeId node) const noexcept;
    [[nodiscard]] bool is_completed(SafetyStateId id, NodeId node) const noexcept;
    [[nodiscard]] bool is_light_consumed(SafetyStateId id, NodeId node) const noexcept;
    // 与 PlannerState 中的节点位图使用同一索引；路线收益标签复用它以避免维护第二套映射。
    [[nodiscard]] std::optional<PlannerNodeMask> node_mask(NodeId node) const noexcept { return bit(node); }
    [[nodiscard]] const std::vector<OnDemandSafetyAction>* actions(SafetyStateId id, std::string* error = nullptr);

    [[nodiscard]] std::size_t state_count() const noexcept { return m_states.size(); }

    [[nodiscard]] const MapSnapshot& map() const noexcept { return *m_map; }

    [[nodiscard]] const RunState& source_run() const noexcept { return *m_run; }

private:
    [[nodiscard]] std::optional<SafetyStateId> intern(PlannerState state, std::string* error);
    [[nodiscard]] RunState materialize(const PlannerState& state) const;
    [[nodiscard]] bool state_is_endpoint(const PlannerState& state) const noexcept;
    [[nodiscard]] bool state_is_goal(const PlannerState& state) const noexcept;
    [[nodiscard]] std::optional<PlannerNodeMask> bit(NodeId node) const noexcept;

    const MapSnapshot* m_map = nullptr;
    const RunState* m_run = nullptr;
    StateExpansionOptions m_options;
    SafetyStateId m_initial_state = 0;
    std::vector<NodeId> m_indexed_nodes;
    std::unordered_map<NodeId, std::uint8_t> m_node_indices;
    std::vector<PlannerState> m_states;
    std::unordered_map<PlannerState, SafetyStateId, PlannerStateHash> m_ids;
    std::deque<std::optional<std::vector<OnDemandSafetyAction>>> m_actions;
    std::unique_ptr<BlackFlowCompactStateSpace> m_compact;
};

struct ProjectedMoveOutcome
{
    RunState run;
    int action_point_gain = 0;
};

[[nodiscard]] std::optional<ProjectedMoveOutcome> project_move_outcome(
    const MapSnapshot& map,
    const RunState& run,
    const MoveCandidate& move,
    int exact_action_point_cost,
    std::string* error = nullptr);

} // namespace asst::blackflow
