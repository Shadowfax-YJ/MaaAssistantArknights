#pragma once

#include <algorithm>
#include <iterator>
#include <utility>

#include "BlackFlowExpeditionRules.h"
#include "BlackFlowSession.h"
#include "BlackFlowTaskPort.h"
#include "MaaUtils/NoWarningCVMat.hpp"

namespace asst::blackflow
{
inline bool record_expedition_evidence(
    BlackFlowSession& session,
    IBlackFlowTaskPort& port,
    std::string_view phase,
    json::object details,
    const cv::Mat& image,
    std::string* error = nullptr)
{
    const auto failed = [&](std::string reason) {
        if (error != nullptr) {
            *error = reason;
        }
        session.fail("expedition_evidence_failed", std::move(reason));
        return false;
    };
    constexpr std::string_view phases[] = {
        "candidates",          "choice_planned",      "choice_attempt",
        "choice_input_failed", "choice_click_issued", "choice_confirmed",
        "dispatch_prepared",   "dispatch_attempt",    "dispatch_click_issued",
        "secondary_event",     "dispatch_confirmed",  "transition_before_advance",
        "cancelled",           "unresolved",
    };
    if (std::ranges::find(phases, phase) == std::end(phases) || details.get("expedition_id", std::string()).empty() ||
        details.get("expedition_phase", std::string()) != phase || details.get("evidence_sequence", 0) <= 0) {
        return failed("expedition evidence phase or identity is invalid");
    }
    const auto& page = session.page_context();
    if (session.profile() != "automation_collection" || !page.has_value() ||
        page->stage != PageExecutionStage::Running || page->floor != 2 || is_combat_node_type(page->node_type) ||
        page->node_name != ExpeditionEventName ||
        details.get("run_revision", std::uint64_t(0)) != session.run_revision() ||
        details.get("page_revision", std::uint64_t(0)) != page->page_revision) {
        return failed("expedition evidence has no matching running expedition page");
    }
    details["event_name"] = std::string(ExpeditionEventName);
    details["source_context"] = json::object {
        { "run_revision", session.run_revision() },
        { "map_generation", page->map_generation },
        { "page_revision", page->page_revision },
        { "floor", page->floor },
        { "node", page->node },
        { "transaction_id", page->transaction_id },
    };
    if (!image.empty()) {
        details["capture_kind"] = "expedition_evidence";
        // Keep stems no longer than existing event captures, including deeply nested Windows instances.
        const std::string_view capture_phase = phase == "candidates"           ? "exp-cand"
                                               : phase == "dispatch_prepared"  ? "exp-prep"
                                               : phase == "dispatch_confirmed" ? "exp-done"
                                               : phase == "secondary_event"    ? "exp-next"
                                                                               : "exp-step";
        if (!port.capture_event_detail(ExpeditionEventName, capture_phase, details, image, error)) {
            return failed(error != nullptr && !error->empty() ? *error : "expedition image could not be saved");
        }
    }
    else if (
        phase == "candidates" || phase == "dispatch_prepared" || phase == "secondary_event" ||
        phase == "dispatch_confirmed" || phase == "transition_before_advance") {
        return failed("required expedition screenshot is empty");
    }
    json::object state = session.run_log_state();
    const std::string unresolved_reason = details.get("reason", std::string("dispatch confirmation is unresolved"));
    const RunLogEvent event {
        .level = phase == "unresolved" || phase == "choice_input_failed" ? RunLogLevel::Warning : RunLogLevel::Info,
        .action = "node.expedition." + std::string(phase),
        .phase = std::string(phase),
        .outcome = phase == "dispatch_confirmed" || phase == "choice_confirmed" ? "confirmed"
                   : phase == "unresolved"                                      ? "unknown"
                   : phase == "cancelled"                                       ? "not_dispatched"
                   : phase == "choice_input_failed"                             ? "failed"
                                                                                : "pending",
        .task = "RoguelikeStageEncounterTaskPlugin",
        .transaction_id = page->transaction_id,
        .state = std::move(state),
        .details = std::move(details),
    };
    if (!port.record_run_event(session.run_revision(), event, nullptr, false, error)) {
        return failed(error != nullptr && !error->empty() ? *error : "expedition event could not be saved");
    }
    if (phase == "unresolved") {
        session.fail("expedition_unresolved", unresolved_reason);
    }
    return true;
}
}
