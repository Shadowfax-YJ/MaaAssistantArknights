#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace asst::blackflow::perception
{

// 地图节点名称来自封闭词表。两到三字的短名称即使有一个 OCR 错字，只要长度
// 未丢失且相对次优候选仍唯一领先，也比把一个已点亮节点静默回退成空格可靠。
[[nodiscard]] constexpr bool accept_node_ocr_label_match(
    bool exact,
    std::size_t input_length,
    std::size_t label_length,
    std::size_t edit_distance,
    double similarity,
    double runner_up_similarity,
    double minimum_similarity,
    double minimum_margin,
    int short_fuzzy_length) noexcept
{
    if (exact) {
        return true;
    }
    const bool uniquely_leading = similarity - runner_up_similarity >= minimum_margin;
    const bool short_unique_single_error =
        label_length >= 2 && label_length <= static_cast<std::size_t>(std::max(0, short_fuzzy_length)) &&
        input_length == label_length && edit_distance == 1;
    const bool regular_match =
        label_length > static_cast<std::size_t>(std::max(0, short_fuzzy_length)) &&
        similarity >= minimum_similarity;
    return uniquely_leading && (short_unique_single_error || regular_match);
}

// “作战”只有两个字，类似“作占”的单字误差虽然可用于普通节点兜底，却不能推翻
// “非希望沃土的实托邦中心必为紧急作战”这一确定规则。完整精确 OCR 仍保留为冲突证据。
[[nodiscard]] constexpr bool weak_normal_battle_ocr_defers_to_ideal_source_prediction(
    std::string_view node_type,
    std::string_view identity_source,
    bool ocr_exact_match) noexcept
{
    return node_type == "battle_normal" && identity_source == "ocr" && !ocr_exact_match;
}

} // namespace asst::blackflow::perception
