#pragma once

#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include "BlackFlowObservation.h"

namespace asst::blackflow
{
struct ResidentSettlementPrediction
{
    std::vector<GridPosition> candidates;
    std::optional<GridPosition> exact;
    std::vector<GridPosition> initial_residents;
    std::vector<GridPosition> possible_overlap_residents;
    std::size_t hypothesis_count = 0;
};

[[nodiscard]] inline bool deterministic_prediction_conflicts_with_observation(
    const Node& predicted,
    const ObservedNode& observed,
    bool resolved_current_landing) noexcept
{
    if (!predicted.identity_from_prediction || !observed.type.has_value() ||
        !observed.identity_revealed.value_or(false) || *observed.type == predicted.type) {
        return false;
    }
    // 该来源只表示拓扑模板确认这里有一个槽位，但 OCR 没有读到节点身份。
    // 流窜居民等标记遮住节点时，它会暂时把节点写成空地；这不是“看到了空地”，
    // 因而不能推翻确定性身份预测。
    if (observed.identity_source == "map_topology_no_ocr_empty") {
        return false;
    }
    // 非重复节点结算后会显示成空地。这是页面状态变化，不是否定进入前由确定性规则
    // 得到的原始身份。识别可能发生在离开落点后的下一帧，因此同时接受“当前正在
    // 结算”和探索笔记已标记 completed 两种证据。
    return !(*observed.type == NodeType::Empty &&
             (resolved_current_landing || predicted.progress == NodeProgress::Completed));
}

[[nodiscard]] inline bool resident_settlement_hypotheses_have_exact_consensus(
    const std::vector<std::vector<GridPosition>>& candidates_by_hypothesis) noexcept
{
    if (candidates_by_hypothesis.empty() || candidates_by_hypothesis.front().size() != 1) {
        return false;
    }
    const GridPosition expected = candidates_by_hypothesis.front().front();
    return std::ranges::all_of(candidates_by_hypothesis, [&](const auto& candidates) {
        return candidates.size() == 1 && candidates.front() == expected;
    });
}

[[nodiscard]] constexpr bool utopia_effect_expires_after_node_completion(
    bool page_completed,
    std::string_view utopia_ideology,
    const std::optional<GridPosition>& ideal_source,
    GridPosition resolved_position) noexcept
{
    return page_completed && utopia_ideology != "hopeful-soil" && ideal_source.has_value() &&
           resolved_position == *ideal_source;
}

// 移植自 lubiao-wiki：只在主地图 2/4/5 层使用初始流窜“居民”的空间证据。
// 追忆四层不使用林间空地数量约束。初始居民必定位于林间空地；若空地上的其他
// 标记可能遮住居民标记，分别枚举“没有重合”及合法重合组合；只有所有假设都给出
// 同一个唯一候选时才锁定据点。
[[nodiscard]] std::vector<std::vector<GridPosition>> resident_marker_hypotheses(
    const MapObservationBatch& map);

[[nodiscard]] ResidentSettlementPrediction predict_resident_settlement(
    const MapObservationBatch& map,
    NodeId original_start,
    bool floor_four_remembrance);

void apply_exact_resident_settlement_prediction(
    MapObservationBatch& map,
    const ResidentSettlementPrediction& prediction);

[[nodiscard]] bool reject_resident_settlement_prediction(
    ResidentSettlementPrediction& prediction,
    GridPosition contradicted_position) noexcept;
} // namespace asst::blackflow
