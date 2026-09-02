#pragma once

#include <string>
#include <utility>

#include "BlackFlowRevealSemantics.h"
#include "BlackFlowTaskPort.h"

#include "Utils/Logger.hpp"

namespace asst::blackflow
{
enum class RoutingCycleStatus
{
    MoveCommitted,
    MoveCommittedToMap,
    MovementSelectionRequired,
    MovementInventoryObservationRequired,
    DirectExhaustionRequired,
    ReplanRequired,
    PreviewNeedsDismiss,
    SessionTerminated,
    NeedsPageRecovery,
    Failed,
};

struct RoutingCycleOutcome
{
    RoutingCycleStatus status = RoutingCycleStatus::Failed;
    std::string failure_code;
    std::string error;
};

template <typename Session>
void record_routing_run_event(
    Session& session,
    IBlackFlowTaskPort& port,
    RunLogLevel level,
    std::string action,
    std::string phase,
    std::string outcome,
    json::object details = {},
    bool capture_image = false)
{
    if constexpr (requires {
                      session.profile();
                      session.run_revision();
                      session.run_log_state();
                  }) {
        if (session.profile() != "automation_collection") {
            return;
        }
        json::object state = session.run_log_state();
        const RunLogEvent event {
            .level = level,
            .action = std::move(action),
            .phase = std::move(phase),
            .outcome = std::move(outcome),
            .task = "BlackFlowRouting",
            .transaction_id = state.get("transaction_id", std::string()),
            .state = std::move(state),
            .details = std::move(details),
        };
        std::string ignored_error;
        port.record_run_event(session.run_revision(), event, nullptr, capture_image, &ignored_error);
    }
}

template <typename Session>
bool refresh_with_retries(Session& session, IBlackFlowTaskPort& port, std::string* error)
{
    const std::optional<int> current_floor = session.current_floor();
    if (!current_floor.has_value()) {
        if (error != nullptr) {
            *error = "current floor has not been recognized by NextLevel";
        }
        return false;
    }

    bool viewport_already_normalized = false;
    if constexpr (requires { session.consume_viewport_preserved_after_inventory(); }) {
        viewport_already_normalized = session.consume_viewport_preserved_after_inventory();
    }

    std::string latest_error;
    for (int attempt = 0; attempt < TransientRevealObservationMaximumAttempts; ++attempt) {
        BlackFlowObservationRequest request;
        request.floor = *current_floor;
        request.viewport_already_normalized = viewport_already_normalized;
        if constexpr (requires { session.difficulty(); }) {
            request.difficulty = session.difficulty();
        }
        if constexpr (requires { session.map_generation(); }) {
            request.map_generation = session.map_generation();
            request.inspect_utopia = true;
        }
        request.attempt_count = attempt + 1;
        record_routing_run_event(
            session,
            port,
            RunLogLevel::Info,
            "map.refresh",
            "started",
            "pending",
            json::object { { "floor", request.floor }, { "attempt", attempt + 1 } });
        BlackFlowPerceptionSnapshot snapshot;
        std::string current_error;
        if (port.refresh(request, snapshot, &current_error)) {
            // refresh() 已经完成了本层需要的视口归一化；因短暂 OCR 占位补抓后续帧时，
            // 尤其是五层，不能再重复执行左滑定位。
            viewport_already_normalized = true;
            bool retry_transient_reveal = false;
            if constexpr (requires { session.should_retry_initial_reveal_observation(snapshot); }) {
                retry_transient_reveal = attempt + 1 < TransientRevealObservationMaximumAttempts &&
                                         session.should_retry_initial_reveal_observation(snapshot);
            }
            if constexpr (requires { session.should_retry_post_move_reveal_observation(snapshot); }) {
                retry_transient_reveal = retry_transient_reveal ||
                                         (attempt + 1 < TransientRevealObservationMaximumAttempts &&
                                          session.should_retry_post_move_reveal_observation(snapshot));
            }
            if (retry_transient_reveal) {
                Log.info(
                    "BlackFlow retries a transient reveal observation with topology-only empty nodes",
                    "floor",
                    request.floor,
                    "attempt",
                    attempt + 1,
                    "of",
                    TransientRevealObservationMaximumAttempts);
                latest_error = "reveal mismatch includes a topology-only empty OCR fallback";
                continue;
            }
            if (session.update(snapshot, &current_error)) {
                std::string popup_error;
                if (!port.resolve_pending_collection_popups(&popup_error)) {
                    Log.warn("BlackFlow pending collection popup attribution failed after map refresh", popup_error);
                }
                record_routing_run_event(
                    session,
                    port,
                    RunLogLevel::Info,
                    "map.refresh",
                    "completed",
                    "success",
                    json::object { { "floor", request.floor }, { "attempt", attempt + 1 } },
                    true);
                return true;
            }
        }
        const auto* failure = current_error.empty() ? "unknown" : current_error.c_str();
        if (attempt + 1 < TransientRevealObservationMaximumAttempts) {
            Log.info(
                "BlackFlow map rebuild attempt failed",
                "floor",
                request.floor,
                "attempt",
                attempt + 1,
                "of",
                TransientRevealObservationMaximumAttempts,
                "error",
                failure);
        }
        else {
            Log.debug(
                "BlackFlow map rebuild attempt failed",
                "floor",
                request.floor,
                "attempt",
                attempt + 1,
                "of",
                TransientRevealObservationMaximumAttempts,
                "error",
                failure);
        }
        if (!current_error.empty()) {
            latest_error = std::move(current_error);
        }
        record_routing_run_event(
            session,
            port,
            attempt + 1 < TransientRevealObservationMaximumAttempts ? RunLogLevel::Warning : RunLogLevel::Error,
            "map.refresh",
            "failed",
            "error",
            json::object {
                { "floor", request.floor },
                { "attempt", attempt + 1 },
                { "error", latest_error },
            },
            true);
    }
    if (error != nullptr) {
        *error = latest_error.empty() ? "map rebuild exhausted its retry budget" : std::move(latest_error);
    }
    return false;
}

template <typename Session>
bool session_requires_movement_inventory_observation(const Session& session)
{
    if constexpr (requires { session.movement_inventory_refresh_required(); }) {
        return session.movement_inventory_refresh_required();
    }
    else {
        return false;
    }
}

template <typename Session>
bool session_requires_map_settle(Session& session)
{
    if constexpr (requires {
                      session.map_settle_required();
                      session.mark_map_settled();
                  }) {
        if (!session.map_settle_required()) {
            return false;
        }
        session.mark_map_settled();
        return true;
    }
    else {
        return false;
    }
}

template <typename Session>
bool validate_session_commit(Session& session, std::string* error)
{
    if constexpr (requires { session.validate_commit(error); }) {
        return session.validate_commit(error);
    }
    else {
        return true;
    }
}

template <typename Session>
bool session_requires_page_dispatch(const Session& session)
{
    if constexpr (requires { session.page_dispatch_required(); }) {
        return session.page_dispatch_required();
    }
    else {
        return true;
    }
}

template <typename Session>
void invalidate_session_movement_inventory(Session& session)
{
    if constexpr (requires { session.invalidate_movement_inventory(); }) {
        session.invalidate_movement_inventory();
    }
}

template <typename Session>
RoutingCycleOutcome execute_preview_cycle(Session& session, IBlackFlowTaskPort& port)
{
    std::string error;
    if (session.transaction() == nullptr) {
        return { RoutingCycleStatus::Failed,
                 "transaction_proposal_failed",
                 "move preview has no proposed transaction" };
    }

    MovePreview preview;
    bool panel_open = false;
    const MoveCandidate candidate = session.transaction()->proposal();
    record_routing_run_event(
        session,
        port,
        RunLogLevel::Info,
        "move.preview",
        "started",
        "pending",
        json::object {
            { "action_id", candidate.action_id },
            { "movement", std::string(to_string(candidate.movement)) },
            { "source", candidate.source },
            { "target", candidate.target },
            { "landing", candidate.landing },
        });
    if (!port.preview(candidate, session.viewport(), preview, panel_open, &error)) {
        record_routing_run_event(
            session,
            port,
            RunLogLevel::Warning,
            "move.preview",
            "failed",
            "error",
            json::object { { "panel_open", panel_open }, { "error", error } },
            true);
        session.cancel_transaction();
        if (panel_open) {
            return { RoutingCycleStatus::PreviewNeedsDismiss, "move_preview_failed", std::move(error) };
        }
        // 没有打开预览通常表示地图坐标已过期或目标暂时不可点击。回到地图重建后重规划，
        // 不能把一次可恢复的 UI 失配升级为放弃整局。
        return { RoutingCycleStatus::ReplanRequired, "move_preview_failed", std::move(error) };
    }
    const PreviewDisposition disposition = session.accept_preview(std::move(preview), &error);
    record_routing_run_event(
        session,
        port,
        disposition == PreviewDisposition::ReadyToCommit ? RunLogLevel::Info : RunLogLevel::Warning,
        "move.preview",
        "completed",
        disposition == PreviewDisposition::ReadyToCommit ? "success" : "rejected",
        json::object { { "disposition", static_cast<int>(disposition) }, { "error", error } },
        true);
    if (disposition == PreviewDisposition::ReplanAfterDismiss) {
        return { RoutingCycleStatus::PreviewNeedsDismiss, {}, {} };
    }
    if (disposition == PreviewDisposition::Failed || session.transaction() == nullptr) {
        session.cancel_transaction();
        return { RoutingCycleStatus::PreviewNeedsDismiss, "move_preview_rejected", std::move(error) };
    }
    if (!validate_session_commit(session, &error)) {
        session.cancel_transaction();
        return { RoutingCycleStatus::PreviewNeedsDismiss, "move_confirmation_invalidated", std::move(error) };
    }
    EnteredPageObservation entered_page;
    record_routing_run_event(
        session,
        port,
        RunLogLevel::Info,
        "move.confirm",
        "started",
        "pending");
    if (!port.confirm(*session.transaction(), entered_page, &error)) {
        record_routing_run_event(
            session,
            port,
            RunLogLevel::Error,
            "move.confirm",
            "failed",
            "error",
            json::object { { "error", error } },
            true);
        if (move_confirmation_failure_is_recoverable(error)) {
            session.cancel_transaction();
            return { RoutingCycleStatus::PreviewNeedsDismiss, "move_confirmation_retry_exhausted", std::move(error) };
        }
        return { RoutingCycleStatus::Failed, "move_confirmation_failed", std::move(error) };
    }
    if (entered_page.inventory_cleanup_performed) {
        // 零件箱过载拦截发生在移动真正提交之前。整理可能丢掉原计划使用的加工品，必须取消
        // 旧事务、重新观察零件箱并重新规划，不能把本次点击误记成已经到达节点。
        session.cancel_transaction();
        invalidate_session_movement_inventory(session);
        return { RoutingCycleStatus::PreviewNeedsDismiss,
                 "inventory_overload_cleaned",
                 "parts-box overload was cleaned; movement was not submitted and will be replanned" };
    }
    if (!session.commit(std::move(entered_page), &error)) {
        return { RoutingCycleStatus::Failed, "move_confirmation_state_failed", std::move(error) };
    }
    std::string popup_error;
    if (!port.resolve_pending_collection_popups(&popup_error)) {
        Log.warn("BlackFlow pending collection popup attribution failed after move commit", popup_error);
    }
    record_routing_run_event(
        session,
        port,
        RunLogLevel::Info,
        "move.confirm",
        "completed",
        "success",
        {},
        true);
    return { session_requires_page_dispatch(session) ? RoutingCycleStatus::MoveCommitted
                                                     : RoutingCycleStatus::MoveCommittedToMap,
             {},
             {} };
}

template <typename Session>
RoutingCycleOutcome execute_pending_routing_cycle(Session& session, IBlackFlowTaskPort& port)
{
    std::string error;
    if (!session.begin_pending_transaction(&error)) {
        session.clear_pending_candidate();
        return { RoutingCycleStatus::ReplanRequired, "pending_movement_invalidated", std::move(error) };
    }
    return execute_preview_cycle(session, port);
}

template <typename Session>
RoutingCycleOutcome execute_routing_cycle(Session& session, IBlackFlowTaskPort& port)
{
    std::string error;
    // 终止判定先于地图重建：楼层识别就可能已经把这一局判完，那就不该再为一张用不上的地图折腾。
    if (session.terminated()) {
        return { RoutingCycleStatus::SessionTerminated, {}, {} };
    }
    // 零件箱的库存是规划输入，且关箱不会移动地图；在已知需要扫描时先扫库存，避免
    // 先为即将失效的旧库存重建一次地图。扫描后仍保留下方的复查，覆盖 refresh 新识别楼层的情况。
    if (session_requires_movement_inventory_observation(session)) {
        return { RoutingCycleStatus::MovementInventoryObservationRequired, {}, {} };
    }

    bool reuse_battle_preview_map = false;
    BattlePreviewMapCheck preview_map_check;
    std::string preview_map_error;
    if (port.check_battle_preview_map(preview_map_check, &preview_map_error)) {
        reuse_battle_preview_map = preview_map_check.disposition == BattlePreviewMapDisposition::Unchanged;
        if (preview_map_check.disposition != BattlePreviewMapDisposition::NotPending) {
            record_routing_run_event(
                session,
                port,
                RunLogLevel::Info,
                "battle-intel.map-cache",
                "checked",
                reuse_battle_preview_map ? "reused" : "rebuild_required",
                json::object { { "mean_difference", preview_map_check.mean_difference },
                               { "threshold", BattlePreviewMapMaximumMeanDifference } });
        }
    }
    else {
        Log.warn("BlackFlow battle preview map cache check failed; rebuilding map", preview_map_error);
    }
    if (!reuse_battle_preview_map && !refresh_with_retries(session, port, &error)) {
        return { RoutingCycleStatus::NeedsPageRecovery, "map_rebuild_failed", std::move(error) };
    }
    if (session_requires_map_settle(session)) {
        return { RoutingCycleStatus::ReplanRequired, {}, {} };
    }
    if constexpr (requires {
                      session.next_battle_intel_probe();
                      session.record_battle_intel_probe(
                          InvalidNodeId,
                          std::optional<std::string> {},
                          std::string {},
                          &error);
                  }) {
        if (const std::optional<NodeId> target = session.next_battle_intel_probe(); target.has_value()) {
            BattleIntelPreview intel;
            std::string inspect_error;
            record_routing_run_event(
                session,
                port,
                RunLogLevel::Info,
                "battle-intel.preview",
                "started",
                "pending",
                json::object { { "node", *target } });
            const bool inspected = port.inspect_battle(*target, session.viewport(), intel, &inspect_error);
            record_routing_run_event(
                session,
                port,
                inspected ? RunLogLevel::Info : RunLogLevel::Warning,
                "battle-intel.preview",
                inspected ? "completed" : "failed",
                inspected ? "success" : "error",
                json::object {
                    { "node", *target },
                    { "panel_open", intel.panel_open },
                    { "target_verified", intel.target_verified },
                    { "stage_name", intel.stage_name.value_or(std::string()) },
                    { "error", inspect_error },
                },
                true);
            if (intel.panel_open) {
                // 面板已经打开就说明本轮探查动作确实发生了。即使标题 OCR 失败、节点仍显示
                // “未知的凶戾”，或点击位置因视口变化落到别的节点，也必须记录这次尝试；否则
                // 关闭预览并重建地图后会无条件再次探查同一节点，形成无上限死循环。
                std::string record_error;
                if (!session.record_battle_intel_probe(
                        *target,
                        intel.target_verified ? std::move(intel.stage_name) : std::optional<std::string> {},
                        inspected ? std::string {} : inspect_error,
                        &record_error)) {
                    return { RoutingCycleStatus::PreviewNeedsDismiss,
                             "battle_intel_record_failed",
                             std::move(record_error) };
                }
                // 无论关卡名是否成功记录，同一节点在当前一局里都只打开一次；关闭后重新观测。
                return { RoutingCycleStatus::PreviewNeedsDismiss,
                          inspected                           ? std::string {}
                          : intel.target_verified             ? "battle_intel_ocr_failed"
                                                              : "battle_intel_preview_identity_mismatch",
                          inspected ? std::string {} : std::move(inspect_error) };
            }
            return { RoutingCycleStatus::Failed, "battle_intel_preview_failed", std::move(inspect_error) };
        }
    }
    if (session_requires_movement_inventory_observation(session)) {
        return { RoutingCycleStatus::MovementInventoryObservationRequired, {}, {} };
    }

    BlackFlowPlan plan = session.plan(&error);
    if (!plan) {
        if (session.terminated()) {
            return { RoutingCycleStatus::SessionTerminated, {}, {} };
        }
        return { RoutingCycleStatus::Failed, "planning_failed", std::move(error) };
    }
    const MoveCandidate candidate = *plan.decision.selected;
    if (candidate.direct_exhaustion) {
        if constexpr (requires { session.record_direct_exhaustion_decision(&error); }) {
            if (!session.record_direct_exhaustion_decision(&error)) {
                return { RoutingCycleStatus::Failed, "direct_exhaustion_state_failed", std::move(error) };
            }
        }
        return { RoutingCycleStatus::DirectExhaustionRequired, {}, {} };
    }
    if constexpr (requires {
                      session.run().active_movement;
                      session.save_pending_candidate(candidate, &error);
                  }) {
        if (!session.run().active_movement.has_value() || *session.run().active_movement != candidate.movement) {
            if (!session.save_pending_candidate(candidate, &error)) {
                return { RoutingCycleStatus::Failed, "movement_selection_proposal_failed", std::move(error) };
            }
            return { RoutingCycleStatus::MovementSelectionRequired, {}, {} };
        }
    }
    if (!session.begin_transaction(candidate, &error)) {
        return { RoutingCycleStatus::Failed, "transaction_proposal_failed", std::move(error) };
    }
    return execute_preview_cycle(session, port);
}
} // namespace asst::blackflow
