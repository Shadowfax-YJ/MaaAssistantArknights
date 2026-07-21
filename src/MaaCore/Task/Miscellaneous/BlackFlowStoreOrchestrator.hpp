#pragma once

#include "BlackFlowStoreStateMachine.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace asst
{
enum class BlackFlowStorePluginInvocation
{
    None,
    CollectStore,
    FinalizeReturnedToEntry,
    FinalizeFailedExit,
};

inline constexpr BlackFlowStorePluginInvocation classify_black_flow_store_plugin_invocation(
    bool collection_trigger,
    bool has_pending_collection,
    bool process_task_start,
    std::string_view task_name) noexcept
{
    if (has_pending_collection) {
        if (!process_task_start) {
            return BlackFlowStorePluginInvocation::None;
        }
        if (task_name == "MiniGame@BlackFlow@StartExplore") {
            return BlackFlowStorePluginInvocation::FinalizeReturnedToEntry;
        }
        if (task_name == "Stop") {
            return BlackFlowStorePluginInvocation::FinalizeFailedExit;
        }
        return BlackFlowStorePluginInvocation::None;
    }
    return collection_trigger ? BlackFlowStorePluginInvocation::CollectStore : BlackFlowStorePluginInvocation::None;
}

namespace black_flow_store_tasks
{
inline constexpr std::string_view ObserveStorePage = "MiniGame@BlackFlow@ObserveStorePage";
inline constexpr std::string_view OpenRefreshDialog = "MiniGame@BlackFlow@OpenRefreshDialog";
inline constexpr std::string_view ConfirmRefresh = "MiniGame@BlackFlow@ConfirmRefresh";
inline constexpr std::string_view DismissRefreshDialog = "MiniGame@BlackFlow@DismissRefreshDialog";
inline constexpr std::string_view StorePageReady = "MiniGame@BlackFlow@StorePageReady";
inline constexpr std::string_view RefreshDialogVisible = "MiniGame@BlackFlow@RefreshDialogVisible";
} // namespace black_flow_store_tasks

struct BlackFlowStoreObservedPage
{
    BlackFlowStorePageClassification classification = BlackFlowStorePageClassification::NotReady;
    BlackFlowStoreTitleFingerprint title_fingerprint {};
};

enum class BlackFlowStoreSnapshotOrigin
{
    Live,
    Recovery,
};

inline constexpr std::string_view black_flow_store_snapshot_origin_name(BlackFlowStoreSnapshotOrigin origin) noexcept
{
    switch (origin) {
    case BlackFlowStoreSnapshotOrigin::Live:
        return "live";
    case BlackFlowStoreSnapshotOrigin::Recovery:
        return "recovery";
    }
    return "live";
}

struct BlackFlowStoreSnapshot
{
    std::string exploration_id;
    size_t page_index = 0;
    size_t attempt = 0;
    std::string page_status;
    std::string png_relative_path;
    std::string json_relative_path;
    BlackFlowStoreSnapshotOrigin origin = BlackFlowStoreSnapshotOrigin::Live;
};

struct BlackFlowStoreCaptureResult
{
    bool advances_completed_pages = false;
    bool should_notify = false;
    BlackFlowStoreSnapshot snapshot;
};

enum class BlackFlowStoreCycleStatus
{
    Completed,
    Failed,
    Stopped,
};

enum class BlackFlowStoreCycleFailure
{
    None,
    MissingClient,
    BeginCollection,
    ObserveStorePage,
    CaptureStorePage,
    OpenRefreshDialog,
    ConfirmRefresh,
    DismissRefreshDialog,
    InvalidCommand,
    StepLimit,
    Unexpected,
};

inline constexpr std::string_view black_flow_store_cycle_status_name(BlackFlowStoreCycleStatus status) noexcept
{
    switch (status) {
    case BlackFlowStoreCycleStatus::Completed:
        return "completed";
    case BlackFlowStoreCycleStatus::Failed:
        return "failed";
    case BlackFlowStoreCycleStatus::Stopped:
        return "stopped";
    }
    return "failed";
}

inline constexpr std::string_view black_flow_store_cycle_failure_name(BlackFlowStoreCycleFailure failure) noexcept
{
    switch (failure) {
    case BlackFlowStoreCycleFailure::None:
        return "none";
    case BlackFlowStoreCycleFailure::MissingClient:
        return "missing_client";
    case BlackFlowStoreCycleFailure::BeginCollection:
        return "begin_collection";
    case BlackFlowStoreCycleFailure::ObserveStorePage:
        return "observe_store_page";
    case BlackFlowStoreCycleFailure::CaptureStorePage:
        return "capture_store_page";
    case BlackFlowStoreCycleFailure::OpenRefreshDialog:
        return "open_refresh_dialog";
    case BlackFlowStoreCycleFailure::ConfirmRefresh:
        return "confirm_refresh";
    case BlackFlowStoreCycleFailure::DismissRefreshDialog:
        return "dismiss_refresh_dialog";
    case BlackFlowStoreCycleFailure::InvalidCommand:
        return "invalid_command";
    case BlackFlowStoreCycleFailure::StepLimit:
        return "step_limit";
    case BlackFlowStoreCycleFailure::Unexpected:
        return "unexpected";
    }
    return "unexpected";
}

struct BlackFlowStoreCollectionSummary
{
    BlackFlowStoreCycleStatus status = BlackFlowStoreCycleStatus::Failed;
    BlackFlowStoreCycleFailure failure = BlackFlowStoreCycleFailure::None;
    size_t completed_pages = 0;
    size_t successful_refreshes = 0;
};

class BlackFlowStoreCyclePort
{
public:
    virtual ~BlackFlowStoreCyclePort() = default;

    virtual bool run_named_task(std::string_view task_name) = 0;
    virtual void recover_pending_pages() = 0;
    virtual bool begin_collection() = 0;
    virtual std::optional<BlackFlowStoreObservedPage>
        observe_store_page(const std::optional<BlackFlowStoreTitleFingerprint>& last_committed_fingerprint) = 0;
    virtual BlackFlowStoreCaptureResult capture_store_page(size_t page_index, size_t attempt) = 0;
    virtual void snapshot_committed(const BlackFlowStoreSnapshot& snapshot) = 0;
    virtual void collection_ended(const BlackFlowStoreCollectionSummary& summary) = 0;
    virtual bool wait_for(std::chrono::milliseconds duration) = 0;
    virtual bool stop_requested() const noexcept = 0;
    virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

struct BlackFlowStoreCycleOutcome
{
    BlackFlowStoreCycleStatus status = BlackFlowStoreCycleStatus::Failed;
    BlackFlowStoreCycleFailure failure = BlackFlowStoreCycleFailure::None;
    size_t completed_pages = 0;
    size_t successful_refreshes = 0;
};

BlackFlowStoreCycleOutcome run_black_flow_store_cycle(BlackFlowStoreCyclePort& port);
} // namespace asst
