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
constexpr std::string_view RewardTask = "BlackFlow@Roguelike@GetDropTrophyReward";
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
} // namespace

bool BlackFlowTrophyRewardTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    return msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask" &&
           details.get("details", "task", std::string()) == RewardTask;
}

bool BlackFlowTrophyRewardTaskPlugin::_run()
{
    LogTraceFunction;

    BlackFlowTrophyRewardAnalyzer analyzer(ctrler()->get_image());
    if (analyzer.analyze()) {
        const auto& detections = analyzer.get_detections();
        const auto selected = std::ranges::min_element(detections, {}, rank_reward);
        if (selected != detections.end()) {
            const size_t rank = rank_reward(*selected);
            std::vector<Rect> option_buttons;
            option_buttons.reserve(detections.size());
            for (const auto& detection : detections) {
                option_buttons.emplace_back(detection.button_rect);
            }
            const auto selection = resolve_drop_option_selection(option_buttons, selected->button_rect);
            Log.info(
                __FUNCTION__,
                "| selecting trophy reward",
                selected->name,
                selected->category,
                "priority",
                rank);
            json::object details {
                { "name", selected->name },
                { "category", selected->category },
                { "priority", rank },
                { "rect", json::array { selected->button_rect.x, selected->button_rect.y,
                                         selected->button_rect.width, selected->button_rect.height } },
            };
            if (selection.has_value()) {
                details["option_count"] = selection->option_count;
                details["selected_option_index_from_left"] = selection->selected_index_from_left;
            }
            std::string capture_error;
            if (m_port != nullptr && !m_port->capture_get_drop(RewardTask, selected->button_rect, &capture_error)) {
                // 证据采集失败不能阻塞奖励领取；实际选择仍继续执行。
                Log.warn(__FUNCTION__, "| trophy reward node capture failed", capture_error);
            }
            record_run_event(
                RunLogLevel::Info,
                "reward.trophy.select",
                "started",
                "pending",
                std::move(details),
                "BlackFlowTrophyReward",
                nullptr,
                true);
            ctrler()->click(selected->button_rect);
            return true;
        }
    }

    // ProcessTask 已经用同一按钮模板命中了当前页面；多目标识别异常时保留原来的单目标点击行为。
    if (const auto hit = get_hit_detail<Matcher::Result>(); hit != nullptr) {
        Log.warn(__FUNCTION__, "| trophy reward OCR failed, clicking matcher fallback", hit->rect);
        std::string capture_error;
        if (m_port != nullptr && !m_port->capture_get_drop(RewardTask, hit->rect, &capture_error)) {
            Log.warn(__FUNCTION__, "| trophy reward fallback node capture failed", capture_error);
        }
        record_run_event(
            RunLogLevel::Warning,
            "reward.trophy.select-fallback",
            "started",
            "pending",
            json::object { { "rect", json::array { hit->rect.x, hit->rect.y, hit->rect.width, hit->rect.height } } },
            "BlackFlowTrophyReward",
            get_hit_image());
        ctrler()->click(hit->rect);
        return true;
    }

    Log.error(__FUNCTION__, "| trophy reward selection has no clickable fallback");
    return false;
}
} // namespace asst::blackflow
