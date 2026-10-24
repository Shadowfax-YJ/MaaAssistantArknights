#include "BlackFlowCollectionPopupTaskPlugin.h"

#include <utility>

#include "BlackFlowCollectionPopup.h"

#include "Controller/Controller.h"
#include "Utils/Logger.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"

namespace asst::blackflow
{
BlackFlowCollectionPopupTaskPlugin::BlackFlowCollectionPopupTaskPlugin(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain,
    Capture capture,
    Failure failure) :
    AbstractTaskPlugin(callback, inst, task_chain),
    m_capture(std::move(capture)),
    m_failure(std::move(failure))
{
    set_block(true);
    set_may_change_ui(false);
    set_retry_times(0);
}

bool BlackFlowCollectionPopupTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string task = details.get("details", "task", std::string());
    if (!collection_popup_button(task).has_value()) {
        return false;
    }
    m_pending_task = task;
    return true;
}

bool BlackFlowCollectionPopupTaskPlugin::_run()
{
    if (m_pending_task.empty()) {
        return true;
    }
    const std::string task = std::exchange(m_pending_task, {});
    auto* process = dynamic_cast<ProcessTask*>(m_task_ptr);
    std::string error;
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        if (m_capture && m_capture(task, &error)) {
            if (process != nullptr) {
                process->set_skip_current_action(false);
            }
            return true;
        }
        if (!m_capture) {
            error = "collection popup capture callback is missing";
        }
        // Returning false from a plugin does not veto ProcessTask's action.
        // Explicitly suppress the old hit until its evidence is safely stored.
        if (process != nullptr) {
            process->set_skip_current_action();
        }
        const cv::Mat current = ctrler()->get_image();
        if (!current.empty()) {
            PipelineAnalyzer popup(current);
            popup.set_tasks({ task });
            if (!popup.analyze().has_value()) {
                Log.info("BlackFlow popup changed before capture; resuming page dispatch", task);
                return true;
            }
        }
        if (attempt < 2 && !sleep(200)) {
            break;
        }
    }
    if (process != nullptr) {
        process->set_skip_current_action();
    }
    if (need_exit()) {
        return true;
    }
    Log.error("BlackFlow collection popup capture failed; close input suppressed", task, error);
    if (m_failure) {
        m_failure(task, error);
    }
    if (m_task_ptr != nullptr) {
        m_task_ptr->set_enable(false);
    }
    return true;
}
} // namespace asst::blackflow
