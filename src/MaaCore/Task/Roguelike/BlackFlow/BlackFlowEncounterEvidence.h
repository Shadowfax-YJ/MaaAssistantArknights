#pragma once

#include <algorithm>
#include <iterator>

#include "BlackFlowSession.h"
#include "BlackFlowTaskPort.h"

namespace asst::blackflow
{
// Choice receipts and sacrifice observations share the original page transaction.
inline bool record_encounter_evidence(
    BlackFlowSession& session,
    IBlackFlowTaskPort& port,
    std::string_view event_name,
    std::string_view phase,
    json::object details,
    const cv::Mat& image,
    std::string* error = nullptr)
{
    const auto failed = [&](std::string reason) {
        if (error) {
            *error = reason;
        }
        session.fail("encounter_evidence_failed", std::move(reason));
        return false;
    };
    const bool choice = details.get("capture_kind", std::string()) == "encounter_choice";
    constexpr std::string_view phases[] = { "candidates",   "order",        "planned",   "attempt",
                                            "input_failed", "click_issued", "confirmed", "unresolved" };
    if (choice && std::ranges::find(phases, phase) == std::end(phases)) {
        return failed("invalid encounter choice phase");
    }
    const auto& page = session.page_context();
    if (session.profile() != "automation_collection" || !page || page->stage != PageExecutionStage::Running ||
        is_combat_node_type(page->node_type) || page->node_name != event_name ||
        (event_name != "货从口出" && event_name != "回滚文明") ||
        details.get("run_revision", std::uint64_t(0)) != session.run_revision() ||
        details.get("page_revision", std::uint64_t(0)) != page->page_revision ||
        details.get("evidence_sequence", 0) <= 0 ||
        details.get(choice ? "choice_id" : "sacrifice_id", std::string()).empty()) {
        return failed("encounter evidence has no matching running event page or identity");
    }
    details["event_name"] = std::string(event_name);
    details["source_context"] = json::object {
        { "run_revision", session.run_revision() },
        { "page_revision", page->page_revision },
        { "map_generation", page->map_generation },
        { "floor", page->floor },
        { "node", page->node },
        { "transaction_id", page->transaction_id },
    };
    const bool required_image = !choice || phase == "candidates" || phase == "attempt" || phase == "confirmed";
    if (required_image && image.empty()) {
        return failed("required encounter screenshot is empty");
    }
    if (!image.empty()) {
        // Sacrifice keeps its existing phase names and layout for old consumers.
        const std::string capture_phase = !choice                 ? std::string(phase)
                                          : phase == "candidates" ? "cc-cand"
                                          : phase == "confirmed"  ? "cc-done"
                                                                  : "cc-try";
        if (!port.capture_event_detail(event_name, capture_phase, details, image, error)) {
            return failed(error && !error->empty() ? *error : "encounter image could not be saved");
        }
    }
    const bool unresolved = phase == "unresolved";
    const RunLogEvent event {
        .level = unresolved || phase == "input_failed" ? RunLogLevel::Warning : RunLogLevel::Info,
        .action = (choice ? "node.encounter.choice." : "node.sacrifice.") + std::string(phase),
        .phase = std::string(phase),
        .outcome = phase == "confirmed" ? "confirmed"
                   : unresolved         ? "unknown"
                                        : "pending",
        .task = "RoguelikeStageEncounterTaskPlugin",
        .transaction_id = page->transaction_id,
        .state = session.run_log_state(),
        .details = std::move(details),
    };
    if (!port.record_run_event(session.run_revision(), event, nullptr, false, error)) {
        return failed(error && !error->empty() ? *error : "encounter event could not be saved");
    }
    if (unresolved) {
        session.fail("encounter_choice_unresolved", "event choice was not confirmed");
    }
    return true;
}
}
