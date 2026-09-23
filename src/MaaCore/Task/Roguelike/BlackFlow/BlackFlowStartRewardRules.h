#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/AsstTypes.h"
#include "Utils/FuzzyTextMatcher.h"

namespace asst::blackflow
{
struct AutomationCollectionStartReward
{
    std::string_view display_name;
};

// 襁褓动物是自动化收集的最高优先级，按数组顺序选择；全部不存在时才进入下方随机奖励池。
inline constexpr std::array<std::string_view, 5> AutomationCollectionPreferredStartRewards = {
    "襁褓金乌",
    "襁褓天马",
    "襁褓白泽",
    "襁褓巨龙",
    "襁褓骏鹰",
};

// 开局界面实际可能出现的完整标题词表。自动化收集只用 OCR 识别这些标题，
// 不再用奖励卡片模板兜底；模板在动画、缩放和局部遮挡下比标题 OCR 更不稳定。
inline constexpr std::array<std::string_view, 11> AutomationCollectionStartRewardTitles = {
    "襁褓金乌",
    "襁褓天马",
    "襁褓白泽",
    "襁褓巨龙",
    "襁褓骏鹰",
    "未编号物",
    "退行补偿",
    "林间代步",
    "巢寄生",
    "调查预付款",
    "空间租赁",
};

// 没有襁褓动物时只从这四种行动奖励中抽取。调用方先做均匀洗牌，再按洗牌顺序匹配；
// 四项都可见时，第一项等概率为四者之一，个别标题 OCR 失败时仍可回退到其余允许项。
inline constexpr std::array<AutomationCollectionStartReward, 4> AutomationCollectionStartRewards = {
    AutomationCollectionStartReward { "未编号物" },
    AutomationCollectionStartReward { "退行补偿" },
    AutomationCollectionStartReward { "林间代步" },
    AutomationCollectionStartReward { "巢寄生" },
};

// 前两级候选均未出现时不能停在奖励页无限重试。优先选择无条件获得源石锭的调查预付款；
// 若它也不存在，空间租赁仍比无法开始本局更可取。
inline constexpr std::array<std::string_view, 2> AutomationCollectionFallbackStartRewards = {
    "调查预付款",
    "空间租赁",
};

struct AutomationCollectionStartRewardSelection
{
    std::size_t detected_index = 0;
    std::string canonical;
    bool preferred = false;
};

// 奖励被选中后，“你确定要这么做”提示与底部确认勾位于同一卡片中轴线上。
// y=595 是 1280x720 基准坐标；Controller 会统一处理设备缩放。
[[nodiscard]] inline Point automation_collection_start_reward_confirmation_point(const Rect& prompt_rect) noexcept
{
    return Point { prompt_rect.x + prompt_rect.width / 2, 595 };
}

[[nodiscard]] inline std::optional<std::string>
normalize_automation_collection_start_reward_title(std::string_view title)
{
    std::vector<std::string> candidates;
    candidates.reserve(AutomationCollectionStartRewardTitles.size());
    for (const std::string_view candidate_title : AutomationCollectionStartRewardTitles) {
        candidates.emplace_back(candidate_title);
    }

    const utils::FuzzyTextMatch match = utils::fuzzy_match_ocr_text(title, candidates);
    // 四字奖励标题允许两个 OCR 字误，但仍要求候选唯一领先。真实样本
    // “强裸骏鹰”因此可归一为“襁褓骏鹰”，而不会把模糊结果静默猜成相邻奖励。
    const bool accepted = match.exact ||
                          (match.edit_distance <= 2 && match.similarity >= 0.5 &&
                           match.similarity - match.runner_up_similarity >= 0.12);
    return accepted ? std::optional<std::string>(match.canonical) : std::nullopt;
}

[[nodiscard]] inline std::optional<AutomationCollectionStartRewardSelection>
select_automation_collection_start_reward(
    const std::vector<std::string>& detected_titles,
    const std::vector<std::string_view>& ordinary_priority)
{
    struct RecognizedTitle
    {
        std::size_t detected_index = 0;
        std::string canonical;
    };
    std::vector<RecognizedTitle> recognized;
    for (std::size_t index = 0; index < detected_titles.size(); ++index) {
        if (const auto canonical = normalize_automation_collection_start_reward_title(detected_titles[index])) {
            recognized.emplace_back(RecognizedTitle { index, *canonical });
        }
    }

    const auto select = [&](std::string_view wanted, bool preferred)
        -> std::optional<AutomationCollectionStartRewardSelection> {
        const auto found = std::ranges::find(recognized, wanted, &RecognizedTitle::canonical);
        if (found == recognized.end()) {
            return std::nullopt;
        }
        return AutomationCollectionStartRewardSelection {
            .detected_index = found->detected_index,
            .canonical = found->canonical,
            .preferred = preferred,
        };
    };
    for (const std::string_view preferred : AutomationCollectionPreferredStartRewards) {
        if (auto result = select(preferred, true); result.has_value()) {
            return result;
        }
    }
    for (const std::string_view ordinary : ordinary_priority) {
        if (auto result = select(ordinary, false); result.has_value()) {
            return result;
        }
    }
    for (const std::string_view fallback : AutomationCollectionFallbackStartRewards) {
        if (auto result = select(fallback, false); result.has_value()) {
            return result;
        }
    }
    return std::nullopt;
}

// 保留 OCR 原始顺序和全部框；未能唯一归一的文字仍保留，但不冒充已识别奖励。
[[nodiscard]] inline json::object automation_collection_start_reward_candidates_details(
    const std::vector<TextRect>& detected,
    const std::optional<AutomationCollectionStartRewardSelection>& selected)
{
    json::array candidates;
    for (std::size_t index = 0; index < detected.size(); ++index) {
        auto candidate = detected[index].to_json();
        candidate["detected_index"] = index;
        const auto canonical = normalize_automation_collection_start_reward_title(detected[index].text);
        candidate["canonical"] = canonical.has_value() ? json::value(*canonical) : json::value(nullptr);
        candidates.emplace_back(std::move(candidate));
    }
    json::object details {
        { "recognition_scope", "visible_title_roi" },
        { "recognition_status", detected.empty() ? "no_titles" : "titles_detected" },
        { "candidate_count", detected.size() },
        { "candidates", std::move(candidates) },
        { "planned_selection", nullptr },
    };
    if (selected.has_value() && selected->detected_index < detected.size()) {
        details["planned_selection"] = json::object {
            { "detected_index", selected->detected_index },
            { "name", selected->canonical },
            { "preferred", selected->preferred },
        };
    }
    return details;
}
} // namespace asst::blackflow
