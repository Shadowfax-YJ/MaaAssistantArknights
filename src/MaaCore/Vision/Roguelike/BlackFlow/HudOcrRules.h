#pragma once

#include <cmath>
#include <optional>
#include <string_view>

namespace asst::blackflow::perception
{
[[nodiscard]] inline std::optional<int> parse_ingots_ocr(std::string_view text, double confidence) noexcept
{
    // 地图上的斜线零曾以 0.438 的置信度被读成 10，不能直接写进事件决策上下文。
    if (!std::isfinite(confidence) || confidence < 0.8 || text.empty() || text.size() > 3) {
        return std::nullopt;
    }
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + ch - '0';
    }
    return value;
}
}
