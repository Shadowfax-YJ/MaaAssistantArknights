#pragma once

#include "Vision/Roguelike/BlackFlowStorePageAnalyzer.hpp"

#include <cstddef>
#include <optional>

namespace asst
{
inline constexpr size_t BlackFlowStorePageLimit = 3;
inline constexpr size_t BlackFlowStoreSuccessfulRefreshLimit = BlackFlowStorePageLimit - 1;
inline constexpr size_t BlackFlowStorePageAttemptLimit = 3;

enum class BlackFlowStorePhase
{
    Idle,
    AwaitingStorePage,
    AwaitingCaptureRetry,
    CapturingStorePage,
    AwaitingRefreshDialog,
    AwaitingRefreshConfirmation,
    AwaitingRefreshedStorePage,
    DismissingRefreshDialog,
    Finished,
    Stopped,
};

enum class BlackFlowStoreCommandKind
{
    ObserveStorePage,
    CaptureStorePage,
    OpenRefreshDialog,
    ConfirmRefresh,
    DismissRefreshDialog,
    Finish,
    Stop,
};

struct BlackFlowStoreCommand
{
    BlackFlowStoreCommandKind kind = BlackFlowStoreCommandKind::Stop;
    size_t page_index = 0;
    size_t attempt = 0;

    bool operator==(const BlackFlowStoreCommand&) const = default;
};

enum class BlackFlowStoreEventKind
{
    StorePageObserved,
    StorePageObservationFailed,
    PageCaptureSucceeded,
    PageCaptureFailed,
    RefreshDialogOpened,
    RefreshDialogFailed,
    RefreshConfirmationSucceeded,
    RefreshConfirmationFailed,
    RefreshDialogDismissed,
    RefreshDialogDismissalFailed,
    StepLimitReached,
    StopRequested,
};

struct BlackFlowStoreEvent
{
    BlackFlowStoreEventKind kind = BlackFlowStoreEventKind::StopRequested;
    BlackFlowStorePageClassification page_classification = BlackFlowStorePageClassification::NotReady;
    BlackFlowStoreTitleFingerprint title_fingerprint {};

    static BlackFlowStoreEvent store_page_observed(
        BlackFlowStorePageClassification classification,
        BlackFlowStoreTitleFingerprint fingerprint) noexcept;
    static BlackFlowStoreEvent store_page_observation_failed() noexcept;
    static BlackFlowStoreEvent page_capture_succeeded() noexcept;
    static BlackFlowStoreEvent page_capture_failed() noexcept;
    static BlackFlowStoreEvent refresh_dialog_opened() noexcept;
    static BlackFlowStoreEvent refresh_dialog_failed() noexcept;
    static BlackFlowStoreEvent refresh_confirmation_succeeded() noexcept;
    static BlackFlowStoreEvent refresh_confirmation_failed() noexcept;
    static BlackFlowStoreEvent refresh_dialog_dismissed() noexcept;
    static BlackFlowStoreEvent refresh_dialog_dismissal_failed() noexcept;
    static BlackFlowStoreEvent step_limit_reached() noexcept;
    static BlackFlowStoreEvent stop_requested() noexcept;
};

struct BlackFlowStoreState
{
    BlackFlowStorePhase phase = BlackFlowStorePhase::Idle;
    size_t completed_pages = 0;
    size_t successful_refreshes = 0;
    bool refresh_submission_latched = false;
    size_t current_page_attempts = 0;
    std::optional<BlackFlowStoreTitleFingerprint> last_committed_fingerprint;
};

class BlackFlowStoreStateMachine final
{
public:
    BlackFlowStoreCommand start() noexcept;
    BlackFlowStoreCommand handle(const BlackFlowStoreEvent& event) noexcept;

    const BlackFlowStoreState& state() const noexcept { return m_state; }

private:
    BlackFlowStoreCommand observe_store_page() noexcept;
    BlackFlowStoreCommand capture_observed_page(const BlackFlowStoreEvent& event) noexcept;
    BlackFlowStoreCommand dismiss_refresh_dialog() noexcept;
    BlackFlowStoreCommand stop() noexcept;

    BlackFlowStoreState m_state;
    std::optional<BlackFlowStoreTitleFingerprint> m_pending_fingerprint;
};
} // namespace asst
