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
    BlackFlowStoreCycleFailure failure = BlackFlowStoreCycleFailure::None;
    bool collection_ended = false;

    const auto finish_collection = [&](BlackFlowStoreCycleStatus status, BlackFlowStoreCycleFailure final_failure) {
        const auto result = outcome(status, final_failure, machine.state());
        if (!collection_ended) {
            collection_ended = true;
            port.collection_ended(
                BlackFlowStoreCollectionSummary {
                    .status = result.status,
                    .failure = result.failure,
                    .completed_pages = result.completed_pages,
                    .successful_refreshes = result.successful_refreshes,
                });
        }
        return result;
    };

    try {
        port.recover_pending_pages();
        if (port.stop_requested()) {
            return finish_collection(BlackFlowStoreCycleStatus::Stopped, BlackFlowStoreCycleFailure::None);
        }
        if (!port.begin_collection()) {
            return finish_collection(BlackFlowStoreCycleStatus::Failed, BlackFlowStoreCycleFailure::BeginCollection);
        }
        if (port.stop_requested()) {
            return finish_collection(BlackFlowStoreCycleStatus::Stopped, BlackFlowStoreCycleFailure::None);
        }

        BlackFlowStoreCommand command = machine.start();
        std::optional<std::chrono::steady_clock::time_point> observation_deadline;
        size_t cycle_operations = 0;
        size_t observation_polls = 0;

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
            const bool terminal_command =
                command.kind == BlackFlowStoreCommandKind::Finish || command.kind == BlackFlowStoreCommandKind::Stop;
            if (!timed_observation_command && !terminal_command && cycle_operations >= MaxCycleOperations) {
                failure = BlackFlowStoreCycleFailure::StepLimit;
                command = machine.handle(BlackFlowStoreEvent::step_limit_reached());
            }
            else if (!timed_observation_command && !terminal_command) {
                ++cycle_operations;
            }

            switch (command.kind) {
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
                    command = machine.handle(BlackFlowStoreEvent::page_capture_failed());
                    if (command.kind == BlackFlowStoreCommandKind::Stop) {
                        remember_failure(BlackFlowStoreCycleFailure::CaptureStorePage);
                    }
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

            case BlackFlowStoreCommandKind::DismissRefreshDialog: {
                const bool dismissed = port.run_named_task(black_flow_store_tasks::DismissRefreshDialog);
                if (port.stop_requested()) {
                    command = machine.handle(BlackFlowStoreEvent::stop_requested());
                }
                else if (dismissed) {
                    command = machine.handle(BlackFlowStoreEvent::refresh_dialog_dismissed());
                }
                else {
                    remember_failure(BlackFlowStoreCycleFailure::DismissRefreshDialog);
                    command = machine.handle(BlackFlowStoreEvent::refresh_dialog_dismissal_failed());
                }
                break;
            }

            case BlackFlowStoreCommandKind::Finish:
                return finish_collection(BlackFlowStoreCycleStatus::Completed, BlackFlowStoreCycleFailure::None);

            case BlackFlowStoreCommandKind::Stop:
                return finish_collection(
                    port.stop_requested() ? BlackFlowStoreCycleStatus::Stopped : BlackFlowStoreCycleStatus::Failed,
                    failure == BlackFlowStoreCycleFailure::None && !port.stop_requested()
                        ? BlackFlowStoreCycleFailure::InvalidCommand
                        : failure);
            }
        }
    }
    catch (...) {
        return finish_collection(BlackFlowStoreCycleStatus::Failed, BlackFlowStoreCycleFailure::Unexpected);
    }
}
