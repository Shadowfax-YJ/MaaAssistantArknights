#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace asst::blackflow
{

[[nodiscard]] inline int node_ocr_row_top(double node_center_y, int center_offset_y, int row_height) noexcept
{
    return static_cast<int>(std::lround(node_center_y + center_offset_y - 0.5 * row_height));
}

[[nodiscard]] inline double node_ocr_inference_scale(int width, int height, int minimum_side = 64) noexcept
{
    const int shorter_side = std::min(width, height);
    if (shorter_side <= 0 || shorter_side >= minimum_side) {
        return 1.0;
    }
    return static_cast<double>(minimum_side) / static_cast<double>(shorter_side);
}

inline bool node_ocr_fragments_have_mergeable_horizontal_gap(
    int left_x,
    int left_width,
    int right_x,
    int maximum_gap,
    int maximum_overlap) noexcept
{
    const int gap = right_x - (left_x + left_width);
    return gap >= -maximum_overlap && gap <= maximum_gap;
}

// A detector may split one title into adjacent boxes (for example “居民” + “据点”).
// Assign every fragment to the nearest grid column before merging.  The boundary
// is the midpoint between node columns, not the much narrower final-title-center
// tolerance: otherwise the right-hand fragment can be discarded, while merging
// the whole row can incorrectly concatenate two complete neighboring titles.
[[nodiscard]] inline std::optional<int> node_ocr_fragment_column(
    double fragment_center_x,
    double grid_origin_x,
    double grid_spacing_x,
    int columns) noexcept
{
    if (columns <= 0 || grid_spacing_x <= 0.0) {
        return std::nullopt;
    }
    const int column = static_cast<int>(std::lround((fragment_center_x - grid_origin_x) / grid_spacing_x));
    if (column < 0 || column >= columns) {
        return std::nullopt;
    }
    const double center = grid_origin_x + static_cast<double>(column) * grid_spacing_x;
    return std::abs(fragment_center_x - center) < 0.5 * grid_spacing_x ? std::optional<int>(column)
                                                                     : std::nullopt;
}

}
