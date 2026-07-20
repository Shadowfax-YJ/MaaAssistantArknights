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
        return true;
    }

    std::optional<asst::BlackFlowStoreObservedPage>
        observe_store_page(const std::optional<asst::BlackFlowStoreTitleFingerprint>&) override
    {
        if (next_observation == observations.size()) {
            return std::nullopt;
        }
        return observations[next_observation++];
    }

    std::optional<std::vector<std::uint8_t>> encode_observed_page() override
    {
        REQUIRE(next_observation > 0);
        const auto page_index = next_observation;
        encoded_pages.push_back(page_index);
        const auto bytes = "valid-png-page-0" + std::to_string(page_index);
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

    asst::BlackFlowStoreSlotsAnalysis analyze_committed_page(const std::filesystem::path& path) override
    {
        analyzed_pages.push_back(path.filename().string());
        CHECK(read_bytes(path) == "valid-png-page-0" + std::to_string(analyzed_pages.size()));
        return complete_page_analysis();
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

    bool stop_requested() const noexcept override { return false; }

    std::vector<asst::BlackFlowStoreObservedPage> observations {
        { asst::BlackFlowStorePageClassification::StableInitial, fingerprint(1) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(2) },
        { asst::BlackFlowStorePageClassification::StableNew, fingerprint(3) },
    };
    size_t next_observation = 0;
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
