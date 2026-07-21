#include "BlackFlowStoreCycleAdapter.hpp"

#include <span>
#include <string>
#include <string_view>
#include <utility>

asst::BlackFlowStoreCycleAdapter::BlackFlowStoreCycleAdapter(
    BlackFlowStorePageRepository& repository,
    BlackFlowClientType client_type,
    std::filesystem::path callback_relative_root,
    BlackFlowStoreCycleRuntime& runtime) :
    m_repository(repository),
    m_client_type(client_type),
    m_callback_relative_root(std::move(callback_relative_root)),
    m_runtime(runtime)
{
}

bool asst::BlackFlowStoreCycleAdapter::run_named_task(std::string_view task_name)
{
    return m_runtime.run_named_task(task_name);
}

asst::BlackFlowStoreSnapshot asst::BlackFlowStoreCycleAdapter::make_snapshot(
    std::string_view exploration_id,
    size_t page_index,
    const BlackFlowStorePageCommitResult& committed,
    BlackFlowStoreSnapshotOrigin origin) const
{
    return {
        .exploration_id = std::string(exploration_id),
        .page_index = page_index,
        .attempt = static_cast<size_t>(committed.attempt),
        .page_status = std::string(black_flow_sidecar_detail::page_status_name(committed.page_status)),
        .png_relative_path = (m_callback_relative_root / committed.png_relative_path).generic_string(),
        .json_relative_path = (m_callback_relative_root / committed.json_relative_path).generic_string(),
        .origin = origin,
    };
}

void asst::BlackFlowStoreCycleAdapter::recover_pending_pages()
{
    const auto analyze = [&](const std::filesystem::path& committed_png,
                             const BlackFlowStoreStopRequested& cancel_requested) {
        return m_runtime.analyze_recovery_page(committed_png, cancel_requested);
    };
    m_repository.recover_pending_pages(
        analyze,
        [&] { return m_runtime.now(); },
        [&] { return m_runtime.stop_requested(); },
        [&](const BlackFlowStoreRecoveryCommit& recovered) {
            if (!recovered.result.should_notify) {
                return;
            }
            m_runtime.snapshot_committed(make_snapshot(
                recovered.exploration_id,
                static_cast<size_t>(recovered.page_index),
                recovered.result,
                BlackFlowStoreSnapshotOrigin::Recovery));
        });
}

bool asst::BlackFlowStoreCycleAdapter::begin_exploration()
{
    m_exploration = m_repository.begin_exploration(m_client_type);
    m_page_with_committed_png.reset();
    return m_exploration.has_value();
}

std::optional<asst::BlackFlowStoreObservedPage> asst::BlackFlowStoreCycleAdapter::observe_store_page(
    const std::optional<BlackFlowStoreTitleFingerprint>& last_committed_fingerprint)
{
    return m_runtime.observe_store_page(last_committed_fingerprint);
}

asst::BlackFlowStoreCaptureResult
    asst::BlackFlowStoreCycleAdapter::capture_store_page(size_t page_index, size_t attempt)
{
    if (!m_exploration) {
        return { };
    }

    const auto analyze = [&](const std::filesystem::path& committed_png) {
        return m_runtime.analyze_committed_page(committed_png);
    };

    BlackFlowStorePageCommitResult committed;
    if (m_page_with_committed_png != page_index) {
        const auto encoded_png = m_runtime.encode_observed_page();
        if (!encoded_png) {
            return { };
        }
        committed = m_repository.capture_page(
            m_exploration.value(),
            static_cast<int>(page_index),
            static_cast<int>(attempt),
            std::as_bytes(std::span(encoded_png.value())),
            analyze);
    }
    else {
        committed = m_repository.reprocess_page(
            m_exploration.value(),
            static_cast<int>(page_index),
            static_cast<int>(attempt),
            analyze);
    }
    if (committed.png_committed) {
        m_page_with_committed_png = page_index;
    }

    return BlackFlowStoreCaptureResult {
        .advances_completed_pages = committed.advances_completed_pages,
        .should_notify = committed.should_notify,
        .snapshot = make_snapshot(m_exploration->id(), page_index, committed, BlackFlowStoreSnapshotOrigin::Live),
    };
}

void asst::BlackFlowStoreCycleAdapter::snapshot_committed(const BlackFlowStoreSnapshot& snapshot)
{
    m_runtime.snapshot_committed(snapshot);
}

void asst::BlackFlowStoreCycleAdapter::exploration_ended(const BlackFlowStoreExplorationSummary& summary)
{
    const auto exploration_id = m_exploration ? m_exploration->id() : std::string { };
    m_exploration.reset();
    m_page_with_committed_png.reset();
    m_runtime.exploration_ended(summary, exploration_id);
}

bool asst::BlackFlowStoreCycleAdapter::wait_for(std::chrono::milliseconds duration)
{
    return m_runtime.wait_for(duration);
}

bool asst::BlackFlowStoreCycleAdapter::stop_requested() const noexcept
{
    return m_runtime.stop_requested();
}

std::chrono::steady_clock::time_point asst::BlackFlowStoreCycleAdapter::now() const noexcept
{
    return m_runtime.now();
}
