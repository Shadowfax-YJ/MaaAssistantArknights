#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Utils/FuzzyTextMatcher.h"

namespace asst
{
inline std::optional<std::string> resolve_roguelike_battle_stage_name(
    std::string_view captured,
    const std::vector<std::string>& candidates)
{
    const utils::FuzzyTextMatch match = utils::fuzzy_match_ocr_text(captured, candidates);
    if (!match.accepted) {
        return std::nullopt;
    }
    return match.canonical;
}
} // namespace asst
