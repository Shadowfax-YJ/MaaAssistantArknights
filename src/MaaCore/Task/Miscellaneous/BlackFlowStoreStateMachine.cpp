#include "BlackFlowStoreStateMachine.hpp"

#include <utility>

namespace
{
asst::BlackFlowStoreEvent event(asst::BlackFlowStoreEventKind kind) noexcept
{
    return asst::BlackFlowStoreEvent { .kind = kind };
}
} // namespace

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::store_page_observed(
    BlackFlowStorePageClassification classification,
    BlackFlowStoreTitleFingerprint fingerprint) noexcept
{
    return BlackFlowStoreEvent {
        .kind = BlackFlowStoreEventKind::StorePageObserved,
        .page_classification = classification,
        .title_fingerprint = std::move(fingerprint),
    };
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::store_page_observation_failed() noexcept
{
    return event(BlackFlowStoreEventKind::StorePageObservationFailed);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::page_capture_succeeded() noexcept
{
    return event(BlackFlowStoreEventKind::PageCaptureSucceeded);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::page_capture_failed() noexcept
{
    return event(BlackFlowStoreEventKind::PageCaptureFailed);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::refresh_dialog_opened() noexcept
{
    return event(BlackFlowStoreEventKind::RefreshDialogOpened);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::refresh_dialog_failed() noexcept
{
    return event(BlackFlowStoreEventKind::RefreshDialogFailed);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::refresh_confirmation_succeeded() noexcept
{
    return event(BlackFlowStoreEventKind::RefreshConfirmationSucceeded);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::refresh_confirmation_failed() noexcept
{
    return event(BlackFlowStoreEventKind::RefreshConfirmationFailed);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::refresh_dialog_dismissed() noexcept
{
    return event(BlackFlowStoreEventKind::RefreshDialogDismissed);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::refresh_dialog_dismissal_failed() noexcept
{
    return event(BlackFlowStoreEventKind::RefreshDialogDismissalFailed);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::step_limit_reached() noexcept
{
    return event(BlackFlowStoreEventKind::StepLimitReached);
}

asst::BlackFlowStoreEvent asst::BlackFlowStoreEvent::stop_requested() noexcept
{
    return event(BlackFlowStoreEventKind::StopRequested);
}

asst::BlackFlowStoreCommand asst::BlackFlowStoreStateMachine::start() noexcept
{
    if (m_state.phase != BlackFlowStorePhase::Idle) {
        return stop();
    }
    m_state.phase = BlackFlowStorePhase::AwaitingStorePage;
    return observe_store_page();
}

asst::BlackFlowStoreCommand asst::BlackFlowStoreStateMachine::handle(const BlackFlowStoreEvent& event) noexcept
{
    if (event.kind == BlackFlowStoreEventKind::StopRequested) {
        return stop();
    }
    if (event.kind == BlackFlowStoreEventKind::StepLimitReached) {
        return stop();
    }

    switch (m_state.phase) {
    case BlackFlowStorePhase::AwaitingStorePage:
        if (event.kind == BlackFlowStoreEventKind::StorePageObserved) {
            if (event.page_classification == BlackFlowStorePageClassification::StableInitial &&
                m_state.completed_pages == 0U) {
                return capture_observed_page(event);
            }
            return observe_store_page();
        }
        if (event.kind == BlackFlowStoreEventKind::StorePageObservationFailed) {
            return stop();
        }
        break;

    case BlackFlowStorePhase::AwaitingRefreshedStorePage:
        if (event.kind == BlackFlowStoreEventKind::StorePageObserved) {
            if (event.page_classification == BlackFlowStorePageClassification::StableNew &&
                m_state.refresh_submission_latched &&
                m_state.successful_refreshes < BlackFlowStoreSuccessfulRefreshLimit) {
                ++m_state.successful_refreshes;
                m_state.refresh_submission_latched = false;
                return capture_observed_page(event);
            }
            return observe_store_page();
        }
        if (event.kind == BlackFlowStoreEventKind::StorePageObservationFailed) {
            return stop();
        }
        break;

    case BlackFlowStorePhase::AwaitingCaptureRetry:
        if (event.kind == BlackFlowStoreEventKind::StorePageObserved &&
            (event.page_classification == BlackFlowStorePageClassification::StableInitial ||
             event.page_classification == BlackFlowStorePageClassification::StableNew) &&
            m_pending_fingerprint &&
            black_flow_store_fingerprint_distance(event.title_fingerprint, m_pending_fingerprint.value()) <=
                BlackFlowStoreStableFingerprintDistance) {
            ++m_state.current_page_attempts;
            m_state.phase = BlackFlowStorePhase::CapturingStorePage;
            return { BlackFlowStoreCommandKind::CaptureStorePage,
                     m_state.completed_pages + 1U,
                     m_state.current_page_attempts };
        }
        if (event.kind == BlackFlowStoreEventKind::StorePageObserved ||
            event.kind == BlackFlowStoreEventKind::StorePageObservationFailed) {
            return stop();
        }
        break;

    case BlackFlowStorePhase::CapturingStorePage:
        if (event.kind == BlackFlowStoreEventKind::PageCaptureSucceeded && m_pending_fingerprint) {
            ++m_state.completed_pages;
            m_state.last_committed_fingerprint = m_pending_fingerprint;
            m_pending_fingerprint.reset();
            m_state.current_page_attempts = 0;
            if (m_state.completed_pages == BlackFlowStorePageLimit) {
                m_state.phase = BlackFlowStorePhase::Finished;
                return { BlackFlowStoreCommandKind::Finish };
            }
            m_state.phase = BlackFlowStorePhase::AwaitingRefreshDialog;
            return { BlackFlowStoreCommandKind::OpenRefreshDialog };
        }
        if (event.kind == BlackFlowStoreEventKind::PageCaptureFailed) {
            if (m_state.current_page_attempts < BlackFlowStorePageAttemptLimit) {
                m_state.phase = BlackFlowStorePhase::AwaitingCaptureRetry;
                return observe_store_page();
            }
            return stop();
        }
        break;

    case BlackFlowStorePhase::AwaitingRefreshDialog:
        if (event.kind == BlackFlowStoreEventKind::RefreshDialogOpened && !m_state.refresh_submission_latched) {
            m_state.refresh_submission_latched = true;
            m_state.phase = BlackFlowStorePhase::AwaitingRefreshConfirmation;
            return { BlackFlowStoreCommandKind::ConfirmRefresh };
        }
        if (event.kind == BlackFlowStoreEventKind::RefreshDialogFailed) {
            return dismiss_refresh_dialog();
        }
        break;

    case BlackFlowStorePhase::AwaitingRefreshConfirmation:
        if (event.kind == BlackFlowStoreEventKind::RefreshConfirmationSucceeded) {
            m_state.phase = BlackFlowStorePhase::AwaitingRefreshedStorePage;
            return observe_store_page();
        }
        if (event.kind == BlackFlowStoreEventKind::RefreshConfirmationFailed) {
            return dismiss_refresh_dialog();
        }
        break;

    case BlackFlowStorePhase::DismissingRefreshDialog:
        if (event.kind == BlackFlowStoreEventKind::RefreshDialogDismissed ||
            event.kind == BlackFlowStoreEventKind::RefreshDialogDismissalFailed) {
            return stop();
        }
        break;

    case BlackFlowStorePhase::Idle:
    case BlackFlowStorePhase::Finished:
    case BlackFlowStorePhase::Stopped:
        break;
    }

    return stop();
}

asst::BlackFlowStoreCommand asst::BlackFlowStoreStateMachine::observe_store_page() noexcept
{
    return { BlackFlowStoreCommandKind::ObserveStorePage };
}

asst::BlackFlowStoreCommand
    asst::BlackFlowStoreStateMachine::capture_observed_page(const BlackFlowStoreEvent& event) noexcept
{
    m_pending_fingerprint = event.title_fingerprint;
    m_state.current_page_attempts = 1;
    m_state.phase = BlackFlowStorePhase::CapturingStorePage;
    return { BlackFlowStoreCommandKind::CaptureStorePage, m_state.completed_pages + 1U, 1U };
}

asst::BlackFlowStoreCommand asst::BlackFlowStoreStateMachine::dismiss_refresh_dialog() noexcept
{
    m_state.phase = BlackFlowStorePhase::DismissingRefreshDialog;
    return { BlackFlowStoreCommandKind::DismissRefreshDialog };
}

asst::BlackFlowStoreCommand asst::BlackFlowStoreStateMachine::stop() noexcept
{
    m_state.phase = BlackFlowStorePhase::Stopped;
    return { BlackFlowStoreCommandKind::Stop };
}
