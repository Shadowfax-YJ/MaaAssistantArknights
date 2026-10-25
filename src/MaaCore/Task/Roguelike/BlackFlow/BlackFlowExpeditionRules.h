#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BlackFlowAutomationCollectionRules.h"

namespace asst::blackflow
{
inline constexpr std::string_view ExpeditionEventName = "未涉足之树";

struct ExpeditionContext
{
    std::uint64_t run_revision = 0;
    std::uint64_t page_revision = 0;
    int floor = 0;
    std::vector<std::string> operators;
};

// 只在确认派遣成功后调用。第一个选项派遣天猫会令其精二，第二个选项不提供这一效果。
[[nodiscard]] constexpr int confirmed_expedition_core_elite(
    std::optional<std::size_t> initial_choice,
    std::string_view operator_name,
    int previous_elite) noexcept
{
    return initial_choice == 0 && operator_name == AutomationCollectionCoreOperator &&
                   previous_elite >= 0 && previous_elite < 2
               ? 2
               : previous_elite;
}

[[nodiscard]] inline std::vector<std::string>
    prioritized_expedition_operator_order(const std::vector<std::string>& operators)
{
    std::vector<std::string> result;
    for (const auto name : { AutomationCollectionCoreOperator,
                             AutomationCollectionSpecialistOperator,
                             AutomationCollectionDefenderOperator }) {
        if (std::ranges::find(operators, name) != operators.end()) {
            result.emplace_back(name);
        }
    }
    return result;
}

// 天猫继续要求未精二且已证明离队后能安全出层，辅助干员不受该额外路线条件限制。
[[nodiscard]] inline std::vector<std::string> expedition_eligible_operators(
    int floor,
    bool has_gummy,
    bool has_ethan,
    std::optional<int> core_elite,
    bool battle_free_exit)
{
    std::vector<std::string> operators;
    if (floor != 2) {
        return operators;
    }
    if (has_gummy) {
        operators.emplace_back(AutomationCollectionDefenderOperator);
    }
    if (has_ethan) {
        operators.emplace_back(AutomationCollectionSpecialistOperator);
    }
    if (core_elite.has_value() && *core_elite >= 0 && *core_elite < 2 && battle_free_exit) {
        operators.emplace_back(AutomationCollectionCoreOperator);
    }
    return prioritized_expedition_operator_order(operators);
}

// 只接受选中干员后左下角的完整说明，不能把未选择时的提示或头像 OCR 当作姓名。
[[nodiscard]] inline std::string expedition_operator_from_description(std::string_view text)
{
    constexpr std::string_view prefix = "干员";
    constexpr std::string_view suffix = "将会在本区域";
    const auto end = text.find(suffix);
    if (!text.starts_with(prefix) || end == std::string_view::npos || end <= prefix.size()) {
        return {};
    }
    return std::string(text.substr(prefix.size(), end - prefix.size()));
}
} // namespace asst::blackflow
