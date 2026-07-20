#pragma once

#include "BlackFlowStoreStateMachine.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace asst
{
namespace black_flow_store_tasks
{
inline constexpr std::string_view EnterFreshExploration = "MiniGame@BlackFlow@EnterFreshExploration";
inline constexpr std::string_view ObserveStorePage = "MiniGame@BlackFlow@ObserveStorePage";
inline constexpr std::string_view OpenRefreshDialog = "MiniGame@BlackFlow@OpenRefreshDialog";
inline constexpr std::string_view ConfirmRefresh = "MiniGame@BlackFlow@ConfirmRefresh";
inline constexpr std::string_view SafeExit = "MiniGame@BlackFlow@SafeExit";
inline constexpr std::string_view StorePageReady = "MiniGame@BlackFlow@StorePageReady";
inline constexpr std::string_view RefreshDialogVisible = "MiniGame@BlackFlow@RefreshDialogVisible";
inline constexpr std::string_view StartExploreEntryVisible = "MiniGame@BlackFlow@StartExploreEntryVisible";
} // namespace black_flow_store_tasks

struct BlackFlowStoreObservedPage
{
    BlackFlowStorePageClassification classification = BlackFlowStorePageClassification::NotReady;
    BlackFlowStoreTitleFingerprint title_fingerprint {};
};

struct BlackFlowStoreSnapshot
{
    std::string exploration_id;
    size_t page_index = 0;
    size_t attempt = 0;
    std::string page_status;
    std::string png_relative_path;
    std::string json_relative_path;
};

struct BlackFlowStoreCaptureResult
{
    bool advances_completed_pages = false;
    bool should_notify = false;
    BlackFlowStoreSnapshot snapshot;
};

struct BlackFlowStoreExplorationSummary
{
    size_t completed_pages = 0;
    size_t successful_refreshes = 0;
    bool safely_exited = false;
};

class BlackFlowStoreCyclePort
{
public:
    virtual ~BlackFlowStoreCyclePort() = default;

    virtual bool run_named_task(std::string_view task_name) = 0;
    virtual bool begin_exploration() = 0;
    virtual std::optional<BlackFlowStoreObservedPage>
        observe_store_page(const std::optional<BlackFlowStoreTitleFingerprint>& last_committed_fingerprint) = 0;
    virtual BlackFlowStoreCaptureResult capture_store_page(size_t page_index, size_t attempt) = 0;
    virtual void snapshot_committed(const BlackFlowStoreSnapshot& snapshot) = 0;
    virtual void exploration_ended(const BlackFlowStoreExplorationSummary& summary) = 0;
    virtual bool stop_requested() const noexcept = 0;
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
    EnterFreshExploration,
    BeginExploration,
    ObserveStorePage,
    CaptureStorePage,
    OpenRefreshDialog,
    ConfirmRefresh,
    SafeExit,
    InvalidCommand,
    StepLimit,
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
