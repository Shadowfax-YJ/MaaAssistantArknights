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
    asst::BlackFlowStoreTitleFingerprint value {};
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
        current_time += named_task_interval;
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
        if (task_name == asst::black_flow_store_tasks::DismissRefreshDialog) {
            return dismiss_refresh_result;
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
            return {};
        }
        return analyze_committed_page(path);
    }

    void snapshot_committed(const asst::BlackFlowStoreSnapshot& snapshot) override
    {
        committed_snapshots.push_back(snapshot);
        callback_order.push_back("snapshot-" + std::to_string(snapshot.page_index));
    }

    void
        collection_ended(const asst::BlackFlowStoreCollectionSummary& summary, std::string_view exploration_id) override
    {
        ended_summaries.push_back(summary);
        ended_exploration_ids.emplace_back(exploration_id);
        callback_order.emplace_back("collection-ended");
    }

    bool wait_for(std::chrono::milliseconds duration) override
    {
        current_time += duration;
        return !stop_requested();
    }

    bool stop_requested() const noexcept override
    {
        return force_stop || committed_snapshots.size() >= stop_after_snapshots;
    }

    std::chrono::steady_clock::time_point now() const noexcept override { return current_time; }

    std::vector<asst::BlackFlowStoreObservedPage> observations {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(2) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(3) },
    };
    size_t next_observation = 0;
    std::chrono::steady_clock::time_point current_time {};
    std::chrono::steady_clock::duration observation_interval {};
    std::chrono::steady_clock::duration named_task_interval {};
    size_t open_refresh_failures_remaining = 0;
    size_t observe_task_failures_remaining = 0;
    bool confirm_refresh_result = true;
    bool dismiss_refresh_result = true;
    bool force_stop = false;
    size_t analysis_failures_remaining = 0;
    size_t stop_after_snapshots = std::numeric_limits<size_t>::max();
    size_t encode_calls = 0;
    size_t encode_failures_remaining = 0;
    std::vector<std::string> named_tasks;
    std::vector<size_t> encoded_pages;
    std::vector<std::string> analyzed_pages;
    std::vector<asst::BlackFlowStoreSnapshot> committed_snapshots;
    std::vector<asst::BlackFlowStoreCollectionSummary> ended_summaries;
    std::vector<std::string> ended_exploration_ids;
    std::vector<std::string> callback_order;
};

std::optional<asst::BlackFlowPngDimensions> verify_test_png(const std::filesystem::path& path)
{
    return read_bytes(path).starts_with("valid-png-page-")
               ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
               : std::nullopt;
}
} // namespace

TEST_CASE("BlackFlow state machine completes three distinct store pages with two one-shot refreshes")
{
    asst::BlackFlowStoreStateMachine machine;

    CHECK(machine.start() == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::ObserveStorePage });

    const auto page_1 = fingerprint(1);
    CHECK(
        machine.handle(
            asst::BlackFlowStoreEvent::store_page_observed(
                asst::BlackFlowStorePageClassification::StableInitial,
                page_1)) == asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::CaptureStorePage, 1, 1 });
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
        asst::BlackFlowStoreCommand { asst::BlackFlowStoreCommandKind::Finish });
    CHECK(machine.state().completed_pages == 3);
    CHECK(machine.state().phase == asst::BlackFlowStorePhase::Finished);
    CHECK(machine.state().last_committed_fingerprint == page_3);
}

TEST_CASE("BlackFlow task policy contains only store-internal actions")
{
    const auto confirm = asst::black_flow_store_task_policy(asst::black_flow_store_tasks::ConfirmRefresh);
    REQUIRE(confirm.has_value());
    CHECK(confirm->expected_terminal == asst::black_flow_store_tasks::ConfirmRefresh);
    CHECK(confirm->retry_times == 0);
    REQUIRE(confirm->times_limit.has_value());
    CHECK(confirm->times_limit.value() == 1);

    const auto dismiss = asst::black_flow_store_task_policy(asst::black_flow_store_tasks::DismissRefreshDialog);
    REQUIRE(dismiss.has_value());
    CHECK(dismiss->expected_terminal == asst::black_flow_store_tasks::StorePageReady);
    CHECK(dismiss->retry_times == 0);
    REQUIRE(dismiss->times_limit.has_value());
    CHECK(dismiss->times_limit.value() == 1);

    CHECK_FALSE(asst::black_flow_store_task_policy("MiniGame@BlackFlow@PrepareFreshEntry").has_value());
    CHECK_FALSE(asst::black_flow_store_task_policy("MiniGame@BlackFlow@EnterFreshExploration").has_value());
    CHECK_FALSE(asst::black_flow_store_task_policy("MiniGame@BlackFlow@SafeExit").has_value());
}

TEST_CASE("BlackFlow plugin collector trigger is one-shot until the outer flow finalizes it")
{
    using enum asst::BlackFlowStorePluginInvocation;

    CHECK(
        asst::classify_black_flow_store_plugin_invocation(true, false, true, "MiniGame@BlackFlow@InvestSystem") ==
        CollectStore);
    CHECK(
        asst::classify_black_flow_store_plugin_invocation(true, true, true, "MiniGame@BlackFlow@InvestSystem") == None);
    CHECK(
        asst::classify_black_flow_store_plugin_invocation(false, true, true, "MiniGame@BlackFlow@StartExplore") ==
        FinalizeReturnedToEntry);
    CHECK(asst::classify_black_flow_store_plugin_invocation(false, true, true, "Stop") == FinalizeFailedExit);
    CHECK(
        asst::classify_black_flow_store_plugin_invocation(false, false, true, "MiniGame@BlackFlow@StartExplore") ==
        None);
}

TEST_CASE("BlackFlow store collection commits three pages and returns without outer-flow actions")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
    ScriptedBlackFlowStoreRuntime runtime;
    const auto callback_relative_root = std::filesystem::path("debug") / "roguelike" / "black_flow_store";
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        callback_relative_root,
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::None);
    CHECK(outcome.completed_pages == 3);
    CHECK(outcome.successful_refreshes == 2);
    const std::vector<std::string> expected_tasks {
        "MiniGame@BlackFlow@ObserveStorePage",  "MiniGame@BlackFlow@OpenRefreshDialog",
        "MiniGame@BlackFlow@ConfirmRefresh",    "MiniGame@BlackFlow@ObserveStorePage",
        "MiniGame@BlackFlow@OpenRefreshDialog", "MiniGame@BlackFlow@ConfirmRefresh",
        "MiniGame@BlackFlow@ObserveStorePage",
    };
    CHECK(runtime.named_tasks == expected_tasks);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 2);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 2);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 0);
    CHECK(std::ranges::none_of(runtime.named_tasks, [](const auto& name) {
        return name.find("StartExplore") != std::string::npos || name.find("Abandon") != std::string::npos ||
               name.find("Exit") != std::string::npos || name.find("Invest") != std::string::npos;
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
    CHECK(runtime.ended_summaries.front().status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(runtime.ended_summaries.front().failure == asst::BlackFlowStoreCycleFailure::None);
    CHECK(runtime.ended_summaries.front().completed_pages == 3);
    CHECK(runtime.ended_summaries.front().successful_refreshes == 2);
    CHECK(
        runtime.callback_order ==
        std::vector<std::string> { "snapshot-1", "snapshot-2", "snapshot-3", "collection-ended" });
}

TEST_CASE("BlackFlow capture retries create one PNG and then completes the collection")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations = {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(2) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(3) },
    };
    runtime.encode_failures_remaining = 1;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.completed_pages == 3);
    CHECK(runtime.encode_calls == 4);
    CHECK(runtime.encoded_pages == std::vector<size_t> { 1, 2, 3 });
    REQUIRE(runtime.committed_snapshots.size() == 3);
    CHECK(runtime.committed_snapshots.front().page_index == 1);
    CHECK(runtime.committed_snapshots.front().attempt == 2);
}

TEST_CASE("BlackFlow unstable initial page fails after the twenty-second observation timeout")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
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

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::ObserveStorePage);
    CHECK(outcome.completed_pages == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ObserveStorePage) == 200);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 0);
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().status == asst::BlackFlowStoreCycleStatus::Failed);
}

TEST_CASE("BlackFlow store observation retries a transient task miss within its deadline")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observe_task_failures_remaining = 1;
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Completed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::None);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ObserveStorePage) == 4);
    CHECK(runtime.committed_snapshots.size() == 3);
}

TEST_CASE("BlackFlow opening the refresh dialog is limited and only dismisses a possible modal")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
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

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::OpenRefreshDialog);
    CHECK(outcome.completed_pages == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 3);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 1);
    REQUIRE_FALSE(runtime.named_tasks.empty());
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::DismissRefreshDialog);
    CHECK(runtime.committed_snapshots.size() == 1);
}

TEST_CASE("BlackFlow failed refresh confirmation is one-shot and dismisses its modal")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
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

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::ConfirmRefresh);
    CHECK(outcome.completed_pages == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 1);
    CHECK(runtime.named_tasks.back() == asst::black_flow_store_tasks::DismissRefreshDialog);
}

TEST_CASE("BlackFlow repeated old pages never resubmit refresh and fail without outer actions")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations.resize(26);
    runtime.observations.front() = {
        asst::BlackFlowStorePageClassification::StableInitial,
        fingerprint(1),
    };
    for (size_t index = 1; index < runtime.observations.size(); ++index) {
        runtime.observations[index] = {
            asst::BlackFlowStorePageClassification::StableOld,
            fingerprint(1),
        };
    }
    runtime.observation_interval = std::chrono::seconds(1);
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::ObserveStorePage);
    CHECK(outcome.completed_pages == 1);
    CHECK(outcome.successful_refreshes == 0);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::OpenRefreshDialog) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ObserveStorePage) == 21);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 0);
    CHECK(runtime.committed_snapshots.size() == 1);
}

TEST_CASE("BlackFlow observation poll limit preserves its committed snapshot")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
    ScriptedBlackFlowStoreRuntime runtime;
    runtime.observations.resize(600);
    runtime.observations.front() = {
        asst::BlackFlowStorePageClassification::StableInitial,
        fingerprint(1),
    };
    for (size_t index = 1; index < runtime.observations.size(); ++index) {
        runtime.observations[index] = {
            asst::BlackFlowStorePageClassification::StableOld,
            fingerprint(1),
        };
    }
    asst::BlackFlowStoreCycleAdapter port(
        repository,
        asst::BlackFlowClientType::Official,
        std::filesystem::path("debug") / "roguelike" / "black_flow_store",
        runtime);

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
    CHECK(outcome.failure == asst::BlackFlowStoreCycleFailure::StepLimit);
    CHECK(outcome.completed_pages == 1);
    CHECK(outcome.successful_refreshes == 0);
    CHECK(runtime.committed_snapshots.size() == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::ConfirmRefresh) == 1);
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 0);
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().status == asst::BlackFlowStoreCycleStatus::Failed);
}

TEST_CASE("BlackFlow user stop preserves the last committed snapshot without further UI actions")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
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
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 0);
    REQUIRE(runtime.committed_snapshots.size() == 1);
    const auto exploration_id = runtime.committed_snapshots.front().exploration_id;
    CHECK(read_bytes(temporary.path() / exploration_id / "page-01.png") == "valid-png-page-01");
    REQUIRE(runtime.ended_summaries.size() == 1);
    CHECK(runtime.ended_summaries.front().status == asst::BlackFlowStoreCycleStatus::Stopped);
}

TEST_CASE("BlackFlow failed page processing is limited to three attempts on one immutable PNG")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
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

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Failed);
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
    CHECK(std::ranges::count(runtime.named_tasks, asst::black_flow_store_tasks::DismissRefreshDialog) == 0);
}

TEST_CASE("BlackFlow collector recovers pending pages before starting a new repository directory")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, verify_test_png);
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

    const auto outcome = asst::run_black_flow_store_cycle(port);

    CHECK(outcome.status == asst::BlackFlowStoreCycleStatus::Stopped);
    CHECK(runtime.named_tasks.empty());
    REQUIRE(runtime.committed_snapshots.size() == 1);
    const auto& recovered = runtime.committed_snapshots.front();
    CHECK(recovered.origin == asst::BlackFlowStoreSnapshotOrigin::Recovery);
    CHECK(recovered.exploration_id == interrupted->id());
    CHECK(recovered.page_index == 1);
    CHECK(recovered.page_status == "complete");
    REQUIRE(runtime.ended_exploration_ids.size() == 1);
    CHECK(runtime.ended_exploration_ids.front().empty());
    const auto sidecar = json::open(temporary.path() / pending.json_relative_path, true, true);
    REQUIRE(sidecar.has_value());
    CHECK(sidecar->at("page_status").as_string() == "complete");
}

TEST_CASE("BlackFlow page retry requires the same page to become stable again")
{
    asst::BlackFlowStoreStateMachine machine;
    const auto page = fingerprint(1);

    CHECK(machine.start().kind == asst::BlackFlowStoreCommandKind::ObserveStorePage);
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

    CHECK(command.kind == asst::BlackFlowStoreCommandKind::Stop);
    CHECK(machine.state().phase == asst::BlackFlowStorePhase::Stopped);
    CHECK(machine.state().current_page_attempts == 1);
}

TEST_CASE("BlackFlow refresh failures request only the store-modal cleanup")
{
    asst::BlackFlowStoreStateMachine machine;

    CHECK(machine.start().kind == asst::BlackFlowStoreCommandKind::ObserveStorePage);
    CHECK(
        machine
            .handle(
                asst::BlackFlowStoreEvent::store_page_observed(
                    asst::BlackFlowStorePageClassification::StableInitial,
                    fingerprint(1)))
            .kind == asst::BlackFlowStoreCommandKind::CaptureStorePage);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::page_capture_succeeded()).kind ==
        asst::BlackFlowStoreCommandKind::OpenRefreshDialog);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::refresh_dialog_failed()).kind ==
        asst::BlackFlowStoreCommandKind::DismissRefreshDialog);
    CHECK(machine.state().phase == asst::BlackFlowStorePhase::DismissingRefreshDialog);
    CHECK(
        machine.handle(asst::BlackFlowStoreEvent::refresh_dialog_dismissed()).kind ==
        asst::BlackFlowStoreCommandKind::Stop);
}
