#pragma once

#include <set>
#include <string_view>
#include <utility>

#include "Common/AsstTypes.h"

namespace asst
{
enum class BattleSkillClickMode
{
    Automatic,
    Explicit,
};

enum class BattleSkillClickResult
{
    Failed,
    Clicked,
    AlreadyActive,
};

template <typename Click>
[[nodiscard]] BattleSkillClickResult
    click_matched_skill(std::string_view matched_template, BattleSkillClickMode mode, Click&& click)
{
    // 显式作业可以主动关闭技能，自动开技能不能把停止按钮当成就绪按钮。
    if (mode == BattleSkillClickMode::Automatic && matched_template == "BattleSkillStopOnClick-TopView.png") {
        return BattleSkillClickResult::AlreadyActive;
    }
    return std::forward<Click>(click)() ? BattleSkillClickResult::Clicked : BattleSkillClickResult::Failed;
}

class BattleAutoSkillActivationGuard
{
public:
    [[nodiscard]] bool should_attempt(const Point& location) const noexcept
    {
        return !m_activated_locations.contains(location);
    }

    void record_success(const Point& location) { m_activated_locations.emplace(location); }

private:
    std::set<Point> m_activated_locations;
};
} // namespace asst
