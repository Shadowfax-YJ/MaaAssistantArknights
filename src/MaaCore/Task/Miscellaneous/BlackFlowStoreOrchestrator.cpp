#include "BlackFlowStoreOrchestrator.hpp"

#include <utility>

namespace
{
constexpr size_t MaxCycleSteps = 64;

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
    bool exploration_started = false;
    bool exploration_ended = false;

    const auto end_exploration = [&](bool safely_exited) {
        if (exploration_ended || !exploration_started) {
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

    for (size_t step = 0; step < MaxCycleSteps; ++step) {
        if (port.stop_requested() && command.kind != BlackFlowStoreCommandKind::Finish) {
            command = machine.handle(BlackFlowStoreEvent::stop_requested());
        }

        switch (command.kind) {
        case BlackFlowStoreCommandKind::EnterFreshExploration:
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
                exploration_started = true;
                command = machine.handle(BlackFlowStoreEvent::exploration_started());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::BeginExploration);
                exploration_started = true;
                command = machine.handle(BlackFlowStoreEvent::exploration_start_failed());
            }
            break;

        case BlackFlowStoreCommandKind::ObserveStorePage: {
            if (!port.run_named_task(black_flow_store_tasks::ObserveStorePage)) {
                remember_failure(BlackFlowStoreCycleFailure::ObserveStorePage);
                command = machine.handle(BlackFlowStoreEvent::store_page_observation_failed());
                break;
            }
            auto observed = port.observe_store_page(machine.state().last_committed_fingerprint);
            if (!observed) {
                remember_failure(BlackFlowStoreCycleFailure::ObserveStorePage);
                command = machine.handle(BlackFlowStoreEvent::store_page_observation_failed());
                break;
            }
            command = machine.handle(
                BlackFlowStoreEvent::store_page_observed(
                    observed->classification,
                    std::move(observed->title_fingerprint)));
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

        case BlackFlowStoreCommandKind::OpenRefreshDialog:
            if (port.run_named_task(black_flow_store_tasks::OpenRefreshDialog)) {
                command = machine.handle(BlackFlowStoreEvent::refresh_dialog_opened());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::OpenRefreshDialog);
                command = machine.handle(BlackFlowStoreEvent::refresh_dialog_failed());
            }
            break;

        case BlackFlowStoreCommandKind::ConfirmRefresh:
            if (port.run_named_task(black_flow_store_tasks::ConfirmRefresh)) {
                command = machine.handle(BlackFlowStoreEvent::refresh_confirmation_succeeded());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::ConfirmRefresh);
                command = machine.handle(BlackFlowStoreEvent::refresh_confirmation_failed());
            }
            break;

        case BlackFlowStoreCommandKind::SafeExit:
            if (port.run_named_task(black_flow_store_tasks::SafeExit)) {
                command = machine.handle(BlackFlowStoreEvent::safe_exit_succeeded());
            }
            else {
                remember_failure(BlackFlowStoreCycleFailure::SafeExit);
                command = machine.handle(BlackFlowStoreEvent::safe_exit_failed());
            }
            break;

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

    end_exploration(false);
    return outcome(BlackFlowStoreCycleStatus::Failed, BlackFlowStoreCycleFailure::StepLimit, machine.state());
}
