#include "BlackFlowTrophyRewardTaskPlugin.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "BlackFlowCollectionPopup.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/Roguelike/BlackFlow/BlackFlowTrophyRewardAnalyzer.h"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view RewardTask = "BlackFlow@Roguelike@GetDropTrophyRewardConfirmed";
constexpr std::string_view PriorityTask = "BlackFlow@Roguelike@TrophyRewardPriority";
constexpr std::array<std::string_view, 3> Categories = { "加工品", "自然物", "概念体" };

size_t rank_reward(const BlackFlowTrophyRewardAnalyzer::Detection& detection)
{
    const auto task = Task.get<OcrTaskInfo>(std::string(PriorityTask));
    if (task != nullptr) {
        const auto item = std::ranges::find(task->text, detection.name);
        if (item != task->text.end()) {
            return static_cast<size_t>(std::distance(task->text.begin(), item));
        }
    }

    // 名称未识别时才按旧类别顺序兜底；所有已知名称始终优先于类别兜底项。
    const auto category = std::ranges::find(Categories, detection.category);
    if (category != Categories.end()) {
        const size_t known_item_count = task == nullptr ? 0 : task->text.size();
        return known_item_count + static_cast<size_t>(std::distance(Categories.begin(), category));
    }
    return std::numeric_limits<size_t>::max();
}

struct SafeRewardSelection
{
    size_t detection_index = 0;
    size_t priority = std::numeric_limits<size_t>::max();
    DropOptionSelection option;
};

std::optional<SafeRewardSelection>
resolve_safe_reward_selection(const std::vector<BlackFlowTrophyRewardAnalyzer::Detection>& detections)
{
    const auto selected = std::ranges::min_element(detections, {}, rank_reward);
    const size_t priority = selected == detections.end() ? std::numeric_limits<size_t>::max()
                                                         : rank_reward(*selected);
    std::vector<Rect> buttons;
    buttons.reserve(detections.size());
    for (const auto& detection : detections) {
        buttons.emplace_back(detection.button_rect);
    }
    const auto option = selected == detections.end()
        ? std::nullopt
        : resolve_drop_option_selection(buttons, selected->button_rect);
    if (!trophy_reward_click_is_safe(
            detections.size(),
            option.has_value(),
            priority != std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }
    return SafeRewardSelection {
        static_cast<size_t>(std::distance(detections.begin(), selected)),
        priority,
        *option,
    };
}
} // namespace

bool BlackFlowTrophyRewardTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    return msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask" &&
           details.get("details", "task", std::string()) == RewardTask;
}

bool BlackFlowTrophyRewardTaskPlugin::_run()
{
    LogTraceFunction;

    const auto reject_unsafe = [this](std::string_view reason, size_t detection_count) {
        Log.warn(__FUNCTION__, "| trophy reward click skipped", reason, "detections", detection_count);
        record_run_event(
            RunLogLevel::Warning,
            "reward.trophy.skip-unsafe",
            "completed",
            "skipped",
            json::object {
                { "reason", std::string(reason) },
                { "detection_count", detection_count },
            },
            "BlackFlowTrophyReward",
            get_hit_image());
    };

    BlackFlowTrophyRewardAnalyzer initial_analyzer(ctrler()->get_image());
    if (!initial_analyzer.analyze()) {
        reject_unsafe("initial_semantic_analysis_failed", 0);
        return true;
    }
    const auto& initial_detections = initial_analyzer.get_detections();
    const auto initial_selection = resolve_safe_reward_selection(initial_detections);
    if (!initial_selection.has_value()) {
        reject_unsafe(
            "initial_selection_is_not_a_semantic_two_or_three_option_reward",
            initial_detections.size());
        return true;
    }

    std::string capture_error;
    if (m_port != nullptr &&
        !m_port->capture_get_drop(
            RewardTask,
            initial_detections[initial_selection->detection_index].button_rect,
            &capture_error)) {
        reject_unsafe("stable_capture_failed", initial_detections.size());
        Log.warn(__FUNCTION__, "| trophy reward stable capture failed", capture_error);
        return true;
    }

    // 证据采集会等待画面稳定；等待后必须在最新帧上重新做完整语义选择，绝不沿用
    // ProcessTask 或初次分析留下的按钮坐标。
    BlackFlowTrophyRewardAnalyzer confirmed_analyzer(ctrler()->get_image());
    if (!confirmed_analyzer.analyze()) {
        reject_unsafe("confirmed_semantic_analysis_failed", 0);
        return true;
    }
    const auto& detections = confirmed_analyzer.get_detections();
    const auto selection = resolve_safe_reward_selection(detections);
    if (!selection.has_value()) {
        reject_unsafe(
            "confirmed_selection_is_not_a_semantic_two_or_three_option_reward",
            detections.size());
        return true;
    }

    const auto& selected = detections[selection->detection_index];
    Log.info(
        __FUNCTION__,
        "| selecting confirmed trophy reward",
        selected.name,
        selected.category,
        "priority",
        selection->priority);
    record_run_event(
        RunLogLevel::Info,
        "reward.trophy.select",
        "started",
        "pending",
        json::object {
            { "name", selected.name },
            { "category", selected.category },
            { "priority", selection->priority },
            { "rect", json::array { selected.button_rect.x, selected.button_rect.y,
                                     selected.button_rect.width, selected.button_rect.height } },
            { "option_count", selection->option.option_count },
            { "selected_option_index_from_left", selection->option.selected_index_from_left },
        },
        "BlackFlowTrophyReward",
        nullptr,
        true);
    ctrler()->click(selected.button_rect);
    return true;
}
} // namespace asst::blackflow
