#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/AsstTypes.h"
#include "Utils/FuzzyTextMatcher.h"

namespace asst::blackflow
{
inline constexpr std::size_t PreparationCombatSettleDelayMs = 1500;

[[nodiscard]] inline std::optional<std::string> resolve_battle_intel_stage_name(
    std::string_view captured, const std::vector<std::string>& configured_stage_names)
{
    const auto first = captured.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    captured = captured.substr(first, captured.find_last_not_of(" \t\r\n") - first + 1);
    if (captured == "作战" || captured == "紧急作战" || captured == "未知的凶戾" ||
        captured == "未知的诡秘" || captured == "险路恶敌") {
        return std::nullopt;
    }
    // 情报收集不要求已有自动作战脚本。脚本名称仅用于纠正可唯一匹配的 OCR 误差；
    // 未配置的关卡仍保留预览原文，由调用方的连续两帧稳定性检查确认。
    const auto match = utils::fuzzy_match_ocr_text(captured, configured_stage_names);
    return match.accepted ? match.canonical : std::string(captured);
}

// N 在战斗循环内会被反复识别。用众数抵抗偶发 OCR 抖动；票数相同时采用
// 最近一次结果，使后续稳定、纠正后的读数优先于开场读数。
class BattleTotalKillsAggregator
{
public:
    void observe(int total_kills)
    {
        if (total_kills <= 0) {
            return;
        }
        ++m_counts[total_kills];
        m_last_seen.insert_or_assign(total_kills, ++m_observation_sequence);
    }

    [[nodiscard]] std::optional<int> result() const noexcept
    {
        if (m_counts.empty()) {
            return std::nullopt;
        }
        std::size_t best_count = 0;
        for (const auto& [value, count] : m_counts) {
            (void)value;
            best_count = std::max(best_count, count);
        }
        std::optional<int> result;
        std::size_t newest_observation = 0;
        for (const auto& [value, count] : m_counts) {
            const std::size_t last_seen = m_last_seen.at(value);
            if (count == best_count && (!result.has_value() || last_seen > newest_observation)) {
                result = value;
                newest_observation = last_seen;
            }
        }
        return result;
    }

    void clear() noexcept
    {
        m_counts.clear();
        m_last_seen.clear();
        m_observation_sequence = 0;
    }

private:
    std::unordered_map<int, std::size_t> m_counts;
    std::unordered_map<int, std::size_t> m_last_seen;
    std::size_t m_observation_sequence = 0;
};

[[nodiscard]] inline constexpr bool
    preparation_combat_actions_ready(bool has_preparation_camera_animation, std::size_t elapsed_ms) noexcept
{
    return !has_preparation_camera_animation || elapsed_ms >= PreparationCombatSettleDelayMs;
}

// 拖拽后先取消选中，再用部署栏卡片状态确认结果。未取消选中时，失败的拖拽也会
// 让卡片暂时不可用，不能据此判定部署成功。
[[nodiscard]] inline constexpr bool deployment_attempt_confirmed(
    bool selection_cleared,
    bool card_visible,
    bool card_cooling,
    bool card_available) noexcept
{
    return selection_cleared && (!card_visible || card_cooling || !card_available);
}

// Observe returns no value when the current frame cannot establish the deployment result.
// Wait bounds how long the caller can keep the attempted deployment pending.
template <typename Observe, typename Wait>
[[nodiscard]] std::optional<bool> wait_for_deployment_confirmation(Observe&& observe, Wait&& wait)
{
    do {
        if (auto result = observe(); result.has_value()) {
            return result;
        }
    } while (wait());
    return std::nullopt;
}

// 等待中的虚拟装置刚刚开过技能时，技能可能改变部署费用。当前帧来自开技能前，
// 必须等下一帧重新识别费用后才能继续部署。
[[nodiscard]] inline constexpr bool virtual_auto_skill_transition_requires_refresh(
    bool waiting_before,
    bool activated_after) noexcept
{
    return waiting_before && activated_after;
}

struct BattleFixedControls
{
    Point retreat_button;
    Point skill_button;
    bool has_multi_stages = false;

    bool operator==(const BattleFixedControls&) const noexcept = default;
};

class DoubleSpeedRetrySchedule
{
public:
    explicit constexpr DoubleSpeedRetrySchedule(bool has_preparation_camera_animation) noexcept :
        m_next_attempt_after_ms(has_preparation_camera_animation ? PreparationSettleDelayMs : 0)
    {
    }

    [[nodiscard]] constexpr bool due(bool confirmed, std::size_t elapsed_ms) const noexcept
    {
        return !confirmed && elapsed_ms >= m_next_attempt_after_ms;
    }

    constexpr void mark_attempt(std::size_t elapsed_ms) noexcept
    {
        m_next_attempt_after_ms = elapsed_ms + RetryIntervalMs;
    }

private:
    static constexpr std::size_t PreparationSettleDelayMs = 1500;
    static constexpr std::size_t RetryIntervalMs = 2000;

    std::size_t m_next_attempt_after_ms = 0;
};

// TilePack's camera shift is only meaningful for battlefield tiles. The retreat/skill
// buttons and the multi-stage layout are fixed screen UI and must keep the values from
// the unshifted calculation.
[[nodiscard]] inline constexpr BattleFixedControls fixed_battle_controls_after_camera_shift(
    const BattleFixedControls& unshifted,
    const BattleFixedControls&) noexcept
{
    return unshifted;
}

[[nodiscard]] inline constexpr bool should_toggle_to_double_speed(bool already_double_speed) noexcept
{
    return !already_double_speed;
}

template <typename Observe, typename Click, typename Delay>
[[nodiscard]] bool ensure_double_speed(
    Observe&& observe,
    Click&& click,
    Delay&& delay,
    std::size_t max_clicks)
{
    for (std::size_t clicks = 0; clicks < max_clicks; ++clicks) {
        if (observe()) {
            return true;
        }
        if (!click()) {
            return false;
        }
        delay();
    }
    return observe();
}

[[nodiscard]] inline constexpr bool should_use_direct_ready_skill_confirmation(
    bool has_preparation_phase,
    const std::pair<double, double>& battle_camera_shift) noexcept
{
    return has_preparation_phase &&
           (battle_camera_shift.first != 0.0 || battle_camera_shift.second != 0.0);
}
} // namespace asst::blackflow
