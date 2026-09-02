#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "BlackFlowPolicy.h"
#include "BlackFlowStateSpace.h"

namespace asst::blackflow
{
struct RouteSearchOptions
{
    int time_budget_ms = 5000;
    std::size_t total_expansions = 16384;
    std::size_t expansions_per_root = 128;
    int greedy_preview_depth = 2;
    std::size_t greedy_preview_width = 16;
    bool safety_resource_dominance = true;
};

struct BlackFlowPlanRequest
{
    const MapSnapshot* map = nullptr;
    const RunState* run = nullptr;
    const ResolvedPolicy* policy = nullptr;
    const FactStore* facts = nullptr;
    const MissionState* mission = nullptr;
    // 策略声明「达成即收工」的目标节点，与物理出口一起构成端点集合。
    std::unordered_set<NodeId> strategy_terminal_nodes;
    // 待锁定的强制目标，按优先级从高到低排好。plan() 会沿这个序做可行性阶梯：
    // 证得出安全解就锁定，证不出就从末尾降级一个再试，因此不会出现整层无解。
    std::vector<std::string> binding_milestone_candidates;
    // 候选里最前面这几条是无条件必达的，阶梯不会降级它们；不可达时本层照声明判成无解。
    std::size_t undemotable_binding_count = 0;
    // 允许路线把行动力耗尽并进入追猎的策略打开它。打开后锁定目标为空的那几轮不再为出口
    // 预留行动力，路线可以一直走到付不起下一步为止；它不表示整局或策略已经完成。
    bool no_AP_is_terminal = false;
    const std::unordered_set<std::string>* forbidden_actions = nullptr;
    std::unordered_set<NodeType> forbidden_node_types;
    // 只约束当前实际要执行的第一步。用于会在地图上移动的节点标记：后续模拟时
    // 标记位置已经不可靠，不能像固定节点类型一样封死整条路线。
    std::unordered_set<std::string> root_forbidden_marker_types;
    // 对上述移动标记再向前看一拍：枚举它们在首步结算后的相邻/原地结果，并要求每种结果
    // 都各自存在一个安全应对动作。这里只执行首步，随后重新观测；无解时允许退回首步避让。
    bool robust_mobile_marker_lookahead = false;
    std::unordered_set<MovementKind> reserved_movement_kinds;
    int reserved_movement_charges = 0;
    std::optional<NodeId> probe_target;
    // 上一次规划尚未执行的动作序列。规划器只把它当作可验证的热启动提示：当前地图、
    // 库存、动态费用或安全规则不再允许其中任一步时，立即停止复用并继续正常搜索。
    std::vector<std::string> route_hint_action_ids;
    std::size_t maximum_states = 2'000'000;
    RouteSearchOptions route_search;
};

struct PreviewSafetyVerification
{
    bool safe = false;
    int action_points_after = 0;
    int required_action_points_after = UnreachableActionPointRequirement;
    std::optional<std::size_t> proof_depth;
    std::string error;
};

// 安全值的评估结论：当前状态距安全出口还需多少行动力，以及首个动作与证明深度。
struct SafetyAssessment
{
    int required_action_points = UnreachableActionPointRequirement;
    std::optional<std::string> first_action;
    std::optional<std::size_t> proof_depth;
};

struct BlackFlowPlan
{
    SafetyAssessment safety;
    SafetyAssessment relaxed_safety;
    PolicyDecision decision;
    // 本轮实际锁定的强制目标，以及因为证不出可行而降级成倾向的那些。
    std::unordered_set<std::string> binding_milestone_ids;
    std::vector<std::string> demoted_milestone_ids;
    std::uint64_t map_revision = 0;
    std::uint64_t cost_revision = 0;
    std::size_t confirmed_state_count = 0;
    std::size_t relaxed_state_count = 0;
    std::size_t route_search_expansions = 0;
    std::uint64_t planning_elapsed_us = 0;
    std::uint64_t graph_initialization_elapsed_us = 0;
    std::uint64_t confirmed_safety_elapsed_us = 0;
    std::uint64_t relaxed_safety_elapsed_us = 0;
    std::uint64_t route_search_elapsed_us = 0;
    bool planning_graphs_shared = false;
    std::size_t route_labels_generated = 0;
    std::size_t route_labels_dominated = 0;
    std::size_t route_labels_retained_peak = 0;
    std::size_t route_trace_nodes = 0;
    std::size_t route_root_action_count = 0;
    int route_search_time_budget_ms = 0;
    std::size_t route_search_total_expansions = 0;
    std::size_t route_search_expansions_per_root = 0;
    bool route_search_time_exhausted = false;
    bool route_search_expansions_exhausted = false;
    bool route_hint_root_matched = false;
    std::size_t route_hint_replayed_steps = 0;
    // 记录本次实际采用的安全语义，供预览复核与诊断使用。它只表示路线可以把行动力用尽，
    // 不表示整局结束；行动力耗尽后仍会进入追猎流程。
    bool no_AP_is_terminal = false;
    // 1--4 层原本要求抵达出口，但当前观测下暂时证不出安全出口路线时，本轮可逆地退回探索规划。
    // 下一次观测会重新从出口约束开始求解，不会把回退状态写入局内事实。
    bool endpoint_fallback_active = false;
    std::string endpoint_fallback_reason;
    bool mobile_marker_lookahead_active = false;
    bool mobile_marker_lookahead_fallback_active = false;
    std::size_t mobile_marker_lookahead_rejected_candidates = 0;
    std::size_t mobile_marker_outcomes_checked = 0;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty() && decision.selected.has_value(); }
};

// 已选路线的节点序列、总资源与安全结局完全不变时，把能力更弱、价值更低的加工品
// 提前使用，把强移动方式留到后续重规划。返回值表示路线是否发生了规范化交换。
[[nodiscard]] bool prefer_less_valuable_exchangeable_processing_steps(
    const MapSnapshot& map,
    const RunState& run,
    std::vector<PlannedRouteStep>& steps);

class BlackFlowPlanner
{
public:
    // 框架层（Assistant::working_proc）不捕获任务异常；求解器分配失败等异常若从这里逃逸，
    // 整个进程会 std::terminate，因此公开入口统一兜底转为 error 结果。
    [[nodiscard]] BlackFlowPlan plan(const BlackFlowPlanRequest& request) const;
    [[nodiscard]] PreviewSafetyVerification verify_previewed_move(
        const BlackFlowPlanRequest& request,
        const MoveCandidate& move,
        int exact_action_point_cost) const;

private:
    [[nodiscard]] BlackFlowPlan plan_impl(const BlackFlowPlanRequest& request) const;
    [[nodiscard]] PreviewSafetyVerification verify_previewed_move_impl(
        const BlackFlowPlanRequest& request,
        const MoveCandidate& move,
        int exact_action_point_cost) const;
};
} // namespace asst::blackflow
