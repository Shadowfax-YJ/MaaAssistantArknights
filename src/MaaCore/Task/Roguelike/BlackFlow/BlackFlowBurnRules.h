#pragma once

#include <string_view>

namespace asst::blackflow
{
[[nodiscard]] constexpr bool is_burn_profile(std::string_view profile) noexcept
{
    return profile == "burn" || profile == "burn_with_investment";
}

[[nodiscard]] constexpr bool
    should_inspect_burn_utopia(std::string_view profile, int floor, std::string_view outcome) noexcept
{
    return is_burn_profile(profile) && floor == 3 && outcome == "burn_completed";
}

} // namespace asst::blackflow
