#include "BlackFlowProductNameMatcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#define UNI_ALGO_DISABLE_PROP
#define UNI_ALGO_STATIC_DATA
#include <uni_algo/norm.h>

namespace
{
struct NormalizedUtf8
{
    std::string text;
    std::u32string code_points;
};

std::optional<std::u32string> decode_utf8(std::string_view input)
{
    std::u32string output;
    for (size_t index = 0; index < input.size();) {
        const auto first = static_cast<unsigned char>(input[index]);
        char32_t code_point = 0;
        size_t length = 0;
        if (first <= 0x7fU) {
            code_point = first;
            length = 1;
        }
        else if ((first & 0xe0U) == 0xc0U) {
            code_point = first & 0x1fU;
            length = 2;
        }
        else if ((first & 0xf0U) == 0xe0U) {
            code_point = first & 0x0fU;
            length = 3;
        }
        else if ((first & 0xf8U) == 0xf0U) {
            code_point = first & 0x07U;
            length = 4;
        }
        else {
            return std::nullopt;
        }

        if (index + length > input.size()) {
            return std::nullopt;
        }
        for (size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(input[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return std::nullopt;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }

        const bool overlong = (length == 2 && code_point < 0x80U) || (length == 3 && code_point < 0x800U) ||
                              (length == 4 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return std::nullopt;
        }
        output.push_back(code_point);
        index += length;
    }
    return output;
}

std::optional<NormalizedUtf8> normalize_nfc(std::string_view input)
{
    if (!decode_utf8(input)) {
        return std::nullopt;
    }

    auto text = una::norm::to_nfc_utf8(input);
    auto code_points = decode_utf8(text);
    if (!code_points) {
        return std::nullopt;
    }
    return NormalizedUtf8 {
        .text = std::move(text),
        .code_points = std::move(code_points.value()),
    };
}

std::string encode_utf8(std::u32string_view input)
{
    std::string output;
    for (const char32_t code_point : input) {
        if (code_point <= 0x7fU) {
            output.push_back(static_cast<char>(code_point));
        }
        else if (code_point <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
        else if (code_point <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
        else {
            output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
    }
    return output;
}

bool is_unicode_whitespace(char32_t code_point)
{
    return (code_point >= 0x0009U && code_point <= 0x000dU) || code_point == 0x0020U || code_point == 0x0085U ||
           code_point == 0x00a0U || code_point == 0x1680U || (code_point >= 0x2000U && code_point <= 0x200aU) ||
           code_point == 0x2028U || code_point == 0x2029U || code_point == 0x202fU || code_point == 0x205fU ||
           code_point == 0x3000U;
}

bool is_greek_letter(char32_t code_point)
{
    struct CodePointRange
    {
        char32_t first;
        char32_t last;
    };

    // Unicode 15.1 Script=Greek and General_Category=L code points; Greek-block punctuation stays distinct.
    static constexpr std::array GreekLetterRanges {
        CodePointRange { 0x0370U, 0x0373U }, CodePointRange { 0x0376U, 0x0377U }, CodePointRange { 0x037aU, 0x037dU },
        CodePointRange { 0x037fU, 0x037fU }, CodePointRange { 0x0386U, 0x0386U }, CodePointRange { 0x0388U, 0x038aU },
        CodePointRange { 0x038cU, 0x038cU }, CodePointRange { 0x038eU, 0x03a1U }, CodePointRange { 0x03a3U, 0x03e1U },
        CodePointRange { 0x03f0U, 0x03f5U }, CodePointRange { 0x03f7U, 0x03ffU }, CodePointRange { 0x1d26U, 0x1d2aU },
        CodePointRange { 0x1d5dU, 0x1d61U }, CodePointRange { 0x1d66U, 0x1d6aU }, CodePointRange { 0x1dbfU, 0x1dbfU },
        CodePointRange { 0x1f00U, 0x1f15U }, CodePointRange { 0x1f18U, 0x1f1dU }, CodePointRange { 0x1f20U, 0x1f45U },
        CodePointRange { 0x1f48U, 0x1f4dU }, CodePointRange { 0x1f50U, 0x1f57U }, CodePointRange { 0x1f59U, 0x1f59U },
        CodePointRange { 0x1f5bU, 0x1f5bU }, CodePointRange { 0x1f5dU, 0x1f5dU }, CodePointRange { 0x1f5fU, 0x1f7dU },
        CodePointRange { 0x1f80U, 0x1fb4U }, CodePointRange { 0x1fb6U, 0x1fbcU }, CodePointRange { 0x1fbeU, 0x1fbeU },
        CodePointRange { 0x1fc2U, 0x1fc4U }, CodePointRange { 0x1fc6U, 0x1fccU }, CodePointRange { 0x1fd0U, 0x1fd3U },
        CodePointRange { 0x1fd6U, 0x1fdbU }, CodePointRange { 0x1fe0U, 0x1fecU }, CodePointRange { 0x1ff2U, 0x1ff4U },
        CodePointRange { 0x1ff6U, 0x1ffcU }, CodePointRange { 0x2126U, 0x2126U }, CodePointRange { 0xab65U, 0xab65U },
    };
    return std::ranges::any_of(GreekLetterRanges, [code_point](const auto& range) {
        return code_point >= range.first && code_point <= range.last;
    });
}

std::optional<std::u32string> normalize_ocr_text(std::string_view input)
{
    auto normalized = normalize_nfc(input);
    if (!normalized) {
        return std::nullopt;
    }

    auto& code_points = normalized->code_points;
    std::erase_if(code_points, is_unicode_whitespace);
    if (code_points.size() >= 2U && code_points.at(code_points.size() - 2U) == U'-' &&
        is_greek_letter(code_points.back())) {
        code_points.resize(code_points.size() - 2U);
    }
    return code_points;
}

std::optional<NormalizedUtf8> normalize_standard_product_name(std::string_view input)
{
    auto normalized = normalize_nfc(input);
    if (!normalized) {
        return std::nullopt;
    }
    auto& code_points = normalized->code_points;
    if (code_points.size() >= 2U && code_points.at(code_points.size() - 2U) == U'-' &&
        is_greek_letter(code_points.back())) {
        code_points.resize(code_points.size() - 2U);
        normalized->text = encode_utf8(code_points);
    }
    return normalized;
}

size_t levenshtein_distance(std::u32string_view left, std::u32string_view right)
{
    std::vector<size_t> previous(right.size() + 1U);
    std::vector<size_t> current(right.size() + 1U);
    for (size_t index = 0; index < previous.size(); ++index) {
        previous[index] = index;
    }

    for (size_t left_index = 0; left_index < left.size(); ++left_index) {
        current[0] = left_index + 1U;
        for (size_t right_index = 0; right_index < right.size(); ++right_index) {
            const size_t substitution = previous[right_index] + (left[left_index] == right[right_index] ? 0U : 1U);
            current[right_index + 1U] =
                std::min({ previous[right_index + 1U] + 1U, current[right_index] + 1U, substitution });
        }
        previous.swap(current);
    }
    return previous.back();
}

size_t maximum_edit_count(size_t candidate_length)
{
    if (candidate_length <= 3U) {
        return 0U;
    }
    if (candidate_length <= 7U) {
        return 1U;
    }
    return 2U;
}
} // namespace

asst::BlackFlowProductNameMatcher::BlackFlowProductNameMatcher(std::span<const std::string> standard_product_names)
{
    m_standard_product_names.reserve(standard_product_names.size());
    for (const auto& name : standard_product_names) {
        auto normalized = normalize_standard_product_name(name);
        if (normalized && !normalized->text.empty() &&
            std::ranges::find(m_standard_product_names, normalized->text, &StandardProductName::text) ==
                m_standard_product_names.end()) {
            m_standard_product_names.emplace_back(
                StandardProductName {
                    .text = std::move(normalized->text),
                    .code_points = std::move(normalized->code_points),
                });
        }
    }
}

asst::BlackFlowProductNameMatch
    asst::BlackFlowProductNameMatcher::match(std::string_view ocr_text, double ocr_score) const
{
    if (!std::isfinite(ocr_score) || ocr_score < 0.0 || ocr_score > 1.0) {
        return {};
    }

    auto normalized_code_points = normalize_ocr_text(ocr_text);
    if (!normalized_code_points) {
        return {};
    }
    const auto normalized_text = encode_utf8(normalized_code_points.value());

    if (ocr_score >= 0.75) {
        auto exact = std::ranges::find(m_standard_product_names, normalized_text, &StandardProductName::text);
        if (exact != m_standard_product_names.end()) {
            return {
                .kind = BlackFlowProductNameMatchKind::Exact,
                .standard_product_name = exact->text,
            };
        }

        if (normalized_code_points->size() >= 2U && normalized_code_points->front() == U'"' &&
            normalized_code_points->back() == U'"') {
            auto quoted_code_points = normalized_code_points.value();
            quoted_code_points.front() = U'“';
            quoted_code_points.back() = U'”';
            exact = std::ranges::find(
                m_standard_product_names,
                encode_utf8(quoted_code_points),
                &StandardProductName::text);
        }
        if (exact != m_standard_product_names.end()) {
            return {
                .kind = BlackFlowProductNameMatchKind::Exact,
                .standard_product_name = exact->text,
            };
        }
    }

    if (ocr_score < 0.85 || m_standard_product_names.empty()) {
        return {};
    }

    const StandardProductName* best_candidate = nullptr;
    size_t best_distance = std::numeric_limits<size_t>::max();
    size_t second_distance = std::numeric_limits<size_t>::max();
    for (const auto& candidate : m_standard_product_names) {
        const size_t distance = levenshtein_distance(normalized_code_points.value(), candidate.code_points);
        if (distance < best_distance) {
            second_distance = best_distance;
            best_distance = distance;
            best_candidate = &candidate;
        }
        else if (distance < second_distance) {
            second_distance = distance;
        }
    }

    const bool has_required_lead = second_distance == std::numeric_limits<size_t>::max() ||
                                   (second_distance >= best_distance && second_distance - best_distance >= 2U);
    if (best_candidate && best_distance <= maximum_edit_count(best_candidate->code_points.size()) &&
        has_required_lead) {
        return {
            .kind = BlackFlowProductNameMatchKind::Fuzzy,
            .standard_product_name = best_candidate->text,
        };
    }

    return {};
}
