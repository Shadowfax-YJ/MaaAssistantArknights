#include "RoguelikeDebugTaskPlugin.h"

#include <string_view>
#include <utility>

#include "Controller/Controller.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Utils/Logger.hpp"

namespace
{
std::string default_data_collection_abandon_reason(bool on_error, std::string_view task)
{
    if (on_error) {
        return "subtask_error";
    }
    if (task == "RoguelikeControlTaskPlugin-ExitThenStop") {
        return "exit_then_stop";
    }
    if (task == "Roguelike@Abandon") {
        return "abandon_confirm";
    }
    if (task == "Roguelike@ExitThenAbandon_ToHardest") {
        return "switch_to_hardest";
    }
    return "abandon_flow";
}
}

bool asst::RoguelikeDebugTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    m_start_data_collection_run = false;
    m_finish_data_collection_run = false;
    m_finish_data_collection_run_on_error = false;
    m_data_collection_finish_task.clear();

    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (msg == AsstMsg::SubTaskError) {
        m_finish_data_collection_run = true;
        m_finish_data_collection_run_on_error = true;
        m_data_collection_finish_task = details.get("details", "task", "");
        return true;
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
    if (msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask") {
        if (task_view == "Roguelike@StartExplore") {
            m_start_data_collection_run = true;
            return true;
        }
        if (task_view == "Roguelike@ExitThenAbandon" || task_view == "Roguelike@ExitThenAbandon_ToHardest" ||
            task_view == "Roguelike@Abandon" || task_view == "RoguelikeControlTaskPlugin-ExitThenStop") {
            m_finish_data_collection_run = true;
            m_data_collection_finish_task = task_view;
            return true;
        }
        if (task_view == "Roguelike@GamePass") {
            return true;
        }
    }

    return false;
}

bool asst::RoguelikeDebugTaskPlugin::_run()
{
    if (m_start_data_collection_run) {
        RoguelikeDataCollector.start_run_if_enabled();
    }
    if (m_finish_data_collection_run) {
        json::object details { { "task", m_data_collection_finish_task } };
        if (m_finish_data_collection_run_on_error) {
            details["trigger"] = "subtask_error";
        }
        RoguelikeDataCollector.finish_run_as_abandoned(
            default_data_collection_abandon_reason(m_finish_data_collection_run_on_error, m_data_collection_finish_task),
            ctrler()->get_image(),
            std::move(details));
    }
    save_img(utils::path("debug") / utils::path("roguelike"));
    return true;
}
