#include "BlackFlowRunLogTaskPlugin.h"

namespace asst::blackflow
{
bool BlackFlowRunLogTaskPlugin::load_params(const json::value& params)
{
    if (!BlackFlowTaskPluginBase::load_params(params)) {
        return false;
    }
    set_block(false);
    return m_session != nullptr && m_config->get_mode() == RoguelikeMode::BlackFlowAutomationCollection;
}

bool BlackFlowRunLogTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    if (msg != AsstMsg::SubTaskStart && msg != AsstMsg::SubTaskCompleted && msg != AsstMsg::SubTaskError &&
        msg != AsstMsg::SubTaskStopped) {
        return false;
    }
    if (details.get("details", "task", std::string()).empty()) {
        return false;
    }
    m_pending_message = msg;
    m_pending_details = details;
    return true;
}

bool BlackFlowRunLogTaskPlugin::_run()
{
    const std::string task = m_pending_details.get("details", "task", std::string());
    const std::string process_action = m_pending_details.get("details", "action", std::string("DoNothing"));
    std::string phase;
    std::string outcome;
    RunLogLevel level = process_action_changes_ui(process_action) ? RunLogLevel::Info : RunLogLevel::Trace;
    bool capture_after = false;
    std::shared_ptr<cv::Mat> hit_image;
    if (m_pending_message == AsstMsg::SubTaskStart) {
        phase = "started";
        outcome = "pending";
        hit_image = get_hit_image();
    }
    else if (m_pending_message == AsstMsg::SubTaskCompleted) {
        phase = "completed";
        outcome = "success";
        capture_after = process_action_changes_ui(process_action);
    }
    else if (m_pending_message == AsstMsg::SubTaskStopped) {
        phase = "stopped";
        outcome = "interrupted";
        level = RunLogLevel::Warning;
        capture_after = true;
    }
    else {
        phase = "failed";
        outcome = "error";
        level = RunLogLevel::Error;
        capture_after = true;
    }
    json::object details {
        { "process_action", process_action },
        { "callback", m_pending_details },
    };
    record_run_event(
        level,
        "process." + task,
        std::move(phase),
        std::move(outcome),
        std::move(details),
        task,
        std::move(hit_image),
        capture_after);
    m_pending_details = {};
    return true;
}
} // namespace asst::blackflow
