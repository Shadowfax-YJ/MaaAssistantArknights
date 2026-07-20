#pragma once

#include "BlackFlowClientGuard.hpp"
#include "BlackFlowOcrSidecar.hpp"
#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Vision/Roguelike/BlackFlowStorePageAnalyzer.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace asst
{
struct BlackFlowPngDimensions
{
    int width = 0;
    int height = 0;
};

using BlackFlowPngVerifier = std::function<std::optional<BlackFlowPngDimensions>(const std::filesystem::path&)>;
using BlackFlowCommittedPageAnalyzer = std::function<BlackFlowStoreSlotsAnalysis(const std::filesystem::path&)>;

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
    bool advances_completed_pages = false;
    bool should_notify = false;
    std::string error_code;
    std::string error_message;
};

class BlackFlowStorePageRepository final
{
public:
    BlackFlowStorePageRepository(
        std::filesystem::path root,
        BlackFlowStoreConfigContract config,
        BlackFlowPngVerifier png_verifier,
        BlackFlowStoreFaultInjector fault_injector = {});

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

private:
    std::filesystem::path m_root;
    BlackFlowStoreConfigContract m_config;
    BlackFlowPngVerifier m_png_verifier;
    BlackFlowStoreFaultInjector m_fault_injector;
};
} // namespace asst
