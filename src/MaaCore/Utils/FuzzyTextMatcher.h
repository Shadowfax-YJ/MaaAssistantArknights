#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace asst::utils
{
struct FuzzyTextMatch
{
    std::string canonical;
    std::string runner_up;
    double similarity = 0.0;
    double runner_up_similarity = 0.0;
    std::size_t edit_distance = std::numeric_limits<std::size_t>::max();
    bool exact = false;
    bool accepted = false;
};

struct FuzzyTextMatchSettings
{
    double minimum_similarity = 0.75;
    double minimum_margin = 0.12;
    std::size_t maximum_edit_distance = 2;
};

namespace detail
{
inline bool ignored_ocr_character(char32_t value) noexcept
{
    if (value <= 0x7f) {
        return std::isalnum(static_cast<unsigned char>(value)) == 0;
    }
    switch (value) {
    case L'　':
    case L'“':
    case L'”':
    case L'‘':
    case L'’':
    case L'《':
    case L'》':
    case L'〈':
    case L'〉':
    case L'【':
    case L'】':
    case L'（':
    case L'）':
    case L'、':
    case L'，':
    case L'。':
    case L'：':
    case L'；':
    case L'！':
    case L'？':
        return true;
    default:
        return false;
    }
}

inline std::u32string decode_utf8(std::string_view text)
{
    std::u32string decoded;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        char32_t value = 0;
        std::size_t width = 1;
        if ((first & 0x80U) == 0) {
            value = first;
        }
        else if ((first & 0xe0U) == 0xc0U && index + 1 < text.size()) {
            value = first & 0x1fU;
            width = 2;
        }
        else if ((first & 0xf0U) == 0xe0U && index + 2 < text.size()) {
            value = first & 0x0fU;
            width = 3;
        }
        else if ((first & 0xf8U) == 0xf0U && index + 3 < text.size()) {
            value = first & 0x07U;
            width = 4;
        }
        else {
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                valid = false;
                break;
            }
            value = (value << 6) | (continuation & 0x3fU);
        }
        if (valid) {
            decoded.push_back(value);
            index += width;
        }
        else {
            ++index;
        }
    }
    return decoded;
}

inline std::u32string normalize_ocr_text(std::string_view text)
{
    std::u32string normalized;
    for (char32_t value : decode_utf8(text)) {
        if (ignored_ocr_character(value)) {
            continue;
        }
        if (value >= L'A' && value <= L'Z') {
            value = static_cast<char32_t>(value - L'A' + L'a');
        }
        normalized.push_back(value);
    }
    return normalized;
}

inline std::size_t levenshtein_distance(std::u32string_view left, std::u32string_view right)
{
    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t column = 0; column <= right.size(); ++column) {
        previous[column] = column;
    }
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= right.size(); ++column) {
            const std::size_t substitution =
                previous[column - 1] + (left[row - 1] == right[column - 1] ? 0U : 1U);
            current[column] = std::min({ previous[column] + 1, current[column - 1] + 1, substitution });
        }
        previous.swap(current);
    }
    return previous.back();
}
} // namespace detail

inline FuzzyTextMatch fuzzy_match_ocr_text(
    std::string_view captured,
    const std::vector<std::string>& candidates,
    const FuzzyTextMatchSettings& settings = {})
{
    FuzzyTextMatch output;
    const std::u32string input = detail::normalize_ocr_text(captured);
    if (input.empty()) {
        return output;
    }

    std::size_t best_length = 0;
    for (const std::string& candidate : candidates) {
        const std::u32string normalized = detail::normalize_ocr_text(candidate);
        if (normalized.empty()) {
            continue;
        }
        const std::size_t distance = detail::levenshtein_distance(input, normalized);
        const std::size_t length = std::max(input.size(), normalized.size());
        const double similarity = 1.0 - static_cast<double>(distance) / static_cast<double>(length);
        if (output.canonical.empty() || similarity > output.similarity) {
            output.runner_up = std::move(output.canonical);
            output.runner_up_similarity = output.similarity;
            output.canonical = candidate;
            output.similarity = similarity;
            output.edit_distance = distance;
            best_length = normalized.size();
        }
        else if (output.runner_up.empty() || similarity > output.runner_up_similarity) {
            output.runner_up = candidate;
            output.runner_up_similarity = similarity;
        }
    }

    if (output.canonical.empty()) {
        return output;
    }
    output.exact = output.edit_distance == 0;
    const std::size_t proportional_limit = std::max<std::size_t>(1, best_length / 4);
    const std::size_t edit_limit = std::min(settings.maximum_edit_distance, proportional_limit);
    // 两三字候选也允许纠正一个字，但必须唯一领先。长度自适应下限分别是 50% 和约 67%。
    const double one_edit_similarity =
        best_length > 1 ? 1.0 - 1.0 / static_cast<double>(best_length) : 1.0;
    const double required_similarity = std::min(settings.minimum_similarity, one_edit_similarity);
    output.accepted = output.exact ||
                      (best_length > 1 && output.edit_distance <= edit_limit &&
                       output.similarity >= required_similarity &&
                       output.similarity - output.runner_up_similarity >= settings.minimum_margin);
    return output;
}
} // namespace asst::utils
