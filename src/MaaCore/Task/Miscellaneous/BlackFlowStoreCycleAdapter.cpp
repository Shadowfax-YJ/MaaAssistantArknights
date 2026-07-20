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

bool asst::BlackFlowStoreCycleAdapter::begin_exploration()
{
    m_exploration = m_repository.begin_exploration(m_client_type);
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
        return {};
    }

    const auto analyze = [&](const std::filesystem::path& committed_png) {
        return m_runtime.analyze_committed_page(committed_png);
    };

    BlackFlowStorePageCommitResult committed;
    if (attempt == 1U) {
        const auto encoded_png = m_runtime.encode_observed_page();
        if (!encoded_png) {
            return {};
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

    return BlackFlowStoreCaptureResult {
        .advances_completed_pages = committed.advances_completed_pages,
        .should_notify = committed.should_notify,
        .snapshot = {
            .exploration_id = m_exploration->id(),
            .page_index = page_index,
            .attempt = static_cast<size_t>(committed.attempt),
            .page_status = std::string(black_flow_sidecar_detail::page_status_name(committed.page_status)),
            .png_relative_path = (m_callback_relative_root / committed.png_relative_path).generic_string(),
            .json_relative_path = (m_callback_relative_root / committed.json_relative_path).generic_string(),
        },
    };
}

void asst::BlackFlowStoreCycleAdapter::snapshot_committed(const BlackFlowStoreSnapshot& snapshot)
{
    m_runtime.snapshot_committed(snapshot);
}

void asst::BlackFlowStoreCycleAdapter::exploration_ended(const BlackFlowStoreExplorationSummary& summary)
{
    m_runtime.exploration_ended(summary, m_exploration ? m_exploration->id() : std::string_view {});
}

bool asst::BlackFlowStoreCycleAdapter::stop_requested() const noexcept
{
    return m_runtime.stop_requested();
}
