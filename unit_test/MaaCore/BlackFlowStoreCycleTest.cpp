#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Task/Miscellaneous/BlackFlowStoreCycleAdapter.hpp"
#include "Task/Miscellaneous/BlackFlowStoreOrchestrator.hpp"
#include "Task/Miscellaneous/BlackFlowStoreStateMachine.hpp"
#include "Task/Miscellaneous/BlackFlowStoreTaskPolicy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <meojson/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
asst::BlackFlowStoreTitleFingerprint fingerprint(std::uint8_t marker)
{
    asst::BlackFlowStoreTitleFingerprint value { };
    value.front() = marker;
    return value;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        m_path = std::filesystem::temp_directory_path() /
                 ("maa-black-flow-store-cycle-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directory(m_path));
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(m_path); }

    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

asst::BlackFlowStoreConfigContract release_config()
{
    const auto path = std::filesystem::path(MAA_TEST_RESOURCE_DIR) / "black_flow" / "standard_product_names.json";
    const auto document = json::open(path, true, true);
    REQUIRE(document.has_value());
    const auto parsed = asst::parse_black_flow_store_config(document.value());
    REQUIRE(parsed.has_value());
    return parsed.value();
}

std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

asst::BlackFlowStoreSlotsAnalysis complete_page_analysis()
{
    asst::BlackFlowStoreSlotsAnalysis analysis;
    analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : analysis.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    return analysis;
}

class ScriptedBlackFlowStoreRuntime final : public asst::BlackFlowStoreCycleRuntime
{
public:
    bool run_named_task(std::string_view task_name) override
    {
        named_tasks.emplace_back(task_name);
        current_time += task_name == asst::black_flow_store_tasks::SafeExit ? safe_exit_interval : named_task_interval;
        if (task_name == asst::black_flow_store_tasks::EnterFreshExploration) {
            ++enter_fresh_call_count;
            if (enter_fresh_fail_on_call == enter_fresh_call_count) {
                return false;
            }
        }
        if (task_name == asst::black_flow_store_tasks::OpenRefreshDialog && open_refresh_failures_remaining > 0U) {
            --open_refresh_failures_remaining;
            return false;
        }
        if (task_name == asst::black_flow_store_tasks::ObserveStorePage && observe_task_failures_remaining > 0U) {
            --observe_task_failures_remaining;
            return false;
        }
        if (task_name == asst::black_flow_store_tasks::ConfirmRefresh) {
            return confirm_refresh_result;
        }
        return true;
    }

    std::optional<asst::BlackFlowStoreObservedPage>
        observe_store_page(const std::optional<asst::BlackFlowStoreTitleFingerprint>&) override
    {
        if (next_observation == observations.size()) {
            return std::nullopt;
        }
        auto observation = observations[next_observation++];
        current_time += observation_interval;
        return observation;
    }

    std::optional<std::vector<std::uint8_t>> encode_observed_page() override
    {
        ++encode_calls;
        if (encode_failures_remaining > 0U) {
            --encode_failures_remaining;
            return std::nullopt;
        }
        REQUIRE(next_observation > 0);
        const auto page_index = encoded_pages.size() + 1U;
        encoded_pages.push_back(page_index);
        const auto bytes = "valid-png-page-0" + std::to_string(page_index);
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

    asst::BlackFlowStoreSlotsAnalysis analyze_committed_page(const std::filesystem::path& path) override
    {
        analyzed_pages.push_back(path.filename().string());
        CHECK(read_bytes(path).starts_with("valid-png-page-"));
        if (analysis_failures_remaining > 0U) {
            --analysis_failures_remaining;
            auto failed = complete_page_analysis();
            failed.page_status = asst::BlackFlowAnalyzedPageStatus::Failed;
            for (auto& slot : failed.slots) {
                slot.status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
                slot.error_message = "retryable OCR error";
            }
            return failed;
        }
        return complete_page_analysis();
    }

    asst::BlackFlowStoreSlotsAnalysis analyze_recovery_page(
        const std::filesystem::path& path,
        const asst::BlackFlowStoreStopRequested& cancel_requested) override
    {
        if (cancel_requested && cancel_requested()) {
            return { };
        }
        return analyze_committed_page(path);
    }

    void snapshot_committed(const asst::BlackFlowStoreSnapshot& snapshot) override
    {
        committed_snapshots.push_back(snapshot);
        callback_order.push_back("snapshot-" + std::to_string(snapshot.page_index));
    }

    void exploration_ended(const asst::BlackFlowStoreExplorationSummary& summary, std::string_view exploration_id)
        override
    {
        ended_summaries.push_back(summary);
        ended_exploration_ids.emplace_back(exploration_id);
        callback_order.emplace_back("exploration-ended");
    }

    bool wait_for(std::chrono::milliseconds duration) override
    {
        current_time += duration;
        return !stop_requested();
    }

    bool stop_requested() const noexcept override
    {
        return ended_summaries.size() >= stop_after_explorations || committed_snapshots.size() >= stop_after_snapshots;
    }

    std::chrono::steady_clock::time_point now() const noexcept override { return current_time; }

    std::vector<asst::BlackFlowStoreObservedPage> observations {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(2) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(3) },
    };
    size_t next_observation = 0;
    std::chrono::steady_clock::time_point current_time { };
    std::chrono::steady_clock::duration observation_interval { };
    std::chrono::steady_clock::duration named_task_interval { };
    std::chrono::steady_clock::duration safe_exit_interval { };
    size_t open_refresh_failures_remaining = 0;
    size_t observe_task_failures_remaining = 0;
    size_t enter_fresh_call_count = 0;
    size_t enter_fresh_fail_on_call = 0;
    bool confirm_refresh_result = true;
    size_t analysis_failures_remaining = 0;
    size_t stop_after_explorations = std::numeric_limits<size_t>::max();
    size_t stop_after_snapshots = std::numeric_limits<size_t>::max();
    size_t encode_calls = 0;
    size_t encode_failures_remaining = 0;
    std::vector<std::string> named_tasks;
    std::vector<size_t> encoded_pages;
    std::vector<std::string> analyzed_pages;
    std::vector<asst::BlackFlowStoreSnapshot> committed_snapshots;
    std::vector<asst::BlackFlowStoreExplorationSummary> ended_summaries;
    std::vector<std::string> ended_exploration_ids;
    std::vector<std::string> callback_order;
};
} // namespace

TEST_CASE("BlackFlow state machine completes three distinct store pages with two one-shot refreshes")
{
    asst::BlackFlowStoreStateMachine machine;

    CHECK(machine.start() == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::EnterFreshExploration });
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::fresh_exploration_entered()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::BeginExploration });
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::exploration_started()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::ObserveStorePage });

    const auto page_1 = fingerprint(1);
    CHECK(
        machine.handle(
            asst::BlackFlowStoreEvent::store_page_observed(
                asst::BlackFlowStorePageClassification::StableInitial,
                page_1)) == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::CaptureStorePage, 1, 1 });
    CHECK(machine.state().current_page_attempts == 1);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::page_capture_succeeded()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::OpenRefreshDialog });
    CHECK(machine.state().completed_pages == 1);
    CHECK(machine.state().last_committed_fingerprint == page_1);

    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::refresh_dialog_opened()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::ConfirmRefresh });
    CHECK(machine.state().refresh_submission_latched);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::refresh_confirmation_succeeded()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::ObserveStorePage });

    const auto page_2 = fingerprint(2);
    CHECK(
        machine.handle(
            asst::BlackFlowStoreEvent::store_page_observed(
                asst::BlackFlowStorePageClassification::StableNew,
                page_2)) == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::CaptureStorePage, 2, 1 });
    CHECK(machine.state().successful_refreshes == 1);
    CHECK_FALSE(machine.state().refresh_submission_latched);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::page_capture_succeeded()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::OpenRefreshDialog });

    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::refresh_dialog_opened()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::ConfirmRefresh });
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::refresh_confirmation_succeeded()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::ObserveStorePage });

    const auto page_3 = fingerprint(3);
    CHECK(
        machine.handle(
            asst::BlackFlowStoreEvent::store_page_observed(
                asst::BlackFlowStorePageClassification::StableNew,
                page_3)) == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::CaptureStorePage, 3, 1 });
    CHECK(machine.state().successful_refreshes == 2);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::page_capture_succeeded()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::SafeExit });

    CHECK(machine.state().completed_pages == 3);
    CHECK(machine.state().current_page_attempts == 0);
    CHECK(machine.state().last_committed_fingerprint == page_3);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::safe_exit_succeeded()) ==
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::Finish });
    CHECK(machine.state().phase == asst::BlackFlowStorePhase::Finished);
}

TEST_CASE("BlackFlow refresh confirmation task policy permits one recognition and one action")
{
    const auto policy = asst::black_flow_store_task_policy(asst::black_flow_store_tasks::ConfirmRefresh);

    REQUIRE(policy.has_value());
    CHECK(policy->expected_terminal == asst::black_flow_store_tasks::ConfirmRefresh);
    CHECK(policy->retry_times == 0);
    REQUIRE(policy->times_limit.has_value());
    CHECK(policy->times_limit.value() == 1);
}

TEST_CASE("BlackFlow production orchestration commits three pages and safely exits without investment")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    const auto callback_relative_root = std::filesystem::path("debug") / "roguelike" / "black_flow_store";
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        callback_relative_root,
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.completed_pages == 3);
    CHECK(outcome.successful_refreshes == 2);

    const std::vector<std::string> expected_tasks {
        "MiniGame@BlackFlow@EnterFreshExploration",
        "MiniGame@BlackFlow@ObserveStorePage",
        "MiniGame@BlackFlow@OpenRefreshDialog",
        "MiniGame@BlackFlow@ConfirmRefresh",
        "MiniGame@BlackFlow@ObserveStorePage",
        "MiniGame@BlackFlow@OpenRefreshDialog",
        "MiniGame@BlackFlow@ConfirmRefresh",
        "MiniGame@BlackFlow@ObserveStorePage",
        "MiniGame@BlackFlow@SafeExit",
    };
    CHECK(runtime.named_tasks == expected_tasks);
    CHECK(std::ranges::count(runtime.named_tasks, "MiniGame@BlackFlow@ConfirmRefresh") == 2);
    CHECK(std::ranges::count(runtime.named_tasks, "MiniGame@BlackFlow@SafeExit") == 1);
    CHECK(std::ranges::none_of(runtime.named_tasks, [](const auto& name) {
        return name.find("Invest") != std::string::npos;
    }));

    CHECK(runtime.encoded_pages == std::vector<size_t> { 1, 2, 3 });
    CHECK(runtime.analyzed_pages == std::vector<std::string> { "page-01.png", "page-02.png", "page-03.png" });
    REQUIRE(runtime.committed_snapshots.size() == 3);
    REQUIRE(runtime.ended_exploration_ids.size() == 1);
    const auto& exploration_id = runtime.ended_exploration_ids.front();
    REQUIRE_FALSE(exploration_id.empty());
    for (size_t index = 0; index < runtime.committed_snapshots.size(); ++index) {
        const auto page_index = index + 1U;
        const auto stem = "page-0" + std::to_string(page_index);
        const auto& snapshot = runtime.committed_snapshots[index];
        CHECK(snapshot.exploration_id == exploration_id);
        CHECK(snapshot.page_index == page_index);
        CHECK(snapshot.attempt == 1);
        CHECK(snapshot.page_status == "complete");
        CHECK(
            std::filesystem::path(snapshot.png_relative_path) ==
            callback_relative_root / exploration_id / (stem + ".png"));
        CHECK(
            std::filesystem::path(snapshot.json_relative_path) ==
            callback_relative_root / exploration_id / (stem + ".json"));

        const auto png = temporary.path() / exploration_id / (stem + ".png");
        const auto sidecar = temporary.path() / exploration_id / (stem + ".json");
        CHECK(read_bytes(png) == "valid-png-page-0" + std::to_string(page_index));
        const auto sidecar_document = json::open(sidecar, true, true);
        REQUIRE(sidecar_document.has_value());
        const auto parsed_sidecar = asst::parse_black_flow_ocr_sidecar(sidecar_document.value());
        REQUIRE(parsed_sidecar.has_value());
        CHECK(parsed_sidecar->exploration_id == exploration_id);
        CHECK(parsed_sidecar->page_index == static_cast<int>(page_index));
        CHECK(parsed_sidecar->image.file_name == stem + ".png");
    }

    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().completed_pages == 3);
    CHECK(runtime.ended_summaries.front().successful_refreshes == 2);
    CHECK(runtime.ended_summaries.front().safely_exited);
    CHECK(
        runtime.callback_order ==
        std::vector<std::string> { "snapshot-1", "snapshot-2", "snapshot-3", "exploration-ended" });
}

TEST_CASE("BlackFlow capture retries create a PNG until one is committed and then reprocess that PNG")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.encode_failures_remaining = 1;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.completed_pages == 1);
    CHECK(runtime.encode_calls == 2);
    CHECK(runtime.encoded_pages == std::vector<size_t> { 1 });
    REQUIRE(runtime.committed_snapshots.size() == 1);
    CHECK(runtime.committed_snapshots.front().page_index == 1);
    CHECK(runtime.committed_snapshots.front().attempt == 2);
    const auto exploration_id = runtime.committed_snapshots.front().exploration_id;
    CHECK(read_bytes(temporary.path() / exploration_id / "page-01.png") == "valid-png-page-01");
}

TEST_CASE("BlackFlow unstable initial page times out after twenty seconds and exits safely")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path&) -> std::optional<asst::BlackFlowPngDimensions> {
        return std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations.assign(
        250,
        asst::BlackFlowStoreObservedPage {
            asst::BlackFlowStorePageClassification::Unstable,
            fingerprint(1),
        });
    runtime.observation_interval = std::chrono::milliseconds(100);
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::ObserveStorePage);
    CHECK(outcome.completed_pages == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ObserveStorePage) == 200);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().safely_exited);
}

TEST_CASE("BlackFlow store observation retries a transient task miss within its deadline")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.observe_task_failures_remaining = 1;
    runtime.confirm_refresh_result = false;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.completed_pages == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ObserveStorePage) == 2);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    CHECK(runtime.committed_snapshots.front().page_index == 1);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
}

TEST_CASE("BlackFlow opening the refresh dialog is limited to three attempts within ten seconds")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.open_refresh_failures_remaining = 3;
    runtime.named_task_interval = std::chrono::seconds(3);
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::OpenRefreshDialog);
    CHECK(outcome.completed_pages == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 3);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 0);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    CHECK(runtime.committed_snapshots.front().page_index == 1);
}

TEST_CASE("BlackFlow session resets page and refresh counts across two safely exited cycles")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations.clear();
    runtime.observations.resize(6);
    for (size_t index = 0; index < runtime.observations.size(); ++index) {
        auto& observation = runtime.observations[index];
        observation.classification = index % 3U == 0U ? asst::BlackFlowStorePageClassification::StableInitial
                                                      : asst::BlackFlowStorePageClassification::StableNew;
        observation.title_fingerprint.front() = static_cast<std::uint8_t>(index + 1U);
    }
    runtime.stop_after_explorations = 2;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_session(port);

    CHECK(outcome.status == asst::BlackFlowStoreSessionStatus::Stopped);
    CHECK(outcome.failure == asst::BlackFlowStoreSessionFailure::None);
    CHECK(outcome.safely_exited_cycles == 2);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::PrepareFreshEntry) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::EnterFreshExploration) == 2);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 4);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::SafeExit) == 2);
    CHECK(std::ranges::none_of(runtime.named_tasks, [](const auto& name) {
        return name.find("Invest") != std::string::npos;
    }));

    REQUIRE(runtime.ended_summaries.size() == 2);
    for (const auto& summary : runtime.ended_summaries) {
        CHECK(summary.completed_pages == 3);
        CHECK(summary.successful_refreshes == 2);
        CHECK(summary.safely_exited);
    }
    REQUIRE(runtime.ended_exploration_ids.size() == 2);
    CHECK(runtime.ended_exploration_ids[0] != runtime.ended_exploration_ids[1]);
    REQUIRE(runtime.committed_snapshots.size() == 6);
    CHECK(
        std::vector<size_t> {
            runtime.committed_snapshots[0].page_index,
            runtime.committed_snapshots[1].page_index,
            runtime.committed_snapshots[2].page_index,
            runtime.committed_snapshots[3].page_index,
            runtime.committed_snapshots[4].page_index,
            runtime.committed_snapshots[5].page_index,
        } == std::vector<size_t> { 1, 2, 3, 1, 2, 3 });
}

TEST_CASE("BlackFlow failed entry after a completed cycle ends without reusing its exploration identity")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.enter_fresh_fail_on_call = 2;
    runtime.stop_after_explorations = 2;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_session(port);

    CHECK(outcome.status == asst::BlackFlowStoreSessionStatus::Stopped);
    CHECK(outcome.failure == asst::BlackFlowStoreSessionFailure::None);
    CHECK(outcome.safely_exited_cycles == 2);
    REQUIRE(runtime.ended_summaries.size() == 2);
    CHECK(runtime.ended_summaries[0].completed_pages == 3);
    CHECK(runtime.ended_summaries[0].safely_exited);
    CHECK(runtime.ended_summaries[1].completed_pages == 0);
    CHECK(runtime.ended_summaries[1].successful_refreshes == 0);
    CHECK(runtime.ended_summaries[1].safely_exited);
    REQUIRE(runtime.ended_exploration_ids.size() == 2);
    CHECK_FALSE(runtime.ended_exploration_ids[0].empty());
    CHECK(runtime.ended_exploration_ids[1].empty());
    CHECK(runtime.committed_snapshots.size() == 3);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::SafeExit) == 2);
}

TEST_CASE("BlackFlow session recovers pending pages before taking over the game UI")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    const auto interrupted = repository.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(interrupted.has_value());
    auto retryable = complete_page_analysis();
    retryable.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
    retryable.slots.front().status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
    retryable.slots.front().error_message = "retryable OCR error";
    const std::string png = "valid-png-page-01";
    const auto pending = repository.capture_page(
        interrupted.value(),
        1,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return retryable; });
    REQUIRE(pending.disposition == asst::BlackFlowJsonDisposition::FirstCommit);

    ScriptedBlackFlowStoreRuntime runtime;
    runtime.stop_after_snapshots = 1;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_session(port);

    CHECK(outcome.status == asst::BlackFlowStoreSessionStatus::Stopped);
    CHECK(outcome.safely_exited_cycles == 0);
    CHECK(runtime.named_tasks.empty());
    REQUIRE(runtime.committed_snapshots.size() == 1);
    const auto& recovered = runtime.committed_snapshots.front();
    CHECK(recovered.origin == asst::BlackFlowStoreSnapshotOrigin::Recovery);
    CHECK(recovered.exploration_id == interrupted->id());
    CHECK(recovered.page_index == 1);
    CHECK(recovered.page_status == "complete");
    const auto sidecar = json::open(temporary.path() / pending.json_relative_path, true, true);
    REQUIRE(sidecar.has_value());
    CHECK(sidecar->at("page_status").as_string() == "complete");
}

TEST_CASE("BlackFlow startup takeover stops after thirty seconds without starting a cycle")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path&) -> std::optional<asst::BlackFlowPngDimensions> {
        return std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.named_task_interval = std::chrono::seconds(31);
    runtime.stop_after_explorations = 1;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_session(port);

    CHECK(outcome.status == asst::BlackFlowStoreSessionStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreSessionFailure::PrepareFreshEntry);
    CHECK(outcome.safely_exited_cycles == 0);
    CHECK(
        runtime.named_tasks ==
        std::vector<std::string> { std::string(asst::black_flow_store_tasks::PrepareFreshEntry) });
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK_FALSE(runtime.ended_summaries.front().safely_exited);
    CHECK(runtime.callback_order == std::vector<std::string> { "exploration-ended" });
}

TEST_CASE("BlackFlow unconfirmed safe exit emits the ended callback and fails after thirty seconds")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.open_refresh_failures_remaining = 3;
    runtime.safe_exit_interval = std::chrono::seconds(31);
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::SafeExit);
    CHECK(outcome.completed_pages == 1);
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK_FALSE(runtime.ended_summaries.front().safely_exited);
    CHECK(runtime.callback_order == std::vector<std::string> { "snapshot-1", "exploration-ended" });
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
}

TEST_CASE("BlackFlow repeated old pages never resubmit refresh and time out safely")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations.clear();
    runtime.observations.resize(26);
    runtime.observations.front().classification = asst::BlackFlowStorePageClassification::StableInitial;
    runtime.observations.front().title_fingerprint.front() = 1;
    for (size_t index = 1; index < runtime.observations.size(); ++index) {
        runtime.observations[index].classification = asst::BlackFlowStorePageClassification::StableOld;
        runtime.observations[index].title_fingerprint.front() = 1;
    }
    runtime.observation_interval = std::chrono::seconds(1);
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::ObserveStorePage);
    CHECK(outcome.completed_pages == 1);
    CHECK(outcome.successful_refreshes == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ObserveStorePage) == 21);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    CHECK(runtime.committed_snapshots.front().page_index == 1);
}

TEST_CASE("BlackFlow operation step limit preserves its snapshot and still exits safely")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations.resize(600);
    runtime.observations.front().classification = asst::BlackFlowStorePageClassification::StableInitial;
    runtime.observations.front().title_fingerprint.front() = 1;
    for (size_t index = 1; index < runtime.observations.size(); ++index) {
        runtime.observations[index].classification = asst::BlackFlowStorePageClassification::StableOld;
        runtime.observations[index].title_fingerprint.front() = 1;
    }
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::StepLimit);
    CHECK(outcome.completed_pages == 1);
    CHECK(outcome.successful_refreshes == 0);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    CHECK(runtime.committed_snapshots.front().page_index == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::SafeExit) == 1);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().safely_exited);
    CHECK(runtime.callback_order == std::vector<std::string> { "snapshot-1", "exploration-ended" });
}

TEST_CASE("BlackFlow unavailable refresh preserves the committed page and exits safely")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.confirm_refresh_result = false;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::ConfirmRefresh);
    CHECK(outcome.completed_pages == 1);
    CHECK(outcome.successful_refreshes == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 1);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    const auto exploration_id = runtime.committed_snapshots.front().exploration_id;
    CHECK(read_bytes(temporary.path() / exploration_id / "page-01.png") == "valid-png-page-01");
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().safely_exited);
}

TEST_CASE("BlackFlow user stop preserves the last committed snapshot without further UI actions")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.stop_after_snapshots = 1;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Stopped);
    CHECK(outcome.completed_pages == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::SafeExit) == 0);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    const auto exploration_id = runtime.committed_snapshots.front().exploration_id;
    CHECK(read_bytes(temporary.path() / exploration_id / "page-01.png") == "valid-png-page-01");
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK_FALSE(runtime.ended_summaries.front().safely_exited);
}

TEST_CASE("BlackFlow failed page processing is limited to three attempts on one immutable PNG")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png-page-")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
    };
    runtime.analysis_failures_remaining = 3;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::CaptureStorePage);
    CHECK(outcome.completed_pages == 0);
    CHECK(runtime.encode_calls == 1);
    CHECK(runtime.encoded_pages == std::vector<size_t> { 1 });
    CHECK(runtime.analyzed_pages == std::vector<std::string> { "page-01.png", "page-01.png", "page-01.png" });
    REQUIRE(runtime.committed_snapshots.size() == 1);
    const auto& failed_snapshot = runtime.committed_snapshots.front();
    CHECK(failed_snapshot.attempt == 1);
    CHECK(failed_snapshot.page_status == "failed");
    CHECK(read_bytes(temporary.path() / failed_snapshot.exploration_id / "page-01.png") == "valid-png-page-01");
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::SafeExit);
}

TEST_CASE("BlackFlow page retry requires the same page to be stable again")
{
    asst::BlackFlowStoreStateMachine machine;
    const auto page = fingerprint(1);

    CHECK(machine.start().kind == asst::BlackFlowStoreCommandKind::EnterFreshExploration);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::fresh_exploration_entered()).kind ==
        asst::BlackFlowStoreCommandKind::BeginExploration);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::exploration_started()).kind ==
        asst::BlackFlowStoreCommandKind::ObserveStorePage);
    CHECK(
        machine.handle(
            asst::BlackFlowStoreEvent::store_page_observed(
                asst::BlackFlowStorePageClassification::StableInitial,
                page)) == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::CaptureStorePage, 1, 1 });
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::page_capture_failed()).kind ==
        asst::BlackFlowStoreCommandKind::ObserveStorePage);

    const auto command = machine.handle(
        asst::BlackFlowStoreEvent::store_page_observed(asst::BlackFlowStorePageClassification::Unstable, page));

    CHECK(command.kind == asst::BlackFlowStoreCommandKind::SafeExit);
    CHECK(machine.state().current_page_attempts == 1);
}

TEST_CASE("BlackFlow failed fresh-entry action transitions to safe exit")
{
    asst::BlackFlowStoreStateMachine machine;

    CHECK(machine.start().kind == asst::BlackFlowStoreCommandKind::EnterFreshExploration);
    const auto command = machine.handle(asst::BlackFlowStoreEvent::fresh_exploration_failed());

    CHECK(command.kind == asst::BlackFlowStoreCommandKind::SafeExit);
    CHECK(machine.state().phase == asst::BlackFlowStorePhase::SafelyExiting);
}
