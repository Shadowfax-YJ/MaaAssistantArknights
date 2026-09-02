#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace asst::blackflow::perception
{
struct FloorProfile
{
    int floor = 0;
    int rows = 0;
    int columns = 0;

    bool operator==(const FloorProfile&) const noexcept = default;
};

struct FloorViewportProfile
{
    int swipe_left_count = 0;
    bool before_every_capture = false;
};

inline constexpr std::array<FloorProfile, 5> FloorProfiles = {
    FloorProfile { 1, 3, 5 }, FloorProfile { 2, 4, 5 },  FloorProfile { 3, 5, 7 },
    FloorProfile { 4, 5, 8 }, FloorProfile { 5, 5, 10 },
};

inline constexpr std::array FloorFiveProfileCandidates = {
    FloorProfile { 5, 5, 10 },
    FloorProfile { 5, 5, 9 },
};

[[nodiscard]] constexpr std::span<const FloorProfile> floor_profile_candidates(int floor) noexcept
{
    if (floor < 1 || floor > static_cast<int>(FloorProfiles.size())) {
        return {};
    }
    if (floor == 5) {
        return FloorFiveProfileCandidates;
    }
    return std::span<const FloorProfile>(&FloorProfiles[static_cast<std::size_t>(floor - 1)], 1);
}

[[nodiscard]] constexpr std::optional<FloorProfile> floor_profile(int floor) noexcept
{
    const auto candidates = floor_profile_candidates(floor);
    if (candidates.empty()) {
        return std::nullopt;
    }
    return candidates.front();
}

[[nodiscard]] constexpr std::optional<FloorViewportProfile> floor_viewport_profile(int floor) noexcept
{
    if (floor != 5) {
        return std::nullopt;
    }
    // 五层地图比屏幕宽。每次重建前左划一次，让固定网格落在统一坐标上，并让最右列节点
    // 从行动力 HUD 下方露出；节点结算后视口可能变化，因此仍需在每次重建前执行。
    return FloorViewportProfile { 1, true };
}
} // namespace asst::blackflow::perception
