#pragma once

#include <algorithm>
#include <iterator>
#include <unordered_set>

#include "BlackFlowSession.h"
#include "BlackFlowTaskPort.h"
#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"

namespace asst::blackflow
{
inline bool configured_encounter_next(std::string_view from, std::string_view to)
{
    const auto next = RoguelikeStageEncounter.next_event_names(
        RoguelikeTheme::BlackFlow,
        RoguelikeMode::BlackFlowAutomationCollection,
        from);
    return std::ranges::find(next, to) != next.end();
}

inline bool configured_encounter_chain(std::string_view root, std::string_view target)
{
    std::vector<std::string> pending { std::string(root) };
    std::unordered_set<std::string> seen;
    while (!pending.empty()) {
        std::string current = std::move(pending.back());
        pending.pop_back();
        if (!seen.emplace(current).second) {
            continue;
        }
        for (auto& next : RoguelikeStageEncounter.next_event_names(
                 RoguelikeTheme::BlackFlow,
                 RoguelikeMode::BlackFlowAutomationCollection,
                 current)) {
            if (next == target) {
                return true;
            }
            pending.emplace_back(std::move(next));
        }
    }
    return false;
}

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
    constexpr std::string_view phases[] = { "candidates",      "order",         "planned",   "attempt",
                                            "input_failed",    "click_issued",  "confirmed", "unresolved",
                                            "result_observed", "result_pending" };
    if (choice && std::ranges::find(phases, phase) == std::end(phases)) {
        return failed("invalid encounter choice phase");
    }
    const auto& page = session.page_context();
    const bool next_page_result =
        choice &&
        ((phase == "confirmed" && details.get("confirmation_basis", std::string()) == "next_event_candidate_page") ||
         (phase == "result_observed" && details.get("result_basis", std::string()) == "next_event_candidate_page"));
    if (next_page_result && !configured_encounter_next(event_name, details.get("result_event_name", std::string()))) {
        return failed("encounter result is not a configured next event");
    }
    const bool next_candidate_receipt =
        choice && page && details.get("result_event_name", std::string()) == page->node_name && next_page_result;
    const bool chained_choice = choice && page && details.get("choice_step", 0) > 0 &&
                                !details.get("parent_choice_id", std::string()).empty() &&
                                configured_encounter_chain(page->node_name, event_name);
    if (session.profile() != "automation_collection" || !page || page->stage != PageExecutionStage::Running ||
        is_combat_node_type(page->node_type) ||
        (page->node_name != event_name && !next_candidate_receipt && !chained_choice) ||
        (!choice && event_name != "回滚文明") ||
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
    const bool required_image =
        !choice || phase == "candidates" || phase == "attempt" || phase == "confirmed" || phase == "result_observed";
    if (required_image && image.empty()) {
        return failed("required encounter screenshot is empty");
    }
    if (!image.empty()) {
        // Sacrifice keeps its existing phase names and layout for old consumers.
        const std::string capture_phase = !choice                      ? std::string(phase)
                                          : phase == "candidates"      ? "cc-cand"
                                          : phase == "confirmed"       ? "cc-done"
                                          : phase == "result_observed" ? "cc-result"
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
