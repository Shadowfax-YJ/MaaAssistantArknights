#include "BlackFlowTaskPluginBase.h"

#include "Utils/Logger.hpp"

namespace asst::blackflow
{
BlackFlowTaskPluginBase::BlackFlowTaskPluginBase(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain,
    const std::shared_ptr<RoguelikeConfig>& config,
    const std::shared_ptr<RoguelikeControlTaskPlugin>& control,
    std::shared_ptr<BlackFlowSession> session,
    std::shared_ptr<IBlackFlowTaskPort> port) :
    AbstractRoguelikeTaskPlugin(callback, inst, task_chain, config, control),
    m_session(std::move(session)),
    m_port(std::move(port))
{
    set_block(true);
}

bool BlackFlowTaskPluginBase::load_params([[maybe_unused]] const json::value& params)
{
    if (m_config->get_theme() != RoguelikeTheme::BlackFlow || m_session == nullptr) {
        return false;
    }
    m_session->set_difficulty(m_config->get_difficulty());
    return true;
}

void BlackFlowTaskPluginBase::report_outputs()
{
    if (m_session == nullptr) {
        return;
    }
    if (m_port != nullptr) {
        for (const NodeAttributionRecord& record : m_session->take_node_attribution_records()) {
            std::string error;
            if (!m_port->record_node_attribution(
                    m_session->run_revision(),
                    record.floor,
                    record.node,
                    record.virtual_node_name,
                    record.attribution,
                    &error)) {
                Log.warn(
                    "BlackFlow node attribution persistence failed",
                    record.attribution,
                    error);
            }
        }
    }
    for (auto& event : m_session->take_telemetry_events()) {
        const RunLogLevel level = event.what.find("Warning") != std::string::npos ? RunLogLevel::Warning
                                                                                  : RunLogLevel::Info;
        record_run_event(
            level,
            "telemetry." + event.what,
            "observed",
            "success",
            event.details,
            event.what);
        auto info = basic_info_with_what(event.what);
        info["details"] = std::move(event.details);
        callback(AsstMsg::SubTaskExtraInfo, info);
    }
    if (m_port != nullptr) {
        for (const auto& request : m_session->take_diagnostic_requests()) {
            std::string error;
            if (!m_port->persist_diagnostics(request, &error)) {
                Log.warn("BlackFlow diagnostic artifact persistence failed:", error);
            }
        }
    }
    if (m_session->claim_result_report()) {
        auto info = basic_info_with_what("BlackFlowStrategyResult");
        info["details"] = m_session->result()->to_json();
        callback(AsstMsg::SubTaskExtraInfo, info);
    }
}

bool BlackFlowTaskPluginBase::run_logging_enabled() const noexcept
{
    return m_session != nullptr && m_port != nullptr && m_session->profile() == "automation_collection";
}

bool BlackFlowTaskPluginBase::record_run_event(
    RunLogLevel level,
    std::string action,
    std::string phase,
    std::string outcome,
    json::object details,
    std::string task,
    std::shared_ptr<cv::Mat> image,
    bool capture_image) const
{
    if (!run_logging_enabled()) {
        return true;
    }
    json::object state = m_session->run_log_state();
    const RunLogEvent event {
        .level = level,
        .action = std::move(action),
        .phase = std::move(phase),
        .outcome = std::move(outcome),
        .task = std::move(task),
        .transaction_id = state.get("transaction_id", std::string()),
        .state = std::move(state),
        .details = std::move(details),
    };
    std::string error;
    if (!m_port->record_run_event(
            m_session->run_revision(),
            event,
            std::move(image),
            capture_image,
            &error)) {
        Log.error("BlackFlow run log event persistence failed", event.action, error);
        return false;
    }
    return true;
}
} // namespace asst::blackflow
