#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace asst::blackflow
{
struct LightHeaderBand
{
    int y = 0;
    int height = 0;
    double score = 0.0;
};

// 黑流树海的事件选项栏是半透明的。模板会混入背后的事件插画，不能只依赖
// 某一块底纹；改为在没有文字的右侧区域寻找连续的亮色横条。
[[nodiscard]] inline std::vector<LightHeaderBand> find_blackflow_light_header_bands(
    std::span<const double> light_row_ratios,
    int y_offset,
    double row_threshold = 0.75,
    std::size_t minimum_height = 20,
    std::size_t maximum_height = 55,
    std::size_t maximum_gap = 2)
{
    std::vector<LightHeaderBand> result;
    std::size_t begin = light_row_ratios.size();
    std::size_t last_light = 0;
    std::size_t gap = 0;
    double score_sum = 0.0;
    std::size_t score_count = 0;

    const auto flush = [&]() {
        if (begin == light_row_ratios.size()) {
            return;
        }
        const std::size_t height = last_light - begin + 1;
        if (height >= minimum_height && height <= maximum_height) {
            result.emplace_back(LightHeaderBand {
                .y = y_offset + static_cast<int>(begin),
                .height = static_cast<int>(height),
                .score = score_count == 0 ? 0.0 : score_sum / static_cast<double>(score_count),
            });
        }
        begin = light_row_ratios.size();
        gap = 0;
        score_sum = 0.0;
        score_count = 0;
    };

    for (std::size_t row = 0; row < light_row_ratios.size(); ++row) {
        const double ratio = light_row_ratios[row];
        if (ratio >= row_threshold) {
            if (begin == light_row_ratios.size()) {
                begin = row;
            }
            last_light = row;
            gap = 0;
            score_sum += ratio;
            ++score_count;
        }
        else if (begin != light_row_ratios.size() && ++gap > maximum_gap) {
            flush();
        }
    }
    flush();
    return result;
}
} // namespace asst::blackflow
