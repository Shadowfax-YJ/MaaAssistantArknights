#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

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

// Temporary maps have a separate coordinate namespace, not a sixth main floor.
inline constexpr int TreeHoleFloor = 6;

// 以弹层读到的乌托邦名称约束模板，不从地图背景的灰雾或红色标题图标猜颜色。
[[nodiscard]] constexpr std::string_view tree_hole_mist_color(std::string_view effect) noexcept
{
    constexpr std::array<std::pair<std::string_view, std::string_view>, 9> colors = {
        std::pair { "巨人摇篮", "red" },   { "迪斯科狂热", "red" }, { "已知浩劫", "red" },
        { "孤立石林", "red" },           { "全知者盲区", "blue" }, { "未亡者遗怨", "green" },
        { "源石之城", "gold" },           { "消耗螺旋", "orange" }, { "换心联结", "purple" },
    };
    std::string_view result;
    for (const auto& [name, color] : colors) {
        if (effect.find(name) != std::string_view::npos) {
            if (!result.empty() && result != color) {
                return {}; // 混合标题不提供可信的颜色约束。
            }
            result = color;
        }
    }
    return result;
}

[[nodiscard]] constexpr bool topology_matches_tree_hole_color(
    int floor,
    std::string_view template_color,
    std::string_view observed_color) noexcept
{
    return floor != TreeHoleFloor || observed_color.empty() || template_color == observed_color;
}

inline constexpr std::array TreeHoleProfileCandidates = {
    FloorProfile { 6, 4, 5 }, FloorProfile { 6, 3, 3 }, FloorProfile { 6, 3, 5 }, FloorProfile { 6, 1, 7 },
    FloorProfile { 6, 4, 4 }, FloorProfile { 6, 2, 5 }, FloorProfile { 6, 1, 6 }, FloorProfile { 6, 5, 5 },
};

[[nodiscard]] constexpr std::span<const FloorProfile> floor_profile_candidates(int floor) noexcept
{
    if (floor == TreeHoleFloor) {
        return TreeHoleProfileCandidates;
    }
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
    if (floor == TreeHoleFloor) {
        return FloorProfile { 6, 5, 7 };
    }
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
