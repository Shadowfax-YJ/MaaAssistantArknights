#include "BlackFlowTrophyRewardAnalyzer.h"

#include <string_view>

#include "Config/TaskData.h"
#include "Utils/Logger.hpp"
#include "Vision/MultiMatcher.h"
#include "Vision/RegionOCRer.h"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view ButtonTask = "BlackFlow@Roguelike@GetDropTrophyReward";
constexpr std::string_view NameTask = "BlackFlow@Roguelike@TrophyRewardNameOCR";
constexpr std::string_view CategoryTask = "BlackFlow@Roguelike@TrophyRewardCategoryOCR";
constexpr std::string_view PriorityTask = "BlackFlow@Roguelike@TrophyRewardPriority";
} // namespace

bool BlackFlowTrophyRewardAnalyzer::analyze()
{
    m_detections.clear();

    const auto name_task = Task.get<OcrTaskInfo>(std::string(NameTask));
    const auto category_task = Task.get<OcrTaskInfo>(std::string(CategoryTask));
    const auto priority_task = Task.get<OcrTaskInfo>(std::string(PriorityTask));
    if (name_task == nullptr || category_task == nullptr || priority_task == nullptr) {
        Log.error(__FUNCTION__, "| trophy reward OCR task is missing");
        return false;
    }

    MultiMatcher button_matcher(m_image);
    button_matcher.set_task_info(std::string(ButtonTask));
    auto buttons = button_matcher.analyze();
    if (!buttons.has_value() || buttons->empty()) {
        Log.warn(__FUNCTION__, "| no trophy reward button recognized");
        return false;
    }
    sort_by_horizontal_(*buttons);

    m_detections.reserve(buttons->size());
    for (const auto& button : *buttons) {
        Detection detection;
        detection.button_rect = button.rect;

        RegionOCRer name_ocr(m_image);
        name_ocr.set_task_info(name_task);
        name_ocr.set_required(priority_task->text);
        name_ocr.set_roi(button.rect.move(name_task->roi));
        if (const auto result = name_ocr.analyze(); result.has_value()) {
            detection.name_rect = result->rect;
            detection.name = result->text;
            detection.name_score = result->score;
        }

        RegionOCRer category_ocr(m_image);
        category_ocr.set_task_info(category_task);
        category_ocr.set_roi(button.rect.move(category_task->roi));
        if (const auto result = category_ocr.analyze(); result.has_value()) {
            detection.category = result->text;
            detection.category_score = result->score;
        }

        Log.info(
            __FUNCTION__,
            "| trophy reward detected",
            detection.name,
            detection.category,
            detection.button_rect);
        m_detections.emplace_back(std::move(detection));
    }

    return !m_detections.empty();
}
} // namespace asst::blackflow
