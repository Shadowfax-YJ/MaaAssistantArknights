#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace asst::utils
{
template <typename Enum, size_t Size>
inline std::string_view enum_name(Enum value, const std::array<std::pair<Enum, std::string_view>, Size>& names)
{
    const auto iter = std::ranges::find_if(names, [value](const auto& item) { return item.first == value; });
    return iter == names.end() ? std::string_view() : iter->second;
}

template <typename Enum, size_t Size>
inline std::optional<Enum>
    parse_enum(std::string_view value, const std::array<std::pair<Enum, std::string_view>, Size>& names)
{
    const auto iter = std::ranges::find_if(names, [value](const auto& item) { return item.second == value; });
    return iter == names.end() ? std::nullopt : std::optional(iter->first);
}
} // namespace asst::utils
