#pragma once

#include <set>

#include "Common/AsstTypes.h"

namespace asst
{
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
