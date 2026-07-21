#include "BlackFlowStorePageRepository.hpp"

#include "Utils/JsonContract.hpp"
#include "Utils/Sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "MaaUtils/SafeWindows.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
using namespace asst;

std::mutex& repository_operation_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::string utc_timestamp(bool compact)
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fractional = static_cast<int>((milliseconds - seconds).count());
    const std::time_t value = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(seconds));
    std::tm utc { };
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif

    std::ostringstream output;
    output << std::put_time(&utc, compact ? "%Y%m%dT%H%M%S" : "%Y-%m-%dT%H:%M:%S");
    if (!compact) {
        output << '.';
    }
    output << std::setfill('0') << std::setw(3) << fractional << 'Z';
    return output.str();
}

std::string nonce_hex()
{
    static std::atomic<std::uint32_t> counter { 0 };
    const auto ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto mixed = static_cast<std::uint32_t>(ticks ^ (ticks >> 32U)) ^ ++counter;
    return std::format("{:08x}", mixed);
}

std::string page_stem(int page_index)
{
    return std::format("page-{:02}", page_index);
}

std::vector<std::byte> read_binary_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open committed file");
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("failed to determine committed file size");
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<size_t>(length));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), length)) {
        throw std::runtime_error("failed to read committed file");
    }
    return bytes;
}

std::string byte_string(std::span<const std::byte> bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

#ifndef _WIN32
void sync_directory(const std::filesystem::path& directory)
{
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "failed to open artifact directory");
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(), "failed to sync artifact directory");
    }
    ::close(descriptor);
}
#else
void sync_directory(const std::filesystem::path&)
{
}
#endif

void write_exclusive_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
#ifdef _WIN32
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "failed to create staged file");
    }

    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            const int error = static_cast<int>(GetLastError());
            CloseHandle(handle);
            throw std::system_error(error, std::system_category(), "failed to write staged file");
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        const int error = static_cast<int>(GetLastError());
        CloseHandle(handle);
        throw std::system_error(error, std::system_category(), "failed to sync staged file");
    }
    if (!CloseHandle(handle)) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "failed to close staged file");
    }
#else
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "failed to create staged file");
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            const int error = errno;
            ::close(descriptor);
            throw std::system_error(error, std::generic_category(), "failed to write staged file");
        }
        offset += static_cast<size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(), "failed to sync staged file");
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to close staged file");
    }
#endif
}

void publish_without_replacement(const std::filesystem::path& staged, const std::filesystem::path& final)
{
#ifdef _WIN32
    if (!MoveFileExW(staged.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "final artifact already exists");
    }
#else
    if (::link(staged.c_str(), final.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "final artifact already exists");
    }
    if (::unlink(staged.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to remove staged link");
    }
    sync_directory(final.parent_path());
#endif
}

void publish_with_replacement(const std::filesystem::path& staged, const std::filesystem::path& final)
{
#ifdef _WIN32
    if (!ReplaceFileW(final.c_str(), staged.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "failed to replace sidecar");
    }
#else
    if (::rename(staged.c_str(), final.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to replace sidecar");
    }
    sync_directory(final.parent_path());
#endif
}

BlackFlowPageStatus page_status(BlackFlowAnalyzedPageStatus status)
{
    switch (status) {
    case BlackFlowAnalyzedPageStatus::Complete:
        return BlackFlowPageStatus::Complete;
    case BlackFlowAnalyzedPageStatus::Partial:
        return BlackFlowPageStatus::Partial;
    case BlackFlowAnalyzedPageStatus::Failed:
        return BlackFlowPageStatus::Failed;
    }
    return BlackFlowPageStatus::Failed;
}

BlackFlowSlotStatus slot_status(BlackFlowAnalyzedSlotStatus status)
{
    switch (status) {
    case BlackFlowAnalyzedSlotStatus::Matched:
        return BlackFlowSlotStatus::Matched;
    case BlackFlowAnalyzedSlotStatus::Empty:
        return BlackFlowSlotStatus::Empty;
    case BlackFlowAnalyzedSlotStatus::Unmatched:
        return BlackFlowSlotStatus::Unmatched;
    case BlackFlowAnalyzedSlotStatus::OcrError:
        return BlackFlowSlotStatus::OcrError;
    case BlackFlowAnalyzedSlotStatus::MatchError:
        return BlackFlowSlotStatus::MatchError;
    }
    return BlackFlowSlotStatus::NotProcessed;
}

BlackFlowMatchKind match_kind(BlackFlowProductNameMatchKind kind)
{
    switch (kind) {
    case BlackFlowProductNameMatchKind::Exact:
        return BlackFlowMatchKind::Exact;
    case BlackFlowProductNameMatchKind::Fuzzy:
        return BlackFlowMatchKind::Fuzzy;
    case BlackFlowProductNameMatchKind::Unmatched:
        return BlackFlowMatchKind::None;
    }
    return BlackFlowMatchKind::None;
}

BlackFlowOcrSidecar make_sidecar(
    const BlackFlowStoreConfigContract& config,
    const BlackFlowStoreExploration& exploration,
    int page_index,
    std::string captured_at,
    std::string image_file_name,
    std::string image_sha256,
    const BlackFlowStoreSlotsAnalysis& analysis)
{
    auto sidecar = make_minimal_black_flow_ocr_sidecar(exploration.client_type(), config);
    sidecar.exploration_id = exploration.id();
    sidecar.page_index = page_index;
    sidecar.captured_at = std::move(captured_at);
    sidecar.image.file_name = std::move(image_file_name);
    sidecar.image.sha256 = std::move(image_sha256);
    sidecar.page_status = page_status(analysis.page_status);

    for (size_t index = 0; index < sidecar.slots.size(); ++index) {
        const auto& analyzed = analysis.slots[index];
        auto& slot = sidecar.slots[index];
        slot.status = slot_status(analyzed.status);
        slot.ocr_text = analyzed.ocr_text;
        slot.ocr_score = analyzed.ocr_score;
        slot.standard_product_name = analyzed.standard_product_name;
        slot.match_kind = match_kind(analyzed.match_kind);
        slot.errors.clear();
        if (analyzed.status == BlackFlowAnalyzedSlotStatus::OcrError ||
            analyzed.status == BlackFlowAnalyzedSlotStatus::MatchError) {
            slot.errors.emplace_back(
                BlackFlowSidecarError {
                    .stage = analyzed.status == BlackFlowAnalyzedSlotStatus::OcrError ? BlackFlowErrorStage::Ocr
                                                                                      : BlackFlowErrorStage::Match,
                    .code = analyzed.status == BlackFlowAnalyzedSlotStatus::OcrError ? "slot_ocr_failed"
                                                                                     : "slot_matching_failed",
                    .message =
                        analyzed.error_message.empty() ? "BlackFlow slot processing failed" : analyzed.error_message,
                    .retryable = true,
                    .slot_index = static_cast<int>(index + 1U),
                });
        }
    }
    return sidecar;
}

size_t processing_error_slot_count(const BlackFlowOcrSidecar& sidecar)
{
    return static_cast<size_t>(std::ranges::count_if(sidecar.slots, [](const auto& slot) {
        return slot.status == BlackFlowSlotStatus::OcrError || slot.status == BlackFlowSlotStatus::MatchError;
    }));
}

bool is_strict_improvement(const BlackFlowOcrSidecar& previous, const BlackFlowOcrSidecar& candidate)
{
    if (previous.page_status == BlackFlowPageStatus::Failed) {
        return candidate.page_status == BlackFlowPageStatus::Partial ||
               candidate.page_status == BlackFlowPageStatus::Complete;
    }
    if (previous.page_status == BlackFlowPageStatus::Partial) {
        if (candidate.page_status == BlackFlowPageStatus::Complete) {
            return true;
        }
        return candidate.page_status == BlackFlowPageStatus::Partial &&
               processing_error_slot_count(candidate) < processing_error_slot_count(previous);
    }
    return false;
}

struct CaptureContext
{
    std::string exploration_id;
    int page_index = 0;
    std::string captured_at;
    BlackFlowClientType client_type = BlackFlowClientType::Official;
};

std::optional<CaptureContext> read_capture_context(const std::filesystem::path& path)
{
    const auto document = json::parse(byte_string(read_binary_file(path)));
    if (!document ||
        !asst::utils::has_exact_json_fields(
            document.value(),
            std::array {
                std::string_view("schema"),
                std::string_view("version"),
                std::string_view("exploration_id"),
                std::string_view("page_index"),
                std::string_view("captured_at"),
                std::string_view("client_type"),
            }) ||
        !document->at("schema").is_string() || document->at("schema").as_string() != "maa.black_flow.capture_context" ||
        !document->at("version").is<int>() || document->at("version").as_integer() != 1 ||
        !document->at("exploration_id").is_string() || document->at("exploration_id").as_string().empty() ||
        !document->at("page_index").is<int>() || document->at("page_index").as_integer() < 1 ||
        document->at("page_index").as_integer() > 3 || !document->at("captured_at").is_string() ||
        document->at("captured_at").as_string().empty() || !document->at("client_type").is_string()) {
        return std::nullopt;
    }
    const auto client_type = parse_black_flow_client_type(document->at("client_type").as_string());
    if (!client_type) {
        return std::nullopt;
    }
    return CaptureContext {
        .exploration_id = document->at("exploration_id").as_string(),
        .page_index = document->at("page_index").as_integer(),
        .captured_at = document->at("captured_at").as_string(),
        .client_type = client_type.value(),
    };
}

struct RecoveryCandidate
{
    std::string exploration_id;
    BlackFlowClientType client_type = BlackFlowClientType::Official;
    std::filesystem::path relative_directory;
    int page_index = 0;
    std::string captured_at;
};

bool is_plain_regular_file(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status);
}

bool has_retryable_error(const BlackFlowOcrSidecar& sidecar)
{
    if (std::ranges::any_of(sidecar.errors, [](const auto& error) { return error.retryable; })) {
        return true;
    }
    return std::ranges::any_of(sidecar.slots, [](const auto& slot) {
        return std::ranges::any_of(slot.errors, [](const auto& error) { return error.retryable; });
    });
}

std::optional<RecoveryCandidate> recovery_candidate(const std::filesystem::path& directory, int page_index)
{
    const auto exploration_id = directory.filename().string();
    if (exploration_id.empty()) {
        return std::nullopt;
    }

    const auto stem = page_stem(page_index);
    const auto png_path = directory / (stem + ".png");
    const auto json_path = directory / (stem + ".json");
    const auto context_path = directory / ('.' + stem + ".capture-context.json");
    if (!is_plain_regular_file(png_path)) {
        return std::nullopt;
    }

    std::error_code status_error;
    const auto json_status = std::filesystem::symlink_status(json_path, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        return std::nullopt;
    }
    if (!status_error && std::filesystem::exists(json_status)) {
        if (!std::filesystem::is_regular_file(json_status)) {
            return std::nullopt;
        }
        const auto document = json::parse(byte_string(read_binary_file(json_path)));
        const auto sidecar = document ? parse_black_flow_ocr_sidecar(document.value()) : std::nullopt;
        if (!sidecar || sidecar->exploration_id != exploration_id || sidecar->page_index != page_index ||
            sidecar->image.file_name != png_path.filename().string() ||
            (sidecar->page_status != BlackFlowPageStatus::Partial &&
             sidecar->page_status != BlackFlowPageStatus::Failed) ||
            !has_retryable_error(sidecar.value())) {
            return std::nullopt;
        }
        return RecoveryCandidate {
            .exploration_id = sidecar->exploration_id,
            .client_type = sidecar->client_type,
            .relative_directory = directory.filename(),
            .page_index = page_index,
            .captured_at = sidecar->captured_at,
        };
    }

    if (!is_plain_regular_file(context_path)) {
        return std::nullopt;
    }
    const auto context = read_capture_context(context_path);
    if (!context || context->exploration_id != exploration_id || context->page_index != page_index) {
        return std::nullopt;
    }
    return RecoveryCandidate {
        .exploration_id = context->exploration_id,
        .client_type = context->client_type,
        .relative_directory = directory.filename(),
        .page_index = page_index,
        .captured_at = context->captured_at,
    };
}

void remove_if_exists(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

void ensure_not_cancelled(const BlackFlowStoreStopRequested& cancel_requested)
{
    if (!cancel_requested) {
        return;
    }
    try {
        if (!cancel_requested()) {
            return;
        }
    }
    catch (...) {
    }
    throw std::runtime_error("recovery cancelled during page reprocessing");
}
} // namespace

asst::BlackFlowStorePageRepository::BlackFlowStorePageRepository(
    std::filesystem::path root,
    BlackFlowStoreConfigContract config,
    BlackFlowPngVerifier png_verifier,
    BlackFlowStoreFaultInjector fault_injector) :
    m_root(std::move(root)),
    m_config(std::move(config)),
    m_png_verifier(std::move(png_verifier)),
    m_fault_injector(std::move(fault_injector))
{
}

std::optional<asst::BlackFlowStoreExploration>
    asst::BlackFlowStorePageRepository::begin_exploration(BlackFlowClientType client_type)
{
    std::error_code error;
    std::filesystem::create_directories(m_root, error);
    if (error) {
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto id = "exploration-" + utc_timestamp(true) + '-' + nonce_hex();
        const auto relative_directory = std::filesystem::path(id);
        if (std::filesystem::create_directory(m_root / relative_directory, error)) {
            BlackFlowStoreExploration exploration;
            exploration.m_id = id;
            exploration.m_client_type = client_type;
            exploration.m_relative_directory = relative_directory;
            return exploration;
        }
        if (error) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

asst::BlackFlowStorePageCommitResult asst::BlackFlowStorePageRepository::capture_page(
    const BlackFlowStoreExploration& exploration,
    int page_index,
    int attempt,
    std::span<const std::byte> encoded_png,
    const BlackFlowCommittedPageAnalyzer& analyzer)
{
    std::scoped_lock operation_lock(repository_operation_mutex());
    BlackFlowStorePageCommitResult result { .attempt = attempt };
    if (page_index < 1 || page_index > 3 || exploration.m_id.empty()) {
        result.error_code = "invalid_capture_context";
        result.error_message = "BlackFlow capture context is invalid";
        return result;
    }

    const auto directory = m_root / exploration.m_relative_directory;
    const auto stem = page_stem(page_index);
    const auto png_final = directory / (stem + ".png");
    const auto json_final = directory / (stem + ".json");
    const auto context_final = directory / ('.' + stem + ".capture-context.json");
    result.png_relative_path = exploration.m_relative_directory / png_final.filename();
    result.json_relative_path = exploration.m_relative_directory / json_final.filename();
    result.png_committed = is_plain_regular_file(png_final);
    if (attempt < 1 || !m_png_verifier || !analyzer) {
        result.error_code = "invalid_capture_context";
        result.error_message = "BlackFlow capture context is invalid";
        return result;
    }

    if (std::filesystem::exists(png_final) || std::filesystem::exists(json_final)) {
        result.disposition = BlackFlowJsonDisposition::Conflict;
        result.error_code = "page_conflict";
        result.error_message = "A final artifact already exists for this BlackFlow page";
        return result;
    }

    const auto nonce = nonce_hex();
    const auto context_staged = directory / ('.' + stem + '.' + nonce + ".tmp.context.json");
    const auto png_staged = directory / ('.' + stem + '.' + nonce + ".tmp.png");
    const auto json_staged = directory / ('.' + stem + '.' + nonce + ".tmp.json");
    const auto captured_at = utc_timestamp(false);

    try {
        const auto context = json::object {
            { "schema", "maa.black_flow.capture_context" },
            { "version", 1 },
            { "exploration_id", exploration.id() },
            { "page_index", page_index },
            { "captured_at", captured_at },
            { "client_type", std::string(black_flow_client_type_name(exploration.client_type())) },
        };
        const auto context_bytes = context.format() + '\n';
        write_exclusive_file(context_staged, std::as_bytes(std::span(context_bytes)));
        publish_without_replacement(context_staged, context_final);
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::ContextCommitted);
        }

        write_exclusive_file(png_staged, encoded_png);
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::PngStaged);
        }
        const auto staged_dimensions = m_png_verifier(png_staged);
        if (!staged_dimensions || staged_dimensions->width != 1280 || staged_dimensions->height != 720) {
            throw std::runtime_error("staged PNG did not decode as 1280x720");
        }
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::PngValidated);
        }
        publish_without_replacement(png_staged, png_final);
        result.png_committed = true;
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::PngCommitted);
        }

        const auto committed_dimensions = m_png_verifier(png_final);
        if (!committed_dimensions || committed_dimensions->width != 1280 || committed_dimensions->height != 720) {
            throw std::runtime_error("committed PNG did not decode as 1280x720");
        }
        const auto committed_png = read_binary_file(png_final);
        const auto analysis = analyzer(png_final);
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::PageAnalyzed);
        }

        const auto sidecar = make_sidecar(
            m_config,
            exploration,
            page_index,
            captured_at,
            png_final.filename().string(),
            utils::sha256(byte_string(committed_png)),
            analysis);
        const auto document = black_flow_ocr_sidecar_to_json(sidecar);
        const auto json_bytes = document.format() + '\n';
        write_exclusive_file(json_staged, std::as_bytes(std::span(json_bytes)));
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonStaged);
        }
        const auto reparsed_document = json::parse(json_bytes);
        if (!reparsed_document || !parse_black_flow_ocr_sidecar(reparsed_document.value())) {
            throw std::runtime_error("staged sidecar failed strict validation");
        }
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonValidated);
        }
        publish_without_replacement(json_staged, json_final);
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonCommitted);
        }

        remove_if_exists(context_final);
        sync_directory(directory);
        result.disposition = BlackFlowJsonDisposition::FirstCommit;
        result.page_status = sidecar.page_status;
        result.advances_completed_pages = sidecar.page_status != BlackFlowPageStatus::Failed;
        result.should_notify = true;
        return result;
    }
    catch (const std::exception& exception) {
        remove_if_exists(context_staged);
        remove_if_exists(png_staged);
        remove_if_exists(json_staged);
        result.png_committed = is_plain_regular_file(png_final);
        if (std::filesystem::exists(json_final)) {
            remove_if_exists(context_final);
            result.disposition = BlackFlowJsonDisposition::FirstCommit;
            result.should_notify = true;
            if (const auto final_document = json::open(json_final, true, true)) {
                if (const auto final_sidecar = parse_black_flow_ocr_sidecar(final_document.value())) {
                    result.page_status = final_sidecar->page_status;
                    result.advances_completed_pages = result.page_status != BlackFlowPageStatus::Failed;
                }
            }
            return result;
        }
        if (!std::filesystem::exists(png_final)) {
            remove_if_exists(context_final);
        }
        result.error_code = "page_commit_failed";
        result.error_message = exception.what();
        return result;
    }
}

asst::BlackFlowStorePageCommitResult asst::BlackFlowStorePageRepository::reprocess_page(
    const BlackFlowStoreExploration& exploration,
    int page_index,
    int attempt,
    const BlackFlowCommittedPageAnalyzer& analyzer)
{
    return reprocess_page_if_allowed(exploration, page_index, attempt, analyzer, { });
}

asst::BlackFlowStorePageCommitResult asst::BlackFlowStorePageRepository::reprocess_page_if_allowed(
    const BlackFlowStoreExploration& exploration,
    int page_index,
    int attempt,
    const BlackFlowCommittedPageAnalyzer& analyzer,
    const BlackFlowStoreStopRequested& cancel_requested)
{
    std::scoped_lock operation_lock(repository_operation_mutex());
    BlackFlowStorePageCommitResult result { .attempt = attempt };
    if (page_index < 1 || page_index > 3 || exploration.m_id.empty()) {
        result.error_code = "invalid_reprocess_context";
        result.error_message = "BlackFlow reprocessing context is invalid";
        return result;
    }

    const auto directory = m_root / exploration.m_relative_directory;
    const auto stem = page_stem(page_index);
    const auto png_final = directory / (stem + ".png");
    const auto json_final = directory / (stem + ".json");
    const auto context_final = directory / ('.' + stem + ".capture-context.json");
    result.png_relative_path = exploration.m_relative_directory / png_final.filename();
    result.json_relative_path = exploration.m_relative_directory / json_final.filename();
    result.png_committed = is_plain_regular_file(png_final);
    if (attempt < 1 || !m_png_verifier || !analyzer) {
        result.error_code = "invalid_reprocess_context";
        result.error_message = "BlackFlow reprocessing context is invalid";
        return result;
    }
    if (!result.png_committed) {
        result.error_code = "reprocess_artifact_missing";
        result.error_message = "Committed BlackFlow PNG is missing";
        return result;
    }

    const auto nonce = nonce_hex();
    const auto json_staged = directory / ('.' + stem + '.' + nonce + ".tmp.json");
    BlackFlowJsonDisposition published_disposition = BlackFlowJsonDisposition::None;
    bool replacement_advances_completed_page = false;
    try {
        if (!std::filesystem::exists(json_final)) {
            if (!std::filesystem::exists(context_final)) {
                throw std::runtime_error("orphan PNG has no durable capture context");
            }
            const auto context = read_capture_context(context_final);
            if (!context || context->exploration_id != exploration.id() || context->page_index != page_index ||
                context->client_type != exploration.client_type()) {
                throw std::runtime_error("orphan PNG capture context is invalid");
            }

            const auto dimensions = m_png_verifier(png_final);
            if (!dimensions || dimensions->width != 1280 || dimensions->height != 720) {
                throw std::runtime_error("committed PNG did not decode as 1280x720");
            }
            const auto committed_png = read_binary_file(png_final);
            ensure_not_cancelled(cancel_requested);
            const auto analysis = analyzer(png_final);
            ensure_not_cancelled(cancel_requested);
            if (m_fault_injector) {
                m_fault_injector(BlackFlowStoreCommitCheckpoint::PageAnalyzed);
            }
            const auto candidate = make_sidecar(
                m_config,
                exploration,
                page_index,
                context->captured_at,
                png_final.filename().string(),
                utils::sha256(byte_string(committed_png)),
                analysis);
            const auto candidate_document = black_flow_ocr_sidecar_to_json(candidate);
            const auto candidate_bytes = candidate_document.format() + '\n';
            write_exclusive_file(json_staged, std::as_bytes(std::span(candidate_bytes)));
            if (m_fault_injector) {
                m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonStaged);
            }
            const auto reparsed_document = json::parse(candidate_bytes);
            if (!reparsed_document || !parse_black_flow_ocr_sidecar(reparsed_document.value())) {
                throw std::runtime_error("recovered sidecar failed strict validation");
            }
            if (m_fault_injector) {
                m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonValidated);
            }
            ensure_not_cancelled(cancel_requested);
            publish_without_replacement(json_staged, json_final);
            published_disposition = BlackFlowJsonDisposition::FirstCommit;
            if (m_fault_injector) {
                m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonCommitted);
            }
            remove_if_exists(context_final);
            sync_directory(directory);
            result.disposition = BlackFlowJsonDisposition::FirstCommit;
            result.page_status = candidate.page_status;
            result.advances_completed_pages = candidate.page_status != BlackFlowPageStatus::Failed;
            result.should_notify = true;
            return result;
        }

        const auto previous_json_bytes = read_binary_file(json_final);
        const auto previous_document = json::parse(byte_string(previous_json_bytes));
        const auto previous =
            previous_document ? parse_black_flow_ocr_sidecar(previous_document.value()) : std::nullopt;
        if (!previous || previous->exploration_id != exploration.id() || previous->page_index != page_index ||
            previous->client_type != exploration.client_type() ||
            previous->image.file_name != png_final.filename().string()) {
            throw std::runtime_error("committed sidecar identity is invalid");
        }

        const auto dimensions = m_png_verifier(png_final);
        if (!dimensions || dimensions->width != 1280 || dimensions->height != 720) {
            throw std::runtime_error("committed PNG did not decode as 1280x720");
        }
        const auto committed_png = read_binary_file(png_final);
        const auto committed_sha256 = utils::sha256(byte_string(committed_png));
        if (previous->image.sha256 != committed_sha256) {
            throw std::runtime_error("committed PNG hash no longer matches its sidecar");
        }

        ensure_not_cancelled(cancel_requested);
        const auto analysis = analyzer(png_final);
        ensure_not_cancelled(cancel_requested);
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::PageAnalyzed);
        }
        const auto candidate = make_sidecar(
            m_config,
            exploration,
            page_index,
            previous->captured_at,
            previous->image.file_name,
            committed_sha256,
            analysis);
        result.page_status = candidate.page_status;
        if (!is_strict_improvement(previous.value(), candidate)) {
            result.disposition = BlackFlowJsonDisposition::Unchanged;
            return result;
        }
        replacement_advances_completed_page = previous->page_status == BlackFlowPageStatus::Failed &&
                                              candidate.page_status != BlackFlowPageStatus::Failed;

        const auto candidate_document = black_flow_ocr_sidecar_to_json(candidate);
        const auto candidate_bytes = candidate_document.format() + '\n';
        write_exclusive_file(json_staged, std::as_bytes(std::span(candidate_bytes)));
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonStaged);
        }
        const auto reparsed_document = json::parse(candidate_bytes);
        if (!reparsed_document || !parse_black_flow_ocr_sidecar(reparsed_document.value())) {
            throw std::runtime_error("replacement sidecar failed strict validation");
        }
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonValidated);
        }
        ensure_not_cancelled(cancel_requested);
        publish_with_replacement(json_staged, json_final);
        published_disposition = BlackFlowJsonDisposition::Improved;
        if (m_fault_injector) {
            m_fault_injector(BlackFlowStoreCommitCheckpoint::JsonReplaced);
        }

        result.disposition = BlackFlowJsonDisposition::Improved;
        result.advances_completed_pages = replacement_advances_completed_page;
        result.should_notify = true;
        return result;
    }
    catch (const std::exception& exception) {
        remove_if_exists(json_staged);
        result.png_committed = is_plain_regular_file(png_final);
        if (published_disposition != BlackFlowJsonDisposition::None) {
            const auto final_document = json::parse(byte_string(read_binary_file(json_final)));
            const auto final_sidecar =
                final_document ? parse_black_flow_ocr_sidecar(final_document.value()) : std::nullopt;
            if (final_sidecar && final_sidecar->exploration_id == exploration.id() &&
                final_sidecar->page_index == page_index && final_sidecar->client_type == exploration.client_type()) {
                if (published_disposition == BlackFlowJsonDisposition::FirstCommit) {
                    remove_if_exists(context_final);
                }
                result.disposition = published_disposition;
                result.page_status = final_sidecar->page_status;
                result.advances_completed_pages = (published_disposition == BlackFlowJsonDisposition::FirstCommit ||
                                                   replacement_advances_completed_page) &&
                                                  result.page_status != BlackFlowPageStatus::Failed;
                result.should_notify = true;
                return result;
            }
        }
        result.error_code = "page_reprocess_failed";
        result.error_message = exception.what();
        return result;
    }
}

asst::BlackFlowStoreRecoverySummary asst::BlackFlowStorePageRepository::recover_pending_pages(
    const BlackFlowRecoveryPageAnalyzer& analyzer,
    const BlackFlowStoreRecoveryClock& clock,
    const BlackFlowStoreStopRequested& stop_requested,
    const BlackFlowStoreRecoveryObserver& observer)
{
    const auto started_at = clock ? clock() : std::chrono::steady_clock::now();
    const auto time_limit_expired = [&] {
        const auto current = clock ? clock() : std::chrono::steady_clock::now();
        return current - started_at >= BlackFlowStoreRecoveryTimeLimit;
    };
    const auto user_stop_requested = [&] {
        if (!stop_requested) {
            return false;
        }
        try {
            return stop_requested();
        }
        catch (...) {
            return true;
        }
    };
    const auto recovery_cancelled = [&] {
        return time_limit_expired() || user_stop_requested();
    };
    std::vector<RecoveryCandidate> candidates;
    bool scan_time_limit_reached = false;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(
        m_root,
        std::filesystem::directory_options::skip_permission_denied,
        iterator_error);
    const std::filesystem::directory_iterator end;
    while (!iterator_error && iterator != end) {
        if (time_limit_expired()) {
            scan_time_limit_reached = true;
            break;
        }
        if (user_stop_requested()) {
            break;
        }
        const auto entry = *iterator;
        iterator.increment(iterator_error);

        std::error_code status_error;
        const auto status = entry.symlink_status(status_error);
        if (status_error || !std::filesystem::is_directory(status)) {
            continue;
        }
        for (int page_index = 1; page_index <= 3; ++page_index) {
            if (time_limit_expired()) {
                scan_time_limit_reached = true;
                break;
            }
            if (user_stop_requested()) {
                break;
            }
            try {
                if (auto candidate = recovery_candidate(entry.path(), page_index)) {
                    candidates.emplace_back(std::move(candidate.value()));
                }
            }
            catch (const std::exception&) {
                // A corrupt or concurrently modified page must not prevent recovery of the others.
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.captured_at != right.captured_at) {
            return left.captured_at < right.captured_at;
        }
        const auto left_directory = left.relative_directory.generic_string();
        const auto right_directory = right.relative_directory.generic_string();
        if (left_directory != right_directory) {
            return left_directory < right_directory;
        }
        return left.page_index < right.page_index;
    });

    BlackFlowStoreRecoverySummary summary {
        .candidates_found = candidates.size(),
        .candidate_limit_reached = candidates.size() > BlackFlowStoreRecoveryCandidateLimit,
        .time_limit_reached = scan_time_limit_reached,
    };
    for (const auto& candidate : candidates) {
        if (time_limit_expired()) {
            summary.time_limit_reached = true;
            break;
        }
        if (summary.candidates_processed == BlackFlowStoreRecoveryCandidateLimit) {
            break;
        }
        if (user_stop_requested()) {
            break;
        }

        ++summary.candidates_processed;
        try {
            BlackFlowStoreExploration exploration;
            exploration.m_id = candidate.exploration_id;
            exploration.m_client_type = candidate.client_type;
            exploration.m_relative_directory = candidate.relative_directory;
            const BlackFlowCommittedPageAnalyzer cancellable_analyzer = [&](const std::filesystem::path& path) {
                return analyzer(path, recovery_cancelled);
            };
            auto result = reprocess_page_if_allowed(
                exploration,
                candidate.page_index,
                1,
                cancellable_analyzer,
                recovery_cancelled);
            if (time_limit_expired()) {
                summary.time_limit_reached = true;
            }
            if (!result.error_code.empty()) {
                ++summary.candidates_failed;
            }
            if (!result.should_notify) {
                continue;
            }

            ++summary.commits_published;
            if (observer) {
                try {
                    observer(
                        BlackFlowStoreRecoveryCommit {
                            .exploration_id = candidate.exploration_id,
                            .page_index = candidate.page_index,
                            .result = std::move(result),
                        });
                }
                catch (const std::exception&) {
                    // Notification failures cannot undo a durable commit or block later candidates.
                }
            }
        }
        catch (const std::exception&) {
            ++summary.candidates_failed;
        }
    }
    return summary;
}
