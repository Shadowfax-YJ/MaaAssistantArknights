#pragma once

#include <array>
#include <limits>
#include <string_view>

namespace asst::blackflow::perception
{
struct TopologyMatchEvidence
{
    int occupied_symmetric_difference = 0;
    int edge_overlap = 0;
    int edge_extras = 0;
};

struct TopologyTerminalIdentity
{
    std::string_view node_type;
    std::string_view display_name;
};

struct FixedGridAliasRetryPlan
{
    int floor = 0;
    int rows = 0;
    int columns = 0;
    double translation_x = 0.0;
    std::array<double, 3> translation_y {};
};

inline constexpr int InvalidTopologyMatchScore = std::numeric_limits<int>::min();

[[nodiscard]] constexpr std::array<double, 3>
fixed_grid_row_alias_offsets(double primary_offset, double row_spacing) noexcept
{
    return { primary_offset, primary_offset - row_spacing, primary_offset + row_spacing };
}

// 返回一个完全按值持有的重试计划。调用方会向候选 vector 继续追加元素，
// 因此不能在扩容后继续引用 vector 中的首候选。
[[nodiscard]] constexpr FixedGridAliasRetryPlan make_fixed_grid_alias_retry_plan(
    int floor,
    int rows,
    int columns,
    double translation_x,
    double primary_translation_y,
    double row_spacing) noexcept
{
    return FixedGridAliasRetryPlan {
        floor,
        rows,
        columns,
        translation_x,
        fixed_grid_row_alias_offsets(primary_translation_y, row_spacing),
    };
}

[[nodiscard]] constexpr int current_marker_atlas_top(int map_top, int marker_height) noexcept
{
    const int padding = marker_height > 0 ? marker_height : 0;
    return map_top > padding ? map_top - padding : 0;
}

[[nodiscard]] constexpr TopologyTerminalIdentity terminal_identity_for_floor(int floor) noexcept
{
    if (floor == 3 || floor == 5) {
        return { "battle_boss", "险路恶敌" };
    }
    return { "final", "险路尽头" };
}

[[nodiscard]] constexpr int score_topology_match(const TopologyMatchEvidence& evidence) noexcept
{
    if (evidence.occupied_symmetric_difference > 5) {
        return InvalidTopologyMatchScore;
    }
    return -evidence.occupied_symmetric_difference * 200 + evidence.edge_overlap * 5 - evidence.edge_extras;
}

[[nodiscard]] constexpr bool topology_can_recover_disconnected_edge_graph(
    bool model_ready,
    int existing_node_count,
    int connected_components_after_constraint) noexcept
{
    return model_ready && existing_node_count > 0 && connected_components_after_constraint > 1;
}

[[nodiscard]] constexpr bool
topology_occupied_without_identity_is_empty(bool occupied, std::string_view node_type) noexcept
{
    return occupied && (node_type == "null" || node_type == "unclassified");
}

[[nodiscard]] constexpr bool
    retain_topology_extra_edge(bool cnn_connected, bool first_endpoint_permits, bool second_endpoint_permits) noexcept
{
    return cnn_connected && (first_endpoint_permits || second_endpoint_permits);
}
} // namespace asst::blackflow::perception
