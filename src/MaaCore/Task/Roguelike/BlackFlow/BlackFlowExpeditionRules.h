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
    return operators;
}

// 古米/伊桑合为一组，与天猫各占 50%；组内再等概率选择。
// 同组候选连续排列：抽中的组员缺席时先尝试另一名组员，保留这一组的 50% 概率。
// draw_ticket(total) 返回 [0, total) 内的整数。
template <typename DrawTicket>
[[nodiscard]] std::vector<std::string>
    grouped_expedition_operator_order(const std::vector<std::string>& operators, DrawTicket&& draw_ticket)
{
    std::vector<std::string> result;
    for (const auto name : { AutomationCollectionDefenderOperator, AutomationCollectionSpecialistOperator }) {
        if (std::ranges::find(operators, name) != operators.end()) {
            result.emplace_back(name);
        }
    }
    const bool has_core = std::ranges::find(operators, AutomationCollectionCoreOperator) != operators.end();
    const bool core_first = has_core && (result.empty() || draw_ticket(2) == 1);
    if (result.size() == 2 && draw_ticket(2) == 1) {
        std::swap(result[0], result[1]);
    }
    if (has_core) {
        result.insert(core_first ? result.begin() : result.end(), std::string(AutomationCollectionCoreOperator));
    }
    return result;
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
