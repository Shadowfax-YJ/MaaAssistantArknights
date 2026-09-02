#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace asst::blackflow
{
inline constexpr std::string_view AutomationCollectionFirstOperator = "机械师";
inline constexpr std::string_view AutomationCollectionCasterOperator = "卡达";
inline constexpr std::string_view AutomationCollectionCoreOperator = "凯尔希·思衡托";
inline constexpr std::string_view AutomationCollectionDefenderOperator = "古米";
inline constexpr std::string_view AutomationCollectionSpecialistOperator = "伊桑";
inline constexpr std::string_view AutomationCollectionSquad = "堡垒战术分队";
inline constexpr std::string_view AutomationCollectionRoles = "坚不可摧";
inline constexpr int AutomationCollectionRecruitHardSwipeLimit = 99;
inline constexpr int AutomationCollectionRecruitEmptyPageRetryLimit = 2;

// 通用招募页的 5 次上限不足以覆盖某些职业券的完整名单。固定队模式以
// “完成一次有效滑动后页面不变”为正常终止条件，这里的 99 只是识别/动画异常时的硬保险。
[[nodiscard]] inline constexpr int automation_collection_recruit_scan_limit(int configured_limit) noexcept
{
    return std::max(configured_limit, AutomationCollectionRecruitHardSwipeLimit);
}

inline constexpr std::array<std::string_view, 5> AutomationCollectionOperators {
    AutomationCollectionFirstOperator,
    AutomationCollectionCasterOperator,
    AutomationCollectionCoreOperator,
    AutomationCollectionDefenderOperator,
    AutomationCollectionSpecialistOperator,
};

inline constexpr std::array<std::string_view, 1> AutomationCollectionCoreRecruitmentTickets {
    "医疗招募券",
};

inline constexpr std::array<std::string_view, 3> AutomationCollectionDefenderRecruitmentTickets {
    "重装招募券",
    "精锐重装招募券",
    "堡垒协议招募券",
};

inline constexpr std::array<std::string_view, 1> AutomationCollectionSpecialistRecruitmentTickets {
    "特种招募券",
};

inline constexpr std::string_view AutomationCollectionCoreTraining = "医者-新典训";

struct AutomationCollectionTeamProgress
{
    bool first_operator_elite_two = false;
    bool caster_operator_recruited = false;
    bool core_operator_elite_two = false;
    bool defender_operator_recruited = false;
    bool specialist_operator_recruited = false;
};

[[nodiscard]] inline constexpr bool
    automation_collection_team_complete(const AutomationCollectionTeamProgress& progress) noexcept
{
    return progress.first_operator_elite_two && progress.caster_operator_recruited &&
           progress.core_operator_elite_two && progress.defender_operator_recruited &&
           progress.specialist_operator_recruited;
}

// 固定队招募页只有在滑动后确认页面未变化，或已经耗尽允许的滑动次数时才结束。
// 识别到的卡片数量不能作为末页判据：漏识一张卡会让 8 张变成 7 张。
[[nodiscard]] inline constexpr bool should_continue_automation_collection_recruit_scan(
    bool endpoint_confirmed,
    int completed_swipes,
    int maximum_swipes) noexcept
{
    return !endpoint_confirmed && completed_swipes < maximum_swipes;
}

[[nodiscard]] inline bool automation_collection_recruit_page_is_unchanged_endpoint(
    const std::unordered_set<std::string>& previous,
    const std::unordered_set<std::string>& current,
    int completed_swipes)
{
    return completed_swipes >= 1 && !previous.empty() && previous == current;
}

enum class AutomationCollectionRecruitPageAction
{
    Swipe,
    RetrySamePage,
    StopAtSeenPage,
};

// 招募列表的滑动动画可能在截图前后回弹。对识别失败或曾经见过的页面先原地重截图，
// 只有连续两次看到同一组名称才把它当作稳定结果；完全空白时有限重试后继续扫描，
// 避免单帧 OCR 失败把固定队招募提前截断。
class AutomationCollectionRecruitPageTracker
{
public:
    [[nodiscard]] AutomationCollectionRecruitPageAction observe(
        const std::unordered_set<std::string>& detected_names,
        bool analysis_succeeded,
        int completed_swipes)
    {
        if (detected_names.empty()) {
            clear_pending();
            if (m_empty_page_retries++ < AutomationCollectionRecruitEmptyPageRetryLimit) {
                return AutomationCollectionRecruitPageAction::RetrySamePage;
            }
            m_empty_page_retries = 0;
            return AutomationCollectionRecruitPageAction::Swipe;
        }
        m_empty_page_retries = 0;

        const bool seen_after_swipe = completed_swipes >= 1 && page_was_seen(detected_names);
        if (analysis_succeeded && !seen_after_swipe) {
            remember_page(detected_names);
            clear_pending();
            return AutomationCollectionRecruitPageAction::Swipe;
        }

        if (!m_pending_page || *m_pending_page != detected_names) {
            m_pending_page = detected_names;
            return AutomationCollectionRecruitPageAction::RetrySamePage;
        }

        clear_pending();
        if (seen_after_swipe) {
            return AutomationCollectionRecruitPageAction::StopAtSeenPage;
        }

        remember_page(detected_names);
        return AutomationCollectionRecruitPageAction::Swipe;
    }

private:
    [[nodiscard]] bool page_was_seen(const std::unordered_set<std::string>& names) const
    {
        return std::ranges::find(m_seen_pages, names) != m_seen_pages.end();
    }

    void remember_page(const std::unordered_set<std::string>& names)
    {
        if (!page_was_seen(names)) {
            m_seen_pages.emplace_back(names);
        }
    }

    void clear_pending() { m_pending_page.reset(); }

    std::vector<std::unordered_set<std::string>> m_seen_pages;
    std::optional<std::unordered_set<std::string>> m_pending_page;
    int m_empty_page_retries = 0;
};

// base.json 要求慢滑距离保持在 400~500，既保证至少露出一列，也不能跨过整列。
[[nodiscard]] inline constexpr int automation_collection_recruit_swipe_distance(int rightmost_oper_x) noexcept
{
    return std::clamp(rightmost_oper_x - 200, 400, 500);
}

[[nodiscard]] inline constexpr bool is_automation_collection_operator(std::string_view name) noexcept
{
    for (const std::string_view oper : AutomationCollectionOperators) {
        if (name == oper) {
            return true;
        }
    }
    return false;
}

// 自动化收集的招募券只服务固定五人队伍：未入队时直接招募；已入队时只接受精二晋升。
// 这条判断独立于通用招募优先级，避免为了比较无关候选而扫描完整列表。
[[nodiscard]] inline constexpr bool should_recruit_visible_automation_collection_operator(
    std::string_view name,
    bool already_recruited,
    int owned_elite,
    int displayed_elite) noexcept
{
    if (!is_automation_collection_operator(name)) {
        return false;
    }
    if (!already_recruited) {
        return true;
    }
    return owned_elite < 2 && displayed_elite >= 2;
}

[[nodiscard]] inline constexpr bool is_automation_collection_core_recruitment_ticket(std::string_view name) noexcept
{
    for (const std::string_view ticket : AutomationCollectionCoreRecruitmentTickets) {
        if (name == ticket) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline constexpr bool is_automation_collection_defender_recruitment_ticket(
    std::string_view name) noexcept
{
    for (const std::string_view ticket : AutomationCollectionDefenderRecruitmentTickets) {
        if (name == ticket) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline constexpr bool is_automation_collection_specialist_recruitment_ticket(
    std::string_view name) noexcept
{
    for (const std::string_view ticket : AutomationCollectionSpecialistRecruitmentTickets) {
        if (name == ticket) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline constexpr bool is_automation_collection_recruitment_ticket(std::string_view name) noexcept
{
    return is_automation_collection_core_recruitment_ticket(name) ||
           is_automation_collection_defender_recruitment_ticket(name) ||
           is_automation_collection_specialist_recruitment_ticket(name);
}

[[nodiscard]] inline constexpr bool automation_collection_shop_progress_purchase_allowed(
    std::string_view name,
    const AutomationCollectionTeamProgress& progress) noexcept
{
    if (automation_collection_team_complete(progress) &&
        (is_automation_collection_recruitment_ticket(name) || name == AutomationCollectionCoreTraining)) {
        return false;
    }
    if (progress.core_operator_elite_two &&
        (is_automation_collection_core_recruitment_ticket(name) || name == AutomationCollectionCoreTraining)) {
        return false;
    }
    if (progress.defender_operator_recruited && is_automation_collection_defender_recruitment_ticket(name)) {
        return false;
    }
    if (progress.specialist_operator_recruited && is_automation_collection_specialist_recruitment_ticket(name)) {
        return false;
    }
    return true;
}
} // namespace asst::blackflow
