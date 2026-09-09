#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asst::blackflow
{
inline constexpr std::string_view SacrificeEventName = "回滚文明";

struct SacrificeContext
{
    std::uint64_t run_revision = 0;
    std::uint64_t page_revision = 0;
    bool operator==(const SacrificeContext&) const = default;
};

enum class SacrificePhase
{
    Initial,
    AwaitPicker,
    AwaitReward,
    AwaitRepeat,
    BeforeCivilization,
    AwaitCivilization,
    Finished,
};

inline bool is_restore_civilization(std::string text)
{
    for (const std::string_view token : { "\"", "'", "“", "”", "‘", "’", " ", "\t" }) {
        for (auto pos = text.find(token); pos != std::string::npos; pos = text.find(token)) {
            text.erase(pos, token.size());
        }
    }
    return text == "复原文明";
}

// 保留同名物品的数量：两份相同自然物被消耗时，结果中也必须出现两次。
inline std::vector<std::string> removed_natural_items(std::vector<std::string> before, std::vector<std::string> after)
{
    std::ranges::sort(before);
    std::ranges::sort(after);
    std::vector<std::string> removed;
    std::ranges::set_difference(before, after, std::back_inserter(removed));
    return removed;
}

template <typename Options>
std::vector<std::size_t> sacrifice_initial_choices(const Options& options)
{
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].enabled && is_restore_civilization(options[i].text)) {
            return { i };
        }
    }
    std::vector<std::size_t> choices;
    for (std::size_t i = 0; i < std::min(std::size_t(2), options.size()); ++i) {
        if (options[i].enabled && options[i].text != "离开") {
            choices.push_back(i);
        }
    }
    return choices;
}
}
