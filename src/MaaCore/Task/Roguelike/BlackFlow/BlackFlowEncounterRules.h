#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "Utils/FuzzyTextMatcher.h"

namespace asst::blackflow
{
inline constexpr const char* LakeFairyEventName = "湖中仙女";
inline constexpr const char* GoldStasisEventName = "金色凝滞";
inline constexpr const char* PeaceGuardEventName = "和平守卫者";
inline constexpr const char* PeaceGuardFollowupEventName = "和平守卫者-2";
inline constexpr const char* LoneSurvivorEventName = "独活";
inline constexpr const char* LoneSurvivorFollowupEventName = "独活-2";
inline constexpr const char* FinalEncounterEventName = "险路尽头";
inline constexpr const char* HealingHeartEventName = "愈创之心";
inline constexpr const char* HealingHeartPreferredChoice = "比对像素小猫和树干刻痕";
inline constexpr const char* HealingHeartActionPointChoice = "紧紧拥抱愈创木";
inline constexpr const char* HealingHeartProcessingItemChoice = "让这里有一个秋千";
inline constexpr const char* HealingHeartSafetyFallbackChoice = "检测此地有无危险";
inline constexpr const char* HealingHeartRestFallbackChoice = "闭上眼感受宁静";
inline constexpr int HealingHeartActionPointCost = 3;

enum class ChainedEncounterPageState
{
    Waiting,
    EventReady,
    MapReturned,
};

// 黑流树海的地图与事件页都会显示生命值。地图识别必须拥有最高优先级，
// 否则回到地图后会把地图 HUD 当成下一页事件已经就绪。
[[nodiscard]] inline constexpr ChainedEncounterPageState classify_chained_encounter_page(
    bool is_blackflow,
    bool map_visible,
    bool hp_visible) noexcept
{
    if (is_blackflow && map_visible) {
        return ChainedEncounterPageState::MapReturned;
    }
    return hp_visible ? ChainedEncounterPageState::EventReady : ChainedEncounterPageState::Waiting;
}

struct LakeFairyContext
{
    bool core_operator_elite_two = false;
    int ingots = 0;
};

struct LakeFairyChoicePlan
{
    std::array<std::size_t, 4> initial_choices {};
    std::size_t initial_choice_count = 0;
};

struct LinkedEncounterRouteValue
{
    bool viable = false;
    int required_action_points = 0;
    std::vector<int> lexicographic_score;
    std::vector<std::string> lexicographic_score_labels;
};

// 随机路线先以“所有结果都安全”为硬门槛；通过之后直接按期望分数字典序决胜。
// 总分使用整数交叉相乘比较平均值，避免浮点误差；期望完全相同时维持原配置优先级。
struct RandomRouteAggregate
{
    bool all_viable = true;
    bool compatible_scores = true;
    std::size_t sample_count = 0;
    int worst_required_action_points = 0;
    std::int64_t required_action_point_sum = 0;
    std::vector<int> worst_score;
    std::vector<std::int64_t> lexicographic_score_sum;
    std::vector<std::string> lexicographic_score_labels;
};

[[nodiscard]] inline bool random_route_is_viable(const RandomRouteAggregate& route) noexcept
{
    return route.sample_count > 0 && route.all_viable && route.compatible_scores;
}

inline void append_random_route_sample(RandomRouteAggregate& aggregate, const LinkedEncounterRouteValue& sample)
{
    aggregate.all_viable = aggregate.all_viable && sample.viable;
    aggregate.worst_required_action_points =
        std::max(aggregate.worst_required_action_points, sample.required_action_points);
    aggregate.required_action_point_sum += sample.required_action_points;

    if (aggregate.sample_count == 0) {
        aggregate.lexicographic_score_labels = sample.lexicographic_score_labels;
        aggregate.worst_score = sample.lexicographic_score;
        aggregate.lexicographic_score_sum.assign(sample.lexicographic_score.size(), 0);
    }
    else if (sample.lexicographic_score_labels != aggregate.lexicographic_score_labels ||
             sample.lexicographic_score.size() != aggregate.lexicographic_score_sum.size()) {
        aggregate.compatible_scores = false;
    }

    if (aggregate.compatible_scores && sample.lexicographic_score.size() == aggregate.lexicographic_score_sum.size()) {
        for (std::size_t index = 0; index < sample.lexicographic_score.size(); ++index) {
            aggregate.worst_score[index] = std::max(aggregate.worst_score[index], sample.lexicographic_score[index]);
            aggregate.lexicographic_score_sum[index] += sample.lexicographic_score[index];
        }
    }
    ++aggregate.sample_count;
}

[[nodiscard]] inline bool random_route_is_safe_expected_improvement(
    const RandomRouteAggregate& lhs,
    const RandomRouteAggregate& rhs) noexcept
{
    const bool lhs_viable = random_route_is_viable(lhs);
    const bool rhs_viable = random_route_is_viable(rhs);
    if (lhs_viable != rhs_viable) {
        return lhs_viable;
    }
    if (!lhs_viable || lhs.lexicographic_score_labels != rhs.lexicographic_score_labels ||
        lhs.lexicographic_score_sum.size() != rhs.lexicographic_score_sum.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.lexicographic_score_sum.size(); ++index) {
        const std::int64_t lhs_scaled = lhs.lexicographic_score_sum[index] *
                                        static_cast<std::int64_t>(rhs.sample_count);
        const std::int64_t rhs_scaled = rhs.lexicographic_score_sum[index] *
                                        static_cast<std::int64_t>(lhs.sample_count);
        if (lhs_scaled != rhs_scaled) {
            return lhs_scaled < rhs_scaled;
        }
    }
    return false;
}

using HealingHeartRouteAggregate = RandomRouteAggregate;

[[nodiscard]] inline bool healing_heart_route_is_viable(const HealingHeartRouteAggregate& route) noexcept
{
    return random_route_is_viable(route);
}

inline void append_healing_heart_route_sample(
    HealingHeartRouteAggregate& aggregate,
    const LinkedEncounterRouteValue& sample)
{
    append_random_route_sample(aggregate, sample);
}

[[nodiscard]] inline bool healing_heart_route_is_better(
    const HealingHeartRouteAggregate& lhs,
    const HealingHeartRouteAggregate& rhs) noexcept
{
    return random_route_is_safe_expected_improvement(lhs, rhs);
}

[[nodiscard]] inline std::vector<std::string> make_healing_heart_choice_order(
    const HealingHeartRouteAggregate& action_point_route,
    const HealingHeartRouteAggregate& processing_item_route)
{
    std::vector<std::string> order { HealingHeartPreferredChoice };
    const bool action_point_viable = healing_heart_route_is_viable(action_point_route);
    const bool processing_item_viable = healing_heart_route_is_viable(processing_item_route);

    if (action_point_viable && processing_item_viable) {
        // 原配置把行动力选项排在加工品选项之前；只有加工品路线严格更优时才交换。
        if (healing_heart_route_is_better(processing_item_route, action_point_route)) {
            order.emplace_back(HealingHeartProcessingItemChoice);
            order.emplace_back(HealingHeartActionPointChoice);
        }
        else {
            order.emplace_back(HealingHeartActionPointChoice);
            order.emplace_back(HealingHeartProcessingItemChoice);
        }
    }
    else if (action_point_viable) {
        order.emplace_back(HealingHeartActionPointChoice);
    }
    else if (processing_item_viable) {
        order.emplace_back(HealingHeartProcessingItemChoice);
    }

    // 无法证明安全的代价选项不会重新追加；OCR 若发现前面的选项不可用，就直接按
    // 原配置优先级进入这两个无代价回退项。
    order.emplace_back(HealingHeartSafetyFallbackChoice);
    order.emplace_back(HealingHeartRestFallbackChoice);
    return order;
}

[[nodiscard]] inline LinkedEncounterRouteValue adjusted_linked_encounter_route_value(
    LinkedEncounterRouteValue route,
    int immediate_revealed_node_count,
    int immediate_effective_node_weight)
{
    for (std::size_t index = 0;
         index < route.lexicographic_score_labels.size() && index < route.lexicographic_score.size();
         ++index) {
        const std::string& label = route.lexicographic_score_labels[index];
        if (label == "revealed_node_count") {
            route.lexicographic_score[index] -= std::max(0, immediate_revealed_node_count);
        }
        else if (label == "effective_node_count") {
            route.lexicographic_score[index] -= std::max(0, immediate_effective_node_weight);
        }
    }
    return route;
}

[[nodiscard]] inline bool linked_encounter_free_transfer_is_stably_better(
    const LinkedEncounterRouteValue& baseline,
    const LinkedEncounterRouteValue& transferred,
    int immediate_effective_node_weight,
    int baseline_immediate_revealed_node_count,
    int transferred_immediate_revealed_node_count)
{
    RandomRouteAggregate baseline_aggregate;
    RandomRouteAggregate transferred_aggregate;
    append_random_route_sample(
        baseline_aggregate,
        adjusted_linked_encounter_route_value(baseline, baseline_immediate_revealed_node_count, 0));
    append_random_route_sample(
        transferred_aggregate,
        adjusted_linked_encounter_route_value(
            transferred,
            transferred_immediate_revealed_node_count,
            immediate_effective_node_weight));
    return random_route_is_safe_expected_improvement(transferred_aggregate, baseline_aggregate);
}

[[nodiscard]] inline constexpr LakeFairyChoicePlan make_lake_fairy_choice_plan(
    const LakeFairyContext& context) noexcept
{
    if (context.core_operator_elite_two && context.ingots >= 3) {
        return { { 1, 1, 1, 1 }, 4 };
    }
    return { { 1, 2, 0, 0 }, 2 };
}

[[nodiscard]] inline int
    gold_stasis_choice_priority_bucket(std::string_view choice, bool core_operator_elite_two)
{
    if (!core_operator_elite_two) {
        return 0;
    }
    if (choice == "查看无人机" || choice == "向泉水许愿") {
        return 1;
    }

    static const std::vector<std::string> DeferredChoices { "查看无人机", "向泉水许愿" };
    const auto match = utils::fuzzy_match_ocr_text(choice, DeferredChoices);
    return match.accepted ? 1 : 0;
}
} // namespace asst::blackflow
