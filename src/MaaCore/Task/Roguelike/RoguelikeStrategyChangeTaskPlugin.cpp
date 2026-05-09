#include "RoguelikeStrategyChangeTaskPlugin.h"

#include <utility>

#include "Config/TaskData.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Utils/Logger.hpp"

namespace
{
std::string jiegarden_data_collection_strategy(std::string_view floor)
{
    if (floor == "洪陆楼" || floor == "山水阁") {
        return "_dataCollection";
    }
    if (floor == "云瓦亭" || floor == "汝吾门" || floor == "见字祠" || floor == "始末陵") {
        return "_exit";
    }
    if (floor == "是非境") {
        return "_boskyPassageDefault";
    }
    return {};
}
}

bool asst::RoguelikeStrategyChangeTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (!RoguelikeConfig::is_valid_theme(m_config->get_theme())) {
        Log.error("Roguelike name doesn't exist!");
        return false;
    }
    const std::string roguelike_name = m_config->get_theme() + "@";
    const std::string& task = details.get("details", "task", "");
    std::string_view task_view = task;
    if (task_view.starts_with(roguelike_name)) {
        task_view.remove_prefix(roguelike_name.length());
    }
    if (task_view == "Roguelike@StrategyChange") {
        m_result = details.get("details", "result", json::object());
        return true;
    }
    else {
        return false;
    }
}

bool asst::RoguelikeStrategyChangeTaskPlugin::_run()
{
    LogTraceFunction;

    const std::string theme = m_config->get_theme();
    const std::string stages_task_name = theme + "@Roguelike@Stages";
    const std::string ocr_text = m_result.get("text", "");
    std::string current_strategy = ocr_text;
    std::string current_floor;
    if (theme == RoguelikeTheme::JieGarden && m_config->get_mode() == RoguelikeMode::DataCollection) {
        current_floor = RoguelikeDataCollection::normalize_jiegarden_floor_name(ocr_text);
        if (!current_floor.empty()) {
            current_strategy = jiegarden_data_collection_strategy(current_floor);
        }
        else if (!ocr_text.starts_with("_")) {
            current_strategy.clear();
        }
    }

    if (current_strategy.empty() || current_strategy.find("_SKIP_") != std::string::npos) {
        Log.info("Skip strategy change, OCR text is", ocr_text, "current floor is", current_floor);
        return true;
    }
    const std::string strategy_task_name = stages_task_name + current_strategy;

    if (Task.get(strategy_task_name) == nullptr) [[unlikely]] {
        Log.error("Strategy task", strategy_task_name, "doesn't exist!");
        return false;
    }

    if (m_config->get_mode() == RoguelikeMode::DataCollection) {
        if (!current_floor.empty()) {
            RoguelikeDataCollector.note_floor_ocr(current_floor);
        }
        else {
            RoguelikeDataCollector.note_strategy_change(current_strategy);
        }
        json::object details {
            { "strategy", current_strategy },
            { "floor", current_floor },
            { "ocr_text", ocr_text },
            { "stages_task", strategy_task_name },
        };
        RoguelikeDataCollector.log_event(current_strategy == "_exit" ? "floor_exit" : "strategy_change", details);
        if (current_strategy == "_exit") {
            RoguelikeDataCollector.set_pending_abandon_reason("floor_exit", std::move(details));
        }
    }

    Task.set_task_base(stages_task_name, strategy_task_name);

    return true;
}
