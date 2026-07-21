#pragma once

#include "BlackFlowClientGuard.hpp"
#include "BlackFlowOcrSidecar.hpp"
#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Vision/Roguelike/BlackFlowStorePageAnalyzer.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace asst
{
inline constexpr size_t BlackFlowStoreRecoveryCandidateLimit = 20;
inline constexpr auto BlackFlowStoreRecoveryTimeLimit = std::chrono::seconds(30);

struct BlackFlowPngDimensions
{
    int width = 0;
    int height = 0;
};

using BlackFlowPngVerifier = std::function<std::optional<BlackFlowPngDimensions>(const std::filesystem::path&)>;
using BlackFlowStoreStopRequested = std::function<bool()>;
using BlackFlowCommittedPageAnalyzer = std::function<BlackFlowStoreSlotsAnalysis(const std::filesystem::path&)>;
using BlackFlowRecoveryPageAnalyzer =
    std::function<BlackFlowStoreSlotsAnalysis(const std::filesystem::path&, const BlackFlowStoreStopRequested&)>;

enum class BlackFlowJsonDisposition
{
    None,
    FirstCommit,
    Improved,
    Unchanged,
    Conflict,
};

enum class BlackFlowStoreCommitCheckpoint
{
    ContextCommitted,
    PngStaged,
    PngValidated,
    PngCommitted,
    PageAnalyzed,
    JsonStaged,
    JsonValidated,
    JsonCommitted,
    JsonReplaced,
};

using BlackFlowStoreFaultInjector = std::function<void(BlackFlowStoreCommitCheckpoint)>;

class BlackFlowStoreExploration
{
public:
    const std::string& id() const noexcept { return m_id; }

    BlackFlowClientType client_type() const noexcept { return m_client_type; }

private:
    friend class BlackFlowStorePageRepository;

    std::string m_id;
    BlackFlowClientType m_client_type = BlackFlowClientType::Official;
    std::filesystem::path m_relative_directory;
};

struct BlackFlowStorePageCommitResult
{
    BlackFlowJsonDisposition disposition = BlackFlowJsonDisposition::None;
    BlackFlowPageStatus page_status = BlackFlowPageStatus::Failed;
    std::filesystem::path png_relative_path;
    std::filesystem::path json_relative_path;
    int attempt = 0;
    bool png_committed = false;
    bool advances_completed_pages = false;
    bool should_notify = false;
    std::string error_code;
    std::string error_message;
};

struct BlackFlowStoreRecoveryCommit
{
    std::string exploration_id;
    int page_index = 0;
    BlackFlowStorePageCommitResult result;
};

struct BlackFlowStoreRecoverySummary
{
    size_t candidates_found = 0;
    size_t candidates_processed = 0;
    size_t commits_published = 0;
    size_t candidates_failed = 0;
    bool candidate_limit_reached = false;
    bool time_limit_reached = false;
};

using BlackFlowStoreRecoveryClock = std::function<std::chrono::steady_clock::time_point()>;
using BlackFlowStoreRecoveryObserver = std::function<void(const BlackFlowStoreRecoveryCommit&)>;

class BlackFlowStorePageRepository final
{
public:
    BlackFlowStorePageRepository(
        std::filesystem::path root,
        BlackFlowStoreConfigContract config,
        BlackFlowPngVerifier png_verifier,
        BlackFlowStoreFaultInjector fault_injector = { });

    std::optional<BlackFlowStoreExploration> begin_exploration(BlackFlowClientType client_type);

    BlackFlowStorePageCommitResult capture_page(
        const BlackFlowStoreExploration& exploration,
        int page_index,
        int attempt,
        std::span<const std::byte> encoded_png,
        const BlackFlowCommittedPageAnalyzer& analyzer);

    BlackFlowStorePageCommitResult reprocess_page(
        const BlackFlowStoreExploration& exploration,
        int page_index,
        int attempt,
        const BlackFlowCommittedPageAnalyzer& analyzer);

    BlackFlowStoreRecoverySummary recover_pending_pages(
        const BlackFlowRecoveryPageAnalyzer& analyzer,
        const BlackFlowStoreRecoveryClock& clock,
        const BlackFlowStoreStopRequested& stop_requested,
        const BlackFlowStoreRecoveryObserver& observer);

private:
    BlackFlowStorePageCommitResult reprocess_page_if_allowed(
        const BlackFlowStoreExploration& exploration,
        int page_index,
        int attempt,
        const BlackFlowCommittedPageAnalyzer& analyzer,
        const BlackFlowStoreStopRequested& cancel_requested);

    std::filesystem::path m_root;
    BlackFlowStoreConfigContract m_config;
    BlackFlowPngVerifier m_png_verifier;
    BlackFlowStoreFaultInjector m_fault_injector;
};
} // namespace asst
