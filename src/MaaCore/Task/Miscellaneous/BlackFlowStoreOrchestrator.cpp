#include "BlackFlowStoreOrchestrator.hpp"

#include <utility>

namespace
{
constexpr size_t MaxCycleOperations = 64;
constexpr size_t MaxObservationPolls = 512;
constexpr auto StorePageObservationTimeout = std::chrono::seconds(20);
constexpr auto StorePageObservationRetryDelay = std::chrono::milliseconds(100);
constexpr size_t OpenRefreshDialogAttemptLimit = 3;
constexpr auto OpenRefreshDialogTimeout = std::chrono::seconds(10);
constexpr auto SafeExitTimeout = std::chrono::seconds(30);

asst::BlackFlowStoreCycleOutcome outcome(
    asst::BlackFlowStoreCycleStatus status,
    asst::BlackFlowStoreCycleFailure failure,
    const asst::BlackFlowStoreState& state) noexcept
{
    return {
        .status = status,
        .failure = failure,
        .completed_pages = state.completed_pages,
        .successful_refreshes = state.successful_refreshes,
    };
}
} // namespace

asst::BlackFlowStoreCycleOutcome asst::run_black_flow_store_cycle(BlackFlowStoreCyclePort& port)
{
    BlackFlowStoreStateMachine machine;
    BlackFlowStoreCommand command = machine.start();
    BlackFlowStoreCycleFailure failure = BlackFlowStoreCycleFailure::None;
    bool exploration_callback_required = false;
    bool exploration_ended = false;
    std::optional<std::chrono::steady_clock::time_point> observation_deadline;
    size_t cycle_operations = 0;
    size_t observation_polls = 0;

    const auto end_exploration = [&](bool safely_exited) {
        if (exploration_ended || !exploration_callback_required) {
            return;
        }
        exploration_ended = true;
        port.exploration_ended(
            BlackFlowStoreExplorationSummary {
                .completed_pages = machine.state().completed_pages,
                .successful_refreshes = machine.state().successful_refreshes,
                .safely_exited = safely_exited,
            });
    };

    const auto remember_failure = [&](BlackFlowStoreCycleFailure value) {
        if (failure == BlackFlowStoreCycleFailure::None) {
            failure = value;
        }
    };

    while (true) {
        if (port.stop_requested() && command.kind != BlackFlowStoreCommandKind::Finish) {
            command = machine.handle(BlackFlowStoreEvent::stop_requested());
        }

        const bool timed_observation_command =
            command.kind == BlackFlowStoreCommandKind::ObserveStorePage &&
            (machine.state().phase == BlackFlowStorePhase::AwaitingStorePage ||
             machine.state().phase == BlackFlowStorePhase::AwaitingRefreshedStorePage);
        const bool terminal_cleanup_command = command.kind == BlackFlowStoreCommandKind::SafeExit ||
                                              command.kind == BlackFlowStoreCommandKind::Finish ||
                                              command.kind == BlackFlowStoreCommandKind::Stop;
        if (!timed_observation_command && !terminal_cleanup_command && cycle_operations >= MaxCycleOperations) {
            failure = BlackFlowStoreCycleFailure::StepLimit;
            command = machine.handle(BlackFlowStoreEvent::step_limit_reached());
        }
        else if (!timed_observation_command && !terminal_cleanup_command) {
            ++cycle_operations;
        }

        switch (command.kind) {
        case BlackFlowStoreCommandKind::EnterFreshExploration:
            exploration_callback_required = true;
            if (port.run_named_task(black_flow_store_tasks::EnterFreshExploration)) {
                command = machine.handle(BlackFlowStoreEvent::fresh_exploration_entered());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::EnterFreshExploration);
                command = machine.handle(BlackFlowStoreEvent::fresh_exploration_failed());
            }
            break;

        case BlackFlowStoreCommandKind::BeginExploration:
            if (port.begin_exploration()) {
                command = machine.handle(BlackFlowStoreEvent::exploration_started());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::BeginExploration);
                command = machine.handle(BlackFlowStoreEvent::exploration_start_failed());
            }
            break;

        case BlackFlowStoreCommandKind::ObserveStorePage: {
            const bool timed_observation = machine.state().phase == BlackFlowStorePhase::AwaitingStorePage ||
                                           machine.state().phase == BlackFlowStorePhase::AwaitingRefreshedStorePage;
            if (timed_observation && !observation_deadline) {
                observation_deadline = port.now() + StorePageObservationTimeout;
                observation_polls = 0;
            }
            if (timed_observation && port.now() >= observation_deadline.value()) {
                remember_failure(BlackFlowStoreCycleFailure::ObserveStorePage);
                command = machine.handle(BlackFlowStoreEvent::store_page_observation_failed());
                observation_deadline.reset();
                break;
            }
            if (timed_observation && observation_polls >= MaxObservationPolls) {
                failure = BlackFlowStoreCycleFailure::StepLimit;
                command = machine.handle(BlackFlowStoreEvent::step_limit_reached());
                observation_deadline.reset();
                observation_polls = 0;
                break;
            }
            if (timed_observation) {
                ++observation_polls;
            }
            const auto retry_or_fail_observation = [&] {
                if (timed_observation && port.now() < observation_deadline.value() && !port.stop_requested() &&
                    port.wait_for(StorePageObservationRetryDelay) && !port.stop_requested() &&
                    port.now() < observation_deadline.value()) {
                    return;
                }
                if (port.stop_requested()) {
                    command = machine.handle(BlackFlowStoreEvent::stop_requested());
                }
                else {
                    remember_failure(BlackFlowStoreCycleFailure::ObserveStorePage);
                    command = machine.handle(BlackFlowStoreEvent::store_page_observation_failed());
                }
                observation_deadline.reset();
                observation_polls = 0;
            };
            if (!port.run_named_task(black_flow_store_tasks::ObserveStorePage)) {
                retry_or_fail_observation();
                break;
            }
            auto observed = port.observe_store_page(machine.state().last_committed_fingerprint);
            if (!observed) {
                retry_or_fail_observation();
                break;
            }
            if (timed_observation && port.now() >= observation_deadline.value()) {
                remember_failure(BlackFlowStoreCycleFailure::ObserveStorePage);
                command = machine.handle(BlackFlowStoreEvent::store_page_observation_failed());
                observation_deadline.reset();
                observation_polls = 0;
                break;
            }
            command = machine.handle(
                BlackFlowStoreEvent::store_page_observed(
                    observed->classification,
                    std::move(observed->title_fingerprint)));
            if (command.kind != BlackFlowStoreCommandKind::ObserveStorePage) {
                observation_deadline.reset();
                observation_polls = 0;
            }
            break;
        }

        case BlackFlowStoreCommandKind::CaptureStorePage: {
            auto captured = port.capture_store_page(command.page_index, command.attempt);
            if (captured.should_notify) {
                port.snapshot_committed(captured.snapshot);
            }
            if (captured.advances_completed_pages) {
                command = machine.handle(BlackFlowStoreEvent::page_capture_succeeded());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::CaptureStorePage);
                command = machine.handle(BlackFlowStoreEvent::page_capture_failed());
            }
            break;
        }

        case BlackFlowStoreCommandKind::OpenRefreshDialog: {
            const auto deadline = port.now() + OpenRefreshDialogTimeout;
            bool opened = false;
            for (size_t attempt = 0; attempt < OpenRefreshDialogAttemptLimit && port.now() < deadline; ++attempt) {
                if (port.stop_requested()) {
                    break;
                }
                if (port.run_named_task(black_flow_store_tasks::OpenRefreshDialog) && port.now() <= deadline) {
                    opened = true;
                    break;
                }
            }
            if (port.stop_requested()) {
                command = machine.handle(BlackFlowStoreEvent::stop_requested());
            }
            else if (opened) {
                command = machine.handle(BlackFlowStoreEvent::refresh_dialog_opened());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::OpenRefreshDialog);
                command = machine.handle(BlackFlowStoreEvent::refresh_dialog_failed());
            }
            break;
        }

        case BlackFlowStoreCommandKind::ConfirmRefresh:
            if (port.run_named_task(black_flow_store_tasks::ConfirmRefresh)) {
                command = machine.handle(BlackFlowStoreEvent::refresh_confirmation_succeeded());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::ConfirmRefresh);
                command = machine.handle(BlackFlowStoreEvent::refresh_confirmation_failed());
            }
            break;

        case BlackFlowStoreCommandKind::SafeExit: {
            const auto started_at = port.now();
            const bool exited = port.run_named_task(black_flow_store_tasks::SafeExit);
            if (port.stop_requested()) {
                command = machine.handle(BlackFlowStoreEvent::stop_requested());
            }
            else if (exited && port.now() - started_at <= SafeExitTimeout) {
                command = machine.handle(BlackFlowStoreEvent::safe_exit_succeeded());
            }
            else {
                failure = BlackFlowStoreCycleFailure::SafeExit;
                command = machine.handle(BlackFlowStoreEvent::safe_exit_failed());
            }
            break;
        }

        case BlackFlowStoreCommandKind::Finish:
            end_exploration(true);
            return outcome(BlackFlowStoreCycleStatus::Completed, failure, machine.state());

        case BlackFlowStoreCommandKind::Stop:
            end_exploration(false);
            return outcome(
                port.stop_requested() ? BlackFlowStoreCycleStatus::Stopped : BlackFlowStoreCycleStatus::Failed,
                failure == BlackFlowStoreCycleFailure::None ? BlackFlowStoreCycleFailure::InvalidCommand : failure,
                machine.state());
        }
    }
}

asst::BlackFlowStoreSessionOutcome asst::run_black_flow_store_session(BlackFlowStoreCyclePort& port)
{
    if (port.stop_requested()) {
        return { .status = BlackFlowStoreSessionStatus::Stopped };
    }
    port.recover_pending_pages();
    if (port.stop_requested()) {
        return { .status = BlackFlowStoreSessionStatus::Stopped };
    }
    const auto prepare_started_at = port.now();
    const bool prepared = port.run_named_task(black_flow_store_tasks::PrepareFreshEntry);
    if (port.stop_requested()) {
        return { .status = BlackFlowStoreSessionStatus::Stopped };
    }
    if (!prepared || port.now() - prepare_started_at > SafeExitTimeout) {
        port.exploration_ended(BlackFlowStoreExplorationSummary { .safely_exited = false });
        return {
            .status = BlackFlowStoreSessionStatus::Failed,
            .failure = BlackFlowStoreSessionFailure::PrepareFreshEntry,
        };
    }

    size_t safely_exited_cycles = 0;
    while (!port.stop_requested()) {
        const auto cycle = run_black_flow_store_cycle(port);
        if (cycle.status == BlackFlowStoreCycleStatus::Stopped) {
            return {
                .status = BlackFlowStoreSessionStatus::Stopped,
                .safely_exited_cycles = safely_exited_cycles,
            };
        }
        if (cycle.status == BlackFlowStoreCycleStatus::Failed) {
            return {
                .status = BlackFlowStoreSessionStatus::Failed,
                .failure = BlackFlowStoreSessionFailure::Cycle,
                .safely_exited_cycles = safely_exited_cycles,
            };
        }
        ++safely_exited_cycles;
    }

    return {
        .status = BlackFlowStoreSessionStatus::Stopped,
        .safely_exited_cycles = safely_exited_cycles,
    };
}
