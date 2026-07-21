#pragma once

#include "BlackFlowStoreOrchestrator.hpp"
#include "BlackFlowStorePageRepository.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace asst
{
class BlackFlowStoreCycleRuntime
{
public:
    virtual ~BlackFlowStoreCycleRuntime() = default;

    virtual bool run_named_task(std::string_view task_name) = 0;
    virtual std::optional<BlackFlowStoreObservedPage>
        observe_store_page(const std::optional<BlackFlowStoreTitleFingerprint>& last_committed_fingerprint) = 0;
    virtual std::optional<std::vector<std::uint8_t>> encode_observed_page() = 0;
    virtual BlackFlowStoreSlotsAnalysis analyze_committed_page(const std::filesystem::path& path) = 0;
    virtual BlackFlowStoreSlotsAnalysis analyze_recovery_page(
        const std::filesystem::path& path,
        const BlackFlowStoreStopRequested& cancel_requested) = 0;
    virtual void snapshot_committed(const BlackFlowStoreSnapshot& snapshot) = 0;
    virtual void
        exploration_ended(const BlackFlowStoreExplorationSummary& summary, std::string_view exploration_id) = 0;
    virtual bool wait_for(std::chrono::milliseconds duration) = 0;
    virtual bool stop_requested() const noexcept = 0;
    virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

class BlackFlowStoreCycleAdapter final : public BlackFlowStoreCyclePort
{
public:
    BlackFlowStoreCycleAdapter(
        BlackFlowStorePageRepository& repository,
        BlackFlowClientType client_type,
        std::filesystem::path callback_relative_root,
        BlackFlowStoreCycleRuntime& runtime);

    bool run_named_task(std::string_view task_name) override;
    void recover_pending_pages() override;
    bool begin_exploration() override;
    std::optional<BlackFlowStoreObservedPage>
        observe_store_page(const std::optional<BlackFlowStoreTitleFingerprint>& last_committed_fingerprint) override;
    BlackFlowStoreCaptureResult capture_store_page(size_t page_index, size_t attempt) override;
    void snapshot_committed(const BlackFlowStoreSnapshot& snapshot) override;
    void exploration_ended(const BlackFlowStoreExplorationSummary& summary) override;
    bool wait_for(std::chrono::milliseconds duration) override;
    bool stop_requested() const noexcept override;
    std::chrono::steady_clock::time_point now() const noexcept override;

private:
    BlackFlowStoreSnapshot make_snapshot(
        std::string_view exploration_id,
        size_t page_index,
        const BlackFlowStorePageCommitResult& committed,
        BlackFlowStoreSnapshotOrigin origin) const;

    BlackFlowStorePageRepository& m_repository;
    BlackFlowClientType m_client_type;
    std::filesystem::path m_callback_relative_root;
    BlackFlowStoreCycleRuntime& m_runtime;
    std::optional<BlackFlowStoreExploration> m_exploration;
    std::optional<size_t> m_page_with_committed_png;
};
} // namespace asst
