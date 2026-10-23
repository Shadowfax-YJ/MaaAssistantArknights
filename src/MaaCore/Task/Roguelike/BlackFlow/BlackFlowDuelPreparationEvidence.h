#pragma once

#include "BlackFlowSession.h"
#include "BlackFlowTaskPort.h"
#include "MaaUtils/NoWarningCVMat.hpp"

namespace asst::blackflow
{
inline bool record_duel_preparation_evidence(
    BlackFlowSession& session,
    IBlackFlowTaskPort& port,
    std::string_view phase,
    json::object details,
    const cv::Mat& image,
    std::string* error = nullptr)
{
    const auto& page = session.page_context();
    if (session.profile() != "automation_collection" || !page.has_value() ||
        page->stage != PageExecutionStage::Running || page->node_type != NodeType::Duel) {
        if (error != nullptr) {
            *error = "duel preparation has no running collection duel page";
        }
        return false;
    }
    if (phase == "before-start" && !image.empty()) {
        details["capture_kind"] = "duel_preparation_before_start";
        return port.capture_event_detail(page->node_name, "duel-before-start", std::move(details), image, error);
    }
    if (phase != "selected") {
        if (error != nullptr) {
            *error = "duel preparation evidence phase or screenshot is invalid";
        }
        return false;
    }
    json::object state = session.run_log_state();
    const RunLogEvent event {
        .action = "node.battle.duel.select",
        .phase = "selected",
        .outcome = "planned",
        .task = "RoguelikeBattleTaskPlugin",
        .transaction_id = state.get("transaction_id", std::string()),
        .state = std::move(state),
        .details = std::move(details),
    };
    return port.record_run_event(session.run_revision(), event, nullptr, false, error);
}
}
