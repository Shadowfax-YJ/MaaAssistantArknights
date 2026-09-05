#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include <meojson/json.hpp>

#include "Task/BattleAutoSkillRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowModel.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMovementRecognition.h"
#include "Task/Roguelike/BlackFlow/BlackFlowObservation.h"
#include "Task/Roguelike/BlackFlow/BlackFlowOcrFragmentRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationCollectionRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowBattleRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowCollectionPopup.h"
#include "Task/Roguelike/BlackFlow/BlackFlowEncounterRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowDiagnosticTimeline.h"
#include "Task/Roguelike/BlackFlow/BlackFlowDeterministicPrediction.h"
#include "Task/Roguelike/BlackFlow/BlackFlowFailureRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowInventoryRefresh.h"
#include "Task/Roguelike/BlackFlow/BlackFlowInventoryRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowLifecycleRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowOcrMatcher.h"
#include "Task/Roguelike/BlackFlow/BlackFlowPlanner.h"
#include "Task/Roguelike/BlackFlow/BlackFlowPlannerRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowPolicy.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRevealSemantics.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRunLog.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRunArchive.h"
#include "Task/Roguelike/BlackFlow/BlackFlowStartRewardRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTaskPort.h"
#include "Task/Roguelike/RoguelikeBattleStageNameRules.h"
#include "Vision/Roguelike/BlackFlow/BlackFlowFloor.h"
#include "Vision/Roguelike/BlackFlow/IdealDomainStability.h"
#include "Vision/Roguelike/BlackFlow/BlackFlowOptionHeaderRules.h"
#include "Vision/Roguelike/BlackFlow/NodeOcrRules.h"
#include "Vision/Roguelike/BlackFlow/BlackFlowTopologyMatcher.h"

using namespace asst::blackflow;
using namespace asst::blackflow::perception;

namespace
{
class ScopedDirectoryCleanup
{
public:
    explicit ScopedDirectoryCleanup(std::filesystem::path path) : m_path(std::move(path)) {}
    ~ScopedDirectoryCleanup()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

private:
    std::filesystem::path m_path;
};

RunResources resources_authorizing(MovementKind movement)
{
    RunResources resources;
    if (movement != MovementKind::Walk) {
        resources.movement_charges.emplace(movement, 1);
    }
    return resources;
}
} // namespace

TEST_CASE("BlackFlow ideal source remains fixed within one map generation")
{
    SameMapIdealDomainState<GridPosition> state;
    state.observe("recognized", GridPosition { 2, 3 }, { GridPosition { 2, 2 }, GridPosition { 2, 3 } });
    state.observe("recognized", GridPosition { 2, 2 }, { GridPosition { 2, 1 }, GridPosition { 2, 2 } });

    REQUIRE(state.source() == GridPosition { 2, 3 });
    REQUIRE(state.domain() == std::vector<GridPosition> { GridPosition { 2, 2 }, GridPosition { 2, 3 } });

    state.reset();
    REQUIRE_FALSE(state.source().has_value());
    REQUIRE(state.domain().empty());
}

TEST_CASE("Roguelike battle stage name fuzzy matching normalizes a unique OCR error")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::vector<std::string> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(
             repository_root / "resource/roguelike/BlackFlow/autopilot")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto stage = json::open(entry.path());
        REQUIRE(stage.has_value());
        if (stage->is_array()) {
            for (const auto& item : stage->as_array()) {
                candidates.emplace_back(item.at("stage_name").as_string());
            }
        }
        else {
            candidates.emplace_back(stage->at("stage_name").as_string());
        }
    }
    REQUIRE(std::ranges::find(candidates, "湖中魇") != candidates.end());

    const auto match = asst::resolve_roguelike_battle_stage_name("湖中魔", candidates);

    REQUIRE(match.has_value());
    REQUIRE(*match == "湖中魇");
}

TEST_CASE("Roguelike battle stage name fuzzy matching refuses an ambiguous OCR error")
{
    const std::vector<std::string> candidates { "湖中魇", "湖中鬼" };

    REQUIRE_FALSE(asst::resolve_roguelike_battle_stage_name("湖中魔", candidates).has_value());
}

TEST_CASE("BlackFlow replay log enforces ordered timestamped JPEG events")
{
    REQUIRE(to_string(RunLogLevel::Trace) == "TRACE");
    REQUIRE(to_string(RunLogLevel::Warning) == "WARN");
    REQUIRE(process_action_changes_ui("ClickSelf"));
    REQUIRE(process_action_changes_ui("Swipe"));
    REQUIRE_FALSE(process_action_changes_ui("DoNothing"));

    std::vector<json::value> events {
        json::object {
            { "schema_version", BlackFlowRunLogSchemaVersion },
            { "sequence", 1 },
            { "timestamp", "2026-08-30T04:17:28.577Z" },
            { "elapsed_ms", 0 },
            { "level", "INFO" },
            { "action", "run.started" },
        },
        json::object {
            { "schema_version", BlackFlowRunLogSchemaVersion },
            { "sequence", 2 },
            { "timestamp", "2026-08-30T04:17:29.010Z" },
            { "elapsed_ms", 433 },
            { "level", "INFO" },
            { "action", "map.observed" },
            { "image", json::object { { "path", "images/000002-map-observed.jpg" } } },
        },
    };
    std::string error;
    REQUIRE(validate_run_log_replay_stream(events, &error));

    events[1]["sequence"] = 3;
    REQUIRE_FALSE(validate_run_log_replay_stream(events, &error));
    REQUIRE(error.find("continuous") != std::string::npos);

    events[1]["sequence"] = 2;
    events[1]["image"]["path"] = "images/map.png";
    REQUIRE_FALSE(validate_run_log_replay_stream(events, &error));
    REQUIRE(error.find("JPEG") != std::string::npos);
}

TEST_CASE("BlackFlow run log stores only stable semantic screenshots")
{
    REQUIRE(
        run_log_image_capture_mode("process.BlackFlow@Roguelike@MapPrepare-Ready", "started", true, false) ==
        RunLogImageCaptureMode::Omit);
    REQUIRE(
        run_log_image_capture_mode("process.BlackFlow@Roguelike@MovementInventoryClose", "completed", false, true) ==
        RunLogImageCaptureMode::Omit);
    REQUIRE(
        run_log_image_capture_mode("process.BlackFlow@Roguelike@Move", "failed", false, true) ==
        RunLogImageCaptureMode::StableSnapshot);

    REQUIRE(
        run_log_image_capture_mode("move.confirm", "completed", false, true) ==
        RunLogImageCaptureMode::StableSnapshot);
    REQUIRE(
        run_log_image_capture_mode("move.preview", "completed", true, true) ==
        RunLogImageCaptureMode::ProvidedObservation);
    REQUIRE(
        run_log_image_capture_mode("movement.processing-item.select", "started", false, true) ==
        RunLogImageCaptureMode::ImmediateSnapshot);

    REQUIRE(run_log_frame_is_stable(0.0));
    REQUIRE(run_log_frame_is_stable(BlackFlowRunLogStableFrameMaximumMeanDifference));
    REQUIRE_FALSE(run_log_frame_is_stable(BlackFlowRunLogStableFrameMaximumMeanDifference + 0.01));
}

TEST_CASE("BlackFlow eerie merchant capture keeps the full page while extending the scrolled shelf")
{
    const std::vector<EerieStoreStitchAnchor> top_goods {
        { "一次性喷气背包", 612, 181 },
        { "堡垒协议招募券", 824, 181 },
        { "波纹之手", 612, 390 },
        { "小格兰法洛", 824, 390 },
    };
    const std::vector<EerieStoreStitchAnchor> bottom_goods {
        { "波纹之手", 612, 181 },
        { "小格兰法洛", 824, 181 },
        { "医者-自医", 612, 390 },
        { "医者-新典训", 824, 390 },
    };

    const auto offset = eerie_store_scroll_offset(top_goods, bottom_goods);
    REQUIRE(offset == 209);

    const auto layout = eerie_store_stitch_layout(1280, 720, asst::Rect { 395, 159, 857, 401 }, *offset);
    REQUIRE(layout.has_value());
    REQUIRE(layout->output_width == 1280);
    REQUIRE(layout->output_height == 929);
    REQUIRE(layout->base_source == asst::Rect { 0, 0, 1280, 720 });
    REQUIRE(layout->base_destination == asst::Rect { 0, 0, 1280, 720 });
    REQUIRE(layout->continuation_source == asst::Rect { 395, 159, 885, 561 });
    REQUIRE(layout->continuation_destination == asst::Rect { 395, 368, 885, 561 });

    REQUIRE(
        eerie_store_scroll_offset(
            std::vector<EerieStoreStitchAnchor> {
                { "第一排甲", 612, 181 },
                { "第二排甲", 612, 390 },
            },
            std::vector<EerieStoreStitchAnchor> {
                { "第二排乙", 612, 181 },
                { "第三排乙", 612, 390 },
            }) == 209);
    REQUIRE_FALSE(
        eerie_store_scroll_offset(
            std::vector<EerieStoreStitchAnchor> { { "第一排甲", 612, 181 } },
            std::vector<EerieStoreStitchAnchor> { { "医者-地缘策略", 1036, 390 } })
            .has_value());

    // The overlapping row can contain no purchasable goods at all. Capture
    // alignment must still be able to use unfiltered UI text such as the
    // repeated price labels instead of depending on the purchase whitelist.
    REQUIRE(
        eerie_store_scroll_offset(
            std::vector<EerieStoreStitchAnchor> {
                { "价格", 560, 310 },
                { "价格", 765, 310 },
                { "价格", 560, 522 },
                { "价格", 765, 522 },
            },
            std::vector<EerieStoreStitchAnchor> {
                { "价格", 560, 310 },
                { "价格", 765, 310 },
                { "价格", 560, 522 },
                { "价格", 765, 522 },
            }) == 212);
}

TEST_CASE("BlackFlow move preview recognition accepts stable semantics while pixels animate")
{
    using enum MovePreviewFrameState;

    REQUIRE_FALSE(move_preview_observation_is_stable(Missing, Missing, true));
    REQUIRE_FALSE(move_preview_observation_is_stable(Missing, Reachable, true));
    REQUIRE_FALSE(move_preview_observation_is_stable(Reachable, Blocked, true));
    REQUIRE_FALSE(move_preview_observation_is_stable(Reachable, Reachable, false));
    REQUIRE(move_preview_observation_is_stable(Reachable, Reachable, true));
    REQUIRE(move_preview_observation_is_stable(Blocked, Blocked, true));
    REQUIRE(MovePreviewStabilityInterval <= 200);
    REQUIRE(MovePreviewStabilityAttempts * MovePreviewStabilityInterval >= 3000);
}

TEST_CASE("BlackFlow battle preview map cache separates stable and shifted captured frames")
{
    // 2026-09-02 runs 191442 and 195916: 44 unchanged preview-close pairs peaked
    // at 3.192 in the map crop, while the smallest four-floor shift measured 6.904.
    REQUIRE(battle_preview_map_frame_is_unchanged(3.192));
    REQUIRE(battle_preview_map_frame_is_unchanged(BattlePreviewMapMaximumMeanDifference));
    REQUIRE_FALSE(battle_preview_map_frame_is_unchanged(6.904));
    REQUIRE_FALSE(battle_preview_map_frame_is_unchanged(-1.0));
}

TEST_CASE("BlackFlow move preview keeps one panel open until its semantic content is stable")
{
    MovePreviewSemanticStability stability;

    // The captured failure sequence was already pixel-stable while the sliding title was still
    // truncated.  An unrecognized frame must not complete the preview, and a complete identity
    // must agree on two consecutive frames before the caller is allowed to act on it.
    REQUIRE_FALSE(stability.observe(std::nullopt));
    REQUIRE_FALSE(stability.observe("秘境行商|商店|-1"));
    REQUIRE(stability.observe("秘境行商|商店|-1"));

    REQUIRE_FALSE(stability.observe("不期而遇|事件|-1"));
    REQUIRE_FALSE(stability.observe(std::nullopt));
    REQUIRE_FALSE(stability.observe("不期而遇|事件|-1"));
    REQUIRE(stability.observe("不期而遇|事件|-1"));
}

TEST_CASE("BlackFlow encounter option clicks wait until scroll inertia settles")
{
    EncounterOptionPositionStability stability;

    // Captured from the failed 无人商店 run: the third option was first located at
    // y=579, but kept moving to y=524 and finally y=444 after the swipe task returned.
    REQUIRE_FALSE(stability.observe(579));
    REQUIRE_FALSE(stability.observe(524));
    REQUIRE_FALSE(stability.observe(444));
    REQUIRE(stability.observe(443));

    stability.reset();
    REQUIRE_FALSE(stability.observe(443));
    REQUIRE_FALSE(stability.observe(std::nullopt));
    REQUIRE_FALSE(stability.observe(443));
    REQUIRE(stability.observe(442));

    REQUIRE(encounter_option_navigation_should_continue(0));
    REQUIRE(encounter_option_navigation_should_continue(EncounterOptionNavigationAttemptLimit - 1));
    REQUIRE_FALSE(encounter_option_navigation_should_continue(EncounterOptionNavigationAttemptLimit));
}

TEST_CASE("BlackFlow run log preserves floor-encoded 64-bit node ids")
{
    constexpr std::uint64_t node = 281475043819522ULL;
    const auto parsed = run_log_node_id(json::value(node));
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == node);
    REQUIRE_FALSE(run_log_node_id(json::value("281475043819522")).has_value());
    REQUIRE_FALSE(run_log_node_id(json::value(-1)).has_value());
}

TEST_CASE("BlackFlow run log does not materialize the unvisited floor entrance")
{
    constexpr std::uint64_t floor_entrance = 281475010265090ULL;
    constexpr std::uint64_t visited_node = 281474976710658ULL;
    const json::object state {
        { "current_node", floor_entrance },
        { "visited_nodes", json::array { visited_node } },
    };
    REQUIRE(run_log_collection_nodes_to_materialize(state) == std::vector<std::uint64_t> { visited_node });
}

TEST_CASE("BlackFlow secret merchant captures cultivation result before confirmation")
{
    REQUIRE(automation_store_should_capture_cultivation_result(
        "BlackFlow@Roguelike@AutomationCultivateHarvestReady"));
    REQUIRE_FALSE(automation_store_should_capture_cultivation_result(
        "BlackFlow@Roguelike@AutomationCultivateHarvestConfirm"));
    REQUIRE_FALSE(automation_store_should_capture_cultivation_result(
        "BlackFlow@Roguelike@AutomationCultivateConfirm"));
}

TEST_CASE("BlackFlow run log reuses only semantically redundant screenshots")
{
    REQUIRE(should_reuse_run_log_image(0.0, false, false));
    REQUIRE(should_reuse_run_log_image(0.05, true, true));
    REQUIRE_FALSE(should_reuse_run_log_image(0.05, false, true));
    REQUIRE_FALSE(should_reuse_run_log_image(0.05, true, false));
    REQUIRE_FALSE(should_reuse_run_log_image(1.0, true, true));
}

TEST_CASE("BlackFlow processing-item evidence never creates a synthetic floor zero")
{
    REQUIRE(diagnostic_processing_item_floor(0) == 1);
    REQUIRE(diagnostic_processing_item_floor(1) == 1);
    REQUIRE(diagnostic_processing_item_floor(4) == 4);
}

TEST_CASE("BlackFlow rebuilds a floor map before scanning its parts box and reuses that map afterward")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream input(
        repository_root /
        "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowRoutingLoop.h",
        std::ios::binary);
    REQUIRE(input.good());
    const std::string source {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const std::size_t cycle = source.find("RoutingCycleOutcome execute_routing_cycle");
    REQUIRE(cycle != std::string::npos);
    const std::size_t refresh = source.find("refresh_with_retries(session, port", cycle);
    const std::size_t inventory =
        source.find("session_requires_movement_inventory_observation(session)", cycle);
    const std::size_t battle_preview = source.find("session.next_battle_intel_probe();", cycle);
    const std::size_t movement_preview = source.find("BlackFlowPlan plan = session.plan(&error);", cycle);
    REQUIRE(refresh != std::string::npos);
    REQUIRE(inventory != std::string::npos);
    REQUIRE(battle_preview != std::string::npos);
    REQUIRE(movement_preview != std::string::npos);
    REQUIRE(refresh < inventory);
    REQUIRE(battle_preview < inventory);
    REQUIRE(inventory < movement_preview);
    REQUIRE(source.find("session_can_reuse_map_after_inventory(session)", cycle) != std::string::npos);
    REQUIRE(
        source.find("!reuse_inventory_map && !reuse_battle_preview_map", cycle) !=
        std::string::npos);
}

TEST_CASE("BlackFlow collection popups classify buttons and explicit sources")
{
    const std::string next =
        "BlackFlow@Roguelike@GetDrops@(BlackFlow@Roguelike@CloseCollectionContinue)";
    const std::string close =
        "BlackFlow@Roguelike@StageEncounterReward@(BlackFlow@Roguelike@CloseCollection)";
    REQUIRE(collection_popup_button(next) == CollectionPopupButton::Continue);
    REQUIRE(collection_popup_button(close) == CollectionPopupButton::Close);
    REQUIRE(
        collection_popup_button("BlackFlow@Roguelike@StageEncounterSpecialClose") ==
        CollectionPopupButton::Close);
    REQUIRE_FALSE(collection_popup_button("BlackFlow@Roguelike@CloseEvent").has_value());

    REQUIRE(
        collection_popup_source(
            "BlackFlow@Roguelike@LastRewardConfirm@(BlackFlow@Roguelike@CloseCollection)") ==
        CollectionPopupSource::StartReward);
    REQUIRE(
        collection_popup_source(
            "BlackFlow@Roguelike@SquadConfirm@(BlackFlow@Roguelike@CloseCollection)") ==
        CollectionPopupSource::SquadReward);
    REQUIRE(
        collection_popup_source(
            "BlackFlow@Roguelike@NextLevel@(BlackFlow@Roguelike@CloseCollection)") ==
        CollectionPopupSource::FloorEntry);
}

TEST_CASE("BlackFlow attributes Stages popups after an exit page to the entering floor")
{
    const std::string stages_continue =
        "BlackFlow@Roguelike@Stages@BlackFlow@Roguelike@CloseCollectionContinue";
    const std::string stages_close =
        "BlackFlow@Roguelike@Stages@BlackFlow@Roguelike@CloseCollection";

    REQUIRE(
        collection_popup_pending_floor_entry(
            stages_continue,
            2,
            NodeType::Final,
            "default",
            false) ==
        3);
    REQUIRE(
        collection_popup_pending_floor_entry(stages_close, 2, NodeType::Final, "default", false) ==
        3);

    // The reward still owned by the exit node is closed under StageEncounterReward and must
    // remain in that node's directory. Only the subsequent generic Stages chain is floor-entry.
    REQUIRE_FALSE(
        collection_popup_pending_floor_entry(
            "BlackFlow@Roguelike@StageEncounterReward@BlackFlow@Roguelike@CloseCollection",
            2,
            NodeType::Final,
            "default",
            false)
            .has_value());
    REQUIRE_FALSE(
        collection_popup_pending_floor_entry(
            stages_continue,
            2,
            NodeType::Incident,
            "default",
            false)
            .has_value());
    REQUIRE_FALSE(
        collection_popup_pending_floor_entry(
            stages_continue,
            2,
            NodeType::Final,
            "final.pass",
            false)
            .has_value());
}

TEST_CASE("BlackFlow attributes late floor-transition popups to the entering floor")
{
    REQUIRE(
        collection_popup_pending_floor_entry(
            "BlackFlow@Roguelike@MapPrepare-FloorEnterZoom@(BlackFlow@Roguelike@CloseCollectionContinue)",
            2,
            NodeType::Final,
            "default",
            false) ==
        3);
    REQUIRE(
        collection_popup_pending_floor_entry(
            "BlackFlow@Roguelike@MapCapturePopupDrain@(BlackFlow@Roguelike@CloseCollection)",
            2,
            NodeType::Final,
            "default",
            false) ==
        3);
}

TEST_CASE("BlackFlow drains delayed floor-entry reward groups before map interaction")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string continue_popup =
        "BlackFlow@Roguelike@NextLevel@(BlackFlow@Roguelike@CloseCollectionContinue)";
    const std::string close_popup =
        "BlackFlow@Roguelike@NextLevel@(BlackFlow@Roguelike@CloseCollection)";
    const std::string map_ready = "BlackFlow@Roguelike@MapPrepare-Ready";
    const auto next_level = tasks->at("BlackFlow@Roguelike@NextLevel")
                                .get("next", std::vector<std::string> {});
    const auto continue_it = std::ranges::find(next_level, continue_popup);
    const auto close_it = std::ranges::find(next_level, close_popup);
    const auto ready_it = std::ranges::find(next_level, map_ready);
    REQUIRE(continue_it != next_level.end());
    REQUIRE(close_it != next_level.end());
    REQUIRE(ready_it != next_level.end());
    REQUIRE(continue_it < close_it);
    REQUIRE(close_it < ready_it);

    const auto& floor_zoom = tasks->at("BlackFlow@Roguelike@MapPrepare-FloorEnterZoom");
    REQUIRE(floor_zoom.get("baseTask", std::string {}) == "BlackFlow@Roguelike@MapPrepare-Ready");
    REQUIRE(
        floor_zoom.get("template", std::vector<std::string> {}) ==
        std::vector<std::string> {
            "BlackFlow@Roguelike@MapZoomIn.png",
            "BlackFlow@Roguelike@MapZoomOut.png",
        });
    REQUIRE(floor_zoom.get("action", std::string {}) == "DoNothing");
    REQUIRE(
        floor_zoom.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@MapPrepare-FloorEnterZoomGuard" });

    const auto& floor_zoom_guard = tasks->at("BlackFlow@Roguelike@MapPrepare-FloorEnterZoomGuard");
    REQUIRE(floor_zoom_guard.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(
        floor_zoom_guard.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> {
            "BlackFlow@Roguelike@MapPrepare-FloorEnterZoomGuard@(BlackFlow@Roguelike@CloseCollectionContinue)",
            "BlackFlow@Roguelike@MapPrepare-FloorEnterZoomGuard@(BlackFlow@Roguelike@CloseCollection)",
            "BlackFlow@Roguelike@MapPrepare-FloorEnterZoomClick",
        });
    const auto& floor_zoom_click = tasks->at("BlackFlow@Roguelike@MapPrepare-FloorEnterZoomClick");
    REQUIRE(floor_zoom_click.get("action", std::string {}) == "ClickSelf");
    REQUIRE(floor_zoom_click.get("preDelay", 0) == 0);

    const auto drain_next = tasks->at("BlackFlow@Roguelike@MapCapturePopupDrain")
                                .get("next", std::vector<std::string> {});
    REQUIRE(
        drain_next ==
        std::vector<std::string> {
            "BlackFlow@Roguelike@MapCapturePopupDrain@(BlackFlow@Roguelike@CloseCollectionContinue)",
            "BlackFlow@Roguelike@MapCapturePopupDrain@(BlackFlow@Roguelike@CloseCollection)",
            "BlackFlow@Roguelike@MapCapturePopupDrainDone",
        });

    std::ifstream source_input(
        repository_root / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp",
        std::ios::binary);
    REQUIRE(source_input.good());
    const std::string source {
        std::istreambuf_iterator<char>(source_input),
        std::istreambuf_iterator<char>()
    };
    const std::size_t capture = source.find("bool capture_stable_map");
    const std::size_t drain = source.find("execute({ std::string(MapCapturePopupDrainTask) }", capture);
    const std::size_t accept = source.find("map_capture_candidate_is_unobstructed", capture);
    REQUIRE(capture != std::string::npos);
    REQUIRE(drain != std::string::npos);
    REQUIRE(accept != std::string::npos);
    REQUIRE(drain < accept);
}

TEST_CASE("BlackFlow loot-page entry captures immediate collection popups before blind clicking")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto successors = tasks->at("BlackFlow@Roguelike@ClickToDrops")
                                .get("next", std::vector<std::string> {});
    const auto continue_popup = std::ranges::find(
        successors,
        "BlackFlow@Roguelike@ClickToDrops@(BlackFlow@Roguelike@CloseCollectionContinue)");
    const auto close_popup = std::ranges::find(
        successors,
        "BlackFlow@Roguelike@ClickToDrops@(BlackFlow@Roguelike@CloseCollection)");
    const auto drops_flag =
        std::ranges::find(successors, "BlackFlow@Roguelike@DropsFlag");

    REQUIRE(continue_popup != successors.end());
    REQUIRE(close_popup != successors.end());
    REQUIRE(drops_flag != successors.end());
    REQUIRE(continue_popup < drops_flag);
    REQUIRE(close_popup < drops_flag);
}

TEST_CASE("BlackFlow checks pursuit collection popups before the first loot-page fallback click")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto successors = tasks->at("BlackFlow@Roguelike@MissionCompletedFlag")
                                .get("next", std::vector<std::string> {});
    const auto continue_popup = std::ranges::find(
        successors,
        "BlackFlow@Roguelike@MissionCompletedFlag@(BlackFlow@Roguelike@CloseCollectionContinue)");
    const auto close_popup = std::ranges::find(
        successors,
        "BlackFlow@Roguelike@MissionCompletedFlag@(BlackFlow@Roguelike@CloseCollection)");
    const auto blind_click =
        std::ranges::find(successors, "BlackFlow@Roguelike@ClickToDrops");

    REQUIRE(continue_popup != successors.end());
    REQUIRE(close_popup != successors.end());
    REQUIRE(blind_click != successors.end());
    REQUIRE(continue_popup < blind_click);
    REQUIRE(close_popup < blind_click);
    REQUIRE(pursuit_first_loot_click_task("ClickToDrops", 1));
    REQUIRE_FALSE(pursuit_first_loot_click_task("ClickToDrops", 2));
}

TEST_CASE("BlackFlow loot completion requires the all-rewards message and never the shared leave icon")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    for (const std::string_view entry : {
             "BlackFlow@Roguelike@DropsFlag_default",
             "BlackFlow@Roguelike@DropsFlag_mode1",
         }) {
        const auto successors = tasks->at(std::string(entry)).get("next", std::vector<std::string> {});
        const auto direct_leave = std::ranges::find(successors, "BlackFlow@Roguelike@GetDropLeave");
        const auto completed = std::ranges::find(successors, "BlackFlow@Roguelike@GetDropCompleted");
        REQUIRE(direct_leave != successors.end());
        REQUIRE(completed != successors.end());
        REQUIRE(direct_leave < completed);
    }

    const auto& direct_leave = tasks->at("BlackFlow@Roguelike@GetDropLeave");
    REQUIRE(direct_leave.get("action", std::string {}) == "DoNothing");
    REQUIRE(
        direct_leave.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@GetDropLeaveWait" });
    REQUIRE(tasks->at("BlackFlow@Roguelike@GetDropLeaveWait").get("postDelay", 0) >= 600);

    const auto& first_detection = tasks->at("BlackFlow@Roguelike@GetDropCompleted");
    REQUIRE(first_detection.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(
        first_detection.get("text", std::vector<std::string> {}) ==
        std::vector<std::string> { "已领取所有奖励" });
    REQUIRE_FALSE(first_detection.contains("template"));
    REQUIRE(first_detection.get("action", std::string {}) == "DoNothing");
    REQUIRE(first_detection.get("postDelay", -1) == 0);
    REQUIRE(
        first_detection.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@GetDropCompletedWait" });

    const auto& wait = tasks->at("BlackFlow@Roguelike@GetDropCompletedWait");
    REQUIRE(wait.get("postDelay", 0) >= 200);
    REQUIRE(wait.get("postDelay", 0) <= 300);
    const auto wait_successors = wait.get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(wait_successors, "BlackFlow@Roguelike@GetDrops#next") != wait_successors.end());
    const auto direct_leave_recheck =
        std::ranges::find(wait_successors, "BlackFlow@Roguelike@GetDropLeave");
    const auto completed_recheck =
        std::ranges::find(wait_successors, "BlackFlow@Roguelike@GetDropCompletedConfirmed");
    REQUIRE(direct_leave_recheck != wait_successors.end());
    REQUIRE(completed_recheck != wait_successors.end());
    REQUIRE(direct_leave_recheck < completed_recheck);

    const auto& confirmed = tasks->at("BlackFlow@Roguelike@GetDropCompletedConfirmed");
    REQUIRE(confirmed.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(
        confirmed.get("text", std::vector<std::string> {}) ==
        std::vector<std::string> { "已领取所有奖励" });
    REQUIRE_FALSE(confirmed.contains("template"));
    REQUIRE(confirmed.get("action", std::string {}) == "ClickRect");
    REQUIRE(confirmed.get("postDelay", -1) >= 0);
    REQUIRE(confirmed.get("postDelay", -1) <= 200);
    REQUIRE(
        confirmed.get("specificRect", std::vector<int> {}) ==
        std::vector<int> { 600, 480, 80, 100 });
    const auto confirmed_successors = confirmed.get("next", std::vector<std::string> {});
    REQUIRE(
        std::ranges::find(
            confirmed_successors,
            "BlackFlow@Roguelike@NodeCompletionAction") !=
        confirmed_successors.end());
}

TEST_CASE("BlackFlow reward clicks are revalidated after reward-card layout changes")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    constexpr std::array guarded_clicks {
        std::array<std::string_view, 4> {
            "BlackFlow@Roguelike@GetDrop",
            "BlackFlow@Roguelike@GetDropWait",
            "BlackFlow@Roguelike@GetDropConfirmed",
            "BlackFlow@Roguelike@GetDrop.png",
        },
        std::array<std::string_view, 4> {
            "BlackFlow@Roguelike@GetDropSelect",
            "BlackFlow@Roguelike@GetDropSelectWait",
            "BlackFlow@Roguelike@GetDropSelectConfirmed",
            "BlackFlow@Roguelike@GetDropSelect.png",
        },
        std::array<std::string_view, 4> {
            "BlackFlow@Roguelike@GetDropSelectReward",
            "BlackFlow@Roguelike@GetDropSelectRewardWait",
            "BlackFlow@Roguelike@GetDropSelectRewardConfirmed",
            "BlackFlow@Roguelike@GetDropSelectReward.png",
        },
        std::array<std::string_view, 4> {
            "BlackFlow@Roguelike@GetDropTrophyReward",
            "BlackFlow@Roguelike@GetDropTrophyRewardWait",
            "BlackFlow@Roguelike@GetDropTrophyRewardConfirmed",
            "BlackFlow@Roguelike@GetDropTrophyReward.png",
        },
    };
    for (const auto& [candidate_name, wait_name, confirmed_name, expected_template] : guarded_clicks) {
        CAPTURE(candidate_name, wait_name, confirmed_name, expected_template);
        const auto& candidate = tasks->at(std::string(candidate_name));
        REQUIRE(candidate.get("action", std::string {}) == "DoNothing");
        REQUIRE(
            candidate.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { std::string(wait_name) });

        const auto& wait = tasks->at(std::string(wait_name));
        REQUIRE(wait.get("algorithm", std::string {}) == "JustReturn");
        REQUIRE(wait.get("postDelay", 0) >= 300);
        const auto wait_successors = wait.get("next", std::vector<std::string> {});
        REQUIRE(std::ranges::find(wait_successors, confirmed_name) != wait_successors.end());

        const auto& confirmed = tasks->at(std::string(confirmed_name));
        REQUIRE(confirmed.get("action", std::string {}) == "DoNothing");
        REQUIRE(confirmed.get("template", std::string {}) == expected_template);
    }
}

TEST_CASE("BlackFlow trophy reward selector rejects the captured one-button unidentified transition frame")
{
    REQUIRE_FALSE(trophy_reward_click_is_safe(1, false, false));
    REQUIRE_FALSE(trophy_reward_click_is_safe(2, false, true));
    REQUIRE_FALSE(trophy_reward_click_is_safe(2, true, false));
    REQUIRE(trophy_reward_click_is_safe(2, true, true));
    REQUIRE(trophy_reward_click_is_safe(3, true, true));
}

TEST_CASE("BlackFlow eerie merchant settles refreshes and relocates a cached good before clicking")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    REQUIRE(tasks->at("BlackFlow@Roguelike@AutomationShopRefreshConfirm").get("postDelay", 0) >= 1200);
    const auto& capture_anchors = tasks->at("BlackFlow@Roguelike@AutomationShopCaptureAnchors");
    REQUIRE(capture_anchors.at("algorithm") == "OcrDetect");
    REQUIRE_FALSE(capture_anchors.contains("text"));

    std::ifstream source_file(
        repository_root /
            "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowAutomationStoreTaskPlugin.cpp",
        std::ios::binary);
    REQUIRE(source_file);
    const std::string source {
        std::istreambuf_iterator<char>(source_file),
        std::istreambuf_iterator<char>() };
    REQUIRE(source.find("if (!relocate_selection(*selection, recognition_task))") != std::string::npos);
    REQUIRE(source.find("ShopDecisionEntry") != std::string::npos);
    REQUIRE(source.find("queue_eerie_store_snapshot(\"initial\"") != std::string::npos);
    REQUIRE(source.find("queue_eerie_store_snapshot(\"after_refresh\"") != std::string::npos);
    REQUIRE(source.find("capture_pending_eerie_store_snapshot(") != std::string::npos);
    REQUIRE(source.find("ShopCaptureAnchorsTask") != std::string::npos);
    REQUIRE(source.find("top_capture_anchors, bottom_capture_anchors") != std::string::npos);
}

TEST_CASE("BlackFlow node evidence classifies exact GetDrop screens")
{
    REQUIRE(pursuit_first_loot_click_task("ClickToDrops", 1));
    REQUIRE(pursuit_first_loot_click_task("BlackFlow@Roguelike@ClickToDrops", 1));
    REQUIRE_FALSE(pursuit_first_loot_click_task("ClickToDrops", 2));
    REQUIRE_FALSE(pursuit_first_loot_click_task("ClickToDrops", 3));
    REQUIRE_FALSE(pursuit_first_loot_click_task("DropsFlag", 1));
    REQUIRE(
        node_get_drop_screen("BlackFlow@Roguelike@GetDrop") ==
        NodeGetDropScreen::Drop);
    REQUIRE(
        node_get_drop_screen("BlackFlow@Roguelike@GetDropSelect") ==
        NodeGetDropScreen::Select);
    REQUIRE(
        node_get_drop_screen("BlackFlow@Roguelike@GetDropSelectReward") ==
        NodeGetDropScreen::Select);
    REQUIRE(
        node_get_drop_screen("BlackFlow@Roguelike@GetDropTrophyReward") ==
        NodeGetDropScreen::Select);
    REQUIRE(node_get_drop_uses_custom_selector("BlackFlow@Roguelike@GetDropTrophyReward"));
    REQUIRE_FALSE(node_get_drop_uses_custom_selector("BlackFlow@Roguelike@GetDropSelectReward"));
    REQUIRE_FALSE(node_get_drop_screen("BlackFlow@Roguelike@GetDropSelectRecruit").has_value());
    REQUIRE(node_recruitment_page_task("BlackFlow@Roguelike@ChooseOper"));
    REQUIRE(node_recruitment_page_task("BlackFlow@StartExplore@Roguelike@ChooseOper"));
    // ProcessTask callbacks expose the resolved task basename, not necessarily the
    // fully-qualified resource key. The node-evidence plugin receives this exact
    // value when the recruitment page is handed to RoguelikeRecruitTaskPlugin.
    REQUIRE(node_recruitment_page_task("ChooseOper"));
    REQUIRE(node_recruitment_page_task("Roguelike@ChooseOper"));
    REQUIRE(node_recruitment_page_task("StartExplore@Roguelike@ChooseOper"));
    REQUIRE_FALSE(node_recruitment_page_task("BlackFlow@Roguelike@ChooseOperFlag"));
    REQUIRE(
        recruitment_page_flag_task("BlackFlow@Roguelike@ChooseOper") ==
        "BlackFlow@Roguelike@ChooseOperFlag");
    REQUIRE(
        recruitment_page_flag_task("ChooseOper") ==
        "BlackFlow@Roguelike@ChooseOperFlag");
    REQUIRE(
        recruitment_page_flag_task("StartExplore@Roguelike@ChooseOper") ==
        "BlackFlow@StartExplore@Roguelike@ChooseOperFlag");
    REQUIRE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDropSelectReward", false));
    REQUIRE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDropSelectReward", true));
    REQUIRE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDrop", true));
    REQUIRE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDropTrophyReward", true));
    REQUIRE_FALSE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDropTrophyReward", false));
    REQUIRE_FALSE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDrop", false));
    REQUIRE_FALSE(node_get_drop_should_capture("BlackFlow@Roguelike@GetDropSelectRecruit", true));
    REQUIRE_FALSE(
        node_get_drop_screen(
            "BlackFlow@Roguelike@GetDrop@(BlackFlow@Roguelike@CloseCollection)")
            .has_value());
    REQUIRE(is_node_evidence_run_log_action(CollectionPopupRunLogAction));
    REQUIRE(is_node_evidence_run_log_action(NodeEventRunLogAction));
    REQUIRE(is_node_evidence_run_log_action(NodeGetDropRunLogAction));
    REQUIRE(is_node_evidence_run_log_action(NodeRecruitmentRunLogAction));
    REQUIRE(is_node_evidence_run_log_action(NodeStoreRunLogAction));
    REQUIRE(is_node_evidence_run_log_action(NodeStorePurchaseRunLogAction));
    const auto two_option_selection = resolve_drop_option_selection(
        { asst::Rect { 840, 500, 180, 60 }, asst::Rect { 260, 500, 180, 60 } },
        asst::Rect { 840, 500, 180, 60 });
    REQUIRE(two_option_selection.has_value());
    REQUIRE(two_option_selection->option_count == 2);
    REQUIRE(two_option_selection->selected_index_from_left == 2);

    const auto three_option_selection = resolve_drop_option_selection(
        { asst::Rect { 830, 500, 180, 60 },
          asst::Rect { 230, 500, 180, 60 },
          asst::Rect { 530, 500, 180, 60 } },
        asst::Rect { 530, 500, 180, 60 });
    REQUIRE(three_option_selection.has_value());
    REQUIRE(three_option_selection->option_count == 3);
    REQUIRE(three_option_selection->selected_index_from_left == 2);
    REQUIRE_FALSE(
        resolve_drop_option_selection(
            { asst::Rect { 530, 500, 180, 60 } },
            asst::Rect { 530, 500, 180, 60 })
            .has_value());
    REQUIRE_FALSE(is_node_evidence_run_log_action("map.observed"));
}

TEST_CASE("BlackFlow persists resolved hidden-node identity as a routing-history checkpoint")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream telemetry_file(
        repository_root / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTelemetry.cpp",
        std::ios::binary);
    std::ifstream history_file(
        repository_root / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.cpp",
        std::ios::binary);
    std::ifstream session_file(
        repository_root / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowSession.cpp",
        std::ios::binary);
    REQUIRE(telemetry_file);
    REQUIRE(history_file);
    REQUIRE(session_file);
    const std::string telemetry {
        std::istreambuf_iterator<char>(telemetry_file),
        std::istreambuf_iterator<char>() };
    const std::string history {
        std::istreambuf_iterator<char>(history_file),
        std::istreambuf_iterator<char>() };
    const std::string session {
        std::istreambuf_iterator<char>(session_file),
        std::istreambuf_iterator<char>() };
    REQUIRE(telemetry.find("node_identity_resolved") != std::string::npos);
    REQUIRE(history.find("DiagnosticTrigger::NodeIdentityResolved") != std::string::npos);
    REQUIRE(session.find("diagnostic_node_identity_checkpoint_required(") != std::string::npos);
    REQUIRE(session.find("notebook_identity_updated && first_content") == std::string::npos);
}

TEST_CASE("BlackFlow checkpoints identity when an entered event title was already observed")
{
    // classify_entered_event_name() seeds observed_contents before RoguelikeEvent reports the
    // same title. It is not the first content callback, but changing the notebook from
    // hide_invisible to evacuate still has to produce the final old-floor checkpoint.
    const EnteredPageObservation entered = classify_entered_event_name("三重身");
    REQUIRE(entered.event_name == "三重身");
    const std::vector<std::string> observed_contents { *entered.event_name };
    REQUIRE_FALSE(observed_contents.empty());
    const PageContentEffect effect = classify_page_content_effect("RoguelikeEvent", "三重身");
    REQUIRE(effect.resolved_type == NodeType::Evacuate);
    REQUIRE(diagnostic_node_identity_checkpoint_required(true, effect.resolved_type.has_value()));
    REQUIRE_FALSE(diagnostic_node_identity_checkpoint_required(false, true));
    REQUIRE_FALSE(diagnostic_node_identity_checkpoint_required(true, false));
}

TEST_CASE("BlackFlow battle total kills uses the mode and latest observation as tie breaker")
{
    BattleTotalKillsAggregator aggregate;
    aggregate.observe(0);
    REQUIRE_FALSE(aggregate.result().has_value());

    aggregate.observe(41);
    aggregate.observe(42);
    aggregate.observe(41);
    REQUIRE(aggregate.result() == 41);

    aggregate.observe(42);
    REQUIRE(aggregate.result() == 42);

    aggregate.clear();
    REQUIRE_FALSE(aggregate.result().has_value());
}

TEST_CASE("BlackFlow store purchase success requires a decreasing ingot balance")
{
    REQUIRE(automation_store_purchase_succeeded(30, 22));
    REQUIRE_FALSE(automation_store_purchase_succeeded(30, 30));
    REQUIRE_FALSE(automation_store_purchase_succeeded(30, 34));
    REQUIRE_FALSE(automation_store_purchase_succeeded(std::nullopt, 22));
    REQUIRE_FALSE(automation_store_purchase_succeeded(30, std::nullopt));
}

TEST_CASE("BlackFlow collection popup directories separate abstract nodes and other captures")
{
    const NodeId node = *make_stable_node_id(2, GridPosition { 1, 3 });
    REQUIRE(
        collection_popup_regular_node_directory(2, node).generic_string() ==
        "collection-popups/floor-2/node-" + std::to_string(node));
    REQUIRE(
        collection_popup_virtual_node_directory(2, "安眠一隅", 17).generic_string() ==
        "collection-popups/floor-2/node-redacted-rest-corner-p17");
    REQUIRE(
        collection_popup_virtual_node_directory(3, "追猎").generic_string() ==
        "collection-popups/floor-3/node-dangerous-enemy");
    REQUIRE(collection_popup_other_directory().generic_string() == "collection-popups/other");
    REQUIRE(collection_popup_needs_landing_resolution(
        "BlackFlow@Roguelike@EnteredPageClassificationRewardPrepare@"
        "(BlackFlow@Roguelike@CloseCollection)"));
}

TEST_CASE("BlackFlow routing HTML keeps route and node detail overlays separate")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream input(
        repository_root /
            "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.cpp",
        std::ios::binary);
    REQUIRE(input);
    const std::string source {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };

    REQUIRE(source.find("routeModeRail") == std::string::npos);
    REQUIRE(source.find("route-label-leader") == std::string::npos);
    REQUIRE(source.find("arrowPoints") != std::string::npos);
    REQUIRE(source.find("function readableRouteAngle") != std::string::npos);
    REQUIRE(source.find("function placeRouteLabel") == std::string::npos);
    REQUIRE(source.find("function routeSegmentsVisuallyOverlap") != std::string::npos);
    REQUIRE(source.find("marker-end") == std::string::npos);
    REQUIRE(source.find("svgEl('polygon'") != std::string::npos);
    REQUIRE(source.find("if(item&&geometry)") != std::string::npos);
    REQUIRE(source.find("appendStepBadge(startId,false,0,true)") != std::string::npos);
    REQUIRE(source.find("0 为起点") != std::string::npos);
    REQUIRE(source.find("nativeTitle") == std::string::npos);
    REQUIRE(source.find("node-label-coordinate") == std::string::npos);
    REQUIRE(source.find("routing-history-data.js") != std::string::npos);
    REQUIRE(source.find("processing-item-history-data.js") != std::string::npos);
    REQUIRE(source.find("append_framed_json_array_entry") != std::string::npos);
    REQUIRE(source.find("<script id=\"data\"") == std::string::npos);
}

TEST_CASE("BlackFlow node OCR merges slightly overlapping adjacent fragments")
{
    REQUIRE(node_ocr_fragments_have_mergeable_horizontal_gap(513, 28, 537, 16, 6));

    REQUIRE(node_ocr_fragments_have_mergeable_horizontal_gap(558, 41, 597, 16, 6));

    REQUIRE_FALSE(node_ocr_fragments_have_mergeable_horizontal_gap(100, 20, 113, 16, 6));
    REQUIRE_FALSE(node_ocr_fragments_have_mergeable_horizontal_gap(100, 20, 137, 16, 6));

    // 最近六局的真实拆框：“居民”[408, 39] + “据点”[450, 27]。
    REQUIRE(node_ocr_fragments_have_mergeable_horizontal_gap(408, 39, 450, 16, 6));
    REQUIRE(node_ocr_inference_scale(681, 60) == 64.0 / 60.0);
    REQUIRE(node_ocr_inference_scale(681, 64) == 1.0);
}

TEST_CASE("BlackFlow node OCR assigns fragments before merging neighboring labels")
{
    // “居民” + “据点” are two fragments of the same title, even when the second
    // fragment center is just outside the old fixed 25px assignment tolerance.
    REQUIRE(node_ocr_fragment_column(427.5, 438.0, 101.0, 5) == 0);
    REQUIRE(node_ocr_fragment_column(463.5, 438.0, 101.0, 5) == 0);

    // 07:16 fourth-floor capture: two complete neighboring labels had only a
    // 12px box gap.  They belong to different cells and must never be merged.
    REQUIRE(node_ocr_fragment_column(891.5, 286.75, 100.75, 8) == 6);
    REQUIRE(node_ocr_fragment_column(988.0, 286.75, 100.75, 8) == 7);
}

TEST_CASE("BlackFlow node OCR keeps detector context above a map title")
{
    // 10:28 的三层现场：节点中心 y=144，完整“紧急作战”从 y=158 开始。
    // 旧 ROI [153,193) 只留 5px 上文，DBDetector 收缩成 [617,161,43,9]
    // 并输出“作占”；新 ROI [139,199) 保留 19px 上文。
    const int top = node_ocr_row_top(144.0, 25, 60);
    REQUIRE(top == 139);
    REQUIRE(158 - top >= 16);
}

TEST_CASE("BlackFlow weak normal battle OCR does not override the ideal-source emergency rule")
{
    REQUIRE(weak_normal_battle_ocr_defers_to_ideal_source_prediction("battle_normal", "ocr", false));
    REQUIRE_FALSE(weak_normal_battle_ocr_defers_to_ideal_source_prediction("battle_normal", "ocr", true));
    REQUIRE_FALSE(weak_normal_battle_ocr_defers_to_ideal_source_prediction("battle_elite", "ocr", false));
}

TEST_CASE("Roguelike first recruitment rechecks the page after the custom voucher plugin")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/base.json");
    REQUIRE(tasks.has_value());

    // 03:33:22 的现场：插件已经确认进入干员列表，父任务仍补点旧券坐标 (418, 512)，
    // 选中了第四张卡。入口只能通知插件，随后重新识别列表或仍待使用的招募券。
    const auto& entry = tasks->at("Roguelike@RecruitMain");
    REQUIRE(entry.get("action", std::string {}) == "DoNothing");
    const std::vector<std::string> expected_next {
        "StartExplore@Roguelike@ChooseOperFlag",
        "StartExplore@Roguelike@RecruitCloseGuide",
        "StartExplore@Roguelike@RecruitOther",
    };
    REQUIRE(entry.get("next", std::vector<std::string> {}) == expected_next);

    // 未配置自选干员或插件没有打开券时，仍须重新匹配并点击招募按钮。
    // RecruitOther 继承入口资源，必须显式保留点击动作。
    const auto& voucher = tasks->at("Roguelike@RecruitOther");
    REQUIRE(voucher.get("action", std::string {}) == "ClickSelf");
    REQUIRE(voucher.get("algorithm", std::string("MatchTemplate")) == "MatchTemplate");
    REQUIRE(voucher.get("roi", std::vector<int> {}) == std::vector<int> { 80, 470, 1120, 154 });
}

TEST_CASE("BlackFlow automation collection recruits the fixed five-person team")
{
    REQUIRE(AutomationCollectionFirstOperator == "机械师");
    REQUIRE(AutomationCollectionCasterOperator == "卡达");
    REQUIRE(AutomationCollectionCoreOperator == "凯尔希·思衡托");
    REQUIRE(AutomationCollectionDefenderOperator == "古米");
    REQUIRE(AutomationCollectionSpecialistOperator == "伊桑");
    REQUIRE(AutomationCollectionSquad == "堡垒战术分队");
    REQUIRE(AutomationCollectionRoles == "坚不可摧");
    REQUIRE(AutomationCollectionPassiveTaskDelay == 150);
    REQUIRE(AutomationCollectionOperators.size() == 5);
    REQUIRE(is_automation_collection_operator("机械师"));
    REQUIRE(is_automation_collection_operator("卡达"));
    REQUIRE(is_automation_collection_operator("凯尔希·思衡托"));
    REQUIRE(is_automation_collection_operator("古米"));
    REQUIRE(is_automation_collection_operator("伊桑"));
    REQUIRE_FALSE(is_automation_collection_operator("维什戴尔"));
    REQUIRE(should_recruit_visible_automation_collection_operator("卡达", false, 0, 1));
    REQUIRE(should_recruit_visible_automation_collection_operator("伊桑", false, 0, 1));
    REQUIRE(should_recruit_visible_automation_collection_operator("古米", true, 1, 2));
    REQUIRE_FALSE(should_recruit_visible_automation_collection_operator("古米", true, 1, 1));
    REQUIRE_FALSE(should_recruit_visible_automation_collection_operator("古米", true, 2, 2));
    REQUIRE_FALSE(should_recruit_visible_automation_collection_operator("维什戴尔", false, 0, 2));
    REQUIRE(AutomationCollectionCoreRecruitmentTickets.size() == 1);
    REQUIRE(is_automation_collection_core_recruitment_ticket("医疗招募券"));
    REQUIRE_FALSE(is_automation_collection_core_recruitment_ticket("高级医疗招募券"));
    REQUIRE_FALSE(is_automation_collection_core_recruitment_ticket("精锐医疗招募券"));
    REQUIRE(is_automation_collection_defender_recruitment_ticket("重装招募券"));
    REQUIRE_FALSE(is_automation_collection_defender_recruitment_ticket("高级重装招募券"));
    REQUIRE(is_automation_collection_defender_recruitment_ticket("精锐重装招募券"));
    REQUIRE(is_automation_collection_defender_recruitment_ticket("堡垒协议招募券"));
    REQUIRE(is_automation_collection_specialist_recruitment_ticket("特种招募券"));
    REQUIRE_FALSE(is_automation_collection_core_recruitment_ticket("狙击招募券"));

    const AutomationCollectionTeamProgress incomplete {
        .first_operator_elite_two = true,
        .caster_operator_recruited = true,
        .core_operator_elite_two = true,
        .defender_operator_recruited = true,
        .specialist_operator_recruited = false,
    };
    REQUIRE_FALSE(automation_collection_team_complete(incomplete));
    REQUIRE(automation_collection_shop_progress_purchase_allowed("特种招募券", incomplete));

    AutomationCollectionTeamProgress complete = incomplete;
    complete.specialist_operator_recruited = true;
    REQUIRE(automation_collection_team_complete(complete));
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("特种招募券", complete));
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("医疗招募券", complete));
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("医者-新典训", complete));
}

TEST_CASE("BlackFlow Defeated Leaves maps Ethan deployment to an explicit recruitment group")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto recruitment =
        json::open(repository_root / "resource/roguelike/BlackFlow/recruitment.json");
    const auto autopilot =
        json::open(repository_root / std::filesystem::path(u8"resource/roguelike/BlackFlow/autopilot/败叶.json"));
    REQUIRE(recruitment.has_value());
    REQUIRE(autopilot.has_value());

    const bool has_explicit_ethan_group = std::ranges::any_of(recruitment->at("priority").as_array(), [](const auto& group) {
        if (group.get("name", std::string {}) != "伊桑") {
            return false;
        }
        return std::ranges::any_of(group.at("opers").as_array(), [](const auto& oper) {
            return oper.get("name", std::string {}) == "伊桑";
        });
    });
    REQUIRE(has_explicit_ethan_group);

    const bool deploys_ethan_group = std::ranges::any_of(autopilot->at("deploy_plan").as_array(), [](const auto& plan) {
        const auto groups = plan.get("groups", std::vector<std::string> {});
        return std::ranges::find(groups, "伊桑") != groups.end();
    });
    REQUIRE(deploys_ethan_group);
}

TEST_CASE("BlackFlow Lake Fairy locks the requested option sequence before choosing the unique follow-up")
{
    const LakeFairyChoicePlan promoted = make_lake_fairy_choice_plan({
        .core_operator_elite_two = true,
        .ingots = 3,
    });
    REQUIRE(promoted.initial_choice_count == 4);
    REQUIRE(promoted.initial_choices[0] == 1);
    REQUIRE(promoted.initial_choices[1] == 1);
    REQUIRE(promoted.initial_choices[2] == 1);
    REQUIRE(promoted.initial_choices[3] == 1);

    for (const LakeFairyContext context : {
             LakeFairyContext { .core_operator_elite_two = false, .ingots = 20 },
             LakeFairyContext { .core_operator_elite_two = true, .ingots = 2 },
         }) {
        const LakeFairyChoicePlan conservative = make_lake_fairy_choice_plan(context);
        REQUIRE(conservative.initial_choice_count == 2);
        REQUIRE(conservative.initial_choices[0] == 1);
        REQUIRE(conservative.initial_choices[1] == 2);
    }
}

TEST_CASE("BlackFlow Healing Heart compares safe tradeoffs by the planner's average lexicographic score")
{
    const auto sample = [](bool viable, int required_action_points, std::vector<int> score) {
        return LinkedEncounterRouteValue {
            .viable = viable,
            .required_action_points = required_action_points,
            .lexicographic_score = std::move(score),
            .lexicographic_score_labels = { "effective_node_count", "processing_move_count" },
        };
    };

    HealingHeartRouteAggregate action_points;
    append_healing_heart_route_sample(action_points, sample(true, 4, { -2, 4 }));

    HealingHeartRouteAggregate processing_items;
    append_healing_heart_route_sample(processing_items, sample(true, 4, { -2, 2 }));
    append_healing_heart_route_sample(processing_items, sample(true, 4, { -2, 4 }));

    REQUIRE(healing_heart_route_is_better(processing_items, action_points));
    REQUIRE(
        make_healing_heart_choice_order(action_points, processing_items) ==
        std::vector<std::string> {
            HealingHeartPreferredChoice,
            HealingHeartProcessingItemChoice,
            HealingHeartActionPointChoice,
            HealingHeartSafetyFallbackChoice,
            HealingHeartRestFallbackChoice,
        });

    HealingHeartRouteAggregate higher_expectation_with_worse_requirement;
    append_healing_heart_route_sample(higher_expectation_with_worse_requirement, sample(true, 5, { -3, 1 }));
    append_healing_heart_route_sample(higher_expectation_with_worse_requirement, sample(true, 5, { -3, 1 }));
    REQUIRE(healing_heart_route_is_better(higher_expectation_with_worse_requirement, action_points));
}

TEST_CASE("BlackFlow random route benefit uses expectation after every outcome is proven safe")
{
    const auto sample = [](int score) {
        return LinkedEncounterRouteValue {
            .viable = true,
            .required_action_points = 4,
            .lexicographic_score = { score },
            .lexicographic_score_labels = { "effective_node_count" },
        };
    };

    RandomRouteAggregate baseline;
    append_random_route_sample(baseline, sample(-3));
    append_random_route_sample(baseline, sample(-1));

    RandomRouteAggregate candidate;
    append_random_route_sample(candidate, sample(0));
    append_random_route_sample(candidate, sample(-5));

    // One outcome is worse than the baseline worst, but every outcome remains safe and the mean
    // improves from -2 to -2.5.  Expected lexicographic value is therefore decisive.
    REQUIRE(random_route_is_safe_expected_improvement(candidate, baseline));

    RandomRouteAggregate unsafe = candidate;
    unsafe.all_viable = false;
    REQUIRE_FALSE(random_route_is_safe_expected_improvement(unsafe, baseline));
}

TEST_CASE("BlackFlow Healing Heart omits every tradeoff that cannot always reach the endpoint")
{
    const LinkedEncounterRouteValue safe {
        .viable = true,
        .required_action_points = 3,
        .lexicographic_score = { -1 },
        .lexicographic_score_labels = { "effective_node_count" },
    };
    const LinkedEncounterRouteValue unsafe {
        .viable = false,
        .required_action_points = UnreachableActionPointRequirement,
        .lexicographic_score = { -10 },
        .lexicographic_score_labels = { "effective_node_count" },
    };

    HealingHeartRouteAggregate action_points;
    append_healing_heart_route_sample(action_points, unsafe);
    HealingHeartRouteAggregate processing_items;
    append_healing_heart_route_sample(processing_items, safe);
    append_healing_heart_route_sample(processing_items, unsafe);

    REQUIRE_FALSE(healing_heart_route_is_viable(action_points));
    REQUIRE_FALSE(healing_heart_route_is_viable(processing_items));
    REQUIRE(
        make_healing_heart_choice_order(action_points, processing_items) ==
        std::vector<std::string> {
            HealingHeartPreferredChoice,
            HealingHeartSafetyFallbackChoice,
            HealingHeartRestFallbackChoice,
        });
}

TEST_CASE("BlackFlow Healing Heart keeps the configured action-point priority on an exact tie")
{
    const LinkedEncounterRouteValue tied {
        .viable = true,
        .required_action_points = 2,
        .lexicographic_score = { -3, 1 },
        .lexicographic_score_labels = { "effective_node_count", "route_length" },
    };
    HealingHeartRouteAggregate action_points;
    HealingHeartRouteAggregate processing_items;
    append_healing_heart_route_sample(action_points, tied);
    append_healing_heart_route_sample(processing_items, tied);
    append_healing_heart_route_sample(processing_items, tied);

    const auto order = make_healing_heart_choice_order(action_points, processing_items);
    REQUIRE(order[0] == HealingHeartPreferredChoice);
    REQUIRE(order[1] == HealingHeartActionPointChoice);
    REQUIRE(order[2] == HealingHeartProcessingItemChoice);
}

TEST_CASE("BlackFlow Healing Heart priority is expressed by configured option names")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto encounter =
        json::open(repository_root / "resource/roguelike/BlackFlow/encounter/default.json");
    REQUIRE(encounter.has_value());

    bool found = false;
    for (const auto& event : encounter->at("stage").as_array()) {
        if (event.get("name", std::string {}) != HealingHeartEventName) {
            continue;
        }
        found = true;
        const auto groups = event.at("choice_groups").as_array();
        REQUIRE(groups.size() == 1);
        REQUIRE(
            groups[0].get("choices", std::vector<std::string> {}) ==
            std::vector<std::string> {
                HealingHeartPreferredChoice,
                HealingHeartActionPointChoice,
                HealingHeartProcessingItemChoice,
                HealingHeartSafetyFallbackChoice,
                HealingHeartRestFallbackChoice,
            });
    }
    REQUIRE(found);
}

TEST_CASE("BlackFlow map HUD observation synchronizes ingots before encounter handling")
{
    const RunObservation observation = make_map_hud_run_observation(6, 3, MovementKind::Walk);
    REQUIRE(observation.action_points == 6);
    REQUIRE(observation.ingots == 3);
    REQUIRE(observation.active_movement == MovementKind::Walk);
}

TEST_CASE("BlackFlow fixed-team recruitment scans past incomplete OCR pages")
{
    REQUIRE(automation_collection_recruit_scan_limit(5) == 99);
    REQUIRE(automation_collection_recruit_scan_limit(24) == 99);
    REQUIRE(automation_collection_recruit_scan_limit(120) == 120);
    REQUIRE(should_continue_automation_collection_recruit_scan(false, 0, 5));
    REQUIRE(should_continue_automation_collection_recruit_scan(false, 4, 5));
    REQUIRE_FALSE(should_continue_automation_collection_recruit_scan(true, 1, 5));
    REQUIRE_FALSE(should_continue_automation_collection_recruit_scan(false, 5, 5));

    const std::unordered_set<std::string> rightmost_page { "安赛尔", "伊桑" };
    REQUIRE(automation_collection_recruit_page_is_unchanged_endpoint(rightmost_page, rightmost_page, 1));
    REQUIRE_FALSE(automation_collection_recruit_page_is_unchanged_endpoint({}, rightmost_page, 1));
}

TEST_CASE("BlackFlow fixed-team recruitment abandons unrelated professions after five swipes")
{
    REQUIRE(AutomationCollectionRecruitRoleProbeSwipeLimit == 5);

    const AutomationCollectionTeamProgress only_specialist_pending {
        .first_operator_elite_two = true,
        .caster_operator_recruited = true,
        .core_operator_elite_two = true,
        .defender_operator_recruited = true,
        .specialist_operator_recruited = false,
    };
    REQUIRE(
        automation_collection_pending_milestone_operators(only_specialist_pending) ==
        std::vector<std::string_view> { AutomationCollectionSpecialistOperator });

    REQUIRE_FALSE(automation_collection_should_abandon_after_role_probe(4, false));
    REQUIRE(automation_collection_should_abandon_after_role_probe(5, false));
    REQUIRE(automation_collection_should_abandon_after_role_probe(6, false));
    REQUIRE_FALSE(automation_collection_should_abandon_after_role_probe(5, true));
}

TEST_CASE("BlackFlow fixed-team recruitment waits out rebound frames and stops page cycles")
{
    using Action = AutomationCollectionRecruitPageAction;

    const std::unordered_set<std::string> seven_names { "A", "B", "C", "D", "E", "F", "G" };
    const std::unordered_set<std::string> four_names { "H", "I", "J", "K" };
    AutomationCollectionRecruitPageTracker cycle_tracker;

    REQUIRE(cycle_tracker.observe(seven_names, true, 0) == Action::Swipe);
    REQUIRE(cycle_tracker.observe(four_names, true, 1) == Action::Swipe);
    REQUIRE(cycle_tracker.observe(seven_names, true, 2) == Action::RetrySamePage);
    REQUIRE(cycle_tracker.observe(seven_names, true, 2) == Action::StopAtSeenPage);

    const std::unordered_set<std::string> unavailable_names { "玛恩纳", "史尔特尔" };
    AutomationCollectionRecruitPageTracker unavailable_tracker;
    REQUIRE(unavailable_tracker.observe(seven_names, true, 0) == Action::Swipe);
    REQUIRE(unavailable_tracker.observe(unavailable_names, false, 1) == Action::RetrySamePage);
    REQUIRE(unavailable_tracker.observe(unavailable_names, false, 1) == Action::Swipe);
    REQUIRE(unavailable_tracker.observe(unavailable_names, false, 2) == Action::RetrySamePage);
    REQUIRE(unavailable_tracker.observe(unavailable_names, false, 2) == Action::StopAtSeenPage);

    AutomationCollectionRecruitPageTracker empty_tracker;
    REQUIRE(empty_tracker.observe({}, false, 1) == Action::RetrySamePage);
    REQUIRE(empty_tracker.observe({}, false, 1) == Action::RetrySamePage);
    REQUIRE(empty_tracker.observe({}, false, 1) == Action::Swipe);

    REQUIRE(automation_collection_recruit_swipe_distance(1124) == 500);
    REQUIRE(automation_collection_recruit_swipe_distance(734) == 500);
    REQUIRE(automation_collection_recruit_swipe_distance(500) == 400);
}

TEST_CASE("Battle auto skill activates a shared operator and device tile only once per scan")
{
    asst::BattleAutoSkillActivationGuard guard;
    const asst::Point shared_tile { 5, 4 };
    const asst::Point other_tile { 4, 4 };

    REQUIRE(guard.should_attempt(shared_tile));
    guard.record_success(shared_tile);
    REQUIRE_FALSE(guard.should_attempt(shared_tile));
    REQUIRE(guard.should_attempt(other_tile));
}

TEST_CASE("Battle auto skill skips active skills and can activate them again after cooldown")
{
    using asst::BattleSkillClickMode;
    using asst::BattleSkillClickResult;

    int clicks = 0;
    const auto click = [&] {
        ++clicks;
        return true;
    };
    const auto attempt = [&](std::string_view matched_template) {
        return asst::click_matched_skill(matched_template, BattleSkillClickMode::Automatic, click);
    };

    REQUIRE(attempt("BattleSkillReadyOnClick-TopView.png") == BattleSkillClickResult::Clicked);
    REQUIRE(clicks == 1);

    // Replay repeated false-positive ready classifications while the panel shows Stop.
    for (int scan = 0; scan < 4; ++scan) {
        REQUIRE(attempt("BattleSkillStopOnClick-TopView.png") == BattleSkillClickResult::AlreadyActive);
        REQUIRE(clicks == 1);
    }

    REQUIRE(attempt("BattleSkillReadyOnClick-TopView2.png") == BattleSkillClickResult::Clicked);
    REQUIRE(clicks == 2);
    REQUIRE(asst::click_matched_skill("BattleSkillReadyOnClick-TopView.png", BattleSkillClickMode::Automatic, [] {
                return false;
            }) == BattleSkillClickResult::Failed);
}

TEST_CASE("Battle explicit skill actions can still stop active skills")
{
    int clicks = 0;
    REQUIRE(asst::click_matched_skill("BattleSkillStopOnClick-TopView.png", asst::BattleSkillClickMode::Explicit, [&] {
                ++clicks;
                return true;
            }) == asst::BattleSkillClickResult::Clicked);
    REQUIRE(clicks == 1);
}

TEST_CASE("BlackFlow Gold Stasis defers drone and fountain choices only after Kal'tsit promotion")
{
    REQUIRE(gold_stasis_choice_priority_bucket("查看无人机", false) == 0);
    REQUIRE(gold_stasis_choice_priority_bucket("向泉水许愿", false) == 0);
    REQUIRE(gold_stasis_choice_priority_bucket("查看无人机", true) == 1);
    REQUIRE(gold_stasis_choice_priority_bucket("S查看无人机", true) == 1);
    REQUIRE(gold_stasis_choice_priority_bucket("向泉水许愿", true) == 1);
    REQUIRE(gold_stasis_choice_priority_bucket("坐下休息", true) == 0);
}

TEST_CASE("BlackFlow Gold Stasis keeps the fountain wish second-last by default")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto encounter =
        json::open(repository_root / "resource/roguelike/BlackFlow/encounter/default.json");
    REQUIRE(encounter.has_value());

    bool gold_stasis_found = false;
    for (const auto& event : encounter->at("stage").as_array()) {
        if (event.get("name", std::string {}) != GoldStasisEventName) {
            continue;
        }
        gold_stasis_found = true;
        const auto groups = event.at("choice_groups").as_array();
        REQUIRE(groups.size() == 1);
        const auto choices = groups[0].get("choices", std::vector<std::string> {});
        REQUIRE(choices.size() >= 2);
        REQUIRE(choices[choices.size() - 2] == "向泉水许愿");
    }
    REQUIRE(gold_stasis_found);
}

TEST_CASE("BlackFlow praised shadow keeps chaining until the UI returns to the map")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto encounter =
        json::open(repository_root / "resource/roguelike/BlackFlow/encounter/default.json");
    REQUIRE(encounter.has_value());

    bool event_found = false;
    for (const auto& event : encounter->at("stage").as_array()) {
        if (event.get("name", std::string {}) != "被歌颂的影子") {
            continue;
        }
        event_found = true;
        // 所有选项都可能继续同名事件；是否退出只能由实际页面是否回到地图判断。
        REQUIRE(event.get("next_event", std::string {}) == "被歌颂的影子");
        const auto groups = event.at("choice_groups").as_array();
        REQUIRE(groups.size() == 1);
        REQUIRE(groups[0].get("choices", std::vector<std::string> {}) == std::vector<std::string> { "加入它们" });
        REQUIRE(groups[0].get("next_event", std::string {}).empty());
    }
    REQUIRE(event_found);
}

TEST_CASE("BlackFlow chained encounter treats a returned map as stronger evidence than the shared HP HUD")
{
    REQUIRE(
        classify_chained_encounter_page(true, true, true) == ChainedEncounterPageState::MapReturned);
    REQUIRE(
        classify_chained_encounter_page(true, false, true) == ChainedEncounterPageState::EventReady);
    REQUIRE(
        classify_chained_encounter_page(true, false, false) == ChainedEncounterPageState::Waiting);
    REQUIRE(
        classify_chained_encounter_page(false, true, true) == ChainedEncounterPageState::EventReady);
}

TEST_CASE("BlackFlow encounter header detection ignores translucent card background texture")
{
    std::vector<double> light_row_ratios(585, 0.05);
    std::fill(light_row_ratios.begin() + 171, light_row_ratios.begin() + 212, 0.99);
    std::fill(light_row_ratios.begin() + 335, light_row_ratios.begin() + 376, 0.98);

    // A one-row texture seam must not split a real 40-pixel header, while a short
    // bright patch in the event illustration must not become an option.
    light_row_ratios[190] = 0.40;
    std::fill(light_row_ratios.begin() + 450, light_row_ratios.begin() + 459, 0.99);
    std::fill(light_row_ratios.begin() + 480, light_row_ratios.begin() + 560, 0.99);

    const auto bands = find_blackflow_light_header_bands(light_row_ratios, 50);
    REQUIRE(bands.size() == 2);
    REQUIRE(bands[0].y == 221);
    REQUIRE(bands[1].y == 385);
}

TEST_CASE("BlackFlow duel battle state is initialized without toggling an existing double speed")
{
    REQUIRE(should_toggle_to_double_speed(false));
    REQUIRE_FALSE(should_toggle_to_double_speed(true));
    REQUIRE_FALSE(should_use_direct_ready_skill_confirmation(false, { 0.0, 0.0 }));
    REQUIRE(should_use_direct_ready_skill_confirmation(true, { 9.0, 0.0 }));

    const std::array observed_speed_states { false, false, true };
    std::size_t observations = 0;
    std::size_t clicks = 0;
    std::size_t delays = 0;
    const bool double_speed_confirmed = ensure_double_speed(
        [&]() { return observed_speed_states.at(observations++); },
        [&]() {
            ++clicks;
            return true;
        },
        [&]() { ++delays; },
        3);
    REQUIRE(double_speed_confirmed);
    REQUIRE(observations == 3);
    REQUIRE(clicks == 2);
    REQUIRE(delays == 2);

    const BattleFixedControls normal_controls {
        .retreat_button = { 1110, 640 },
        .skill_button = { 1175, 500 },
        .has_multi_stages = false,
    };
    const BattleFixedControls shifted_tile_recalculation {
        .retreat_button = { 1230, 640 },
        .skill_button = { 1294, 406 },
        .has_multi_stages = true,
    };
    REQUIRE(fixed_battle_controls_after_camera_shift(normal_controls, shifted_tile_recalculation) == normal_controls);
}

TEST_CASE("BlackFlow duel defers and keeps retrying double speed after the preparation camera animation")
{
    DoubleSpeedRetrySchedule duel_schedule(true);

    REQUIRE_FALSE(duel_schedule.due(false, 0));
    REQUIRE_FALSE(duel_schedule.due(false, 1499));
    REQUIRE(duel_schedule.due(false, 1500));

    duel_schedule.mark_attempt(1500);
    REQUIRE_FALSE(duel_schedule.due(false, 3499));
    REQUIRE(duel_schedule.due(false, 3500));

    duel_schedule.mark_attempt(3500);
    REQUIRE_FALSE(duel_schedule.due(true, 10000));
}

TEST_CASE("BlackFlow preparation combat waits for the camera and verifies the first deployment")
{
    REQUIRE_FALSE(preparation_combat_actions_ready(true, 0));
    REQUIRE_FALSE(preparation_combat_actions_ready(true, 1499));
    REQUIRE(preparation_combat_actions_ready(true, 1500));
    REQUIRE(preparation_combat_actions_ready(false, 0));

    // A still-selected deployment card is temporarily unavailable even when the drag failed.
    // Do not trust card state until the selection has been cleared and observed again.
    REQUIRE_FALSE(deployment_attempt_confirmed(false, true, false, false));
    REQUIRE_FALSE(deployment_attempt_confirmed(true, true, false, true));
    REQUIRE(deployment_attempt_confirmed(true, true, true, false));
    REQUIRE(deployment_attempt_confirmed(true, true, false, false));
    REQUIRE(deployment_attempt_confirmed(true, false, false, false));
}

TEST_CASE("BlackFlow refreshes the battle frame after the awaited virtual device activates")
{
    REQUIRE(virtual_auto_skill_transition_requires_refresh(true, true));
    REQUIRE_FALSE(virtual_auto_skill_transition_requires_refresh(false, true));
    REQUIRE_FALSE(virtual_auto_skill_transition_requires_refresh(true, false));
}

TEST_CASE("BlackFlow Hound Pathogen registers both on-map devices")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto config_path = repository_root /
        std::filesystem::path(u8"resource/roguelike/BlackFlow/autopilot/猎犬病原.json");
    std::ifstream input(config_path, std::ios::binary);
    REQUIRE(input.good());
    const std::string text(std::istreambuf_iterator<char>(input), {});
    const auto config = json::parse(text);
    REQUIRE(config.has_value());

    const auto devices = config->at("virtual_auto_skill_devices").as_array();
    REQUIRE(devices.size() == 1);
    const auto locations = devices[0].at("locations").as_array();
    REQUIRE(locations.size() == 2);
    REQUIRE(locations[0].as_array()[0].as_integer() == 2);
    REQUIRE(locations[0].as_array()[1].as_integer() == 1);
    REQUIRE(locations[1].as_array()[0].as_integer() == 1);
    REQUIRE(locations[1].as_array()[1].as_integer() == 1);
}

TEST_CASE("BlackFlow all virtual auto skill device coordinates use battle-screen axes")
{
    using LocationSet = std::set<std::pair<int, int>>;
    const std::map<std::string, LocationSet> expected {
        { "冰冷流亡", { { 6, 5 } } },
        { "急不可耐", { { 4, 5 }, { 3, 1 } } },
        { "枯枝", { { 7, 4 } } },
        { "湖中魇", { { 4, 3 } } },
        { "灌水贤者", { { 7, 1 }, { 7, 2 } } },
        { "猎犬病原", { { 2, 1 }, { 1, 1 } } },
        { "虫虫游戏厅", { { 2, 3 }, { 2, 4 } } },
        { "无主地", { { 6, 1 }, { 6, 3 } } },
        { "本性难移", { { 2, 6 }, { 6, 3 } } },
        { "远北猎场", { { 8, 3 }, { 8, 4 } } },
        { "枝头陷阱", { { 2, 5 }, { 7, 2 } } },
        { "预算否决", { { 7, 5 }, { 9, 2 } } },
        { "生存习性", { { 9, 3 }, { 3, 4 } } },
        { "辟路行进", { { 4, 4 }, { 6, 4 } } },
        { "趁火打劫", { { 2, 4 }, { 4, 1 } } },
        { "焦土之家", { { 4, 5 }, { 7, 1 } } },
        { "黑色逆流", { { 6, 3 }, { 5, 4 } } },
        { "未来见闻", { { 4, 5 }, { 7, 1 } } },
        { "种植园惊魂", { { 3, 6 }, { 3, 2 } } },
        { "选边站", { { 4, 2 }, { 6, 2 } } },
        { "诞生礼", { { 1, 5 }, { 2, 5 } } },
        { "暴力押运", { { 2, 4 }, { 5, 4 } } },
        { "长刀寒夜", { { 5, 3 }, { 6, 3 } } },
        { "侵占家园", { { 1, 2 }, { 5, 6 } } },
        { "一步之遥", { { 3, 3 }, { 7, 3 } } },
        { "物欲遮天", { { 2, 6 }, { 3, 6 } } },
        { "恶意讨薪", { { 5, 5 }, { 5, 1 } } },
        { "送别仪式", { { 1, 4 }, { 1, 5 } } },
        { "彼处水如酒", { { 1, 6 }, { 1, 5 } } },
        { "丛林法则", { { 11, 1 }, { 10, 1 } } },
        { "过度繁殖", { { 1, 6 }, { 1, 5 } } },
        { "命运共享", { { 3, 1 }, { 2, 1 } } },
        { "同域共存", { { 3, 4 }, { 1, 4 } } },
        { "活木", { { 2, 3 }, { 1, 6 } } },
        { "败叶", { { 4, 1 } } },
        { "遗忘时间", { { 4, 1 }, { 7, 3 } } },
    };

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto autopilot_directory = repository_root /
        std::filesystem::path(u8"resource/roguelike/BlackFlow/autopilot");
    std::map<std::string, LocationSet> actual;

    const auto collect = [&](const auto& config) {
        const auto& object = config.as_object();
        if (!object.contains("virtual_auto_skill_devices")) {
            return;
        }
        const std::string stage_name = config.get("stage_name", std::string {});
        for (const auto& device : config.at("virtual_auto_skill_devices").as_array()) {
            for (const auto& location : device.at("locations").as_array()) {
                actual[stage_name].emplace(
                    location.as_array()[0].as_integer(),
                    location.as_array()[1].as_integer());
            }
        }
    };

    for (const auto& entry : std::filesystem::directory_iterator(autopilot_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        REQUIRE(input.good());
        const std::string text(std::istreambuf_iterator<char>(input), {});
        const auto parsed = json::parse(text);
        REQUIRE(parsed.has_value());
        if (parsed->is_array()) {
            for (const auto& config : parsed->as_array()) {
                collect(config);
            }
        }
        else {
            collect(*parsed);
        }
    }

    REQUIRE(actual == expected);
}

TEST_CASE("BlackFlow Withered Branch waits for its configured device before deploying")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto config_path = repository_root /
        std::filesystem::path(u8"resource/roguelike/BlackFlow/autopilot/枯枝.json");
    std::ifstream input(config_path, std::ios::binary);
    REQUIRE(input.good());
    const std::string text(std::istreambuf_iterator<char>(input), {});
    const auto config = json::parse(text);
    REQUIRE(config.has_value());

    const auto devices = config->at("virtual_auto_skill_devices").as_array();
    REQUIRE(devices.size() == 1);
    REQUIRE(devices[0].get("skill_times", 0) == 1);
    const auto device_locations = devices[0].get("locations", std::vector<std::vector<int>> {});
    REQUIRE(device_locations.size() == 1);
    REQUIRE(config->get("deploy_after_virtual_auto_skill", std::vector<int> {}) == device_locations.front());
}

TEST_CASE("BlackFlow Irrigation Sage intentionally shares a device tile with the Mechanist")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto config_path = repository_root /
        std::filesystem::path(u8"resource/roguelike/BlackFlow/autopilot/灌水贤者.json");
    std::ifstream input(config_path, std::ios::binary);
    REQUIRE(input.good());
    const std::string text(std::istreambuf_iterator<char>(input), {});
    const auto config = json::parse(text);
    REQUIRE(config.has_value());

    std::set<std::pair<int, int>> device_locations;
    for (const auto& device : config->at("virtual_auto_skill_devices").as_array()) {
        for (const auto& location : device.at("locations").as_array()) {
            device_locations.emplace(
                location.as_array()[0].as_integer(),
                location.as_array()[1].as_integer());
        }
    }

    const auto& deploy_plan = config->at("deploy_plan").as_array();
    const auto mechanist = std::ranges::find_if(deploy_plan, [](const auto& deploy) {
        const auto groups = deploy.at("groups").as_array();
        return !groups.empty() && groups[0].as_string() == "机械师";
    });
    REQUIRE(mechanist != deploy_plan.end());
    const auto& location = mechanist->at("location").as_array();
    REQUIRE(device_locations.contains({ location[0].as_integer(), location[1].as_integer() }));
}

TEST_CASE("BlackFlow automation collection samples only the four requested start rewards")
{
    REQUIRE(AutomationCollectionStartRewards.size() == 4);
    REQUIRE(AutomationCollectionStartRewards[0].display_name == "未编号物");
    REQUIRE(AutomationCollectionStartRewards[1].display_name == "退行补偿");
    REQUIRE(AutomationCollectionStartRewards[2].display_name == "林间代步");
    REQUIRE(AutomationCollectionStartRewards[3].display_name == "巢寄生");
}

TEST_CASE("BlackFlow automation collection prioritizes swaddled-animal start rewards")
{
    REQUIRE(AutomationCollectionPreferredStartRewards.size() == 5);
    REQUIRE(AutomationCollectionPreferredStartRewards[0] == "襁褓金乌");
    REQUIRE(AutomationCollectionPreferredStartRewards[1] == "襁褓天马");
    REQUIRE(AutomationCollectionPreferredStartRewards[2] == "襁褓白泽");
    REQUIRE(AutomationCollectionPreferredStartRewards[3] == "襁褓巨龙");
    REQUIRE(AutomationCollectionPreferredStartRewards[4] == "襁褓骏鹰");

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& ocr = tasks->at("BlackFlow@Roguelike@AutomationCollectionPreferredStartReward");
    REQUIRE(ocr.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(ocr.get("roi", std::vector<int> {}) == std::vector<int> { 100, 380, 1100, 130 });
    REQUIRE(ocr.get("text", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "襁褓金乌",
                "襁褓天马",
                "襁褓白泽",
                "襁褓巨龙",
                "襁褓骏鹰",
                "未编号物",
                "退行补偿",
                "林间代步",
                "巢寄生",
                "调查预付款",
                "空间租赁",
            });
    REQUIRE(ocr.get("fullMatch", false));
    REQUIRE(ocr.get("fuzzyMatch", false));

    const std::vector<std::string> latest_run_titles {
        "强裸骏鹰",
        "空间租货",
        "林间代步",
        "调查预付款",
    };
    const std::vector<std::string_view> ordinary_priority { "林间代步", "未编号物", "退行补偿", "巢寄生" };
    const auto selected = select_automation_collection_start_reward(latest_run_titles, ordinary_priority);
    REQUIRE(selected.has_value());
    REQUIRE(selected->detected_index == 0);
    REQUIRE(selected->canonical == "襁褓骏鹰");
    REQUIRE(selected->preferred);
}

TEST_CASE("BlackFlow automation collection falls back when only resource rewards remain")
{
    const std::vector<std::string_view> ordinary_priority { "林间代步", "未编号物", "退行补偿", "巢寄生" };

    SECTION("prefer the free ingot reward when both fallback rewards remain")
    {
        const auto selected = select_automation_collection_start_reward(
            { "调查预付款", "空间租赁" },
            ordinary_priority);
        REQUIRE(selected.has_value());
        REQUIRE(selected->detected_index == 0);
        REQUIRE(selected->canonical == "调查预付款");
        REQUIRE_FALSE(selected->preferred);
    }

    SECTION("select the capacity reward when it is the only remaining reward")
    {
        const auto selected = select_automation_collection_start_reward({ "空间租赁" }, ordinary_priority);
        REQUIRE(selected.has_value());
        REQUIRE(selected->detected_index == 0);
        REQUIRE(selected->canonical == "空间租赁");
        REQUIRE_FALSE(selected->preferred);
    }

    SECTION("keep an allowed ordinary reward ahead of fallback rewards")
    {
        const auto selected = select_automation_collection_start_reward(
            { "调查预付款", "退行补偿", "空间租赁" },
            ordinary_priority);
        REQUIRE(selected.has_value());
        REQUIRE(selected->detected_index == 1);
        REQUIRE(selected->canonical == "退行补偿");
        REQUIRE_FALSE(selected->preferred);
    }
}

TEST_CASE("BlackFlow start reward selection is OCR-only and confirms the selected card")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& enter = tasks->at("BlackFlow@Roguelike@LastReward-EnterPoint");
    REQUIRE(enter.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(!enter.contains("template"));
    REQUIRE(enter.get("roi", std::vector<int> {}) == std::vector<int> { 100, 380, 1100, 130 });
    REQUIRE(enter.get("fullMatch", false));
    REQUIRE(enter.get("fuzzyMatch", false));
    const auto enter_next = enter.get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(
                enter_next,
                "BlackFlow@Roguelike@LastRewardConfirm@(BlackFlow@Roguelike@CloseCollection)") !=
            enter_next.end());
    const auto reward_popup_close = std::ranges::find(
        enter_next,
        "BlackFlow@Roguelike@LastRewardConfirm@(BlackFlow@Roguelike@CloseCollection)");
    const auto repeated_reward_entry = std::ranges::find(enter_next, "BlackFlow@Roguelike@LastReward-EnterPoint");
    // 06:19 失败现场：第一次奖励详情关闭后页面仍显示“还可再选 1 次”。关闭详情后必须
    // 重新探测奖励入口，直到页面真正进入职业组合、招募或地图，不能只等待后续页面。
    REQUIRE(repeated_reward_entry != enter_next.end());
    REQUIRE(reward_popup_close < repeated_reward_entry);
    // 自动化收集插件会在 LastReward-EnterPoint 内直接完成卡片二次确认。确认后可能
    // 直接进入职业组合页，不再经过 LastRewardConfirm，因此入口自身必须能接续后续页。
    REQUIRE(std::ranges::find(enter_next, "BlackFlow@Roguelike@RolesDefault") != enter_next.end());
    REQUIRE(std::ranges::find(enter_next, "BlackFlow@Roguelike@RecruitMain") != enter_next.end());
    REQUIRE(std::ranges::find(enter_next, "BlackFlow@Roguelike@Stages#next") != enter_next.end());

    const auto confirm_next = tasks->at("BlackFlow@Roguelike@LastRewardConfirm")
                                  .get("next", std::vector<std::string> {});
    const auto fallback_popup_close = std::ranges::find(
        confirm_next,
        "BlackFlow@Roguelike@LastRewardConfirm@(BlackFlow@Roguelike@CloseCollection)");
    const auto fallback_repeated_entry =
        std::ranges::find(confirm_next, "BlackFlow@Roguelike@LastReward-EnterPoint");
    REQUIRE(fallback_popup_close != confirm_next.end());
    REQUIRE(fallback_repeated_entry != confirm_next.end());
    REQUIRE(fallback_popup_close < fallback_repeated_entry);

    const auto& confirmation = tasks->at("BlackFlow@Roguelike@AutomationCollectionStartRewardConfirmationPrompt");
    REQUIRE(confirmation.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(confirmation.get("roi", std::vector<int> {}) == std::vector<int> { 100, 520, 1080, 55 });
    REQUIRE(confirmation.get("text", std::vector<std::string> {}) ==
            std::vector<std::string> { "你确定要这么做" });
    REQUIRE(confirmation.get("fuzzyMatch", false));

    // 04:32 失败现场中提示框中心 x=213；确认勾固定在同一卡片底部 y=595。
    REQUIRE(
        automation_collection_start_reward_confirmation_point(asst::Rect { 143, 544, 140, 28 }) ==
        asst::Point { 213, 595 });
}

TEST_CASE("BlackFlow start reward templates document all six actual rewards")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::array<std::pair<std::string, std::string>, 6> expected = { {
        { "BlackFlow@Roguelike@LastReward", "退行补偿" },
        { "BlackFlow@Roguelike@LastReward2", "林间代步" },
        { "BlackFlow@Roguelike@LastReward3", "巢寄生" },
        { "BlackFlow@Roguelike@LastReward4", "未编号物" },
        { "BlackFlow@Roguelike@LastReward5", "调查预付款" },
        { "BlackFlow@Roguelike@LastReward6", "空间租赁" },
    } };

    for (const auto& [task_name, reward_name] : expected) {
        CAPTURE(task_name, reward_name);
        REQUIRE(tasks->contains(task_name));
        const std::string doc = tasks->at(task_name).get("Doc", std::string {});
        REQUIRE(doc.find(reward_name) != std::string::npos);
    }
}

TEST_CASE("BlackFlow inspects the utopia title through the fixed map marker")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& toggle = tasks->at("BlackFlow@Roguelike@UtopiaPanelToggle");
    REQUIRE(toggle.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(toggle.get("action", std::string {}) == "ClickRect");
    const auto click_rect = toggle.get("specificRect", std::vector<int> {});
    REQUIRE(click_rect.size() == 4);
    // The recorded (568, 67) click hit the decoration below the marker and was
    // cached as an absent utopia. Every randomized point must stay in its center.
    REQUIRE(click_rect[0] >= 545);
    REQUIRE(click_rect[1] >= 20);
    REQUIRE(click_rect[2] > 0);
    REQUIRE(click_rect[3] > 0);
    REQUIRE(click_rect[0] + click_rect[2] <= 569);
    REQUIRE(click_rect[1] + click_rect[3] <= 44);

    const auto& policy = tasks->at("BlackFlow@Roguelike@UtopiaPanelPolicy");
    REQUIRE(policy.get("algorithm", std::string {}) == "OcrDetect");
    const std::vector<int> expected_policy_roi { 505, 100, 58, 42 };
    REQUIRE(policy.get("roi", std::vector<int> {}) == expected_policy_roi);
    REQUIRE(policy.get("fuzzyMatch", false));
    REQUIRE(policy.get("text", std::vector<std::string> {}).size() == 4);

    const auto& ideology = tasks->at("BlackFlow@Roguelike@UtopiaPanelIdeology");
    REQUIRE(ideology.get("algorithm", std::string {}) == "OcrDetect");
    const std::vector<int> expected_ideology_roi { 563, 100, 130, 42 };
    REQUIRE(ideology.get("roi", std::vector<int> {}) == expected_ideology_roi);
    REQUIRE(ideology.get("fuzzyMatch", false));
    REQUIRE(ideology.get("text", std::vector<std::string> {}).size() == 10);
}

TEST_CASE("BlackFlow treats an unopened utopia panel as an optional absent marker")
{
    REQUIRE(
        classify_utopia_panel_inspection({}) ==
        UtopiaPanelInspectionDisposition::Absent);
    REQUIRE(
        classify_utopia_panel_inspection({ .ideology = "diffused-mist", .policy = "radical" }) ==
        UtopiaPanelInspectionDisposition::Present);
    REQUIRE(
        classify_utopia_panel_inspection({ .ideology = "diffused-mist" }) ==
        UtopiaPanelInspectionDisposition::Incomplete);
}

TEST_CASE("BlackFlow does not leave exploration after confirming an active utopia")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    for (const auto* task : {
             "BlackFlow@Roguelike@UtopiaMapRefresh-Exit",
             "BlackFlow@Roguelike@UtopiaMapRefresh-Continue",
             "BlackFlow@Roguelike@UtopiaMapRefresh-Dismiss",
             "BlackFlow@Roguelike@UtopiaMapRefresh-ZoomOut",
             "BlackFlow@Roguelike@UtopiaMapRefresh-OnMap",
             "BlackFlow@Roguelike@UtopiaMapRefresh-Completed",
             "BlackFlow@Roguelike@UtopiaMapRefresh-Failed",
         }) {
        REQUIRE_FALSE(tasks->contains(task));
    }
}

TEST_CASE("BlackFlow move confirmation handles the leave-region confirmation dialog")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& confirm = tasks->at("BlackFlow@Roguelike@MovePreviewConfirm");
    const auto next = confirm.get("next", std::vector<std::string> {});
    const auto leave_confirm = std::ranges::find(next, "BlackFlow@Roguelike@StageEncounterLeaveConfirm");
    const auto observe = std::ranges::find(next, "BlackFlow@Roguelike@MovePreviewConfirmObserve");
    const auto absent_once = std::ranges::find(next, "BlackFlow@Roguelike@MovePreviewConfirmAbsentOnce");
    REQUIRE(leave_confirm != next.end());
    REQUIRE(observe != next.end());
    REQUIRE(absent_once != next.end());
    REQUIRE(leave_confirm < observe);
    REQUIRE(observe < absent_once);
    REQUIRE(std::ranges::find(next, "BlackFlow@Roguelike@MovePreviewConfirm") == next.end());
    REQUIRE(confirm.get("action", std::string {}) == "ClickRect");
    REQUIRE(confirm.get("specificRect", std::vector<int> {}) == std::vector<int> { 1135, 525, 70, 70 });
    REQUIRE(confirm.get("maxTimes", 0) == 4);
    REQUIRE(
        confirm.get("exceededNext", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@MovePreviewConfirmExceeded" });

    const auto& observation = tasks->at("BlackFlow@Roguelike@MovePreviewConfirmObserve");
    REQUIRE(observation.get("action", std::string {}) == "DoNothing");
    REQUIRE(observation.get("maxTimes", 0) == 2);
    const auto observation_next = observation.get("next", std::vector<std::string> {});
    REQUIRE(
        std::ranges::find(observation_next, "BlackFlow@Roguelike@StageEncounterLeaveConfirm") !=
        observation_next.end());
    REQUIRE(std::ranges::find(observation_next, "#self") != observation_next.end());
    REQUIRE(
        observation.get("exceededNext", std::vector<std::string> {}) ==
        std::vector<std::string> {
            "BlackFlow@Roguelike@MovePreviewConfirm",
            "BlackFlow@Roguelike@MovePreviewConfirmAbsentOnce",
        });

    const auto& absent = tasks->at("BlackFlow@Roguelike@MovePreviewConfirmAbsentOnce");
    REQUIRE(absent.get("action", std::string {}) == "DoNothing");
    REQUIRE(absent.get("postDelay", 0) >= 500);
    REQUIRE(
        absent.get("reduceOtherTimes", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@MovePreviewConfirmObserve*2" });
    const auto absent_next = absent.get("next", std::vector<std::string> {});
    REQUIRE(
        std::ranges::find(absent_next, "BlackFlow@Roguelike@MovePreviewConfirmObserve") != absent_next.end());
    REQUIRE(
        std::ranges::find(absent_next, "BlackFlow@Roguelike@MovePreviewConfirmSucceeded") != absent_next.end());

    REQUIRE(MoveConfirmationStatus::Succeeded != MoveConfirmationStatus::NeedsDismiss);
    REQUIRE(MoveConfirmationStatus::NeedsDismiss != MoveConfirmationStatus::Failed);
}

TEST_CASE("BlackFlow revealed controllable moves do not require entered-page identity classification")
{
    MoveCandidate known_target;
    known_target.controllable = true;
    const MovePreview revealed_preview {
        PreviewReachability::Reachable,
        1,
        NodeType::Empty,
        "林间空地",
        true,
    };
    REQUIRE_FALSE(move_confirmation_requires_entered_page_classification(known_target, revealed_preview));

    MovePreview hidden_preview = revealed_preview;
    hidden_preview.identity_revealed = false;
    REQUIRE(move_confirmation_requires_entered_page_classification(known_target, hidden_preview));

    MoveCandidate random_landing = known_target;
    random_landing.controllable = false;
    REQUIRE(move_confirmation_requires_entered_page_classification(random_landing, revealed_preview));
}

TEST_CASE("BlackFlow transition clicks wait passively for settled destinations")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto require_click_without_self_retry = [&](const std::string& name) {
        CAPTURE(name);
        const auto& task = tasks->at(name);
        const auto next = task.get("next", std::vector<std::string> {});
        REQUIRE(std::ranges::find(next, "#self") == next.end());
        REQUIRE(std::ranges::find(next, name) == next.end());
        REQUIRE(task.get("maxTimes", 0) >= 3);
        REQUIRE_FALSE(task.get("exceededNext", std::vector<std::string> {}).empty());
    };
    for (const std::string& name : {
             "BlackFlow@Roguelike@MovementInventoryOpen",
             "BlackFlow@Roguelike@MovementInventoryClose",
             "BlackFlow@Roguelike@EmployLeave",
             "BlackFlow@Roguelike@EmployLeaveConfirm",
             "BlackFlow@Roguelike@HuntedConfirm",
             "BlackFlow@Roguelike@HuntedDepart",
             "BlackFlow@Roguelike@StageEncounterBattleDepart",
             "BlackFlow@Roguelike@StageEnterBattleAgain",
             "BlackFlow@Roguelike@StageEncounterLeaveConfirm",
             "BlackFlow@Roguelike@StageTraderLeave",
             "BlackFlow@Roguelike@StageTraderLeaveConfirm",
         }) {
        require_click_without_self_retry(name);
    }

    const auto require_passive_observer = [&](const std::string& name) {
        CAPTURE(name);
        const auto& task = tasks->at(name);
        const auto next = task.get("next", std::vector<std::string> {});
        REQUIRE(task.get("action", std::string {}) == "DoNothing");
        REQUIRE(std::ranges::find(next, "#self") != next.end());
    };
    for (const std::string& name : {
             "BlackFlow@Roguelike@MovementInventoryOpenObserve",
             "BlackFlow@Roguelike@MovementInventoryCloseObserve",
             "BlackFlow@Roguelike@MovementInventoryCloseTransitionWait",
             "BlackFlow@Roguelike@EmployLeaveObserve",
             "BlackFlow@Roguelike@EmployLeaveTransitionWait",
             "BlackFlow@Roguelike@EmployLeaveConfirmObserve",
             "BlackFlow@Roguelike@EmployLeaveConfirmTransitionWait",
             "BlackFlow@Roguelike@HuntedConfirmObserve",
             "BlackFlow@Roguelike@HuntedConfirmTransitionWait",
             "BlackFlow@Roguelike@HuntedDepartObserve",
             "BlackFlow@Roguelike@HuntedDepartTransitionWait",
             "BlackFlow@Roguelike@StageEncounterBattleDepartObserve",
             "BlackFlow@Roguelike@StageEncounterBattleDepartTransitionWait",
             "BlackFlow@Roguelike@StageEnterBattleAgainObserve",
             "BlackFlow@Roguelike@StageEnterBattleAgainTransitionWait",
             "BlackFlow@Roguelike@StageEncounterLeaveConfirmObserve",
             "BlackFlow@Roguelike@StageTraderLeaveObserve",
             "BlackFlow@Roguelike@StageTraderLeaveTransitionWait",
             "BlackFlow@Roguelike@StageTraderLeaveConfirmObserve",
             "BlackFlow@Roguelike@StageTraderLeaveConfirmTransitionWait",
         }) {
        require_passive_observer(name);
    }

    for (const auto& [destination, completed] : std::array {
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveDestinationHunted",
                 "BlackFlow@Roguelike@EmployLeaveConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveDestinationHunted",
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmCompleted",
             },
         }) {
        CAPTURE(destination, completed);
        const auto& task = tasks->at(destination);
        REQUIRE(task.get("template", std::string {}) == "BlackFlow@Roguelike@HuntedConfirm.png");
        REQUIRE(task.get("action", std::string {}) == "DoNothing");
        REQUIRE(task.get("next", std::vector<std::string> {}) == std::vector<std::string> { completed });
    }

    for (const auto& [completed, normal_destination] : std::array {
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveConfirmCompleted",
                 "BlackFlow@Roguelike@NodeCompletionAction",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmCompleted",
                 "BlackFlow@Roguelike@Stages",
             },
         }) {
        CAPTURE(completed, normal_destination);
        REQUIRE(tasks->at(completed).get("next", std::vector<std::string> {}) ==
                std::vector<std::string> { "BlackFlow@Roguelike@HuntedConfirm", normal_destination });
    }

    const auto& inventory_hunted = tasks->at("BlackFlow@Roguelike@MovementInventoryCloseDestinationHunted");
    REQUIRE(inventory_hunted.get("template", std::string {}) == "BlackFlow@Roguelike@HuntedConfirm.png");
    REQUIRE(inventory_hunted.get("action", std::string {}) == "DoNothing");
    REQUIRE(inventory_hunted.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@HuntedConfirm" });
    const auto inventory_hunted_resets =
        inventory_hunted.get("reduceOtherTimes", std::vector<std::string> {});
    REQUIRE(std::ranges::find(inventory_hunted_resets, "BlackFlow@Roguelike@MovementInventoryClose*3") !=
            inventory_hunted_resets.end());

    for (const auto& [resetter, observer] : std::array {
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveTransitionWait",
                 "BlackFlow@Roguelike@EmployLeaveObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveConfirmAbsentOnce",
                 "BlackFlow@Roguelike@EmployLeaveConfirmObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveConfirmTransitionWait",
                 "BlackFlow@Roguelike@EmployLeaveConfirmObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@HuntedConfirmAbsentOnce",
                 "BlackFlow@Roguelike@HuntedConfirmObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@HuntedConfirmTransitionWait",
                 "BlackFlow@Roguelike@HuntedConfirmObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@HuntedDepartTransitionWait",
                 "BlackFlow@Roguelike@HuntedDepartObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageEncounterBattleDepartTransitionWait",
                 "BlackFlow@Roguelike@StageEncounterBattleDepartObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageEnterBattleAgainTransitionWait",
                 "BlackFlow@Roguelike@StageEnterBattleAgainObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@MovementInventoryCloseTransitionWait",
                 "BlackFlow@Roguelike@MovementInventoryCloseObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageEncounterLeaveConfirmAbsentOnce",
                 "BlackFlow@Roguelike@StageEncounterLeaveConfirmObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveTransitionWait",
                 "BlackFlow@Roguelike@StageTraderLeaveObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmAbsentOnce",
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmObserve*2",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmTransitionWait",
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmObserve*2",
             },
         }) {
        CAPTURE(resetter, observer);
        const auto resets = tasks->at(resetter).get("reduceOtherTimes", std::vector<std::string> {});
        REQUIRE(std::ranges::find(resets, observer) != resets.end());
    }

    REQUIRE(
        tasks->at("BlackFlow@Roguelike@MovementInventoryOpened").get("template", std::string {}) ==
        "BlackFlow@Roguelike@MovementInventoryCollapse.png");
    const auto inventory_close_next = tasks->at("BlackFlow@Roguelike@MovementInventoryClose")
                                          .get("next", std::vector<std::string> {});
    REQUIRE(inventory_close_next.front() == "BlackFlow@Roguelike@MovementInventoryCloseObserve");

    for (const auto& [name, completed] : std::array {
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveDestinationReady",
                 "BlackFlow@Roguelike@EmployLeaveConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveDestinationZoomOut",
                 "BlackFlow@Roguelike@EmployLeaveConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveDestinationReady",
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveDestinationZoomOut",
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmCompleted",
             },
         }) {
        CAPTURE(name, completed);
        const auto& destination = tasks->at(name);
        REQUIRE(destination.get("action", std::string {}) == "DoNothing");
        REQUIRE(destination.get("next", std::vector<std::string> {}) == std::vector<std::string> { completed });
    }
    const auto& hunted_destination = tasks->at("BlackFlow@Roguelike@HuntedConfirmDestination");
    REQUIRE(hunted_destination.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(
        hunted_destination.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@HuntedConfirmCompleted" });

    for (const auto& [confirm, completed] : std::array {
             std::pair {
                 "BlackFlow@Roguelike@EmployLeaveConfirm",
                 "BlackFlow@Roguelike@EmployLeaveConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@HuntedConfirm",
                 "BlackFlow@Roguelike@HuntedConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageEncounterLeaveConfirm",
                 "BlackFlow@Roguelike@StageEncounterLeaveConfirmCompleted",
             },
             std::pair {
                 "BlackFlow@Roguelike@StageTraderLeaveConfirm",
                 "BlackFlow@Roguelike@StageTraderLeaveConfirmCompleted",
             },
         }) {
        CAPTURE(confirm, completed);
        const auto next = tasks->at(confirm).get("next", std::vector<std::string> {});
        REQUIRE(std::ranges::find(next, completed) == next.end());
    }

    const auto encounter_confirm_next = tasks->at("BlackFlow@Roguelike@StageEncounterLeaveConfirm")
                                            .get("next", std::vector<std::string> {});
    REQUIRE(
        std::ranges::find(
            encounter_confirm_next,
            "BlackFlow@Roguelike@StageEncounterLeaveConfirmCompleted") == encounter_confirm_next.end());
    const auto absent_once_next = tasks->at("BlackFlow@Roguelike@StageEncounterLeaveConfirmAbsentOnce")
                                      .get("next", std::vector<std::string> {});
    REQUIRE(
        std::ranges::find(absent_once_next, "BlackFlow@Roguelike@StageEncounterLeaveConfirmObserve") !=
        absent_once_next.end());
    REQUIRE(
        std::ranges::find(absent_once_next, "BlackFlow@Roguelike@StageEncounterLeaveConfirmCompleted") !=
        absent_once_next.end());
}

TEST_CASE("BlackFlow move preview cost tolerates the observed minus-sign OCR variants")
{
    REQUIRE(parse_move_preview_action_point_cost("-1") == -1);
    REQUIRE(parse_move_preview_action_point_cost("1-1") == -1);
    REQUIRE(parse_move_preview_action_point_cost("--1") == -1);
    REQUIRE(parse_move_preview_action_point_cost("I-1") == -1);
    REQUIRE(parse_move_preview_action_point_cost("1") == -1);
    REQUIRE(parse_move_preview_action_point_cost("0") == 0);
    REQUIRE_FALSE(parse_move_preview_action_point_cost("价格").has_value());
}

TEST_CASE("BlackFlow battle-intel preview dismissal verifies the map and has a recovery fallback")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& cancel = tasks->at("BlackFlow@Roguelike@CancelNodeSelection");
    REQUIRE(cancel.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@CancelNodeSelection-OnMap",
                "BlackFlow@Roguelike@CancelNodeSelectionClick",
            });

    const auto& click = tasks->at("BlackFlow@Roguelike@CancelNodeSelectionClick");
    REQUIRE(click.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@CancelNodeSelection-OnMap",
                "BlackFlow@Roguelike@CancelNodeSelectionClick",
            });
    REQUIRE(click.get("exceededNext", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@RecoverMap-Exit" });

    const auto& on_map = tasks->at("BlackFlow@Roguelike@CancelNodeSelection-OnMap");
    REQUIRE(on_map.get("baseTask", std::string {}) == "BlackFlow@Roguelike@MapPrepare-Ready");
    REQUIRE(on_map.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@MapPrepare" });

    REQUIRE(is_battle_intel_preview_type(NodeType::BattleNormal));
    REQUIRE(is_battle_intel_preview_type(NodeType::BattleElite));
    REQUIRE_FALSE(is_battle_intel_preview_type(NodeType::ScrapShop));
    REQUIRE_FALSE(is_battle_intel_preview_type(NodeType::Shop));

    Node battle;
    battle.type = NodeType::BattleNormal;
    REQUIRE_FALSE(battle_intel_probe_blocked_by_resident(battle));
    battle.marker_type = "savage";
    REQUIRE(battle_intel_probe_blocked_by_resident(battle));
    battle.marker_type.clear();
    battle.marker_resident_overlap_possible = true;
    REQUIRE(battle_intel_probe_blocked_by_resident(battle));
}

TEST_CASE("BlackFlow recognizes the parts-box overload prompt before page dispatch")
{
    const EnteredPageObservation observation =
        classify_entered_page_texts({ "零件箱已满，无法进入下个节点", "是否打开零件箱进行整理?", "取消" });

    REQUIRE(observation.inventory_overloaded);
    REQUIRE_FALSE(observation.classified_type.has_value());
    REQUIRE_FALSE(observation.classification_conflict);

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    const auto& prompt = tasks->at("BlackFlow@Roguelike@MovementInventoryOverloadPrompt");
    REQUIRE(prompt.get("text", std::vector<std::string> {}) == std::vector<std::string> { "零件箱已满" });
    REQUIRE_FALSE(prompt.get("fullMatch", true));
    REQUIRE_FALSE(prompt.get("fuzzyMatch", true));

    const auto& banner = tasks->at("BlackFlow@Roguelike@MovementInventoryOverloadedBanner");
    REQUIRE(banner.get("text", std::vector<std::string> {}) ==
            std::vector<std::string> { "当前零件数量过多" });
    REQUIRE_FALSE(banner.get("fullMatch", true));
    REQUIRE_FALSE(banner.get("fuzzyMatch", true));

    const auto& detail_wait = tasks->at("BlackFlow@Roguelike@MovementInventoryDiscardDetailWait");
    REQUIRE(detail_wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(detail_wait.get("postDelay", 0) >= 500);

    REQUIRE(failure_next_action(FailureDisposition::RestartRun) == "restart_current_run");
    REQUIRE(failure_next_action(FailureDisposition::StopTask) == "stop_task");
}

TEST_CASE("BlackFlow does not mistake the quick-formation operator picker for the formation page")
{
    const EnteredPageObservation details_page =
        classify_entered_page_texts({ "右侧干员以查看详情" });
    const EnteredPageObservation roster_page =
        classify_entered_page_texts({ "助战招募", "机械师", "凯尔希·思衡托" });

    REQUIRE_FALSE(details_page.classified_type.has_value());
    REQUIRE_FALSE(roster_page.classified_type.has_value());
    REQUIRE(details_page.combat_operator_selection_open);
    REQUIRE(roster_page.combat_operator_selection_open);

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    REQUIRE(tasks->contains("BlackFlow@Roguelike@EnteredPageClassificationCombat"));
    REQUIRE(tasks->at("BlackFlow@Roguelike@EnteredPageClassificationCombat")
                .get("template", std::string {}) == "BattleQuickFormation.png");
    REQUIRE(tasks->at("BlackFlow@Roguelike@EnteredPageClassificationCombatOperatorConfirm")
                .get("template", std::string {}) == "BattleQuickFormationConfirm.png");
    REQUIRE(tasks->at("BlackFlow@Roguelike@EnteredPageClassificationCombatOperatorBack")
                .get("roi", std::vector<int> {}) == std::vector<int> { 0, 0, 180, 80 });

    const auto page_combat_next = tasks->at("BlackFlow@Roguelike@Page-Combat")
                                      .get("next", std::vector<std::string> {});
    REQUIRE(page_combat_next ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@ResumeCombatQuickFormation",
                "BlackFlow@Roguelike@QuickFormation",
            });
}

TEST_CASE("BlackFlow can resume a stopped run from the quick-formation operator page")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto begin_next = tasks->at("BlackFlow@Roguelike@Begin")
                                .get("next", std::vector<std::string> {});
    const auto resume = std::ranges::find(begin_next, "BlackFlow@Roguelike@ResumeCombatQuickFormation");
    const auto generic_return = std::ranges::find(begin_next, "BlackFlow@Roguelike@ReturnButtons#next");
    REQUIRE(resume != begin_next.end());
    REQUIRE(generic_return != begin_next.end());
    REQUIRE(resume < generic_return);
    REQUIRE(tasks->at("BlackFlow@Roguelike@ResumeCombatQuickFormation")
                .get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@QuickFormation" });
}

TEST_CASE("BlackFlow move preview identity OCR separates the node category and large title")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto roi = tasks->at("BlackFlow@Roguelike@MovePreviewDisplayedName")
                         .get("roi", std::vector<int> {});
    REQUIRE(roi.size() == 4);
    REQUIRE(roi[1] >= 130);
    REQUIRE(roi[3] <= 55);

    REQUIRE(tasks->contains("BlackFlow@Roguelike@MovePreviewDisplayedCategory"));
    const auto category_roi = tasks->at("BlackFlow@Roguelike@MovePreviewDisplayedCategory")
                                  .get("roi", std::vector<int> {});
    REQUIRE(category_roi.size() == 4);
    REQUIRE(category_roi[1] == 108);
    REQUIRE(category_roi[1] < roi[1]);
    REQUIRE(category_roi[1] + category_roi[3] <= roi[1] + 5);

    const auto& stage_name = tasks->at("BlackFlow@Roguelike@MovePreviewStageName");
    REQUIRE(stage_name.get("baseTask", std::string {}) == "BattleStageName");
    REQUIRE(stage_name.get("roi", std::vector<int> {}) == roi);
    REQUIRE(stage_name.get("fuzzyMatch", false));
}

TEST_CASE("BlackFlow open encounter pages take precedence over the visible floor title")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto reward_next = tasks->at("BlackFlow@Roguelike@StageEncounterReward")
                                 .get("next", std::vector<std::string> {});
    const auto reward_resume = std::ranges::find(
        reward_next,
        "BlackFlow@Roguelike@ResumeEncounterPage");
    const auto reward_floor = std::ranges::find(reward_next, "BlackFlow@Roguelike@NextLevel");
    REQUIRE(reward_resume != reward_next.end());
    REQUIRE(reward_floor != reward_next.end());
    REQUIRE(reward_resume < reward_floor);

    const auto floor_next = tasks->at("BlackFlow@Roguelike@NextLevel")
                                .get("next", std::vector<std::string> {});
    const auto floor_resume = std::ranges::find(
        floor_next,
        "BlackFlow@Roguelike@ResumeEncounterPage");
    const auto floor_reachable_preview = std::ranges::find(
        floor_next,
        "BlackFlow@Roguelike@ResumeMovePreviewEnter");
    const auto floor_blocked_preview = std::ranges::find(
        floor_next,
        "BlackFlow@Roguelike@ResumeMovePreviewCannotEnter");
    const auto floor_self = std::ranges::find(floor_next, "BlackFlow@Roguelike@NextLevel");
    REQUIRE(floor_resume != floor_next.end());
    REQUIRE(floor_reachable_preview != floor_next.end());
    REQUIRE(floor_blocked_preview != floor_next.end());
    REQUIRE(floor_self != floor_next.end());
    REQUIRE(floor_resume < floor_self);
    REQUIRE(floor_reachable_preview < floor_self);
    REQUIRE(floor_blocked_preview < floor_self);

    const auto& resume_enter = tasks->at("BlackFlow@Roguelike@ResumeMovePreviewEnter");
    REQUIRE(resume_enter.get("template", std::string {}) ==
            "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(resume_enter.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@CancelNodeSelection" });
    const auto& resume_blocked = tasks->at("BlackFlow@Roguelike@ResumeMovePreviewCannotEnter");
    REQUIRE(resume_blocked.get("template", std::string {}) ==
            "BlackFlow@Roguelike@MovePreviewCannotEnter.png");
    REQUIRE(resume_blocked.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@CancelNodeSelection" });
}

TEST_CASE("BlackFlow overload cleanup keeps the configured cross-category discard order")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto discard_priority =
        tasks->at("BlackFlow@Roguelike@MovementInventoryDiscardPriority")
            .get("text", std::vector<std::string> {});
    const std::vector<std::string> expected {
        "白模鸟",         "涂装黎博利",   "枯苔藓球",   "报废假肢",       "报废轮子",
        "涂装阿戈尔",     "涂装佩洛",     "白模狗",     "血蕈",           "白模鱼",
        "雾滚草",         "浪花",         "霜晶树",     "回声玉米",       "种子",
        "小八界",         "标准引擎",     "试作外骨骼", "气垫底座",       "多生苔藓",
        "坎诺特的触须",   "重弹簧",       "老妈妈的融雪", "笼控器",       "恋家果",
        "板藤",           "光彩松露",     "一次性喷气背包", "“简易遥控器”", "结构性原理",
    };
    REQUIRE(discard_priority == expected);

    const auto before_quota_excess =
        tasks->at("BlackFlow@Roguelike@MovementInventoryDiscardBeforeQuotaExcess")
            .get("text", std::vector<std::string> {});
    REQUIRE(
        before_quota_excess ==
        std::vector<std::string> { "白模鸟", "涂装黎博利", "枯苔藓球", "报废假肢", "报废轮子" });
}

TEST_CASE("BlackFlow overload cleanup can scan and discard beyond the first inventory columns")
{
    const auto plan = inventory_full_scan_plan();
    REQUIRE(std::ranges::count(plan, InventoryScanAction::AdvanceTowardEnd) == InventoryMaximumSwipes);
    REQUIRE(std::ranges::count(plan, InventoryScanAction::InspectNewRightColumn) == InventoryMaximumSwipes);

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& all_items = tasks->at("BlackFlow@Roguelike@MovementInventoryAllItems");
    const auto names = all_items.get("text", std::vector<std::string> {});
    REQUIRE(std::ranges::find(names, "白模鱼") != names.end());
    REQUIRE(std::ranges::find(names, "枯苔藓球") != names.end());
    REQUIRE(std::ranges::find(names, "结构性原理") != names.end());

    REQUIRE(tasks->at("BlackFlow@Roguelike@MovementInventorySwipe").get("action", std::string {}) == "Swipe");
    REQUIRE(tasks->at("BlackFlow@Roguelike@MovementInventoryDiscardButton")
                .get("text", std::vector<std::string> {}) == std::vector<std::string> { "丢弃" });
    const auto discard_roi = tasks->at("BlackFlow@Roguelike@MovementInventoryDiscardButton")
                                 .get("roi", std::vector<int> {});
    REQUIRE(discard_roi.size() == 4);
    const auto contains = [&](int x, int y) {
        return x >= discard_roi[0] && y >= discard_roi[1] &&
               x < discard_roi[0] + discard_roi[2] && y < discard_roi[1] + discard_roi[3];
    };
    // 2026-08-29 两种实际详情布局：“结构性原理”双按钮页约 (470, 510)，
    // “浪花”单按钮页约 (665, 510)。同一个 OCR 框必须覆盖两个“丢弃”文字中心。
    REQUIRE(contains(470, 510));
    REQUIRE(contains(665, 510));

    // 2026-08-28 现场：名称框 [395,260,67,19]，实际图标中心约为 (428,203)。
    REQUIRE(inventory_part_detail_click_rect({ 395, 260, 67, 19 }) == asst::Rect { 416, 191, 24, 24 });
}

TEST_CASE("BlackFlow inventory scan rejects a right-edge rebound frame")
{
    // 2026-09-01 实际滑动：正常页最右名称中心依次向右推进，到底回弹后从 934 退到 859。
    REQUIRE_FALSE(inventory_scan_rebounded(824, 836));
    REQUIRE_FALSE(inventory_scan_rebounded(836, 891));
    REQUIRE_FALSE(inventory_scan_rebounded(900, 870));
    REQUIRE(inventory_scan_rebounded(934, 859));
    REQUIRE_FALSE(inventory_scan_rebounded(std::nullopt, 859));
}

TEST_CASE("BlackFlow overload cleanup confirms discarding the equipped processing item")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string task_name = "BlackFlow@Roguelike@MovementInventoryEquippedDiscardConfirm";
    REQUIRE(tasks->contains(task_name));
    const auto& confirm = tasks->at(task_name);
    REQUIRE(confirm.get("baseTask", std::string {}) ==
            "BlackFlow@Roguelike@MovementInventoryOverloadOpen");

    std::ifstream input(repository_root /
                        "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp");
    REQUIRE(input.good());
    const std::string source {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    const std::size_t discard = source.find("run_task(InventoryDiscardButtonTask");
    REQUIRE(discard != std::string::npos);
    const std::size_t confirm_discard = source.find("InventoryEquippedDiscardConfirmTask", discard);
    REQUIRE(confirm_discard != std::string::npos);
}

TEST_CASE("BlackFlow overload cleanup revalidates the banner after a full scan before discarding")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream input(repository_root /
                        "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp");
    REQUIRE(input.good());
    const std::string source {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const std::size_t scan = source.find("auto parts = scan_all_parts();");
    REQUIRE(scan != std::string::npos);
    const std::size_t select = source.find("const auto selected = std::ranges::min_element", scan);
    REQUIRE(select != std::string::npos);
    const std::size_t final_overload_check = source.find("inventory_is_overloaded()", scan);

    // A banner sampled immediately after the previous discard may still be stale. The inventory scan
    // takes long enough for the UI to settle, so the live banner must be checked again before committing
    // another destructive action.
    REQUIRE(final_overload_check < select);
}

TEST_CASE("BlackFlow overload cleanup uses remaining charges only within the same processing-item kind")
{
    const InventoryDiscardRank fixed_last { InventoryDiscardBand::BeforeQuotaExcess, 4, 3 };
    const InventoryDiscardRank excess_earlier_kind { InventoryDiscardBand::QuotaExcess, 16, 3 };
    const InventoryDiscardRank excess_later_kind { InventoryDiscardBand::QuotaExcess, 20, 1 };
    const InventoryDiscardRank ordinary_earlier_kind { InventoryDiscardBand::Normal, 5, 1 };
    const InventoryDiscardRank ordinary_later_kind_with_more_uses { InventoryDiscardBand::Normal, 6, 3 };
    const InventoryDiscardRank ordinary_later_kind_with_fewer_uses { InventoryDiscardBand::Normal, 6, 1 };

    REQUIRE(fixed_last < excess_later_kind);
    REQUIRE(excess_later_kind < ordinary_earlier_kind);
    // 多种加工品同时超额时，仍先按原丢弃表中的类型顺序；
    // 不会因为后一种的剩余次数更少就跨类型提前。
    REQUIRE(excess_earlier_kind < excess_later_kind);
    // 非超额的不同类加工品仍严格服从既有丢弃优先级。
    REQUIRE(ordinary_earlier_kind < ordinary_later_kind_with_fewer_uses);
    // 只有同类实例才由剩余次数打破平局。
    REQUIRE(ordinary_later_kind_with_fewer_uses < ordinary_later_kind_with_more_uses);
}

TEST_CASE("BlackFlow overload cleanup promotes only processing-item instances beyond the store quota")
{
    REQUIRE(automation_store_purchase_quota("标准引擎") == 2);
    REQUIRE(automation_store_purchase_quota("坎诺特的触须") == 1);
    REQUIRE_FALSE(automation_store_purchase_quota("一次性喷气背包").has_value());

    const std::array engine_charges { 3, 1, 2 };
    // 三个标准引擎只有一个超出配额，应选剩余 1 次的第二个实例。
    REQUIRE(inventory_quota_excess_indices(engine_charges, 2) == std::vector<std::size_t> { 1 });

    const std::array tentacle_charges { 3, 1, 2 };
    // 触须配额为 1，超出的两个实例按剩余次数从少到多提前。
    REQUIRE(inventory_quota_excess_indices(tentacle_charges, 1) == std::vector<std::size_t> { 1, 2 });

    const std::array within_quota { 1, 3 };
    REQUIRE(inventory_quota_excess_indices(within_quota, 2).empty());
}

TEST_CASE("BlackFlow processing inventory OCR recognizes every ordered type boundary")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto movement_inventory_names =
        tasks->at("BlackFlow@Roguelike@MovementInventoryItems").get("text", std::vector<std::string> {});
    const auto natural_names =
        tasks->at("BlackFlow@Roguelike@InventoryNaturalPriority").get("text", std::vector<std::string> {});
    REQUIRE_FALSE(natural_names.empty());
    for (const std::string& natural_name : natural_names) {
        REQUIRE(std::ranges::find(movement_inventory_names, natural_name) != movement_inventory_names.end());
    }
    const auto concept_names =
        tasks->at("BlackFlow@Roguelike@InventoryConceptPriority").get("text", std::vector<std::string> {});
    REQUIRE_FALSE(concept_names.empty());
    for (const std::string& concept_name : concept_names) {
        REQUIRE(std::ranges::find(movement_inventory_names, concept_name) != movement_inventory_names.end());
    }
    REQUIRE(std::ranges::find(movement_inventory_names, "待收集零件") != movement_inventory_names.end());
}

TEST_CASE("BlackFlow floor five normalizes the viewport and tries every supported grid width")
{
    const auto viewport = floor_viewport_profile(5);
    REQUIRE(viewport.has_value());
    REQUIRE(viewport->swipe_left_count == 1);
    REQUIRE(viewport->before_every_capture);

    const auto profiles = floor_profile_candidates(5);
    REQUIRE(profiles.size() == 2);
    REQUIRE(profiles[0] == FloorProfile { 5, 5, 10 });
    REQUIRE(profiles[1] == FloorProfile { 5, 5, 9 });

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    const auto& swipe = tasks->at("BlackFlow@Roguelike@MapViewportFloor5SwipeLeft");
    REQUIRE(swipe.get("action", std::string {}) == "Swipe");
    const auto start = swipe.get("specificRect", std::vector<int> {});
    const auto finish = swipe.get("rectMove", std::vector<int> {});
    REQUIRE(start.size() == 4);
    REQUIRE(finish.size() == 4);
    REQUIRE(start[0] > finish[0]);

    const auto& wait = tasks->at("BlackFlow@Roguelike@MapCaptureStabilityWait");
    REQUIRE(wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(wait.get("postDelay", 0) >= 200);
}

TEST_CASE("BlackFlow keeps the normalized floor-five viewport after a parts-box round trip")
{
    REQUIRE(should_normalize_map_viewport(true, false));
    REQUIRE_FALSE(should_normalize_map_viewport(true, true));
}

TEST_CASE("BlackFlow resets cached topology when a same-floor recollection creates a new map generation")
{
    REQUIRE_FALSE(topology_cache_requires_reset(std::nullopt, 1));
    REQUIRE_FALSE(topology_cache_requires_reset(std::optional<std::uint64_t> { 1 }, 1));
    REQUIRE(topology_cache_requires_reset(std::optional<std::uint64_t> { 1 }, 2));
}

TEST_CASE("BlackFlow map OCR accepts a uniquely leading one-character error in short node names")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto config =
        json::open(repository_root / "resource/roguelike/BlackFlow/map_perception/edge.json");
    REQUIRE(config.has_value());
    const int short_fuzzy_length = config->get("ocr_short_exact_length", 0);
    REQUIRE(short_fuzzy_length == 3);

    // 实际现场把“作战”读成“乍战”：两字等长、编辑距离一，并显著领先次优候选。
    REQUIRE(accept_node_ocr_label_match(false, 2, 2, 1, 0.5, 0.25, 0.72, 0.12, short_fuzzy_length));
    REQUIRE(
        accept_node_ocr_label_match(
            false,
            3,
            3,
            1,
            2.0 / 3.0,
            1.0 / 3.0,
            0.72,
            0.12,
            short_fuzzy_length));
    REQUIRE_FALSE(
        accept_node_ocr_label_match(false, 1, 2, 1, 0.5, 0.0, 0.72, 0.12, short_fuzzy_length));
    REQUIRE_FALSE(
        accept_node_ocr_label_match(false, 2, 2, 1, 0.5, 0.45, 0.72, 0.12, short_fuzzy_length));
}

TEST_CASE("BlackFlow fixed grid alignment tolerates the observed post-battle viewport offset")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto config =
        json::open(repository_root / "resource/roguelike/BlackFlow/map_perception/edge.json");
    REQUIRE(config.has_value());

    // The captured fourth-floor failure was translated upward by about 38 pixels while the
    // node spacing stayed unchanged. Translation seeding must be able to associate those
    // anchors without widening the final per-cell hit tolerance.
    REQUIRE(config->get("fixed_grid_translation_limit", 0) >= 40);
    REQUIRE(config->get("fixed_grid_translation_tolerance", 0.0) >= 40.0);
    REQUIRE(config->get("fixed_grid_hit_tolerance", 100.0) <= 25.0);
}

TEST_CASE("BlackFlow eerie merchant scans the lower shelf before choosing a product")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& to_top = tasks->at("BlackFlow@Roguelike@AutomationShopGoodsSwipeToTop");
    const auto& to_bottom = tasks->at("BlackFlow@Roguelike@AutomationShopGoodsSwipeToBottom");
    REQUIRE(to_top.get("action", std::string {}) == "Swipe");
    REQUIRE(to_bottom.get("action", std::string {}) == "Swipe");

    const auto top_start = to_top.get("specificRect", std::vector<int> {});
    const auto top_finish = to_top.get("rectMove", std::vector<int> {});
    const auto bottom_start = to_bottom.get("specificRect", std::vector<int> {});
    const auto bottom_finish = to_bottom.get("rectMove", std::vector<int> {});
    REQUIRE(top_start.size() == 4);
    REQUIRE(top_finish.size() == 4);
    REQUIRE(bottom_start.size() == 4);
    REQUIRE(bottom_finish.size() == 4);
    REQUIRE(top_start[1] < top_finish[1]);
    REQUIRE(bottom_start[1] > bottom_finish[1]);

    const auto top_parameters = to_top.get("specialParams", std::vector<int> {});
    const auto bottom_parameters = to_bottom.get("specialParams", std::vector<int> {});
    REQUIRE(top_parameters.size() >= 2);
    REQUIRE(bottom_parameters.size() >= 2);
    // 顶/底视图使用事件、招募同档的短促长划；禁用仅适合单方向的额外滑动。
    REQUIRE(std::abs(top_start[1] - top_finish[1]) >= 400);
    REQUIRE(std::abs(bottom_start[1] - bottom_finish[1]) >= 400);
    REQUIRE(top_parameters[0] >= 150);
    REQUIRE(top_parameters[0] <= 260);
    REQUIRE(bottom_parameters[0] >= 150);
    REQUIRE(bottom_parameters[0] <= 260);
    REQUIRE(top_parameters[1] == 0);
    REQUIRE(bottom_parameters[1] == 0);

    // 秘境行商虽然复用任务定义链，但有独立手势参数；不能被诡意行商调参意外改变。
    const auto& cultivate_to_bottom = tasks->at("BlackFlow@Roguelike@AutomationCultivateGoodsSwipeToBottom");
    const auto cultivate_parameters = cultivate_to_bottom.get("specialParams", std::vector<int> {});
    REQUIRE(cultivate_parameters.size() >= 2);
    REQUIRE(cultivate_parameters[0] == 350);
    REQUIRE(cultivate_parameters[1] == 1);

    const auto top_roi = tasks->at("BlackFlow@Roguelike@AutomationShopGoods")
                             .get("roi", std::vector<int> {});
    const auto bottom_roi = tasks->at("BlackFlow@Roguelike@AutomationShopGoodsBottom")
                                .get("roi", std::vector<int> {});
    REQUIRE(top_roi.size() == 4);
    REQUIRE(bottom_roi.size() == 4);
    // 第一排只在回顶画面识别；第二、三排都在下滑后的画面识别和点击。
    REQUIRE(top_roi[1] + top_roi[3] <= 300);
    REQUIRE(bottom_roi[1] <= top_roi[1] + top_roi[3]);
    REQUIRE(bottom_roi[1] + bottom_roi[3] > 500);

    // 秘境行商最多两行，当前商品名称 ROI 一次覆盖完整货架，不需要翻页。
    const auto scrap_roi = tasks->at("BlackFlow@Roguelike@AutomationCultivateBuyItems")
                               .get("roi", std::vector<int> {});
    REQUIRE(scrap_roi.size() == 4);
    REQUIRE(scrap_roi[3] >= 280);
}

TEST_CASE("BlackFlow automation merchant uses the configured recruitment and part priorities")
{
    constexpr std::array<std::string_view, 29> expected_shop_priority {
        "沙盘β",
        "医者-新典训",
        "医疗招募券",
        "重装招募券",
        "精锐重装招募券",
        "堡垒协议招募券",
        "特种招募券",
        "一次性喷气背包",
        "重弹簧",
        "种子",
        "老妈妈的融雪",
        "坎诺特的触须",
        "试作外骨骼",
        "小八界",
        "气垫底座",
        "标准引擎",
        "报废轮子",
        "报废假肢",
        "多生苔藓",
        "雾滚草",
        "白模鱼",
        "猎犬咖啡",
        "悲伤的红",
        "《炎国字汇》",
        "三尺万象",
        "医者-自医",
        "铁卫-侵掠",
        "迷藏",
        "“小格兰法洛”",
    };
    constexpr std::array<std::string_view, 14> expected_scrap_shop_priority {
        "一次性喷气背包",
        "重弹簧",
        "种子",
        "老妈妈的融雪",
        "坎诺特的触须",
        "试作外骨骼",
        "小八界",
        "气垫底座",
        "标准引擎",
        "报废轮子",
        "报废假肢",
        "多生苔藓",
        "雾滚草",
        "白模鱼",
    };
    REQUIRE(ShopBuyPriority == expected_shop_priority);
    REQUIRE(ScrapShopBuyPriority == expected_scrap_shop_priority);

    const auto shop_wheel = std::ranges::find(ShopBuyPriority, "报废轮子");
    const auto shop_limb = std::ranges::find(ShopBuyPriority, "报废假肢");
    const auto shop_moss = std::ranges::find(ShopBuyPriority, "多生苔藓");
    const auto shop_tumbleweed = std::ranges::find(ShopBuyPriority, "雾滚草");
    const auto shop_fish = std::ranges::find(ShopBuyPriority, "白模鱼");
    REQUIRE(shop_wheel != ShopBuyPriority.end());
    REQUIRE(shop_limb == std::next(shop_wheel));
    REQUIRE(shop_moss == std::next(shop_limb));
    REQUIRE(shop_tumbleweed == std::next(shop_moss));
    REQUIRE(shop_fish == std::next(shop_tumbleweed));

    const auto scrap_wheel = std::ranges::find(ScrapShopBuyPriority, "报废轮子");
    const auto scrap_limb = std::ranges::find(ScrapShopBuyPriority, "报废假肢");
    const auto scrap_moss = std::ranges::find(ScrapShopBuyPriority, "多生苔藓");
    const auto scrap_tumbleweed = std::ranges::find(ScrapShopBuyPriority, "雾滚草");
    const auto scrap_fish = std::ranges::find(ScrapShopBuyPriority, "白模鱼");
    REQUIRE(scrap_wheel != ScrapShopBuyPriority.end());
    REQUIRE(scrap_limb == std::next(scrap_wheel));
    REQUIRE(scrap_moss == std::next(scrap_limb));
    REQUIRE(scrap_tumbleweed == std::next(scrap_moss));
    REQUIRE(scrap_fish == std::next(scrap_tumbleweed));

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto trophy_priority =
        tasks->at("BlackFlow@Roguelike@TrophyRewardPriority").get("text", std::vector<std::string> {});
    const auto seed = std::ranges::find(trophy_priority, "种子");
    REQUIRE(seed != trophy_priority.end());
    REQUIRE(seed != trophy_priority.begin());
    REQUIRE(std::next(seed) != trophy_priority.end());
    REQUIRE(*std::prev(seed) == "小八界");
    REQUIRE(*std::next(seed) == "报废轮子");
    REQUIRE(trophy_priority[trophy_priority.size() - 2] == "白模鸟");
    REQUIRE(trophy_priority.back() == "笼控器");

}

TEST_CASE("BlackFlow merchant purchase limits include usable parts held on node entry")
{
    REQUIRE_FALSE(automation_store_purchase_quota("一次性喷气背包").has_value());
    REQUIRE(automation_store_purchase_quota("坎诺特的触须") == 1);
    REQUIRE(automation_store_purchase_quota("小八界") == 1);

    for (const std::string_view name : SharedStorePartBuyPriority) {
        CAPTURE(name);
        if (name == "一次性喷气背包" || name == "坎诺特的触须" || name == "小八界") {
            continue;
        }
        REQUIRE(automation_store_purchase_quota(name) == 2);
    }
    REQUIRE_FALSE(automation_store_purchase_quota("猎犬咖啡").has_value());

    REQUIRE(automation_store_purchase_quota_allowed("重弹簧", 0, 0));
    REQUIRE(automation_store_purchase_quota_allowed("重弹簧", 0, 1));
    REQUIRE_FALSE(automation_store_purchase_quota_allowed("重弹簧", 0, 2));
    REQUIRE(automation_store_purchase_quota_allowed("重弹簧", 1, 0));
    REQUIRE_FALSE(automation_store_purchase_quota_allowed("重弹簧", 1, 1));
    REQUIRE_FALSE(automation_store_purchase_quota_allowed("重弹簧", 2, 0));
    REQUIRE(automation_store_purchase_quota_allowed("坎诺特的触须", 0, 0));
    REQUIRE_FALSE(automation_store_purchase_quota_allowed("坎诺特的触须", 1, 0));
    REQUIRE(automation_store_purchase_quota_allowed("一次性喷气背包", 100, 100));

    const std::vector<RunResources::MovementInstance> inventory {
        { MovementKind::M04, 1, 0, false },
        { MovementKind::M04, 0, 1, false },
        { MovementKind::M10, 2, 2, true },
    };
    REQUIRE(usable_processing_item_count("重弹簧", inventory) == 1);
    REQUIRE(usable_processing_item_count("坎诺特的触须", inventory) == 1);
    REQUIRE(usable_processing_item_count("种子", inventory) == 0);
}

TEST_CASE("BlackFlow node-entry inventory projection drops an exhausted processing item")
{
    RunState run;
    run.active_movement = MovementKind::M04;
    run.resources.movement_instances = {
        { MovementKind::M04, 2, 0, false },
        { MovementKind::M04, 1, 1, true },
    };
    rebuild_movement_aggregates(run.resources);

    REQUIRE(project_consumed_entry_processing_item(run, MovementKind::M04));
    REQUIRE(run.resources.movement_instances.size() == 1);
    REQUIRE(run.resources.movement_instances.front().remaining_charges == 2);
    REQUIRE_FALSE(run.active_movement.has_value());
    REQUIRE(run.resources.movement_pieces.at(MovementKind::M04) == 1);
    REQUIRE(run.resources.movement_charges.at(MovementKind::M04) == 2);
}

TEST_CASE("BlackFlow eerie merchant limits the configured collectibles across one run")
{
    constexpr std::array<std::string_view, 8> expected_collectibles {
        "猎犬咖啡",
        "悲伤的红",
        "《炎国字汇》",
        "三尺万象",
        "医者-自医",
        "铁卫-侵掠",
        "迷藏",
        "“小格兰法洛”",
    };
    REQUIRE(EerieStoreCollectibleBuyPriority == expected_collectibles);
    REQUIRE(EerieStoreCollectibleRunPurchaseLimit == 2);

    for (const std::string_view name : EerieStoreCollectibleBuyPriority) {
        CAPTURE(name);
        REQUIRE(is_eerie_store_collectible(name));
        REQUIRE(eerie_store_collectible_purchase_allowed(name, 0));
        REQUIRE(eerie_store_collectible_purchase_allowed(name, 1));
        REQUIRE_FALSE(eerie_store_collectible_purchase_allowed(name, 2));
    }
    REQUIRE_FALSE(is_eerie_store_collectible("雾滚草"));
    REQUIRE(eerie_store_collectible_purchase_allowed("雾滚草", 2));
}

TEST_CASE("BlackFlow merchant OCR keeps concept recognition separate from purchase eligibility")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    constexpr std::array<std::string_view, 6> concepts {
        "白模鱼",
        "白模狗",
        "涂装阿戈尔",
        "涂装佩洛",
        "白模鸟",
        "涂装黎博利",
    };
    constexpr std::array<std::string_view, 5> recognized_but_not_purchased {
        "白模狗",
        "涂装阿戈尔",
        "涂装佩洛",
        "白模鸟",
        "涂装黎博利",
    };

    for (const std::string_view task_name : {
             "BlackFlow@Roguelike@AutomationShopGoods",
             "BlackFlow@Roguelike@AutomationCultivateBuyItems",
         }) {
        CAPTURE(task_name);
        const auto ocr_names = tasks->at(std::string(task_name)).get("text", std::vector<std::string> {});
        REQUIRE(tasks->at(std::string(task_name)).get("fuzzyMatch", false));
        for (const std::string_view concept_name : concepts) {
            REQUIRE(std::ranges::find(ocr_names, concept_name) != ocr_names.end());
            const FuzzyTextMatch match = fuzzy_match_ocr_text(concept_name, ocr_names);
            REQUIRE(match.accepted);
            REQUIRE(match.exact);
            REQUIRE(match.canonical == concept_name);
        }
    }

    REQUIRE(std::ranges::find(ShopBuyPriority, "白模鱼") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ScrapShopBuyPriority, "白模鱼") != ScrapShopBuyPriority.end());
    for (const std::string_view protected_concept : recognized_but_not_purchased) {
        CAPTURE(protected_concept);
        REQUIRE(std::ranges::find(ShopBuyPriority, protected_concept) == ShopBuyPriority.end());
        REQUIRE(std::ranges::find(ScrapShopBuyPriority, protected_concept) == ScrapShopBuyPriority.end());
    }

    const std::vector<StoreGoodOffer> dog_only { { "白模狗", 4, true, true } };
    REQUIRE_FALSE(select_preferred_affordable_good(ScrapShopBuyPriority, dog_only, 20).has_value());
    const std::vector<StoreGoodOffer> fish_only { { "白模鱼", 4, true, true } };
    REQUIRE(select_preferred_affordable_good(ScrapShopBuyPriority, fish_only, 20) == 0);
}

TEST_CASE("BlackFlow protected natural parts respect minimum sale prices")
{
    REQUIRE(minimum_natural_sale_price("雾滚草") == 8);
    REQUIRE(minimum_natural_sale_price("板藤") == 32);
    REQUIRE(minimum_natural_sale_price("浪花") == 8);
    REQUIRE(minimum_natural_sale_price("多生苔藓") == 12);
    REQUIRE_FALSE(minimum_natural_sale_price("血蕈").has_value());

    REQUIRE_FALSE(natural_sale_price_allowed("雾滚草", 7));
    REQUIRE(natural_sale_price_allowed("雾滚草", 8));
    REQUIRE_FALSE(natural_sale_price_allowed("板藤", 31));
    REQUIRE(natural_sale_price_allowed("板藤", 32));
    REQUIRE_FALSE(natural_sale_price_allowed("浪花", std::nullopt));
    REQUIRE_FALSE(natural_sale_price_allowed("多生苔藓", 11));
    REQUIRE(natural_sale_price_allowed("多生苔藓", 12));
    REQUIRE(natural_sale_price_allowed("血蕈", std::nullopt));
}

TEST_CASE("BlackFlow store refresh ledger persists per shop in one map generation")
{
    AutomationStoreRefreshLedger ledger;
    const AutomationStoreIdentity eerie { 7, 101, AutomationStoreKind::Eerie };
    const AutomationStoreIdentity same_eerie { 7, 101, AutomationStoreKind::Eerie };
    const AutomationStoreIdentity other_node { 7, 102, AutomationStoreKind::Eerie };
    const AutomationStoreIdentity renewed_map { 8, 101, AutomationStoreKind::Eerie };
    const AutomationStoreIdentity secret { 7, 101, AutomationStoreKind::Secret };

    REQUIRE(ledger.refresh_count(eerie) == 0);
    REQUIRE(ledger.record_refresh(eerie) == 1);
    REQUIRE(ledger.record_refresh(eerie) == 2);
    REQUIRE(ledger.refresh_count(same_eerie) == 2);
    REQUIRE_FALSE(automation_store_can_refresh(ledger.refresh_count(same_eerie), 100, 12));

    REQUIRE(ledger.refresh_count(other_node) == 0);
    REQUIRE(ledger.refresh_count(renewed_map) == 0);
    REQUIRE(ledger.refresh_count(secret) == 0);
    REQUIRE(automation_store_refresh_price(0) == 4);
    REQUIRE(automation_store_refresh_price(1) == 8);

    ledger.clear();
    REQUIRE(ledger.refresh_count(eerie) == 0);
}

TEST_CASE("BlackFlow merchant price OCR prefers the rightmost numeric result")
{
    REQUIRE(price_text_hsv_allowed(0, 12, 238));
    REQUIRE_FALSE(price_text_hsv_allowed(88, 230, 230));
    REQUIRE_FALSE(price_text_hsv_allowed(0, 10, 70));

    // 现场截图里左侧图标杂讯被识别成 114，真正报价 2 位于卡片右侧。
    const std::vector<std::pair<int, std::string_view>> candidates {
        { 714, "114" },
        { 772, "2" },
    };
    REQUIRE(rightmost_numeric_price(candidates) == 2);
    // 现场的 17/19 报价里，细窄的十位 1 会被字符模型读成竖线。
    REQUIRE(rightmost_numeric_price(std::vector<std::pair<int, std::string_view>> { { 772, "|7" } }) == 17);
    REQUIRE(rightmost_numeric_price(std::vector<std::pair<int, std::string_view>> { { 772, "|9" } }) == 19);
    REQUIRE_FALSE(rightmost_numeric_price(std::vector<std::pair<int, std::string_view>> { { 772, "价格" } })
                      .has_value());

    // 上排现场：名称框 [629,182,60,22]，售价位于同一卡片右下。
    const asst::Rect price_roi = merchant_price_roi(asst::Rect { 629, 182, 60, 22 });
    REQUIRE(price_roi == asst::Rect { 729, 332, 100, 45 });
}

TEST_CASE("BlackFlow abandonment reset waits until the settlement page reaches StartExplore")
{
    REQUIRE(
        abandon_reset_disposition(false) == AbandonResetDisposition::DeferUntilStartExplore);
    REQUIRE(abandon_reset_disposition(true) == AbandonResetDisposition::FinishAndStop);

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto base_tasks = json::open(repository_root / "resource/tasks/Roguelike/base.json");
    REQUIRE(base_tasks.has_value());
    const auto successors =
        base_tasks->at("Roguelike@AbandonConfirm").get("next", std::vector<std::string> {});
    const auto settlement = std::ranges::find(successors, "Roguelike@MissionFailedFlag2");
    const auto restart = std::ranges::find(successors, "Roguelike@StartExplore");
    REQUIRE(settlement != successors.end());
    REQUIRE(restart != successors.end());
    REQUIRE(settlement < restart);
}

TEST_CASE("BlackFlow initial StartExplore keeps the initialized run log")
{
    REQUIRE(
        start_explore_run_disposition(false, false) == StartExploreRunDisposition::KeepInitialRun);
    REQUIRE(
        start_explore_run_disposition(false, true) == StartExploreRunDisposition::FinishAndStartNext);
    REQUIRE(
        start_explore_run_disposition(true, false) == StartExploreRunDisposition::FinishAndStartNext);
}

TEST_CASE("BlackFlow completed run is verified before its directory is removed")
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("maa-blackflow-run-archive-test-" + std::to_string(nonce));
    ScopedDirectoryCleanup cleanup(root);
    const std::filesystem::path run = root / "run-20260904-010203-123456";
    const std::filesystem::path nested = run / "collection-popups" / "floor-1" / "node-1";
    REQUIRE(std::filesystem::create_directories(nested));
    {
        std::ofstream output(run / "run.log", std::ios::binary);
        output << "run.started\nrun.ended\n";
    }
    {
        std::ofstream output(nested / std::filesystem::path(u8"归因.json"), std::ios::binary);
        output << R"({"source":"丰饶树冢"})";
    }

    RunArchiveResult result;
    std::string error;
    REQUIRE(archive_completed_run_directory(run, result, &error));
    INFO(error);
    REQUIRE_FALSE(std::filesystem::exists(run));
    REQUIRE(std::filesystem::is_regular_file(result.archive_path));
    REQUIRE(result.entry_count == 2);
    REQUIRE(result.uncompressed_bytes > 0);
}

TEST_CASE("BlackFlow run archive failure preserves the source directory")
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("maa-blackflow-run-archive-collision-test-" + std::to_string(nonce));
    ScopedDirectoryCleanup cleanup(root);
    const std::filesystem::path run = root / "run-20260904-010203-654321";
    REQUIRE(std::filesystem::create_directories(run));
    {
        std::ofstream output(run / "run.log", std::ios::binary);
        output << "must survive";
    }
    {
        std::ofstream output(root / "run-20260904-010203-654321.zip", std::ios::binary);
        output << "existing archive";
    }

    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(run, result, &error));
    REQUIRE(std::filesystem::is_directory(run));
    REQUIRE(std::filesystem::is_regular_file(run / "run.log"));
    REQUIRE(error.find("already exists") != std::string::npos);
}

TEST_CASE("BlackFlow completed run archive queue never waits for compression")
{
    struct AsyncState
    {
        std::mutex mutex;
        std::condition_variable ready;
        bool callback_started = false;
        bool release_callback = false;
        bool callback_completed = false;
        RunArchiveResult result;
        std::string error;
    };

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("maa-blackflow-run-archive-queue-test-" + std::to_string(nonce));
    ScopedDirectoryCleanup cleanup(root);
    const std::filesystem::path run = root / "run-20260904-010203-777777";
    REQUIRE(std::filesystem::create_directories(run));
    {
        std::ofstream output(run / "run.log", std::ios::binary);
        output << "asynchronous archive";
    }

    const auto state = std::make_shared<AsyncState>();
    std::string enqueue_error;
    const auto before = std::chrono::steady_clock::now();
    REQUIRE(enqueue_completed_run_archive(
        run,
        [state](const std::filesystem::path&, const RunArchiveResult& result, const std::string& error) {
            std::unique_lock lock(state->mutex);
            state->callback_started = true;
            state->result = result;
            state->error = error;
            state->ready.notify_all();
            state->ready.wait_for(
                lock,
                std::chrono::seconds(2),
                [state]() { return state->release_callback; });
            state->callback_completed = true;
            state->ready.notify_all();
        },
        &enqueue_error));
    const auto enqueue_duration = std::chrono::steady_clock::now() - before;
    INFO(enqueue_error);
    REQUIRE(enqueue_duration < std::chrono::milliseconds(500));

    std::unique_lock lock(state->mutex);
    REQUIRE(state->ready.wait_for(
        lock,
        std::chrono::seconds(5),
        [state]() { return state->callback_started; }));
    state->release_callback = true;
    state->ready.notify_all();
    REQUIRE(state->ready.wait_for(
        lock,
        std::chrono::seconds(5),
        [state]() { return state->callback_completed; }));
    REQUIRE(state->error.empty());
    REQUIRE(std::filesystem::is_regular_file(state->result.archive_path));
    REQUIRE_FALSE(std::filesystem::exists(run));
}

TEST_CASE("BlackFlow merchant sale recognition never includes processing products")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    // 现场回归图中的这四张卡片都是加工品；即使名称 OCR 全部成功，也不能进入出售候选。
    constexpr std::array<std::string_view, 4> processing_products {
        "报废假肢",
        "标准引擎",
        "老妈妈的融雪",
        "结构性原理",
    };
    for (const std::string_view task_name : {
             "BlackFlow@Roguelike@AutomationShopSellItems",
             "BlackFlow@Roguelike@AutomationCultivateSellItems",
         }) {
        const auto sellable = tasks->at(std::string(task_name)).get("text", std::vector<std::string> {});
        for (const std::string_view processing_product : processing_products) {
            REQUIRE(std::ranges::find(sellable, processing_product) == sellable.end());
        }
    }
}

TEST_CASE("BlackFlow merchant sale OCR recognizes protected concepts without selling them")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    for (const std::string_view task_name : {
             "BlackFlow@Roguelike@AutomationShopSellItems",
             "BlackFlow@Roguelike@AutomationCultivateSellItems",
             "BlackFlow@Roguelike@CultivateSellItems",
         }) {
        CAPTURE(task_name);
        const auto recognized = tasks->at(std::string(task_name)).get("text", std::vector<std::string> {});
        REQUIRE(tasks->at(std::string(task_name)).get("fuzzyMatch", false));
        for (const std::string_view concept_name : MerchantConceptRecognitionVocabulary) {
            REQUIRE(std::ranges::find(recognized, concept_name) != recognized.end());
            const FuzzyTextMatch match = fuzzy_match_ocr_text(concept_name, recognized);
            REQUIRE(match.accepted);
            REQUIRE(match.exact);
            REQUIRE(match.canonical == concept_name);
        }
    }
    for (const std::string_view protected_concept : ProtectedMerchantConcepts) {
        REQUIRE_FALSE(merchant_sale_allowed(protected_concept));
    }
    REQUIRE(merchant_sale_allowed("白模鸟"));
    REQUIRE(merchant_sale_allowed("涂装黎博利"));
}

TEST_CASE("BlackFlow automation shops use the required post-purchase settle delay")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string shop_prefix = "BlackFlow@Roguelike@AutomationShop";
    const std::string shop_settle_name = shop_prefix + "PurchaseSettle";
    const auto& shop_confirm = tasks->at(shop_prefix + "BuyConfirm");
    const auto shop_confirm_next = shop_confirm.get("next", std::vector<std::string> {});
    REQUIRE(shop_confirm_next == std::vector<std::string> { shop_prefix + "PurchaseTransition" });
    REQUIRE(
        shop_confirm.get("reduceOtherTimes", std::vector<std::string> {}) ==
        std::vector<std::string> { shop_prefix + "PurchaseTransition*50" });

    const auto& purchase_transition = tasks->at(shop_prefix + "PurchaseTransition");
    REQUIRE(purchase_transition.get("baseTask", std::string {}) == shop_prefix + "OrdinaryPurchaseTransition");
    REQUIRE(
        tasks->at(shop_prefix + "OrdinaryPurchaseTransition").get("next", std::vector<std::string> {}) ==
        std::vector<std::string> {
            "BlackFlow@Roguelike@ChooseOperFlag",
            shop_settle_name,
            shop_prefix + "PurchaseWait",
        });

    const auto& shop_settle = tasks->at(shop_settle_name);
    REQUIRE(shop_settle.get("algorithm", std::string {}) != "JustReturn");
    REQUIRE(shop_settle.get("template", std::string {}) == "Roguelike@StageTraderLeave.png");
    REQUIRE(shop_settle.get("action", std::string {}) == "DoNothing");
    REQUIRE(shop_settle.get("postDelay", 0) >= 500);
    REQUIRE(
        shop_settle.get("next", std::vector<std::string> {}) == std::vector<std::string> {
                                                                    "BlackFlow@Roguelike@ChooseOperFlag",
                                                                    shop_prefix + "PurchaseSettleConfirmed",
                                                                    shop_prefix + "PurchaseWait",
                                                                });

    const auto& shop_settle_confirmed = tasks->at(shop_prefix + "PurchaseSettleConfirmed");
    REQUIRE(shop_settle_confirmed.get("template", std::string {}) == "Roguelike@StageTraderLeave.png");
    REQUIRE(shop_settle_confirmed.get("action", std::string {}) == "DoNothing");
    REQUIRE(
        shop_settle_confirmed.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { shop_prefix + "Decision" });

    const auto& shop_wait = tasks->at(shop_prefix + "PurchaseWait");
    REQUIRE(shop_wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(shop_wait.get("action", std::string {}) == "DoNothing");
    REQUIRE(shop_wait.get("postDelay", 0) >= 300);
    const auto shop_wait_next = shop_wait.get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(shop_wait_next, "BlackFlow@Roguelike@ChooseOperFlag") != shop_wait_next.end());
    REQUIRE(std::ranges::find(shop_wait_next, shop_settle_name) != shop_wait_next.end());
    REQUIRE(std::ranges::find(shop_wait_next, "#self") != shop_wait_next.end());
    REQUIRE(shop_wait.get("maxTimes", 0) >= 10);

    const auto& recruit_wait = tasks->at(shop_prefix + "RecruitPurchaseWait");
    REQUIRE(recruit_wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(recruit_wait.get("action", std::string {}) == "DoNothing");
    const auto recruit_wait_next = recruit_wait.get("next", std::vector<std::string> {});
    REQUIRE(
        recruit_wait_next == std::vector<std::string> { "BlackFlow@Roguelike@ChooseOperFlag", "#self" });
    REQUIRE(recruit_wait.get("maxTimes", 0) >= 50);
    REQUIRE(std::ranges::find(recruit_wait_next, shop_settle_name) == recruit_wait_next.end());

    const auto choose_oper_resets =
        tasks->at("BlackFlow@Roguelike@ChooseOperFlag").get("reduceOtherTimes", std::vector<std::string> {});
    REQUIRE(std::ranges::find(choose_oper_resets, shop_prefix + "PurchaseWait*20") != choose_oper_resets.end());
    REQUIRE(
        std::ranges::find(choose_oper_resets, shop_prefix + "PurchaseTransition*50") !=
        choose_oper_resets.end());

    const std::string cultivate_prefix = "BlackFlow@Roguelike@AutomationCultivate";
    const std::string cultivate_settle_name = cultivate_prefix + "PurchaseSettle";
    const auto cultivate_confirm_next =
        tasks->at(cultivate_prefix + "BuyConfirm").get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(cultivate_confirm_next, cultivate_settle_name) != cultivate_confirm_next.end());
    const auto& cultivate_settle = tasks->at(cultivate_settle_name);
    REQUIRE(cultivate_settle.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(cultivate_settle.get("postDelay", 0) >= 1000);
    REQUIRE(
        cultivate_settle.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { cultivate_prefix + "Decision" });
}

TEST_CASE("BlackFlow eerie merchant recruitment vouchers use the recruitment-only transition")
{
    REQUIRE(eerie_store_purchase_opens_recruitment("医疗招募券"));
    REQUIRE(eerie_store_purchase_opens_recruitment("精锐重装招募券"));
    REQUIRE(eerie_store_purchase_opens_recruitment("5星随机直升临时招募券"));
    REQUIRE_FALSE(eerie_store_purchase_opens_recruitment("一次性喷气背包"));
    REQUIRE_FALSE(eerie_store_purchase_opens_recruitment("医者-新典训"));
}

TEST_CASE("BlackFlow restart resumes an open encounter page before recognizing its floor label")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto begin_next =
        tasks->at("BlackFlow@Roguelike@Begin").get("next", std::vector<std::string> {});
    const auto resume = std::ranges::find(begin_next, "BlackFlow@Roguelike@ResumeEncounterPage");
    const auto floor = std::ranges::find(begin_next, "BlackFlow@Roguelike@NextLevel");
    REQUIRE(resume != begin_next.end());
    REQUIRE(floor != begin_next.end());
    REQUIRE(resume < floor);

    const auto& resume_task = tasks->at("BlackFlow@Roguelike@ResumeEncounterPage");
    REQUIRE(resume_task.get("baseTask", std::string {}) == "BlackFlow@Roguelike@StageEncounterOcr");
    REQUIRE_FALSE(resume_task.get("fuzzyMatch", true));
    REQUIRE(resume_task.get("action", std::string {}) == "DoNothing");
    REQUIRE(
        resume_task.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@StageEncounterJudgeClick" });

    const auto reward_next =
        tasks->at("BlackFlow@Roguelike@StageEncounterReward").get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(reward_next, "BlackFlow@Roguelike@NextLevel") != reward_next.end());
}

TEST_CASE("BlackFlow failed encounter choices resume the event instead of opening map recovery")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    REQUIRE(
        tasks->at("BlackFlow@Roguelike@StageEncounterResult").get("baseTask", std::string {}) ==
        "BlackFlow@Roguelike@StageEncounterReward");

    const auto recover_enter_next =
        tasks->at("BlackFlow@Roguelike@RecoverMap-Enter").get("next", std::vector<std::string> {});
    REQUIRE_FALSE(recover_enter_next.empty());
    REQUIRE(recover_enter_next.front() == "BlackFlow@Roguelike@ResumeEncounterPage");
    REQUIRE(std::ranges::find(recover_enter_next, "BlackFlow@Roguelike@RecoverMap") != recover_enter_next.end());
}

TEST_CASE("BlackFlow post-battle flow confirms the data synchronization prompt")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string confirmation = "BlackFlow@Roguelike@DataSyncConfirm";
    const auto next = tasks->at("BlackFlow@Roguelike@StartAction")
                          .get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(next, confirmation) != next.end());
    const auto begin_next = tasks->at("BlackFlow@Roguelike@Begin")
                                .get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(begin_next, confirmation) != begin_next.end());

    const auto& task = tasks->at(confirmation);
    REQUIRE(task.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(task.get("text", std::vector<std::string> {}) == std::vector<std::string> { "已更新" });
    REQUIRE_FALSE(task.get("fullMatch", false));
    REQUIRE_FALSE(task.get("fuzzyMatch", false));
    REQUIRE(task.get("action", std::string {}) == "ClickRect");
    REQUIRE(task.get("specificRect", std::vector<int> {}).size() == 4);
}

TEST_CASE("BlackFlow automation collection provides a strategy-change task")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& strategy = tasks->at("BlackFlow@Roguelike@StrategyChange_mode30002");
    REQUIRE(strategy.get("baseTask", std::string {}) == "Roguelike@StrategyChange_default");
}

TEST_CASE("BlackFlow move preview recognizes every explicitly routed named node")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto execution = json::open(repository_root / "resource/roguelike/BlackFlow/node_execution.json");
    REQUIRE(execution.has_value());

    bool fate_found = false;
    for (const auto& entry : execution->at("preview_names").as_array()) {
        if (entry.get("text", std::string {}) != "命运所指") {
            continue;
        }
        fate_found = true;
        REQUIRE(entry.get("node_type", std::string {}) == "incident");
    }
    REQUIRE(fate_found);
}

TEST_CASE("BlackFlow map OCR classifies Fate's Destination as a blocking incident node")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto manifest =
        json::open(repository_root / "resource/roguelike/BlackFlow/map_perception/templates/manifest.json");
    REQUIRE(manifest.has_value());

    bool fate_found = false;
    for (const auto& entry : manifest->at("ocr_labels").as_array()) {
        if (entry.get("text", std::string {}) != "命运所指") {
            continue;
        }
        fate_found = true;
        REQUIRE(entry.get("node_type", std::string {}) == "incident");
    }
    REQUIRE(fate_found);
    REQUIRE(default_traversal_for(NodeType::Incident).blocks_vision);
}

TEST_CASE("BlackFlow combat rewards finish the node transaction before returning to routing")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::vector<std::string> successors = tasks->at("BlackFlow@Roguelike@GetDropCompletedConfirmed")
                                                    .get("next", std::vector<std::string> {});

    REQUIRE(std::ranges::find(successors, "BlackFlow@Roguelike@NodeCompletionAction") != successors.end());
    REQUIRE(std::ranges::find(successors, "BlackFlow@Roguelike@NextLevel") == successors.end());
}

TEST_CASE("BlackFlow trophy reward selection precedes the generic drop click")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    constexpr std::array entry_tasks {
        "BlackFlow@Roguelike@GetDropSelectConfirmed",
        "BlackFlow@Roguelike@GetDrops",
    };
    for (const std::string_view entry_task : entry_tasks) {
        const std::vector<std::string> successors =
            tasks->at(std::string(entry_task)).get("next", std::vector<std::string> {});
        const auto trophy_reward =
            std::ranges::find(successors, "BlackFlow@Roguelike@GetDropTrophyReward");
        const auto generic_drop = std::ranges::find(successors, "BlackFlow@Roguelike@GetDrop");

        REQUIRE(trophy_reward != successors.end());
        REQUIRE(generic_drop != successors.end());
        REQUIRE(trophy_reward < generic_drop);
    }
}

TEST_CASE("BlackFlow encounter rewards hand an opened recruitment page to the recruit flow")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::vector<std::string> successors = tasks->at("BlackFlow@Roguelike@StageEncounterReward")
                                                    .get("next", std::vector<std::string> {});
    const auto recruitment = std::ranges::find(successors, "BlackFlow@Roguelike@ChooseOperFlag");
    const auto map = std::ranges::find(successors, "BlackFlow@Roguelike@Stages-MapReady");
    const auto close = std::ranges::find(successors, "BlackFlow@Roguelike@CloseEventAfterEncounter");

    REQUIRE(recruitment != successors.end());
    REQUIRE(map != successors.end());
    REQUIRE(close != successors.end());
    REQUIRE(recruitment < map);
    REQUIRE(recruitment < close);
}

TEST_CASE("BlackFlow emergency aid dispatches through a JustReturn adapter to the dedicated handler")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::vector<std::string> successors = tasks->at("BlackFlow@Roguelike@StageEncounterReward")
                                                    .get("next", std::vector<std::string> {});
    const auto employ = std::ranges::find(successors, "BlackFlow@Roguelike@EmployLeave");
    const auto recruitment = std::ranges::find(successors, "BlackFlow@Roguelike@ChooseOperFlag");
    const auto map = std::ranges::find(successors, "BlackFlow@Roguelike@Stages-MapReady");

    REQUIRE(employ != successors.end());
    REQUIRE(recruitment != successors.end());
    REQUIRE(map != successors.end());
    REQUIRE(employ < recruitment);
    REQUIRE(employ < map);

    REQUIRE_FALSE(tasks->contains("BlackFlow@Roguelike@Page-Employ"));
    const auto& employ_handler = tasks->at("BlackFlow@Roguelike@EmployLeave");
    REQUIRE(employ_handler.get("template", std::string {}) == "BlackFlow@Roguelike@EmployLeave.png");
    REQUIRE(std::filesystem::exists(
        repository_root / "resource/template/Roguelike/BlackFlow/BlackFlow@Roguelike@EmployLeave.png"));

    REQUIRE(tasks->contains("BlackFlow@Roguelike@EmployLeave-Enter"));
    const auto& employ_entry = tasks->at("BlackFlow@Roguelike@EmployLeave-Enter");
    REQUIRE(employ_entry.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(
        employ_entry.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> { "BlackFlow@Roguelike@EmployLeave" });
    REQUIRE_FALSE(employ_entry.contains("template"));

    const auto execution = json::open(repository_root / "resource/roguelike/BlackFlow/node_execution.json");
    REQUIRE(execution.has_value());
    const auto routes = execution->get("routes", std::vector<json::value> {});
    const auto route = std::ranges::find_if(
        routes,
        [](const json::value& value) { return value.get("id", std::string {}) == "default_employ"; });
    REQUIRE(route != routes.end());
    REQUIRE(route->get("task", std::string {}) == "BlackFlow@Roguelike@EmployLeave-Enter");

    const auto& employ_completed = tasks->at("BlackFlow@Roguelike@EmployLeaveConfirmCompleted");
    REQUIRE(
        employ_completed.get("next", std::vector<std::string> {}) ==
        std::vector<std::string> {
            "BlackFlow@Roguelike@HuntedConfirm",
            "BlackFlow@Roguelike@NodeCompletionAction",
        });
}

TEST_CASE("BlackFlow Lone Survivor follow-up retains the Emergency Aid transfer semantics")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto encounter =
        json::open(repository_root / "resource/roguelike/BlackFlow/encounter/default.json");
    REQUIRE(encounter.has_value());

    const auto stages = encounter->at("stage").as_array();
    const auto lone_survivor = std::ranges::find_if(
        stages,
        [](const json::value& event) { return event.get("name", std::string {}) == LoneSurvivorEventName; });
    const auto followup = std::ranges::find_if(
        stages,
        [](const json::value& event) { return event.get("name", std::string {}) == LoneSurvivorFollowupEventName; });

    REQUIRE(lone_survivor != stages.end());
    REQUIRE(followup != stages.end());
    REQUIRE(lone_survivor->get("choose", 0) == 1);
    REQUIRE(lone_survivor->get("next_event", std::string {}) == LoneSurvivorFollowupEventName);
    REQUIRE(followup->get("choose", 0) == 2);
    REQUIRE(event_reveal_node_type(LoneSurvivorFollowupEventName) == NodeType::Employ);
}

TEST_CASE("BlackFlow encounter options can transition through a battle preview")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::vector<std::string> successors = tasks->at("BlackFlow@Roguelike@StageEncounterReward")
                                                    .get("next", std::vector<std::string> {});
    const auto battle = std::ranges::find(successors, "BlackFlow@Roguelike@StageEncounterBattleDepart");
    const auto map = std::ranges::find(successors, "BlackFlow@Roguelike@Stages-MapReady");
    const auto close = std::ranges::find(successors, "BlackFlow@Roguelike@CloseEventAfterEncounter");

    REQUIRE(battle != successors.end());
    REQUIRE(map != successors.end());
    REQUIRE(close != successors.end());
    REQUIRE(battle < map);
    REQUIRE(battle < close);

    const auto& depart = tasks->at("BlackFlow@Roguelike@StageEncounterBattleDepart");
    REQUIRE(depart.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(depart.get("roi", std::vector<int> {}) == std::vector<int> { 1122, 520, 153, 65 });
    REQUIRE(depart.get("action", std::string {}) == "ClickRect");
    REQUIRE(depart.get("specificRect", std::vector<int> {}) == std::vector<int> { 1135, 525, 70, 70 });
    REQUIRE(depart.get("maxTimes", 0) == 3);
    REQUIRE(depart.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@DirectDepartInventoryOverloadPrompt",
                "BlackFlow@Roguelike@StageEncounterBattleDepartObserve",
                "BlackFlow@Roguelike@StageEncounterBattleDepartDestination",
                "BlackFlow@Roguelike@StageEncounterBattleDepartTransitionWait",
            });

    const auto& observer = tasks->at("BlackFlow@Roguelike@StageEncounterBattleDepartObserve");
    REQUIRE(observer.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(observer.get("action", std::string {}) == "DoNothing");

    const auto& destination = tasks->at("BlackFlow@Roguelike@StageEncounterBattleDepartDestination");
    REQUIRE(destination.get("template", std::string {}) == "BattleQuickFormation.png");
    REQUIRE(destination.get("action", std::string {}) == "DoNothing");
    REQUIRE(destination.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@StageEncounterBattleDepartCompleted" });

    const auto& completed = tasks->at("BlackFlow@Roguelike@StageEncounterBattleDepartCompleted");
    REQUIRE(completed.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@QuickFormation" });
    const auto completed_resets = completed.get("reduceOtherTimes", std::vector<std::string> {});
    REQUIRE(std::ranges::find(completed_resets, "BlackFlow@Roguelike@StageEncounterBattleDepart*3") !=
            completed_resets.end());
}

TEST_CASE("BlackFlow re-enters battle from the preview button instead of the concept-detail area")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    REQUIRE(tasks->contains("BlackFlow@Roguelike@StageEnterBattleAgain"));

    const auto& reenter = tasks->at("BlackFlow@Roguelike@StageEnterBattleAgain");
    REQUIRE(reenter.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(reenter.get("roi", std::vector<int> {}) == std::vector<int> { 1122, 520, 153, 65 });
    REQUIRE(reenter.get("action", std::string {}) == "ClickRect");
    REQUIRE(reenter.get("specificRect", std::vector<int> {}) == std::vector<int> { 1135, 525, 70, 70 });
    REQUIRE(reenter.get("maxTimes", 0) == 3);
    REQUIRE(reenter.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@DirectDepartInventoryOverloadPrompt",
                "BlackFlow@Roguelike@StageEnterBattleAgainObserve",
                "BlackFlow@Roguelike@StageEnterBattleAgainDestination",
                "BlackFlow@Roguelike@StageEnterBattleAgainTransitionWait",
            });
    REQUIRE(reenter.get("exceededNext", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@RecoverMap-Enter" });

    const auto& observer = tasks->at("BlackFlow@Roguelike@StageEnterBattleAgainObserve");
    REQUIRE(observer.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(observer.get("action", std::string {}) == "DoNothing");

    const auto& destination = tasks->at("BlackFlow@Roguelike@StageEnterBattleAgainDestination");
    REQUIRE(destination.get("template", std::string {}) == "Roguelike@StartAction.png");
    REQUIRE(destination.get("action", std::string {}) == "DoNothing");
    REQUIRE(destination.get("sub", std::vector<std::string> {}) == std::vector<std::string> {});
    REQUIRE(destination.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@StageEnterBattleAgainCompleted" });

    const auto& completed = tasks->at("BlackFlow@Roguelike@StageEnterBattleAgainCompleted");
    REQUIRE(completed.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@StartAction" });
    const auto completed_resets = completed.get("reduceOtherTimes", std::vector<std::string> {});
    REQUIRE(std::ranges::find(completed_resets, "BlackFlow@Roguelike@StageEnterBattleAgain*3") !=
            completed_resets.end());
}

TEST_CASE("BlackFlow inventory knowledge expires on floor entry and node completion")
{
    bool refresh_required = false;
    refresh_required = next_movement_inventory_refresh_state(
        refresh_required,
        MovementInventoryRefreshEvent::FloorEntered);
    REQUIRE(refresh_required);

    refresh_required = next_movement_inventory_refresh_state(
        refresh_required,
        MovementInventoryRefreshEvent::ObservationApplied);
    REQUIRE_FALSE(refresh_required);

    refresh_required = next_movement_inventory_refresh_state(
        refresh_required,
        MovementInventoryRefreshEvent::NodeCompleted);
    REQUIRE(refresh_required);
}

TEST_CASE("BlackFlow event titles restore their landing node semantics")
{
    static constexpr std::array<std::string_view, 28> incident_events = {
        "桑尼的邀请",
        "色味不同源",
        "货从口出",
        "沉重的契约",
        "敲动杠杆",
        "血衣之下",
        "擒与缚",
        "沉寂之屋",
        "黑诞",
        "呼吸的红苔",
        "被歌颂的影子",
        "愈创之心",
        "思乡心切",
        "划算买卖",
        "鸭托邦",
        "传奇团伙",
        "湖中仙女",
        "洞中宝",
        "临时中介所",
        "和平守卫者",
        "和平守卫者-2",
        "独活",
        "独活-2",
        "线人",
        "泪之聚落",
        "好奇心之死",
        "窥视箱中",
        "调谐仪式",
    };
    for (const std::string_view event : incident_events) {
        const PageContentEffect effect = classify_page_content_effect("RoguelikeEvent", event);
        INFO(event);
        REQUIRE(effect.resolved_type == NodeType::Incident);
        REQUIRE_FALSE(effect.changes_floor);
        REQUIRE(effect.has_landing);
    }

    static constexpr std::array<std::pair<std::string_view, NodeType>, 10> typed_events = {
        std::pair { "未涉足之树", NodeType::Expedition },
        std::pair { "回滚文明", NodeType::Sacrifice },
        std::pair { "无人商店", NodeType::Wish },
        std::pair { "无人商店-2", NodeType::Wish },
        std::pair { "溯源", NodeType::Portal },
        std::pair { "原始娱乐", NodeType::Duel },
        std::pair { "掠夺成性", NodeType::Duel },
        std::pair { "金色凝滞", NodeType::Rest },
        std::pair { "三重身", NodeType::Evacuate },
        std::pair { "险路尽头", NodeType::Final },
    };
    for (const auto& [event, expected_type] : typed_events) {
        const PageContentEffect effect = classify_page_content_effect("RoguelikeEvent", event);
        INFO(event);
        REQUIRE(effect.resolved_type == expected_type);
        REQUIRE(effect.has_landing);
    }

    const PageContentEffect narrow_path = classify_page_content_effect("RoguelikeEvent", "三重身");
    REQUIRE(narrow_path.changes_floor);

    const PageContentEffect hidden_rest = classify_page_content_effect("RoguelikeEvent", "安眠一隅");
    REQUIRE_FALSE(hidden_rest.resolved_type.has_value());
    REQUIRE(hidden_rest.changes_floor);
    REQUIRE_FALSE(hidden_rest.has_landing);

    const PageContentEffect unrelated_source = classify_page_content_effect("StageInfo", "三重身");
    REQUIRE_FALSE(unrelated_source.resolved_type.has_value());
    REQUIRE_FALSE(unrelated_source.changes_floor);
    REQUIRE(unrelated_source.has_landing);
}

TEST_CASE("BlackFlow event title classification backfills hidden page identity")
{
    const EnteredPageObservation entered = classify_entered_event_name("无人商店");
    REQUIRE(entered.event_name == "无人商店");
    REQUIRE(entered.classified_type == NodeType::Wish);

    const PageIdentityResolution identity =
        resolve_page_identity(NodeType::HideInvisible, "未知的诡秘", nullptr, entered);
    REQUIRE(identity.type == NodeType::Wish);
    REQUIRE(identity.name == "无人商店");

    // 未收录的新事件仍交给通用事件流程，不能因为语义表暂时缺项而阻断对局。
    const EnteredPageObservation unknown = classify_entered_event_name("尚未收录的事件");
    REQUIRE(unknown.event_name == "尚未收录的事件");
    REQUIRE(unknown.classified_type == NodeType::Incident);
}

TEST_CASE("BlackFlow entered-page classifier retains the local ordinary-event fast path in the narrow ROI")
{
    const EnteredPageObservation typed_event = classify_entered_page_texts({ "掠夺成性" });
    REQUIRE(typed_event.event_name == "掠夺成性");
    REQUIRE(typed_event.classified_type == NodeType::Duel);

    const EnteredPageObservation ordinary_event = classify_entered_page_texts({ "行动奖励" });
    REQUIRE(ordinary_event.event_name == "行动奖励");
    REQUIRE(ordinary_event.classified_type == NodeType::Incident);

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto event_whitelist = tasks->at("BlackFlow@Roguelike@StageEncounterOcr")
                                     .get("text", std::vector<std::string> {});
    const auto entered_page_whitelist = tasks->at("BlackFlow@Roguelike@EnteredPageClassification")
                                            .get("text", std::vector<std::string> {});
    for (const std::string& event_name : event_whitelist) {
        INFO(event_name);
        REQUIRE(std::ranges::find(entered_page_whitelist, event_name) != entered_page_whitelist.end());
    }
}

TEST_CASE("BlackFlow entered-page fixed identities use the upstream speaker-name ROI")
{
    REQUIRE(classify_entered_page_texts({ "坎诺特" }).classified_type == NodeType::Shop);
    REQUIRE(classify_entered_page_texts({ "机械师" }).classified_type == NodeType::ScrapShop);

    // The real failure frame also contained operator 黑键, previously fuzzily normalized to 黑诞.
    // The upstream narrow ROI sees 佩德洛 instead, and that fixed identity must win.
    const EnteredPageObservation employ = classify_entered_page_texts({ "佩德洛", "黑诞" });
    REQUIRE(employ.classified_type == NodeType::Employ);
    REQUIRE_FALSE(employ.event_name.has_value());

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    const auto& classify = tasks->at("BlackFlow@Roguelike@EnteredPageClassification");
    REQUIRE(classify.get("roi", std::vector<int> {}) == std::vector<int> { 0, 420, 500, 115 });
    const auto classify_text = classify.get("text", std::vector<std::string> {});
    for (const std::string_view fixed_identity : { "险路尽头", "坎诺特", "机械师", "佩德洛" }) {
        CAPTURE(fixed_identity);
        REQUIRE(std::ranges::find(classify_text, fixed_identity) != classify_text.end());
    }
    REQUIRE(classify.get("ocrReplace", std::vector<std::vector<std::string>> {}) ==
            std::vector<std::vector<std::string>> { { "^古怪商人\\s*坎诺特$", "坎诺特" } });
}

TEST_CASE("BlackFlow event and shop OCR use guarded fuzzy matching")
{
    const std::vector<std::string> events { "临时中介所", "无人商店", "划算买卖" };
    const FuzzyTextMatch event = fuzzy_match_ocr_text("临时介所", events);
    REQUIRE(event.accepted);
    REQUIRE(event.canonical == "临时中介所");
    REQUIRE(event.edit_distance == 1);

    const std::vector<std::string> goods { "一次性喷气背包", "结构性原理", "老妈妈的融雪" };
    for (const std::string_view captured : { "次性喷气背包", "_次性喷气背包" }) {
        const FuzzyTextMatch good = fuzzy_match_ocr_text(captured, goods);
        REQUIRE(good.accepted);
        REQUIRE(good.canonical == "一次性喷气背包");
        REQUIRE(good.edit_distance == 1);
    }

    const std::vector<std::string> encounter_choices {
        "抚摸静止的小动物",
        "查看无人机",
        "整理行囊",
        "向泉水许愿",
        "坐下休息",
    };
    const FuzzyTextMatch choice = fuzzy_match_ocr_text("S查看无人机", encounter_choices);
    REQUIRE(choice.accepted);
    REQUIRE(choice.canonical == "查看无人机");
    REQUIRE(choice.edit_distance == 1);
}

TEST_CASE("BlackFlow fuzzy OCR rejects short and ambiguous guesses")
{
    const std::vector<std::string> short_goods { "种子", "浪花" };
    const FuzzyTextMatch unique_short = fuzzy_match_ocr_text("种了", short_goods);
    REQUIRE(unique_short.accepted);
    REQUIRE(unique_short.canonical == "种子");

    const std::vector<std::string> ambiguous { "临时中介所", "临时中转所" };
    const FuzzyTextMatch result = fuzzy_match_ocr_text("临时中所", ambiguous);
    REQUIRE_FALSE(result.accepted);

    const std::vector<std::string> ambiguous_short { "种子", "种了" };
    REQUIRE_FALSE(fuzzy_match_ocr_text("种天", ambiguous_short).accepted);
}

TEST_CASE("BlackFlow merchants use purchase whitelists without separate blacklist overrides")
{
    for (const std::string_view excluded :
         { "沙盘α", "高级医疗招募券", "精锐医疗招募券", "高级重装招募券", "狙击招募券" }) {
        CAPTURE(excluded);
        REQUIRE(std::ranges::find(ShopBuyPriority, excluded) == ShopBuyPriority.end());
    }
    REQUIRE(std::ranges::find(ScrapShopBuyPriority, "结构性原理") == ScrapShopBuyPriority.end());
    REQUIRE(std::ranges::find(ScrapShopBuyPriority, "简易遥控器") == ScrapShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "结构性原理") == ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "简易遥控器") == ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "沙盘β") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "医者-新典训") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "残弩-新典训") == ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ScrapShopBuyPriority, "种子") != ScrapShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "医疗招募券") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "重装招募券") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "精锐重装招募券") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "堡垒协议招募券") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "特种招募券") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "狙击招募券") == ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ShopBuyPriority, "白模鱼") != ShopBuyPriority.end());
    REQUIRE(std::ranges::find(ScrapShopBuyPriority, "白模鱼") != ScrapShopBuyPriority.end());
    REQUIRE(board_vine_purchase_allowed(AutomationStoreKind::Eerie, "白模鱼", false));
    REQUIRE(board_vine_purchase_allowed(AutomationStoreKind::Secret, "白模鱼", false));

    const AutomationCollectionTeamProgress empty_team;
    REQUIRE(automation_collection_shop_progress_purchase_allowed("医疗招募券", empty_team));
    REQUIRE(automation_collection_shop_progress_purchase_allowed("医者-新典训", empty_team));
    REQUIRE(automation_collection_shop_progress_purchase_allowed("重装招募券", empty_team));
    REQUIRE(automation_collection_shop_progress_purchase_allowed("特种招募券", empty_team));

    AutomationCollectionTeamProgress core_ready;
    core_ready.core_operator_elite_two = true;
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("医疗招募券", core_ready));
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("医者-新典训", core_ready));

    AutomationCollectionTeamProgress defender_ready;
    defender_ready.defender_operator_recruited = true;
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("重装招募券", defender_ready));
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("堡垒协议招募券", defender_ready));

    AutomationCollectionTeamProgress specialist_ready;
    specialist_ready.specialist_operator_recruited = true;
    REQUIRE_FALSE(automation_collection_shop_progress_purchase_allowed("特种招募券", specialist_ready));

    for (const std::string_view conditional : ScrapShopBoardVineConditionalPurchases) {
        CAPTURE(conditional);
        REQUIRE(std::ranges::find(ShopBuyPriority, conditional) != ShopBuyPriority.end());
        REQUIRE(std::ranges::find(ScrapShopBuyPriority, conditional) != ScrapShopBuyPriority.end());
        REQUIRE(board_vine_purchase_allowed(AutomationStoreKind::Eerie, conditional, false));
        REQUIRE_FALSE(board_vine_purchase_allowed(AutomationStoreKind::Secret, conditional, false));
        REQUIRE(board_vine_purchase_allowed(AutomationStoreKind::Secret, conditional, true));
    }
    for (const std::string_view unconditional : { "小八界", "多生苔藓" }) {
        CAPTURE(unconditional);
        REQUIRE(board_vine_purchase_allowed(AutomationStoreKind::Eerie, unconditional, false));
        REQUIRE(board_vine_purchase_allowed(AutomationStoreKind::Secret, unconditional, false));
    }
}

TEST_CASE("BlackFlow shop OCR whitelist exposes every configured purchase candidate")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    const auto names = tasks->at("BlackFlow@Roguelike@AutomationShopGoods")
                           .get("text", std::vector<std::string> {});

    for (const std::string_view expected : ShopBuyPriority) {
        CAPTURE(expected);
        REQUIRE(std::ranges::find(names, expected) != names.end());
    }
    for (const std::string_view removed :
         { "沙盘α", "结构性原理", "“简易遥控器”", "狙击招募券", "高级医疗招募券", "精锐医疗招募券", "高级重装招募券" }) {
        CAPTURE(removed);
        REQUIRE(std::ranges::find(names, removed) == names.end());
    }
}

TEST_CASE("BlackFlow scrap shop starts one cultivation after entry or buying seeds")
{
    // 一次开始培育后的 AtMost 回调会点击“最多”3 次，因此一次流程已经会选择当前全部种子。
    REQUIRE(scrap_shop_should_start_cultivation(true, 1, true));
    REQUIRE(scrap_shop_should_start_cultivation(true, 3, true));
    REQUIRE_FALSE(scrap_shop_should_start_cultivation(false, 3, true));
    REQUIRE_FALSE(scrap_shop_should_start_cultivation(true, 0, true));
    REQUIRE_FALSE(scrap_shop_should_start_cultivation(true, 1, false));
    REQUIRE(scrap_shop_purchase_requests_cultivation("种子"));
    REQUIRE_FALSE(scrap_shop_purchase_requests_cultivation("一次性喷气背包"));
}

TEST_CASE("BlackFlow automation shop retries one missed product click without looping")
{
    std::vector<std::string> attempts;
    REQUIRE(purchase_name_attempt_allowed("种子", attempts));

    attempts.emplace_back("种子");
    REQUIRE(purchase_name_attempt_allowed("种子", attempts));
    REQUIRE(purchase_name_attempt_allowed("一次性喷气背包", attempts));

    attempts.emplace_back("种子");
    REQUIRE_FALSE(purchase_name_attempt_allowed("种子", attempts));
}

TEST_CASE("BlackFlow store skips an unaffordable preferred good for an affordable cheaper good")
{
    const std::array<std::string_view, 2> priority { "一次性喷气背包", "标准引擎" };
    const std::vector<StoreGoodOffer> goods {
        { "一次性喷气背包", 8, true, false },
        { "标准引擎", 2, true, true },
    };

    const auto selected = select_preferred_affordable_good(priority, goods, 5);
    REQUIRE(selected.has_value());
    REQUIRE(goods[*selected].name == "标准引擎");
}

TEST_CASE("BlackFlow store trusts a buyable shelf state over a corrupted price OCR")
{
    const std::array<std::string_view, 1> priority { "老妈妈的融雪" };
    const std::vector<StoreGoodOffer> goods {
        { "老妈妈的融雪", 1148, true, true },
    };

    const auto selected = select_preferred_affordable_good(priority, goods, 31);
    REQUIRE(selected.has_value());
    REQUIRE(goods[*selected].name == "老妈妈的融雪");
}

TEST_CASE("BlackFlow store treats an absent shopping marker as unknown instead of unavailable")
{
    const std::array<std::string_view, 1> priority { "沙盘β" };
    const std::vector<StoreGoodOffer> goods {
        { "沙盘β", 1, true, positive_shelf_buyability_evidence(false) },
    };

    REQUIRE_FALSE(positive_shelf_buyability_evidence(false).has_value());
    REQUIRE(positive_shelf_buyability_evidence(true) == std::optional<bool> { true });
    const auto selected = select_preferred_affordable_good(priority, goods, 22);
    REQUIRE(selected.has_value());
    REQUIRE(goods[*selected].name == "沙盘β");
}

TEST_CASE("BlackFlow store uses the shelf price background as bidirectional buyability evidence")
{
    REQUIRE(merchant_buyability_color_roi(asst::Rect { 629, 182, 60, 22 }) ==
            asst::Rect { 609, 312, 200, 80 });
    REQUIRE(merchant_buyability_color_evidence(620, 33, 800) == std::optional<bool> { false });
    REQUIRE(merchant_buyability_color_evidence(0, 680, 800) == std::optional<bool> { true });
    REQUIRE_FALSE(merchant_buyability_color_evidence(40, 45, 800).has_value());

    const std::array<std::string_view, 1> priority { "回声玉米" };
    const std::vector<StoreGoodOffer> goods {
        { "回声玉米", std::nullopt, true, merchant_buyability_color_evidence(620, 33, 800) },
    };
    REQUIRE_FALSE(select_preferred_affordable_good(priority, goods, std::nullopt).has_value());
}

TEST_CASE("BlackFlow store prefers a known affordable good over an unknown-price preferred good")
{
    const std::array<std::string_view, 2> priority { "一次性喷气背包", "标准引擎" };
    const std::vector<StoreGoodOffer> goods {
        { "一次性喷气背包", std::nullopt, true, std::nullopt },
        { "标准引擎", 2, true, std::nullopt },
    };

    const auto selected = select_preferred_affordable_good(priority, goods, 5);
    REQUIRE(selected.has_value());
    REQUIRE(goods[*selected].name == "标准引擎");
}

TEST_CASE("BlackFlow store with an unknown wallet still tries goods in priority order")
{
    const std::array<std::string_view, 2> priority { "一次性喷气背包", "标准引擎" };
    const std::vector<StoreGoodOffer> goods {
        { "一次性喷气背包", 8, true, std::nullopt },
        { "标准引擎", 2, true, std::nullopt },
    };

    const auto selected = select_preferred_affordable_good(priority, goods, std::nullopt);
    REQUIRE(selected.has_value());
    REQUIRE(goods[*selected].name == "一次性喷气背包");
}

TEST_CASE("BlackFlow floor three pursuit departs before waiting for quick formation")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto battle_entry = tasks->at("BlackFlow@Roguelike@HuntedBattle-Enter")
                                  .get("next", std::vector<std::string> {});
    REQUIRE(battle_entry == std::vector<std::string> { "BlackFlow@Roguelike@HuntedDepart" });

    const auto departure = tasks->at("BlackFlow@Roguelike@HuntedDepart");
    REQUIRE(departure.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(departure.get("action", std::string {}) == "ClickRect");
    REQUIRE(departure.get("specificRect", std::vector<int> {}) == std::vector<int> { 1135, 525, 70, 70 });
    const auto departure_successors = departure.get("next", std::vector<std::string> {});
    const auto overload =
        std::ranges::find(departure_successors, "BlackFlow@Roguelike@DirectDepartInventoryOverloadPrompt");
    const auto observer = std::ranges::find(departure_successors, "BlackFlow@Roguelike@HuntedDepartObserve");
    REQUIRE(overload != departure_successors.end());
    REQUIRE(observer != departure_successors.end());
    REQUIRE(overload < observer);

    const auto& departure_observer = tasks->at("BlackFlow@Roguelike@HuntedDepartObserve");
    REQUIRE(departure_observer.get("template", std::string {}) == "BlackFlow@Roguelike@MovePreviewEnter.png");
    REQUIRE(departure_observer.get("action", std::string {}) == "DoNothing");

    const auto& destination = tasks->at("BlackFlow@Roguelike@HuntedDepartDestination");
    REQUIRE(destination.get("template", std::string {}) == "BattleQuickFormation.png");
    REQUIRE(destination.get("action", std::string {}) == "DoNothing");
    REQUIRE(destination.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@QuickFormation" });

    const auto confirmation_resets = tasks->at("BlackFlow@Roguelike@HuntedConfirmCompleted")
                                         .get("reduceOtherTimes", std::vector<std::string> {});
    REQUIRE(std::ranges::find(confirmation_resets, "BlackFlow@Roguelike@HuntedDepart*3") !=
            confirmation_resets.end());
}

TEST_CASE("BlackFlow pursuit confirmation drains and captures reward popups before battle preview")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    for (const std::string state : {
             "BlackFlow@Roguelike@HuntedConfirm",
             "BlackFlow@Roguelike@HuntedConfirmObserve",
             "BlackFlow@Roguelike@HuntedConfirmAbsentOnce",
             "BlackFlow@Roguelike@HuntedConfirmTransitionWait",
         }) {
        CAPTURE(state);
        const auto successors = tasks->at(state).get("next", std::vector<std::string> {});
        const std::string continue_popup =
            state + "@(BlackFlow@Roguelike@CloseCollectionContinue)";
        const std::string close_popup = state + "@(BlackFlow@Roguelike@CloseCollection)";
        REQUIRE(std::ranges::find(successors, continue_popup) != successors.end());
        REQUIRE(std::ranges::find(successors, close_popup) != successors.end());
    }

    REQUIRE(collection_popup_pursuit_transition_task(
        "BlackFlow@Roguelike@HuntedConfirmTransitionWait@"
        "(BlackFlow@Roguelike@CloseCollectionContinue)"));
    REQUIRE_FALSE(collection_popup_pursuit_transition_task(
        "BlackFlow@Roguelike@MovePreviewConfirmCompleted@"
        "(BlackFlow@Roguelike@CloseCollectionContinue)"));
}

TEST_CASE("BlackFlow direct battle departures route inventory overload through cleanup")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    constexpr std::string_view Prompt = "BlackFlow@Roguelike@DirectDepartInventoryOverloadPrompt";
    constexpr std::string_view Action = "BlackFlow@Roguelike@DirectDepartInventoryOverloadAction";
    for (const std::string_view departure : {
             "BlackFlow@Roguelike@HuntedDepart",
             "BlackFlow@Roguelike@StageEncounterBattleDepart",
             "BlackFlow@Roguelike@StageEnterBattleAgain",
         }) {
        const auto successors = tasks->at(std::string(departure)).get("next", std::vector<std::string> {});
        REQUIRE(std::ranges::find(successors, Prompt) != successors.end());
    }

    const auto& prompt = tasks->at(std::string(Prompt));
    REQUIRE(prompt.get("baseTask", std::string {}) == "BlackFlow@Roguelike@MovementInventoryOverloadPrompt");
    REQUIRE(prompt.get("next", std::vector<std::string> {}) == std::vector<std::string> { std::string(Action) });
    REQUIRE(tasks->at(std::string(Action)).get("baseTask", std::string {}) == "BlackFlow@Roguelike@RecoveryFailed");

    REQUIRE(tasks->at("BlackFlow@Roguelike@DirectDepartHuntedResume")
                .get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@HuntedDepart",
                "BlackFlow@Roguelike@HuntedConfirm",
            });
    REQUIRE(tasks->at("BlackFlow@Roguelike@DirectDepartEncounterResume")
                .get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@StageEncounterBattleDepart",
                "BlackFlow@Roguelike@RecoverMap-Enter",
            });
    REQUIRE(tasks->at("BlackFlow@Roguelike@DirectDepartBattleReenterResume")
                .get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@Roguelike@StageEnterBattleAgain",
                "BlackFlow@Roguelike@RecoverMap-Enter",
            });
}

TEST_CASE("BlackFlow encounter rewards can transition directly into pursuit")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto successors = tasks->at("BlackFlow@Roguelike@StageEncounterReward")
                                .get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(successors, "BlackFlow@Roguelike@HuntedConfirm") != successors.end());
}

TEST_CASE("BlackFlow direct exhaustion opens the movement panel and confirms pursuit")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto entry = tasks->at("BlackFlow@Roguelike@DirectExhaust-Enter")
                           .get("next", std::vector<std::string> {});
    REQUIRE(entry == std::vector<std::string> { "BlackFlow@Roguelike@DirectExhaustOpenPanel" });

    const auto button = tasks->at("BlackFlow@Roguelike@DirectExhaustButton");
    REQUIRE(button.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(button.get("text", std::vector<std::string> {}) == std::vector<std::string> { "直接耗尽" });

    const auto confirmation = tasks->at("BlackFlow@Roguelike@DirectExhaustConfirm");
    REQUIRE(confirmation.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(confirmation.get("text", std::vector<std::string> {}) == std::vector<std::string> { "确认" });
    const auto successors = confirmation.get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(successors, "BlackFlow@Roguelike@HuntedConfirm") != successors.end());
}

TEST_CASE("BlackFlow replaces a route used only to exhaust action points with direct exhaustion")
{
    PolicyCandidateSummary pure_exhaustion;
    pure_exhaustion.revealed_node_count = 0;
    pure_exhaustion.effective_node_count = 0;
    pure_exhaustion.planned_route_steps = {
        PlannedRouteStep { .action_points_before = 2, .action_point_cost = 1, .action_points_after = 1 },
        PlannedRouteStep { .action_points_before = 1, .action_point_cost = 1, .action_points_after = 0 },
    };

    REQUIRE(route_is_pure_action_point_exhaustion(pure_exhaustion, true));
    REQUIRE_FALSE(route_is_pure_action_point_exhaustion(pure_exhaustion, false));

    pure_exhaustion.effective_node_count = 1;
    REQUIRE_FALSE(route_is_pure_action_point_exhaustion(pure_exhaustion, true));
    pure_exhaustion.effective_node_count = 0;
    pure_exhaustion.planned_route_steps.back().move.terminal_on_completion = true;
    REQUIRE_FALSE(route_is_pure_action_point_exhaustion(pure_exhaustion, true));

    pure_exhaustion.planned_route_steps.back().move.direct_exhaustion = true;
    REQUIRE(route_is_pure_action_point_exhaustion(pure_exhaustion, true));
}

TEST_CASE("BlackFlow pursuit planning collects the floor three boss as its last useful landing")
{
    MapSnapshot map;
    const auto add_node = [&](GridPosition position, NodeType type) {
        Node node;
        node.floor = 3;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : to_string(type);
        node.traversal = default_traversal_for(type);
        node.identity_state = NodeIdentityState::Classified;
        node.identity_revealed = true;
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId source = add_node({ 2, 3 }, NodeType::Empty);
    const NodeId detour = add_node({ 1, 3 }, NodeType::Empty);
    const NodeId light = add_node({ 2, 4 }, NodeType::Light);
    const NodeId useful = add_node({ 3, 5 }, NodeType::Wish);
    const NodeId endpoint = add_node({ 2, 5 }, NodeType::BattleBoss);
    REQUIRE(map.upsert_edge({ source, detour, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ source, light, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ useful, endpoint, EdgeKnowledge::Confirmed, {} }));

    RunState run;
    run.floor = 3;
    run.current_node = source;
    run.resources.action_points = 3;
    run.resources.movement_charges.emplace(MovementKind::M03, 3);
    run.visited_nodes.emplace(source);
    for (const auto& [id, _] : map.nodes()) {
        run.revealed_nodes.emplace(id);
    }

    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };
    FactStore facts;
    MissionState mission;
    BlackFlowPlanRequest request;
    request.map = &map;
    request.run = &run;
    request.policy = &policy;
    request.facts = &facts;
    request.mission = &mission;
    request.no_AP_is_terminal = true;
    request.route_search.time_budget_ms = 5'000;
    request.route_search.total_expansions = 20'000;
    request.route_search.expansions_per_root = 2'000;

    const BlackFlowPlan plan = BlackFlowPlanner {}.plan(request);
    INFO(plan.error);
    REQUIRE(plan.decision.selected.has_value());
    REQUIRE(plan.decision.selected->movement == MovementKind::Walk);
    REQUIRE(plan.decision.selected->target == light);
    REQUIRE(plan.decision.planned_route_steps.size() == 3);
    REQUIRE(plan.decision.planned_route_steps[0].move.target == light);
    REQUIRE(plan.decision.planned_route_steps[1].move.target == useful);
    REQUIRE(plan.decision.planned_route_steps[2].move.target == endpoint);
    REQUIRE_FALSE(plan.decision.planned_route_steps[2].move.direct_exhaustion);
    REQUIRE(plan.decision.planned_route_steps[2].action_points_after == 1);
    REQUIRE(plan.decision.planned_route_steps[2].move.source == useful);
    REQUIRE_FALSE(std::ranges::any_of(plan.decision.planned_route_steps, [&](const PlannedRouteStep& step) {
        return step.move.target == detour;
    }));
    const auto selected_summary = std::ranges::find(
        plan.decision.candidate_summaries,
        plan.decision.selected->action_id,
        [](const PolicyCandidateSummary& candidate) { return candidate.move.action_id; });
    REQUIRE(selected_summary != plan.decision.candidate_summaries.end());
    REQUIRE(selected_summary->effective_node_count == 1);

    BlackFlowPlanRequest hinted_request = request;
    for (const PlannedRouteStep& step : plan.decision.planned_route_steps) {
        hinted_request.route_hint_action_ids.emplace_back(step.move.action_id);
    }
    hinted_request.route_search.total_expansions = 2;
    hinted_request.route_search.expansions_per_root = 1;
    const BlackFlowPlan hinted = BlackFlowPlanner {}.plan(hinted_request);
    INFO(hinted.error);
    REQUIRE(hinted.decision.selected.has_value());
    REQUIRE(hinted.route_hint_root_matched);
    REQUIRE(hinted.route_hint_replayed_steps == plan.decision.planned_route_steps.size());
    REQUIRE(hinted.decision.planned_route_steps.size() == plan.decision.planned_route_steps.size());
    for (std::size_t index = 0; index < plan.decision.planned_route_steps.size(); ++index) {
        REQUIRE(
            hinted.decision.planned_route_steps[index].move.action_id ==
            plan.decision.planned_route_steps[index].move.action_id);
    }
}

TEST_CASE("BlackFlow pursuit directly exhausts when the floor three boss needs full-map or two processing moves")
{
    MapSnapshot map;
    const auto add_node = [&](GridPosition position, NodeType type) {
        Node node;
        node.floor = 3;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : to_string(type);
        node.traversal = default_traversal_for(type);
        node.identity_state = NodeIdentityState::Classified;
        node.identity_revealed = true;
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId source = add_node({ 2, 3 }, NodeType::Empty);
    add_node({ 1, 2 }, NodeType::Empty);
    const NodeId staging = add_node({ 0, 1 }, NodeType::Empty);
    const NodeId endpoint = add_node({ 0, 0 }, NodeType::BattleBoss);
    REQUIRE(map.upsert_edge({ staging, endpoint, EdgeKnowledge::Confirmed, {} }));

    RunState run;
    run.floor = 3;
    run.current_node = source;
    run.resources.action_points = 3;
    run.resources.movement_charges.emplace(MovementKind::M08, 1);
    run.resources.movement_charges.emplace(MovementKind::M11, 1);
    run.resources.movement_charges.emplace(MovementKind::M03, 2);
    run.visited_nodes.emplace(source);
    for (const auto& [id, _] : map.nodes()) {
        run.revealed_nodes.emplace(id);
    }

    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };
    FactStore facts;
    MissionState mission;
    BlackFlowPlanRequest request;
    request.map = &map;
    request.run = &run;
    request.policy = &policy;
    request.facts = &facts;
    request.mission = &mission;
    request.no_AP_is_terminal = true;
    request.route_search.time_budget_ms = 5'000;
    request.route_search.total_expansions = 20'000;
    request.route_search.expansions_per_root = 2'000;

    const BlackFlowPlan plan = BlackFlowPlanner {}.plan(request);
    INFO(plan.error);
    REQUIRE(plan.decision.selected.has_value());
    REQUIRE(plan.decision.selected->direct_exhaustion);
    REQUIRE(plan.decision.decisive_rule_id == "direct_exhaustion");
    REQUIRE(plan.decision.planned_route_steps.size() == 1);
    REQUIRE(plan.decision.planned_route_steps.front().move.source == source);
    REQUIRE(plan.decision.planned_route_steps.front().move.target == source);
}

TEST_CASE("BlackFlow pursuit reaches the floor three boss before direct exhaustion without scoring it effective")
{
    MapSnapshot map;
    const auto add_node = [&](GridPosition position, NodeType type) {
        Node node;
        node.floor = 3;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : to_string(type);
        node.traversal = default_traversal_for(type);
        node.identity_state = NodeIdentityState::Classified;
        node.identity_revealed = true;
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId source = add_node({ 2, 2 }, NodeType::Empty);
    const NodeId staging = add_node({ 1, 2 }, NodeType::Empty);
    const NodeId endpoint = add_node({ 0, 1 }, NodeType::BattleBoss);
    REQUIRE(map.upsert_edge({ source, staging, EdgeKnowledge::Confirmed, {} }));

    RunState run;
    run.floor = 3;
    run.current_node = source;
    run.resources.action_points = 3;
    run.resources.movement_charges.emplace(MovementKind::M03, 1);
    run.visited_nodes.emplace(source);
    for (const auto& [id, _] : map.nodes()) {
        run.revealed_nodes.emplace(id);
    }

    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };
    FactStore facts;
    MissionState mission;
    BlackFlowPlanRequest request;
    request.map = &map;
    request.run = &run;
    request.policy = &policy;
    request.facts = &facts;
    request.mission = &mission;
    request.no_AP_is_terminal = true;
    request.route_search.time_budget_ms = 5'000;
    request.route_search.total_expansions = 20'000;
    request.route_search.expansions_per_root = 2'000;

    const BlackFlowPlan plan = BlackFlowPlanner {}.plan(request);
    INFO(plan.error);
    REQUIRE(plan.decision.selected.has_value());
    REQUIRE_FALSE(plan.decision.selected->direct_exhaustion);
    REQUIRE(plan.decision.selected->movement == MovementKind::Walk);
    REQUIRE(plan.decision.selected->target == staging);
    REQUIRE(plan.decision.decisive_rule_id == "floor_three_boss_before_direct_exhaustion");
    REQUIRE(plan.decision.planned_route_steps.size() == 2);
    REQUIRE(plan.decision.planned_route_steps[1].move.movement == MovementKind::M03);
    REQUIRE(plan.decision.planned_route_steps[1].move.target == endpoint);
    const auto selected_summary = std::ranges::find(
        plan.decision.candidate_summaries,
        plan.decision.selected->action_id,
        [](const PolicyCandidateSummary& candidate) { return candidate.move.action_id; });
    REQUIRE(selected_summary != plan.decision.candidate_summaries.end());
    REQUIRE(selected_summary->effective_node_count == 0);
}

TEST_CASE("BlackFlow default route-search budget completes a dense fifth-floor plan")
{
    MapSnapshot map;
    constexpr int Rows = 5;
    constexpr int Columns = 9;
    std::array<std::array<NodeId, Columns>, Rows> ids {};

    for (int row = 0; row < Rows; ++row) {
        for (int column = 0; column < Columns; ++column) {
            Node node;
            node.floor = 5;
            node.position = { row, column };
            node.id = *make_stable_node_id(node.floor, node.position);
            node.type = NodeType::HideInvisible;
            node.name = "未知的诡秘";
            node.traversal = default_traversal_for(node.type);
            node.identity_state = NodeIdentityState::Hidden;
            node.identity_revealed = false;
            ids[row][column] = node.id;
            REQUIRE(map.upsert_node(std::move(node)));
        }
    }

    const NodeId source = ids[2][4];
    Node source_node = *map.find_node(source);
    source_node.type = NodeType::Empty;
    source_node.name = std::string(EmptyNodeName);
    source_node.traversal = default_traversal_for(source_node.type);
    source_node.identity_state = NodeIdentityState::Classified;
    source_node.identity_revealed = true;
    REQUIRE(map.upsert_node(std::move(source_node)));

    for (int row = 0; row < Rows; ++row) {
        for (int column = 0; column < Columns; ++column) {
            if (column + 1 < Columns) {
                REQUIRE(map.upsert_edge({ ids[row][column], ids[row][column + 1], EdgeKnowledge::Confirmed, {} }));
            }
            if (row + 1 < Rows) {
                REQUIRE(map.upsert_edge({ ids[row][column], ids[row + 1][column], EdgeKnowledge::Confirmed, {} }));
            }
        }
    }

    RunState run;
    run.floor = 5;
    run.current_node = source;
    run.resources.action_points = 7;
    run.visited_nodes.emplace(source);
    run.revealed_nodes.emplace(source);
    for (const MovementKind movement : {
             MovementKind::M03,
             MovementKind::M05,
             MovementKind::M08,
             MovementKind::M11,
             MovementKind::M12,
         }) {
        run.resources.movement_charges.emplace(movement, movement == MovementKind::M11 ? 1 : 3);
    }

    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };
    FactStore facts;
    MissionState mission;
    BlackFlowPlanRequest request;
    request.map = &map;
    request.run = &run;
    request.policy = &policy;
    request.facts = &facts;
    request.mission = &mission;
    request.no_AP_is_terminal = true;

    const BlackFlowPlan plan = BlackFlowPlanner {}.plan(request);
    INFO(plan.error);
    INFO("route expansions: " << plan.route_search_expansions);
    REQUIRE(plan.decision.selected.has_value());
    REQUIRE(plan.planning_graphs_shared);
    REQUIRE(plan.route_labels_generated > 0);
    REQUIRE(plan.route_labels_retained_peak > 0);
    REQUIRE(plan.route_trace_nodes > 0);
    REQUIRE_FALSE(plan.route_search_time_exhausted);
    REQUIRE_FALSE(plan.route_search_expansions_exhausted);
    for (const MovementSpec& movement : movement_specs()) {
        if (movement.kind == MovementKind::Walk) {
            continue;
        }
        const int available = run.resources.movement_charges.contains(movement.kind)
                                  ? run.resources.movement_charges.at(movement.kind)
                                  : 0;
        const int planned = static_cast<int>(std::ranges::count(
            plan.decision.planned_route_steps,
            movement.kind,
            [](const PlannedRouteStep& step) { return step.move.movement; }));
        INFO("movement " << to_string(movement.kind) << ": available=" << available << ", planned=" << planned);
        REQUIRE(planned <= available);
    }
}

TEST_CASE("BlackFlow keeps confirmed and relaxed graphs separate when an edge is not confirmed")
{
    MapSnapshot map;
    const auto add_node = [&](int column, NodeType type) {
        Node node;
        node.floor = 2;
        node.position = { 0, column };
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : std::string(to_string(type));
        node.traversal = default_traversal_for(type);
        node.identity_state = NodeIdentityState::Classified;
        node.identity_revealed = true;
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId source = add_node(0, NodeType::Empty);
    const NodeId endpoint = add_node(1, NodeType::Final);
    REQUIRE(map.upsert_edge({ source, endpoint, EdgeKnowledge::Unknown, {} }));

    RunState run;
    run.floor = 2;
    run.current_node = source;
    run.resources.action_points = 1;
    run.visited_nodes.emplace(source);
    run.revealed_nodes = { source, endpoint };

    ResolvedPolicy policy;
    FactStore facts;
    MissionState mission;
    BlackFlowPlanRequest request;
    request.map = &map;
    request.run = &run;
    request.policy = &policy;
    request.facts = &facts;
    request.mission = &mission;

    const BlackFlowPlan plan = BlackFlowPlanner {}.plan(request);
    INFO(plan.error);
    REQUIRE(plan.decision.selected.has_value());
    REQUIRE_FALSE(plan.planning_graphs_shared);
    REQUIRE(plan.safety.required_action_points >= UnreachableActionPointRequirement);
    REQUIRE(plan.relaxed_safety.required_action_points == 1);
    REQUIRE(plan.decision.selected->target == endpoint);
    REQUIRE(plan.decision.selected->requires_preview_verification);
}

TEST_CASE("BlackFlow route search never spends one structural-principle charge three times")
{
    MapSnapshot map;
    const auto add_node = [&](int row, int column, NodeType type, std::string marker = {}) {
        Node node;
        node.floor = 4;
        node.position = { row, column };
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : std::string(to_string(type));
        node.traversal = default_traversal_for(type);
        node.identity_state = type == NodeType::HideInvisible || type == NodeType::HideBattle
                                  ? NodeIdentityState::Hidden
                                  : NodeIdentityState::Classified;
        node.identity_revealed = node.identity_state == NodeIdentityState::Classified;
        node.marker_type = std::move(marker);
        REQUIRE(map.upsert_node(std::move(node)));
    };
    for (const auto& [row, column, type, marker] : std::vector<std::tuple<int, int, NodeType, std::string>> {
             { 0, 2, NodeType::HideInvisible, {} }, { 0, 3, NodeType::Final, {} },
             { 0, 4, NodeType::HideInvisible, {} }, { 0, 5, NodeType::Empty, {} },
             { 0, 6, NodeType::Empty, {} },         { 0, 7, NodeType::Door, {} },
             { 1, 0, NodeType::BattleElite, {} },   { 1, 1, NodeType::HideInvisible, {} },
             { 1, 3, NodeType::Evacuate, {} },      { 1, 4, NodeType::Empty, "savage" },
             { 1, 6, NodeType::HideBattle, {} },    { 1, 7, NodeType::Final, {} },
             { 2, 0, NodeType::Empty, {} },         { 2, 1, NodeType::HideInvisible, {} },
             { 2, 2, NodeType::ScrapShop, {} },     { 2, 4, NodeType::Empty, {} },
             { 2, 5, NodeType::HideInvisible, {} }, { 2, 6, NodeType::HideBattle, {} },
             { 2, 7, NodeType::HideBattle, {} },    { 3, 0, NodeType::BattleNormal, {} },
             { 3, 1, NodeType::Door, {} },          { 3, 2, NodeType::Empty, {} },
             { 3, 3, NodeType::Light, "savage" },  { 3, 4, NodeType::Empty, {} },
             { 3, 5, NodeType::Empty, {} },         { 3, 6, NodeType::Empty, {} },
             { 3, 7, NodeType::Empty, "savage" },  { 4, 0, NodeType::Empty, {} },
             { 4, 1, NodeType::Incident, {} },      { 4, 2, NodeType::Expedition, {} },
             { 4, 3, NodeType::Portal, {} },        { 4, 4, NodeType::Duel, {} },
             { 4, 5, NodeType::Empty, {} },         { 4, 6, NodeType::Final, {} },
             { 4, 7, NodeType::HideInvisible, {} },
         }) {
        add_node(row, column, type, marker);
    }
    const auto id = [](int row, int column) { return *make_stable_node_id(4, { row, column }); };
    for (const auto& [first, second] : std::vector<std::pair<GridPosition, GridPosition>> {
             { { 0, 2 }, { 0, 3 } }, { { 0, 3 }, { 1, 3 } }, { { 0, 4 }, { 0, 5 } },
             { { 0, 4 }, { 1, 4 } }, { { 0, 5 }, { 0, 6 } }, { { 0, 7 }, { 1, 7 } },
             { { 1, 0 }, { 2, 0 } }, { { 1, 1 }, { 2, 1 } }, { { 1, 3 }, { 1, 4 } },
             { { 1, 4 }, { 2, 4 } }, { { 1, 6 }, { 1, 7 } }, { { 1, 6 }, { 2, 6 } },
             { { 2, 0 }, { 3, 0 } }, { { 2, 1 }, { 3, 1 } }, { { 2, 2 }, { 3, 2 } },
             { { 2, 4 }, { 2, 5 } }, { { 2, 5 }, { 2, 6 } }, { { 2, 5 }, { 3, 5 } },
             { { 2, 6 }, { 2, 7 } }, { { 2, 7 }, { 3, 7 } }, { { 3, 0 }, { 3, 1 } },
             { { 3, 0 }, { 4, 0 } }, { { 3, 1 }, { 3, 2 } }, { { 3, 1 }, { 4, 1 } },
             { { 3, 2 }, { 3, 3 } }, { { 3, 3 }, { 3, 4 } }, { { 3, 4 }, { 3, 5 } },
             { { 3, 4 }, { 4, 4 } }, { { 3, 5 }, { 3, 6 } }, { { 3, 5 }, { 4, 5 } },
             { { 3, 6 }, { 3, 7 } }, { { 3, 6 }, { 4, 6 } }, { { 3, 7 }, { 4, 7 } },
             { { 4, 0 }, { 4, 1 } }, { { 4, 1 }, { 4, 2 } }, { { 4, 3 }, { 4, 4 } },
             { { 4, 4 }, { 4, 5 } }, { { 4, 5 }, { 4, 6 } }, { { 4, 6 }, { 4, 7 } },
         }) {
        REQUIRE(map.upsert_edge({ id(first.row, first.column), id(second.row, second.column), EdgeKnowledge::Confirmed, {} }));
    }

    RunState run;
    run.floor = 4;
    run.current_node = id(2, 0);
    run.resources.action_points = 6;
    run.visited_nodes.emplace(run.current_node);
    run.revealed_nodes.emplace(run.current_node);
    run.resources.movement_charges = {
        { MovementKind::M03, 9 }, { MovementKind::M05, 4 }, { MovementKind::M10, 2 },
        { MovementKind::M11, 1 }, { MovementKind::M12, 1 },
    };

    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };
    FactStore facts;
    MissionState mission;
    BlackFlowPlanRequest request;
    request.map = &map;
    request.run = &run;
    request.policy = &policy;
    request.facts = &facts;
    request.mission = &mission;
    request.strategy_terminal_nodes = { id(0, 3), id(1, 3), id(1, 7), id(4, 6) };
    request.forbidden_node_types = { NodeType::BattleElite, NodeType::BattleNormal, NodeType::HideBattle };
    request.root_forbidden_marker_types = { "savage" };
    request.robust_mobile_marker_lookahead = true;

    const BlackFlowPlan plan = BlackFlowPlanner {}.plan(request);
    INFO(plan.error);
    REQUIRE(plan.decision.selected.has_value());
    const int structural_uses = static_cast<int>(std::ranges::count(
        plan.decision.planned_route_steps,
        MovementKind::M11,
        [](const PlannedRouteStep& step) { return step.move.movement; }));
    INFO("planned structural-principle uses: " << structural_uses);
    REQUIRE(structural_uses <= 1);

    // 四层只剩一次全图移动时，保留约束要覆盖整条搜索路线。首步徒步不能让
    // 模拟的第二步绕过约束再用掉结构性原理。
    request.reserved_movement_kinds = {
        MovementKind::M08,
        MovementKind::M09,
        MovementKind::M11,
    };
    request.reserved_movement_charges = 1;
    const BlackFlowPlan preserved_plan = BlackFlowPlanner {}.plan(request);
    INFO(preserved_plan.error);
    REQUIRE(preserved_plan.decision.selected.has_value());
    REQUIRE(std::ranges::none_of(
        preserved_plan.decision.planned_route_steps,
        [](const PlannedRouteStep& step) { return step.move.movement == MovementKind::M11; }));
}

TEST_CASE("BlackFlow equal-cost routes prefer fewer submitted movement actions")
{
    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };

    PolicyCandidate split_walk;
    split_walk.move.action_id = "a-split-walk";
    split_walk.safe = true;
    split_walk.revealed_node_count = 2;
    split_walk.effective_node_count = 1;
    split_walk.route_length = 4;
    split_walk.planned_route_steps = { PlannedRouteStep {}, PlannedRouteStep {}, PlannedRouteStep {}, PlannedRouteStep {} };

    PolicyCandidate direct_walk = split_walk;
    direct_walk.move.action_id = "z-direct-walk";
    direct_walk.planned_route_steps = { PlannedRouteStep {}, PlannedRouteStep {}, PlannedRouteStep {} };

    const PolicyDecision decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { split_walk, direct_walk });

    REQUIRE(decision.selected.has_value());
    REQUIRE(decision.selected->action_id == "z-direct-walk");
}

TEST_CASE("BlackFlow Xiaobajie uses expected lexicographic value once every outcome is safe")
{
    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
    };

    PolicyCandidate deterministic;
    deterministic.move.action_id = "a-deterministic";
    deterministic.safe = true;
    deterministic.revealed_node_count = 1;
    deterministic.effective_node_count = 1;
    deterministic.route_outcomes = {
        PolicyRouteOutcome {
            .revealed_node_count = 1,
            .effective_node_count = 1,
        },
    };

    PolicyCandidate xiaobajie = deterministic;
    xiaobajie.move.action_id = "z-xiaobajie";
    xiaobajie.move.movement = MovementKind::M07;
    xiaobajie.revealed_node_count = 0;
    xiaobajie.effective_node_count = 0;
    xiaobajie.route_outcomes = {
        PolicyRouteOutcome {},
        PolicyRouteOutcome {
            .revealed_node_count = 2,
            .effective_node_count = 3,
        },
    };

    const PolicyDecision decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { deterministic, xiaobajie });

    REQUIRE(decision.selected.has_value());
    REQUIRE(decision.selected->action_id == "z-xiaobajie");
    REQUIRE(decision.decisive_rule_id == "effective_node_count");
}

TEST_CASE("BlackFlow floor one hidden battle must be revealed before a future landing")
{
    const std::unordered_set<NodeType> immediate_forbidden {
        NodeType::HideBattle,
        NodeType::BattleNormal,
        NodeType::BattleElite,
        NodeType::BattleSavage,
    };
    const auto future_forbidden = future_forbidden_landing_types(immediate_forbidden);

    REQUIRE(future_forbidden.contains(NodeType::HideBattle));
    REQUIRE(future_forbidden.contains(NodeType::BattleNormal));
    REQUIRE(future_forbidden.contains(NodeType::BattleElite));
    REQUIRE(future_forbidden.contains(NodeType::BattleSavage));

    MapSnapshot map;
    Node hidden_battle;
    hidden_battle.floor = 1;
    hidden_battle.position = { 0, 1 };
    hidden_battle.id = *make_stable_node_id(hidden_battle.floor, hidden_battle.position);
    hidden_battle.type = NodeType::HideBattle;
    REQUIRE(map.upsert_node(hidden_battle));

    Node hidden_incident;
    hidden_incident.floor = 1;
    hidden_incident.position = { 0, 2 };
    hidden_incident.id = *make_stable_node_id(hidden_incident.floor, hidden_incident.position);
    hidden_incident.type = NodeType::HideInvisible;
    REQUIRE(map.upsert_node(hidden_incident));

    MoveCandidate immediate_hidden_battle;
    immediate_hidden_battle.controllable = true;
    immediate_hidden_battle.target = hidden_battle.id;
    immediate_hidden_battle.landing = hidden_battle.id;
    REQUIRE(move_lands_on_forbidden_node_type(map, immediate_hidden_battle, immediate_forbidden));
    REQUIRE(route_landing_is_forbidden(NodeType::HideBattle, false, true, future_forbidden));
    REQUIRE_FALSE(route_landing_is_forbidden(NodeType::HideBattle, true, true, future_forbidden));
    REQUIRE(route_landing_is_forbidden(NodeType::HideBattle, true, false, future_forbidden));
    REQUIRE(route_landing_is_forbidden(NodeType::BattleNormal, true, true, future_forbidden));

    REQUIRE(map.upsert_edge(Edge { hidden_battle.id, hidden_incident.id, EdgeKnowledge::Confirmed, {} }));
    RunState run;
    run.floor = 1;
    MoveCandidate reveal_move;
    reveal_move.controllable = true;
    reveal_move.target = hidden_incident.id;
    reveal_move.landing = hidden_incident.id;
    const auto revealed = expected_move_reveals(map, run, reveal_move, hidden_incident.id, true);
    REQUIRE(revealed.contains(hidden_battle.id));
    REQUIRE_FALSE(route_landing_is_forbidden(
        NodeType::HideBattle,
        revealed.contains(hidden_battle.id),
        true,
        future_forbidden));
    REQUIRE(route_landing_is_forbidden(
        NodeType::HideBattle,
        revealed.contains(hidden_battle.id),
        false,
        future_forbidden));
}

TEST_CASE("BlackFlow random movement keeps a clickable activation target")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 4, 4 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::ScrapShop;
    source.identity_revealed = true;
    REQUIRE(map.upsert_node(source));

    Node hidden_invisible;
    hidden_invisible.floor = 3;
    hidden_invisible.position = { 0, 0 };
    hidden_invisible.id = *make_stable_node_id(hidden_invisible.floor, hidden_invisible.position);
    hidden_invisible.type = NodeType::HideInvisible;
    REQUIRE(map.upsert_node(hidden_invisible));

    Node other_hidden_invisible;
    other_hidden_invisible.floor = 3;
    other_hidden_invisible.position = { 0, 1 };
    other_hidden_invisible.id = *make_stable_node_id(other_hidden_invisible.floor, other_hidden_invisible.position);
    other_hidden_invisible.type = NodeType::HideInvisible;
    REQUIRE(map.upsert_node(other_hidden_invisible));

    RunState run;
    run.floor = 3;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);
    run.active_movement = MovementKind::M07;

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE_FALSE(random_move->candidate.controllable);
    REQUIRE(random_move->candidate.target != InvalidNodeId);
    REQUIRE(map.find_node(random_move->candidate.target) != nullptr);
    REQUIRE(random_move->candidate.target == hidden_invisible.id);
    REQUIRE_FALSE(move_preview_updates_target_identity(random_move->candidate));
    MoveCandidate directed_preview = random_move->candidate;
    directed_preview.controllable = true;
    REQUIRE(move_preview_updates_target_identity(directed_preview));
    REQUIRE(std::ranges::find(random_move->possible_landings, hidden_invisible.id) !=
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, other_hidden_invisible.id) !=
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, source.id) ==
            random_move->possible_landings.end());

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation {
            random_move->candidate.target,
            asst::Rect { 10, 10, 20, 20 },
            std::nullopt,
            1.0,
            0.0 } },
        map.revision,
        1);
    std::string error;
    auto transaction = MoveTransaction::propose(random_move->candidate, map, viewport, &error);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(
        MovePreview { PreviewReachability::Reachable, 0, NodeType::HideInvisible, "未知的诡秘", false },
        &error));
    REQUIRE(transaction->commit(map.revision, viewport.viewport_revision(), run.resources, &error));

    MoveObservation observation;
    observation.current_node = other_hidden_invisible.id;
    observation.floor = 3;
    observation.action_points = 2;
    observation.landed_type = NodeType::HideInvisible;
    observation.map_revision = map.revision;
    observation.viewport_revision = 2;
    REQUIRE(transaction->observe(observation, &error));
    REQUIRE(transaction->apply(run, &error));
    REQUIRE(run.current_node == other_hidden_invisible.id);
    REQUIRE(run.resources.movement_charges.at(MovementKind::M07) == 1);

    RunState linked_run;
    linked_run.floor = 3;
    linked_run.current_node = source.id;
    linked_run.resources.action_points = 2;
    linked_run.resources.movement_charges.emplace(MovementKind::M07, 1);
    linked_run.active_movement = MovementKind::M07;
    auto linked_transaction = MoveTransaction::propose(random_move->candidate, map, viewport, &error);
    REQUIRE(linked_transaction.has_value());
    REQUIRE(linked_transaction->record_preview(
        MovePreview { PreviewReachability::Reachable, 0, NodeType::HideInvisible, "未知的诡秘", false },
        &error));
    REQUIRE(linked_transaction->commit(map.revision, viewport.viewport_revision(), linked_run.resources, &error));
    REQUIRE(linked_transaction->mark_page_resolved(&error));

    MoveObservation linked_observation;
    linked_observation.current_node = *make_stable_node_id(3, GridPosition { 4, 3 });
    linked_observation.linked_encounter_origin_node = other_hidden_invisible.id;
    linked_observation.floor = 3;
    linked_observation.action_points = 2;
    linked_observation.landed_type = NodeType::Incident;
    linked_observation.map_revision = map.revision + 1;
    linked_observation.viewport_revision = 2;
    REQUIRE(linked_transaction->observe(linked_observation, &error));
    REQUIRE(linked_transaction->apply(linked_run, &error));
    REQUIRE(linked_run.current_node == linked_observation.current_node);
}

TEST_CASE("BlackFlow random movement includes its completed noncombat source in the fallback landing pool")
{
    MapSnapshot map;

    Node source;
    source.floor = 5;
    source.position = { 3, 4 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Empty;
    source.progress = NodeProgress::Completed;
    REQUIRE(map.upsert_node(source));

    Node activation;
    activation.floor = 5;
    activation.position = { 0, 2 };
    activation.id = *make_stable_node_id(activation.floor, activation.position);
    activation.type = NodeType::Empty;
    activation.progress = NodeProgress::Active;
    REQUIRE(map.upsert_node(activation));

    RunState run;
    run.floor = 5;
    run.current_node = source.id;
    run.resources.action_points = 7;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);
    run.active_movement = MovementKind::M07;

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE(random_move->candidate.target == activation.id);
    REQUIRE(std::ranges::find(random_move->possible_landings, source.id) !=
            random_move->possible_landings.end());
}

TEST_CASE("BlackFlow random movement prioritizes hidden noncombat and excludes only explicit resident markers")
{
    MapSnapshot map;
    const auto add_node = [&](GridPosition position,
                              NodeType type,
                              std::string marker = {},
                              bool overlap = false) {
        Node node;
        node.floor = 3;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.marker_type = std::move(marker);
        node.marker_resident_overlap_possible = overlap;
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };

    const NodeId source = add_node({ 4, 4 }, NodeType::ScrapShop);
    const NodeId safe = add_node({ 0, 0 }, NodeType::HideInvisible);
    const NodeId unrecognized_resident_alias = add_node({ 0, 1 }, NodeType::HideInvisible, "resident");
    const NodeId explicit_resident_marker = add_node({ 0, 2 }, NodeType::HideInvisible, "savage");
    const NodeId possible_overlap = add_node({ 0, 3 }, NodeType::HideInvisible, "fruit_cache", true);
    const NodeId duel = add_node({ 0, 4 }, NodeType::Duel);
    const std::vector<NodeId> combat_nodes = {
        add_node({ 1, 0 }, NodeType::BattleNormal),
        add_node({ 1, 1 }, NodeType::BattleElite),
        add_node({ 1, 2 }, NodeType::BattleSavage),
        add_node({ 1, 3 }, NodeType::HideBattle),
        add_node({ 1, 4 }, NodeType::BattleBoss),
    };

    RunState run;
    run.floor = 3;
    run.current_node = source;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, safe) != random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, unrecognized_resident_alias) !=
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, explicit_resident_marker) ==
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, possible_overlap) !=
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, duel) == random_move->possible_landings.end());
    for (const NodeId combat : combat_nodes) {
        REQUIRE(std::ranges::find(random_move->possible_landings, combat) == random_move->possible_landings.end());
    }

    StateExpansionOptions options;
    options.final_is_terminal = false;
    OnDemandStateGraph graph;
    std::string error;
    REQUIRE(graph.initialize(map, run, options, &error));
    const auto* compact_actions = graph.actions(graph.initial_state(), &error);
    REQUIRE(compact_actions != nullptr);
    const auto compact_random = std::ranges::find_if(*compact_actions, [](const OnDemandSafetyAction& action) {
        return action.candidate.movement == MovementKind::M07;
    });
    REQUIRE(compact_random != compact_actions->end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, safe) !=
            compact_random->candidate.possible_landings.end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, unrecognized_resident_alias) !=
            compact_random->candidate.possible_landings.end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, explicit_resident_marker) ==
            compact_random->candidate.possible_landings.end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, possible_overlap) !=
            compact_random->candidate.possible_landings.end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, duel) ==
            compact_random->candidate.possible_landings.end());
    for (const NodeId combat : combat_nodes) {
        REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, combat) ==
                compact_random->candidate.possible_landings.end());
    }
}

TEST_CASE("BlackFlow applies a committed move after the final processing-item charge disappears from inventory")
{
    MapSnapshot map;

    Node source;
    source.floor = 1;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Empty;
    REQUIRE(map.upsert_node(source));

    Node target;
    target.floor = 1;
    target.position = { 0, 1 };
    target.id = *make_stable_node_id(target.floor, target.position);
    target.type = NodeType::Incident;
    REQUIRE(map.upsert_node(target));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { target.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "consume-final-processing-item-charge";
    move.source = source.id;
    move.target = target.id;
    move.landing = target.id;
    move.path = { target.id };
    move.movement = MovementKind::M02;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { target.id };
    move.landing_action_point_gains.emplace(target.id, 0);
    move.controllable = true;

    auto unavailable = MoveTransaction::propose(move, map, viewport);
    REQUIRE(unavailable.has_value());
    REQUIRE(unavailable->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
    std::string error;
    REQUIRE_FALSE(unavailable->commit(map.revision, viewport.viewport_revision(), RunResources {}, &error));
    REQUIRE(error == "movement charge was exhausted before transaction commit");

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
    const RunResources before_move_resources = resources_authorizing(move.movement);
    REQUIRE(transaction->commit(map.revision, viewport.viewport_revision(), before_move_resources));

    MoveObservation observation;
    observation.current_node = target.id;
    observation.floor = 1;
    observation.action_points = 1;
    observation.landed_type = target.type;
    observation.viewport_revision = viewport.viewport_revision() + 1;
    REQUIRE(transaction->observe(observation));

    RunState after_move_inventory;
    after_move_inventory.floor = 1;
    after_move_inventory.current_node = source.id;
    after_move_inventory.resources.action_points = 2;
    // The fresh post-move parts-box scan is authoritative: this committed move
    // consumed the one-charge item, so it is correctly absent here.
    REQUIRE(after_move_inventory.resources.movement_charges.empty());

    REQUIRE(transaction->apply(after_move_inventory, &error));
    REQUIRE(after_move_inventory.current_node == target.id);
    REQUIRE(after_move_inventory.resources.action_points == 1);
}

TEST_CASE("BlackFlow random movement falls back after every hidden noncombat candidate is explicitly resident-marked")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 4, 4 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::ScrapShop;
    source.identity_revealed = true;
    REQUIRE(map.upsert_node(source));

    Node hidden_resident;
    hidden_resident.floor = 3;
    hidden_resident.position = { 0, 0 };
    hidden_resident.id = *make_stable_node_id(hidden_resident.floor, hidden_resident.position);
    hidden_resident.type = NodeType::HideInvisible;
    hidden_resident.marker_type = "savage";
    REQUIRE(map.upsert_node(hidden_resident));

    Node duel;
    duel.floor = 3;
    duel.position = { 0, 1 };
    duel.id = *make_stable_node_id(duel.floor, duel.position);
    duel.type = NodeType::Duel;
    duel.identity_revealed = true;
    REQUIRE(map.upsert_node(duel));

    RunState run;
    run.floor = 3;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, hidden_resident.id) ==
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, duel.id) != random_move->possible_landings.end());

    StateExpansionOptions options;
    options.final_is_terminal = false;
    OnDemandStateGraph graph;
    std::string error;
    REQUIRE(graph.initialize(map, run, options, &error));
    const auto* compact_actions = graph.actions(graph.initial_state(), &error);
    REQUIRE(compact_actions != nullptr);
    const auto compact_random = std::ranges::find_if(*compact_actions, [](const OnDemandSafetyAction& action) {
        return action.candidate.movement == MovementKind::M07;
    });
    REQUIRE(compact_random != compact_actions->end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, hidden_resident.id) ==
            compact_random->candidate.possible_landings.end());
    REQUIRE(std::ranges::find(compact_random->candidate.possible_landings, duel.id) !=
            compact_random->candidate.possible_landings.end());
}

TEST_CASE("BlackFlow random movement carries semantics for a visually hidden final landing")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 4, 4 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::ScrapShop;
    source.identity_revealed = true;
    REQUIRE(map.upsert_node(source));

    Node hidden_incident;
    hidden_incident.floor = 3;
    hidden_incident.position = { 0, 0 };
    hidden_incident.id = *make_stable_node_id(hidden_incident.floor, hidden_incident.position);
    hidden_incident.type = NodeType::HideInvisible;
    hidden_incident.visually_hidden = true;
    REQUIRE(map.upsert_node(hidden_incident));

    Node hidden_final;
    hidden_final.floor = 3;
    hidden_final.position = { 0, 1 };
    hidden_final.id = *make_stable_node_id(hidden_final.floor, hidden_final.position);
    hidden_final.type = NodeType::Final;
    hidden_final.identity_revealed = false;
    hidden_final.identity_state = NodeIdentityState::Hidden;
    hidden_final.identity_from_topology = true;
    hidden_final.visually_hidden = true;
    REQUIRE(map.upsert_node(hidden_final));

    RunState run;
    run.floor = 3;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);
    run.active_movement = MovementKind::M07;

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07 &&
               !action.candidate.bypass_final_on_completion;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, hidden_incident.id) !=
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, hidden_final.id) !=
            random_move->possible_landings.end());
    REQUIRE(move_landing_type(random_move->candidate, hidden_final.id) == NodeType::Final);
    REQUIRE(move_landing_is_terminal(random_move->candidate, hidden_final.id));
}

TEST_CASE("BlackFlow random movement drops revealed noncombat landings beside a hidden final")
{
    MapSnapshot map;

    Node source;
    source.floor = 4;
    source.position = { 2, 7 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::ScrapShop;
    source.identity_revealed = true;
    REQUIRE(map.upsert_node(source));

    Node revealed_incident;
    revealed_incident.floor = 4;
    revealed_incident.position = { 4, 5 };
    revealed_incident.id = *make_stable_node_id(revealed_incident.floor, revealed_incident.position);
    revealed_incident.type = NodeType::Incident;
    revealed_incident.identity_revealed = true;
    REQUIRE(map.upsert_node(revealed_incident));

    Node hidden_final;
    hidden_final.floor = 4;
    hidden_final.position = { 4, 0 };
    hidden_final.id = *make_stable_node_id(hidden_final.floor, hidden_final.position);
    hidden_final.type = NodeType::Final;
    hidden_final.identity_revealed = true;
    hidden_final.identity_from_topology = true;
    hidden_final.visually_hidden = true;
    REQUIRE(map.upsert_node(hidden_final));

    RunState run;
    run.floor = 4;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);
    run.active_movement = MovementKind::M07;

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07 &&
               !action.candidate.bypass_final_on_completion;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, hidden_final.id) !=
            random_move->possible_landings.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, revealed_incident.id) ==
            random_move->possible_landings.end());
}

TEST_CASE("BlackFlow random movement carries semantics for a revealed final landing")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 4, 4 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::ScrapShop;
    source.identity_revealed = true;
    REQUIRE(map.upsert_node(source));

    Node revealed_final;
    revealed_final.floor = 3;
    revealed_final.position = { 0, 1 };
    revealed_final.id = *make_stable_node_id(revealed_final.floor, revealed_final.position);
    revealed_final.type = NodeType::Final;
    revealed_final.identity_revealed = true;
    revealed_final.identity_state = NodeIdentityState::Classified;
    REQUIRE(map.upsert_node(revealed_final));

    RunState run;
    run.floor = 3;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);
    run.active_movement = MovementKind::M07;

    const auto actions = enumerate_move_actions(map, run);
    const auto random_move = std::ranges::find_if(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07 &&
               !action.candidate.bypass_final_on_completion;
    });
    REQUIRE(random_move != actions.end());
    REQUIRE(std::ranges::find(random_move->possible_landings, revealed_final.id) !=
            random_move->possible_landings.end());
    REQUIRE(move_landing_type(random_move->candidate, revealed_final.id) == NodeType::Final);
    REQUIRE(move_landing_is_terminal(random_move->candidate, revealed_final.id));
    REQUIRE(std::ranges::any_of(actions, [](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M07 &&
               action.candidate.bypass_final_on_completion;
    }));
}

TEST_CASE("BlackFlow shop-only movement ignores topology-known shops that are still visually hidden")
{
    MapSnapshot map;

    Node source;
    source.floor = 1;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Incident;
    source.identity_revealed = true;
    REQUIRE(map.upsert_node(source));

    Node hidden_shop;
    hidden_shop.floor = 1;
    hidden_shop.position = { 0, 1 };
    hidden_shop.id = *make_stable_node_id(hidden_shop.floor, hidden_shop.position);
    hidden_shop.type = NodeType::Shop;
    hidden_shop.identity_revealed = true;
    hidden_shop.identity_from_topology = true;
    hidden_shop.identity_source = "map_template_fixed_identity";
    hidden_shop.visually_hidden = true;
    REQUIRE(map.upsert_node(hidden_shop));

    Node revealed_shop = hidden_shop;
    revealed_shop.position = { 1, 1 };
    revealed_shop.id = *make_stable_node_id(revealed_shop.floor, revealed_shop.position);
    revealed_shop.identity_from_topology = false;
    revealed_shop.identity_source = "ocr";
    revealed_shop.visually_hidden = false;
    REQUIRE(map.upsert_node(revealed_shop));

    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M10, 1);

    const auto actions = enumerate_move_actions(map, run);
    REQUIRE_FALSE(std::ranges::any_of(actions, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M10 && action.candidate.target == hidden_shop.id;
    }));
    REQUIRE(std::ranges::any_of(actions, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::M10 && action.candidate.target == revealed_shop.id;
    }));

    StateExpansionOptions options;
    options.final_is_terminal = false;
    OnDemandStateGraph graph;
    std::string error;
    REQUIRE(graph.initialize(map, run, options, &error));
    const auto* compact_actions = graph.actions(graph.initial_state(), &error);
    REQUIRE(compact_actions != nullptr);
    REQUIRE_FALSE(std::ranges::any_of(*compact_actions, [&](const OnDemandSafetyAction& action) {
        return action.candidate.movement == MovementKind::M10 && action.candidate.target == hidden_shop.id;
    }));
    REQUIRE(std::ranges::any_of(*compact_actions, [&](const OnDemandSafetyAction& action) {
        return action.candidate.movement == MovementKind::M10 && action.candidate.target == revealed_shop.id;
    }));
}

TEST_CASE("BlackFlow walk can land on another forest clearing but not reselect the current clearing")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Incident;
    source.traversal = default_traversal_for(source.type);
    REQUIRE(map.upsert_node(source));

    Node clearing;
    clearing.floor = 3;
    clearing.position = { 0, 1 };
    clearing.id = *make_stable_node_id(clearing.floor, clearing.position);
    clearing.type = NodeType::Empty;
    clearing.traversal = default_traversal_for(clearing.type);
    REQUIRE(map.upsert_node(clearing));

    Edge edge;
    edge.first = source.id;
    edge.second = clearing.id;
    edge.knowledge = EdgeKnowledge::Confirmed;
    REQUIRE(map.upsert_edge(edge));

    RunState run;
    run.floor = 3;
    run.current_node = source.id;
    run.resources.action_points = 2;

    const auto from_source = enumerate_move_actions(map, run);
    REQUIRE(std::ranges::any_of(from_source, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.target == clearing.id &&
               action.candidate.landing == clearing.id;
    }));

    run.current_node = clearing.id;
    const auto from_clearing = enumerate_move_actions(map, run);
    REQUIRE_FALSE(std::ranges::any_of(from_clearing, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.target == clearing.id;
    }));
}

TEST_CASE("BlackFlow walk may reenter an already visited repeatable node")
{
    MapSnapshot map;

    Node shop;
    shop.floor = 4;
    shop.position = { 1, 7 };
    shop.id = *make_stable_node_id(shop.floor, shop.position);
    shop.type = NodeType::ScrapShop;
    shop.name = "秘境行商";
    shop.traversal = default_traversal_for(shop.type);
    REQUIRE(shop.traversal.repeatable);
    REQUIRE(map.upsert_node(shop));

    Node clearing;
    clearing.floor = shop.floor;
    clearing.position = { 1, 6 };
    clearing.id = *make_stable_node_id(clearing.floor, clearing.position);
    clearing.type = NodeType::Empty;
    clearing.traversal = default_traversal_for(clearing.type);
    REQUIRE(map.upsert_node(clearing));
    REQUIRE(map.upsert_edge({ shop.id, clearing.id, EdgeKnowledge::Confirmed, {} }));

    RunState run;
    run.floor = shop.floor;
    run.current_node = shop.id;
    run.resources.action_points = 2;
    run.visited_nodes.emplace(shop.id);

    const auto actions = enumerate_move_actions(map, run);
    REQUIRE(std::ranges::any_of(actions, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.source == shop.id &&
               action.candidate.target == shop.id;
    }));
    REQUIRE(std::ranges::any_of(actions, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.target == clearing.id;
    }));

    StateExpansionOptions options;
    options.final_is_terminal = false;
    OnDemandStateGraph graph;
    std::string error;
    REQUIRE(graph.initialize(map, run, options, &error));
    const auto* compact_actions = graph.actions(graph.initial_state(), &error);
    REQUIRE(compact_actions != nullptr);
    REQUIRE(std::ranges::any_of(*compact_actions, [&](const OnDemandSafetyAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.source == shop.id &&
               action.candidate.target == shop.id;
    }));
}

TEST_CASE("BlackFlow current-node walk dominates an equivalent processing-item self move")
{
    MapSnapshot map;

    Node final;
    final.floor = 2;
    final.position = { 3, 4 };
    final.id = *make_stable_node_id(final.floor, final.position);
    final.type = NodeType::Final;
    final.name = "险路尽头";
    final.traversal = default_traversal_for(final.type);
    REQUIRE(map.upsert_node(final));

    RunState run;
    run.floor = final.floor;
    run.current_node = final.id;
    run.resources.action_points = 2;
    run.resources.movement_charges.emplace(MovementKind::M01, 1);
    run.resources.movement_charges.emplace(MovementKind::M02, 1);

    const auto direct_actions = enumerate_move_actions(map, run);
    REQUIRE(std::ranges::any_of(direct_actions, [&](const MoveAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.target == final.id;
    }));
    REQUIRE_FALSE(std::ranges::any_of(direct_actions, [&](const MoveAction& action) {
        return action.candidate.movement != MovementKind::Walk && action.candidate.target == final.id;
    }));

    StateExpansionOptions options;
    options.final_is_terminal = true;
    OnDemandStateGraph graph;
    std::string error;
    REQUIRE(graph.initialize(map, run, options, &error));
    const auto* compact_actions = graph.actions(graph.initial_state(), &error);
    REQUIRE(compact_actions != nullptr);
    REQUIRE(std::ranges::any_of(*compact_actions, [&](const OnDemandSafetyAction& action) {
        return action.candidate.movement == MovementKind::Walk && action.candidate.target == final.id;
    }));
    REQUIRE_FALSE(std::ranges::any_of(*compact_actions, [&](const OnDemandSafetyAction& action) {
        return action.candidate.movement != MovementKind::Walk && action.candidate.target == final.id;
    }));
}

TEST_CASE("BlackFlow applied self move advances the reusable route exactly once")
{
    PlannedRouteStep reenter;
    reenter.move.action_id = "walk:shop:shop";
    reenter.move.source = 17;
    reenter.move.target = 17;
    reenter.move.landing = 17;

    PlannedRouteStep leave;
    leave.move.action_id = "rogue_6_scrap_M_06:shop:clearing";
    leave.move.source = 17;
    leave.move.target = 14;
    leave.move.landing = 14;

    std::vector<PlannedRouteStep> pending { reenter, leave };
    advance_route_hint_after_applied_move(pending, reenter.move.action_id);

    REQUIRE(pending.size() == 1);
    REQUIRE(pending.front().move.action_id == leave.move.action_id);

    // 同一个事务不会被消费两次；对不上首步时丢弃提示，交给规划器正常搜索。
    advance_route_hint_after_applied_move(pending, reenter.move.action_id);
    REQUIRE(pending.empty());
}

TEST_CASE("BlackFlow processing evidence follows the selected diagnostic step")
{
    REQUIRE(diagnostic_evidence_is_visible_at_step(1, 2, 1, 3));
    REQUIRE(diagnostic_evidence_is_visible_at_step(1, 3, 1, 3));
    REQUIRE_FALSE(diagnostic_evidence_is_visible_at_step(1, 4, 1, 3));
    REQUIRE_FALSE(diagnostic_evidence_is_visible_at_step(2, 2, 1, 3));

    REQUIRE(diagnostic_evidence_is_visible_at_step(4, 7, 12, 4, 7, 13));
    REQUIRE_FALSE(diagnostic_evidence_is_visible_at_step(4, 6, 12, 4, 7, 13));

    const std::array evidence {
        DiagnosticEvidenceStamp { 4, 7, 3, "inventory_ocr" },
        DiagnosticEvidenceStamp { 4, 7, 4, "movement_panel_ocr" },
        DiagnosticEvidenceStamp { 4, 7, 5, "inventory_ocr" },
        DiagnosticEvidenceStamp { 4, 8, 6, "movement_panel_ocr" },
    };
    const auto latest = diagnostic_latest_evidence_by_type(evidence, 4, 7, 5);
    REQUIRE(latest == std::vector<std::size_t> { 1, 2 });
}

TEST_CASE("BlackFlow diagnostic report separates map generations and preserves route backgrounds")
{
    REQUIRE(diagnostic_map_section_key(4, 7, false) == "floor-4-generation-7");
    REQUIRE(diagnostic_map_section_key(4, 8, true) == "floor-4-generation-8-remembrance");
    REQUIRE(diagnostic_map_section_label(4, false) == "4 层");
    REQUIRE(diagnostic_map_section_label(4, true) == "追忆 4 层");

    const auto within_budget = diagnostic_image_selection(true, true, 2, 3);
    REQUIRE(within_budget.captured);
    REQUIRE(within_budget.overlay);

    const auto route_after_overlay_budget = diagnostic_image_selection(true, true, 3, 3);
    REQUIRE(route_after_overlay_budget.captured);
    REQUIRE_FALSE(route_after_overlay_budget.overlay);

    const auto routine_after_budget = diagnostic_image_selection(true, false, 3, 3);
    REQUIRE_FALSE(routine_after_budget.captured);
    REQUIRE_FALSE(routine_after_budget.overlay);
}

TEST_CASE("BlackFlow topology score depends only on occupied nodes and edges")
{
    const TopologyMatchEvidence actual_template {
        0,
        13,
        0,
    };
    const TopologyMatchEvidence other_template_whose_start_is_current {
        3,
        8,
        5,
    };

    REQUIRE(score_topology_match(actual_template) == 65);
    REQUIRE(score_topology_match(other_template_whose_start_is_current) == -565);
    REQUIRE(score_topology_match(actual_template) > score_topology_match(other_template_whose_start_is_current));
}

TEST_CASE("BlackFlow cached topology retries the adjacent row alias after viewport auto-pan")
{
    const auto plan = make_fixed_grid_alias_retry_plan(4, 5, 8, 0.0, 31.0, 99.5);
    REQUIRE(plan.floor == 4);
    REQUIRE(plan.rows == 5);
    REQUIRE(plan.columns == 8);
    REQUIRE(plan.translation_x == 0.0);
    REQUIRE(plan.translation_y[0] == 31.0);
    REQUIRE(plan.translation_y[1] == -68.5);
    REQUIRE(plan.translation_y[2] == 130.5);
}

TEST_CASE("BlackFlow topology matching may recover a visually disconnected edge graph")
{
    REQUIRE(topology_can_recover_disconnected_edge_graph(true, 18, 2));
    REQUIRE_FALSE(topology_can_recover_disconnected_edge_graph(false, 18, 2));
    REQUIRE_FALSE(topology_can_recover_disconnected_edge_graph(true, 0, 2));
    REQUIRE_FALSE(topology_can_recover_disconnected_edge_graph(true, 18, 1));
}

TEST_CASE("BlackFlow current marker atlas includes a marker that extends above the map ROI")
{
    REQUIRE(current_marker_atlas_top(95, 47) == 48);
    REQUIRE(current_marker_atlas_top(30, 47) == 0);
}

TEST_CASE("BlackFlow topology turns occupied nodes without an identity into empty nodes")
{
    REQUIRE(topology_occupied_without_identity_is_empty(true, "null"));
    REQUIRE(topology_occupied_without_identity_is_empty(true, "unclassified"));
    REQUIRE_FALSE(topology_occupied_without_identity_is_empty(false, "null"));
    REQUIRE_FALSE(topology_occupied_without_identity_is_empty(true, "incident"));
    REQUIRE(retain_topology_extra_edge(true, true, false));
    REQUIRE(retain_topology_extra_edge(true, false, true));
    REQUIRE_FALSE(retain_topology_extra_edge(true, false, false));
    REQUIRE_FALSE(retain_topology_extra_edge(false, true, true));
}

TEST_CASE("BlackFlow topology terminals use the floor-specific fixed identity")
{
    REQUIRE(terminal_identity_for_floor(1).node_type == "final");
    REQUIRE(terminal_identity_for_floor(1).display_name == "险路尽头");
    REQUIRE(terminal_identity_for_floor(2).node_type == "final");
    REQUIRE(terminal_identity_for_floor(2).display_name == "险路尽头");
    REQUIRE(terminal_identity_for_floor(3).node_type == "battle_boss");
    REQUIRE(terminal_identity_for_floor(3).display_name == "险路恶敌");
    REQUIRE(terminal_identity_for_floor(4).node_type == "final");
    REQUIRE(terminal_identity_for_floor(4).display_name == "险路尽头");
    REQUIRE(terminal_identity_for_floor(5).node_type == "battle_boss");
    REQUIRE(terminal_identity_for_floor(5).display_name == "险路恶敌");
}

TEST_CASE("BlackFlow revealed move preview identity overrides the topology identity")
{
    Node topology_identity;
    topology_identity.type = NodeType::Final;
    topology_identity.name = "险路尽头";
    topology_identity.identity_revealed = true;
    topology_identity.identity_from_topology = true;
    topology_identity.identity_source = "map_template_fixed_identity";

    MovePreview preview;
    preview.reachability = PreviewReachability::Reachable;
    preview.exact_action_point_cost = 1;
    preview.displayed_type = NodeType::BattleBoss;
    preview.displayed_name = "险路恶敌";
    preview.identity_revealed = true;

    REQUIRE(should_apply_revealed_preview_identity(topology_identity, preview));
}

TEST_CASE("BlackFlow resident settlement preview ignores quote typography differences")
{
    Node map_identity;
    map_identity.type = NodeType::BattleSavage;
    map_identity.name = "“居民”据点";
    map_identity.identity_revealed = true;
    map_identity.identity_source = "ocr";

    MovePreview preview;
    preview.reachability = PreviewReachability::Reachable;
    preview.exact_action_point_cost = 1;
    preview.displayed_type = NodeType::BattleSavage;
    preview.displayed_name = "\"居民\"据点";
    preview.identity_revealed = true;

    REQUIRE_FALSE(should_apply_revealed_preview_identity(map_identity, preview));
}

TEST_CASE("BlackFlow empty preview display name does not invalidate normalized empty identity")
{
    MapSnapshot map;
    Node normalized_empty;
    normalized_empty.floor = 2;
    normalized_empty.position = { 1, 4 };
    normalized_empty.id = *make_stable_node_id(normalized_empty.floor, normalized_empty.position);
    normalized_empty.type = NodeType::Empty;
    normalized_empty.name.clear();
    normalized_empty.identity_revealed = true;
    REQUIRE(map.upsert_node(normalized_empty));

    const Node* stored = map.find_node(normalized_empty.id);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->name == "林间空地");

    MovePreview preview;
    preview.reachability = PreviewReachability::Reachable;
    preview.exact_action_point_cost = 1;
    preview.displayed_type = NodeType::Empty;
    preview.displayed_name = "林间空地";
    preview.identity_revealed = true;

    REQUIRE_FALSE(should_apply_revealed_preview_identity(*stored, preview));
}

TEST_CASE("BlackFlow topology rebuild preserves a move preview identity correction")
{
    NormalizedMap map;
    MapObservationBatch topology;
    topology.floor = 3;
    ObservedNode endpoint;
    endpoint.position = { 0, 3 };
    endpoint.type = NodeType::Final;
    endpoint.name = "险路尽头";
    endpoint.identity_state = NodeIdentityState::Classified;
    endpoint.identity_revealed = true;
    endpoint.identity_from_topology = true;
    endpoint.identity_source = "map_template_fixed_identity";
    topology.nodes.emplace_back(endpoint);
    REQUIRE(map.merge(topology, MapMergePurpose::CurrentObservation));

    const Node* stored = map.snapshot().find_node(3, endpoint.position);
    REQUIRE(stored != nullptr);
    Node corrected = *stored;
    corrected.type = NodeType::BattleBoss;
    corrected.name = "险路恶敌";
    corrected.traversal = default_traversal_for(corrected.type);
    corrected.identity_from_topology = false;
    corrected.identity_source = "move_preview_ocr";
    corrected.detected_by_vision = true;
    REQUIRE(map.snapshot().upsert_node(std::move(corrected)));

    REQUIRE(map.merge(topology, MapMergePurpose::CurrentObservation));
    const Node* rebuilt = map.snapshot().find_node(3, endpoint.position);
    REQUIRE(rebuilt != nullptr);
    REQUIRE(rebuilt->type == NodeType::BattleBoss);
    REQUIRE(rebuilt->name == "险路恶敌");
    REQUIRE(rebuilt->identity_source == "move_preview_ocr");
}

TEST_CASE("BlackFlow current observation replaces a completed battle with observed empty")
{
    NormalizedMap map;
    MapObservationBatch recognized;
    recognized.floor = 1;
    ObservedNode battle;
    battle.position = { 0, 1 };
    battle.type = NodeType::BattleNormal;
    battle.name = "第一层起点相邻作战";
    battle.identity_state = NodeIdentityState::Classified;
    battle.identity_revealed = true;
    battle.identity_source = "map_topology_fixed_identity";
    recognized.nodes.emplace_back(battle);
    REQUIRE(map.merge(recognized));

    MapObservationBatch fallback;
    fallback.floor = 1;
    ObservedNode inferred_empty;
    inferred_empty.position = battle.position;
    inferred_empty.type = NodeType::Empty;
    inferred_empty.name = "";
    inferred_empty.identity_state = NodeIdentityState::Classified;
    inferred_empty.identity_revealed = true;
    inferred_empty.identity_from_topology = true;
    inferred_empty.identity_source = "map_topology_no_ocr_empty";
    inferred_empty.detected_by_vision = false;
    inferred_empty.confirmed_by_topology = true;
    fallback.nodes.emplace_back(inferred_empty);
    REQUIRE(map.merge(fallback));

    const Node* merged = map.snapshot().find_node(1, battle.position);
    REQUIRE(merged != nullptr);
    REQUIRE(merged->type == NodeType::Empty);
    REQUIRE(merged->name == EmptyNodeName);
    REQUIRE(merged->identity_source == "map_topology_no_ocr_empty");
    REQUIRE_FALSE(merged->detected_by_vision);
    REQUIRE(merged->confirmed_by_topology);
}

TEST_CASE("BlackFlow current observation does not let one missed OCR erase an active revealed node")
{
    NormalizedMap map;
    MapObservationBatch recognized;
    recognized.floor = 3;
    ObservedNode expedition;
    expedition.position = { 3, 1 };
    expedition.type = NodeType::Expedition;
    expedition.name = "先行一步";
    expedition.identity_state = NodeIdentityState::Classified;
    expedition.identity_revealed = true;
    expedition.identity_source = "ocr";
    expedition.detected_by_vision = true;
    expedition.confirmed_by_topology = true;
    recognized.nodes.emplace_back(expedition);
    REQUIRE(map.merge(recognized, MapMergePurpose::CurrentObservation));

    MapObservationBatch missed_ocr;
    missed_ocr.floor = 3;
    ObservedNode weak_empty;
    weak_empty.position = expedition.position;
    weak_empty.type = NodeType::Empty;
    weak_empty.name = "";
    weak_empty.identity_state = NodeIdentityState::Classified;
    weak_empty.identity_revealed = true;
    weak_empty.identity_from_topology = true;
    weak_empty.identity_source = "map_topology_no_ocr_empty";
    weak_empty.detected_by_vision = true;
    weak_empty.confirmed_by_topology = true;
    missed_ocr.nodes.emplace_back(weak_empty);
    REQUIRE(map.merge(missed_ocr, MapMergePurpose::CurrentObservation));

    const Node* active = map.snapshot().find_node(3, expedition.position);
    REQUIRE(active != nullptr);
    REQUIRE(active->type == NodeType::Expedition);
    REQUIRE(active->name == "先行一步");
    REQUIRE(active->identity_source == "ocr");

    Node completed = *active;
    completed.progress = NodeProgress::Completed;
    REQUIRE(map.snapshot().upsert_node(std::move(completed)));
    REQUIRE(map.merge(missed_ocr, MapMergePurpose::CurrentObservation));
    const Node* resolved = map.snapshot().find_node(3, expedition.position);
    REQUIRE(resolved != nullptr);
    REQUIRE(resolved->type == NodeType::Empty);
    REQUIRE(resolved->progress == NodeProgress::Completed);
}

TEST_CASE("BlackFlow exploration notebook preserves revealed identity after the current node becomes empty")
{
    NormalizedMap notebook;
    MapObservationBatch recognized;
    recognized.floor = 1;
    ObservedNode battle;
    battle.position = { 0, 1 };
    battle.type = NodeType::BattleNormal;
    battle.name = "本层作战关卡";
    battle.identity_state = NodeIdentityState::Classified;
    battle.identity_revealed = true;
    battle.identity_source = "ocr";
    recognized.nodes.emplace_back(battle);
    REQUIRE(notebook.merge(recognized, MapMergePurpose::ExplorationNotebook));

    MapObservationBatch completed_observation;
    completed_observation.floor = 1;
    ObservedNode observed_empty;
    observed_empty.position = battle.position;
    observed_empty.type = NodeType::Empty;
    observed_empty.name = "";
    observed_empty.identity_state = NodeIdentityState::Classified;
    observed_empty.identity_revealed = true;
    observed_empty.identity_source = "map_topology_no_ocr_empty";
    completed_observation.nodes.emplace_back(observed_empty);
    REQUIRE(notebook.merge(completed_observation, MapMergePurpose::ExplorationNotebook));

    const Node* noted = notebook.snapshot().find_node(1, battle.position);
    REQUIRE(noted != nullptr);
    REQUIRE(noted->type == NodeType::BattleNormal);
    REQUIRE(noted->name == "本层作战关卡");
    REQUIRE(noted->identity_source == "ocr");
}

TEST_CASE("BlackFlow exploration notebook preserves a previewed battle stage across generic map OCR")
{
    NormalizedMap notebook;
    MapObservationBatch initial_observation;
    initial_observation.floor = 2;
    ObservedNode generic_battle;
    generic_battle.position = { 1, 3 };
    generic_battle.type = NodeType::BattleNormal;
    generic_battle.name = "作战";
    generic_battle.identity_state = NodeIdentityState::Classified;
    generic_battle.identity_revealed = true;
    generic_battle.identity_source = "ocr";
    initial_observation.nodes.emplace_back(generic_battle);
    REQUIRE(notebook.merge(initial_observation, MapMergePurpose::ExplorationNotebook));

    Node previewed = *notebook.snapshot().find_node(2, generic_battle.position);
    previewed.name = "远北猎场";
    previewed.identity_source = "move_preview_stage_name";
    REQUIRE(notebook.snapshot().upsert_node(std::move(previewed)));

    MapObservationBatch subsequent_map_ocr;
    subsequent_map_ocr.floor = 2;
    subsequent_map_ocr.nodes.emplace_back(generic_battle);
    REQUIRE(notebook.merge(subsequent_map_ocr, MapMergePurpose::ExplorationNotebook));

    const Node* noted = notebook.snapshot().find_node(2, generic_battle.position);
    REQUIRE(noted != nullptr);
    REQUIRE(noted->type == NodeType::BattleNormal);
    REQUIRE(noted->name == "远北猎场");
    REQUIRE(noted->identity_source == "move_preview_stage_name");
    REQUIRE(battle_stage_name(*noted) == "远北猎场");

    Node generic = *noted;
    generic.name = "作战";
    REQUIRE(battle_stage_name(generic).empty());
}

TEST_CASE("BlackFlow exploration notebook preserves a recognized boss stage across its generic node name")
{
    NormalizedMap notebook;
    MapObservationBatch initial_observation;
    initial_observation.floor = 3;
    ObservedNode generic_boss;
    generic_boss.position = { 2, 4 };
    generic_boss.type = NodeType::BattleBoss;
    generic_boss.name = "险路恶敌";
    generic_boss.identity_state = NodeIdentityState::Classified;
    generic_boss.identity_revealed = true;
    generic_boss.identity_source = "map_template_fixed_identity";
    initial_observation.nodes.emplace_back(generic_boss);
    REQUIRE(notebook.merge(initial_observation, MapMergePurpose::ExplorationNotebook));

    Node recognized = *notebook.snapshot().find_node(3, generic_boss.position);
    recognized.name = "失落的人偶";
    recognized.identity_source = "battle_stage_name";
    REQUIRE(notebook.snapshot().upsert_node(std::move(recognized)));

    MapObservationBatch subsequent_map_observation;
    subsequent_map_observation.floor = 3;
    subsequent_map_observation.nodes.emplace_back(generic_boss);
    REQUIRE(notebook.merge(subsequent_map_observation, MapMergePurpose::ExplorationNotebook));

    const Node* noted = notebook.snapshot().find_node(3, generic_boss.position);
    REQUIRE(noted != nullptr);
    REQUIRE(noted->type == NodeType::BattleBoss);
    REQUIRE(noted->name == "失落的人偶");
    REQUIRE(noted->identity_source == "battle_stage_name");
    REQUIRE(battle_stage_name(*noted) == "失落的人偶");

    Node generic = *noted;
    generic.name = "险路恶敌";
    REQUIRE(battle_stage_name(generic).empty());
}

TEST_CASE("BlackFlow exploration notebook preserves resolved event identity after a real empty observation")
{
    NormalizedMap notebook;
    MapObservationBatch entered;
    entered.floor = 2;
    ObservedNode incident;
    incident.position = { 1, 3 };
    incident.type = NodeType::Incident;
    incident.name = "划算买卖";
    incident.progress = NodeProgress::Completed;
    incident.identity_state = NodeIdentityState::Classified;
    incident.identity_revealed = true;
    incident.identity_source = "event_name";
    entered.nodes.emplace_back(incident);
    REQUIRE(notebook.merge(entered, MapMergePurpose::ExplorationNotebook));

    MapObservationBatch returned_to_map;
    returned_to_map.floor = 2;
    ObservedNode observed_empty;
    observed_empty.position = incident.position;
    observed_empty.type = NodeType::Empty;
    observed_empty.name = "林间空地";
    observed_empty.progress = NodeProgress::Completed;
    observed_empty.identity_state = NodeIdentityState::Classified;
    observed_empty.identity_revealed = true;
    observed_empty.identity_source = "ocr";
    returned_to_map.nodes.emplace_back(observed_empty);
    REQUIRE(notebook.merge(returned_to_map, MapMergePurpose::ExplorationNotebook));

    const Node* noted = notebook.snapshot().find_node(2, incident.position);
    REQUIRE(noted != nullptr);
    REQUIRE(noted->type == NodeType::Incident);
    REQUIRE(noted->name == "划算买卖");
    REQUIRE(noted->progress == NodeProgress::Completed);
    REQUIRE(noted->identity_source == "event_name");
}

TEST_CASE("BlackFlow completed fixed battle is not resurrected by the topology template")
{
    NormalizedMap current_map;
    MapObservationBatch first_observation;
    first_observation.floor = 1;
    ObservedNode fixed_battle;
    fixed_battle.position = { 2, 2 };
    fixed_battle.type = NodeType::BattleNormal;
    fixed_battle.name = "作战";
    fixed_battle.identity_state = NodeIdentityState::Classified;
    fixed_battle.identity_revealed = true;
    fixed_battle.identity_from_topology = true;
    fixed_battle.identity_source = "map_template_fixed_identity";
    first_observation.nodes.emplace_back(fixed_battle);
    REQUIRE(current_map.merge(first_observation, MapMergePurpose::CurrentObservation));

    Node completed = *current_map.snapshot().find_node(1, fixed_battle.position);
    completed.type = NodeType::Empty;
    completed.name = EmptyNodeName;
    completed.progress = NodeProgress::Completed;
    completed.traversal = default_traversal_for(NodeType::Empty);
    completed.identity_state = NodeIdentityState::Classified;
    completed.identity_revealed = true;
    completed.identity_from_topology = false;
    completed.identity_source = "node_resolution_becomes_empty";
    REQUIRE(current_map.snapshot().upsert_node(std::move(completed)));

    MapObservationBatch next_observation;
    next_observation.floor = 1;
    next_observation.nodes.emplace_back(fixed_battle);
    REQUIRE(current_map.merge(next_observation, MapMergePurpose::CurrentObservation));

    const Node* observed = current_map.snapshot().find_node(1, fixed_battle.position);
    REQUIRE(observed != nullptr);
    REQUIRE(observed->type == NodeType::Empty);
    REQUIRE(observed->name == EmptyNodeName);
    REQUIRE(observed->progress == NodeProgress::Completed);
    REQUIRE(observed->identity_source == "node_resolution_becomes_empty");
}

TEST_CASE("BlackFlow semantic page finalization keeps current viewport coordinates usable")
{
    MapSnapshot map;
    Node node;
    node.floor = 1;
    node.position = { 0, 1 };
    node.id = *make_stable_node_id(node.floor, node.position);
    node.type = NodeType::BattleNormal;
    REQUIRE(map.upsert_node(node));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { node.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);
    const std::uint64_t observed_revision = map.revision;

    node.type = NodeType::Empty;
    node.progress = NodeProgress::Completed;
    REQUIRE(map.upsert_node(node));
    REQUIRE_FALSE(viewport.clickable_rect(node.id, map.revision, viewport.viewport_revision()).has_value());
    REQUIRE(viewport.rebind_map_revision_after_semantic_update(observed_revision, map.revision));
    REQUIRE((viewport.clickable_rect(node.id, map.revision, viewport.viewport_revision()) ==
             asst::Rect { 500, 200, 60, 60 }));
}

TEST_CASE("BlackFlow reconciles the last floor three move after adapted pursuit enters floor four")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Empty;
    REQUIRE(map.upsert_node(source));

    Node target;
    target.floor = 3;
    target.position = { 0, 1 };
    target.id = *make_stable_node_id(target.floor, target.position);
    target.type = NodeType::ScrapShop;
    REQUIRE(map.upsert_node(target));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { target.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "floor-three-last-move";
    move.source = source.id;
    move.target = target.id;
    move.landing = target.id;
    move.path = { target.id };
    move.movement = MovementKind::M06;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { target.id };
    move.landing_action_point_gains.emplace(target.id, 0);
    move.controllable = true;

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
    REQUIRE(transaction->commit(
        map.revision,
        viewport.viewport_revision(),
        resources_authorizing(move.movement)));
    REQUIRE(transaction->mark_page_resolved());

    MoveObservation floor_four;
    floor_four.current_node = *make_stable_node_id(4, GridPosition { 0, 0 });
    floor_four.floor = 4;
    floor_four.action_points = 7;
    floor_four.viewport_revision = viewport.viewport_revision() + 1;

    std::string error;
    REQUIRE_FALSE(transaction->observe(floor_four, &error));
    REQUIRE(error == "next map observation does not match the committed move");

    floor_four.advanced_via_adapted_pursuit = true;
    REQUIRE(transaction->observe(floor_four, &error));
}

TEST_CASE("BlackFlow reconciles a floor three move preempted by adapted pursuit before its page resolves")
{
    MapSnapshot map;

    Node source;
    source.floor = 3;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Empty;
    REQUIRE(map.upsert_node(source));

    Node target;
    target.floor = 3;
    target.position = { 0, 1 };
    target.id = *make_stable_node_id(target.floor, target.position);
    target.type = NodeType::ScrapShop;
    REQUIRE(map.upsert_node(target));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { target.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "floor-three-pursuit-preempts-page";
    move.source = source.id;
    move.target = target.id;
    move.landing = target.id;
    move.path = { target.id };
    move.movement = MovementKind::M06;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { target.id };
    move.landing_action_point_gains.emplace(target.id, 0);
    move.controllable = true;

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
    REQUIRE(transaction->commit(
        map.revision,
        viewport.viewport_revision(),
        resources_authorizing(move.movement)));
    REQUIRE(transaction->stage() == MoveTransactionStage::Committed);

    MoveObservation floor_four;
    floor_four.current_node = *make_stable_node_id(4, GridPosition { 0, 0 });
    floor_four.floor = 4;
    floor_four.action_points = 7;
    floor_four.viewport_revision = viewport.viewport_revision() + 1;
    floor_four.advanced_via_adapted_pursuit = true;

    std::string error;
    REQUIRE(transaction->observe(floor_four, &error));
}

TEST_CASE("BlackFlow reconciles a resolved hidden page that advances to the next floor")
{
    MapSnapshot map;

    Node source;
    source.floor = 1;
    source.position = { 1, 1 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::ScrapShop;
    REQUIRE(map.upsert_node(source));

    Node hidden_transfer;
    hidden_transfer.floor = 1;
    hidden_transfer.position = { 1, 0 };
    hidden_transfer.id = *make_stable_node_id(hidden_transfer.floor, hidden_transfer.position);
    hidden_transfer.type = NodeType::HideInvisible;
    REQUIRE(map.upsert_node(hidden_transfer));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { hidden_transfer.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "hidden-page-advances-floor";
    move.source = source.id;
    move.target = hidden_transfer.id;
    move.landing = hidden_transfer.id;
    move.path = { hidden_transfer.id };
    move.movement = MovementKind::M02;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { hidden_transfer.id };
    move.landing_action_point_gains.emplace(hidden_transfer.id, 0);
    move.controllable = true;

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
    REQUIRE(transaction->commit(
        map.revision,
        viewport.viewport_revision(),
        resources_authorizing(move.movement)));
    REQUIRE(transaction->mark_page_resolved());

    MoveObservation floor_two;
    floor_two.current_node = *make_stable_node_id(2, GridPosition { 0, 0 });
    floor_two.floor = 2;
    floor_two.action_points = 6;
    floor_two.viewport_revision = viewport.viewport_revision() + 1;

    std::string error;
    REQUIRE_FALSE(transaction->observe(floor_two, &error));
    REQUIRE(error == "next map observation does not match the committed move");

    floor_two.advanced_via_resolved_page = true;
    REQUIRE(transaction->observe(floor_two, &error));
}

TEST_CASE("BlackFlow applies an uncontrollable move whose hidden landing advances to the next floor")
{
    MapSnapshot map;

    Node source;
    source.floor = 1;
    source.position = { 0, 2 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Empty;
    REQUIRE(map.upsert_node(source));

    Node hidden_transfer;
    hidden_transfer.floor = 1;
    hidden_transfer.position = { 1, 0 };
    hidden_transfer.id = *make_stable_node_id(hidden_transfer.floor, hidden_transfer.position);
    hidden_transfer.type = NodeType::HideInvisible;
    REQUIRE(map.upsert_node(hidden_transfer));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { hidden_transfer.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "xiao-bajie-hidden-terminal";
    move.source = source.id;
    move.target = hidden_transfer.id;
    move.movement = MovementKind::M07;
    move.predicted_action_point_cost = 0;
    move.possible_landings = { hidden_transfer.id };
    move.landing_action_point_gains.emplace(hidden_transfer.id, 0);
    move.controllable = false;

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 0 }));
    REQUIRE(transaction->commit(
        map.revision,
        viewport.viewport_revision(),
        resources_authorizing(move.movement)));
    REQUIRE(transaction->mark_page_resolved());

    MoveObservation floor_two;
    floor_two.current_node = *make_stable_node_id(2, GridPosition { 0, 0 });
    floor_two.floor = 2;
    floor_two.action_points = 6;
    floor_two.viewport_revision = viewport.viewport_revision() + 1;
    floor_two.advanced_via_resolved_page = true;

    std::string error;
    REQUIRE(transaction->observe(floor_two, &error));

    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 1;
    run.resources.movement_charges.emplace(MovementKind::M07, 1);
    REQUIRE(transaction->apply(run, &error));
    REQUIRE(run.floor == 2);
    REQUIRE(run.current_node == floor_two.current_node);
    REQUIRE(run.resources.action_points == 6);
    REQUIRE(run.resources.movement_charges.at(MovementKind::M07) == 1);
}

TEST_CASE("BlackFlow accepts an uncontrollable final landing through its observed semantics")
{
    MapSnapshot map;

    Node source;
    source.floor = 1;
    source.position = { 0, 2 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Empty;
    REQUIRE(map.upsert_node(source));

    Node terminal;
    terminal.floor = 1;
    terminal.position = { 1, 0 };
    terminal.id = *make_stable_node_id(terminal.floor, terminal.position);
    terminal.type = NodeType::Final;
    terminal.identity_revealed = true;
    REQUIRE(map.upsert_node(terminal));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { terminal.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "xiao-bajie-revealed-terminal";
    move.source = source.id;
    move.target = terminal.id;
    move.movement = MovementKind::M07;
    move.predicted_action_point_cost = 0;
    move.possible_landings = { terminal.id };
    move.landing_node_types.emplace(terminal.id, NodeType::Final);
    move.landing_action_point_gains.emplace(terminal.id, 0);
    move.controllable = false;

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 0 }));
    REQUIRE(transaction->commit(
        map.revision,
        viewport.viewport_revision(),
        resources_authorizing(move.movement)));
    REQUIRE(transaction->mark_page_resolved());

    MoveObservation floor_two;
    floor_two.current_node = *make_stable_node_id(2, GridPosition { 0, 0 });
    floor_two.floor = 2;
    floor_two.action_points = 6;
    floor_two.landed_type = NodeType::Final;
    floor_two.viewport_revision = viewport.viewport_revision() + 1;

    std::string error;
    REQUIRE(transaction->observe(floor_two, &error));
}

TEST_CASE("BlackFlow reconciles a floor four terminal that opens a recollection map on the same floor")
{
    MapSnapshot map;

    Node source;
    source.floor = 4;
    source.position = { 1, 7 };
    source.id = *make_stable_node_id(source.floor, source.position);
    source.type = NodeType::Door;
    REQUIRE(map.upsert_node(source));

    Node terminal;
    terminal.floor = 4;
    terminal.position = { 0, 7 };
    terminal.id = *make_stable_node_id(terminal.floor, terminal.position);
    terminal.type = NodeType::Final;
    REQUIRE(map.upsert_node(terminal));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { terminal.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);

    MoveCandidate move;
    move.action_id = "floor-four-recollection";
    move.source = source.id;
    move.target = terminal.id;
    move.landing = terminal.id;
    move.path = { terminal.id };
    move.movement = MovementKind::Walk;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { terminal.id };
    move.landing_action_point_gains.emplace(terminal.id, 0);
    move.controllable = true;

    SECTION("a known terminal reached by walking") {}

    SECTION("M07 learns the old terminal identity from its completed page")
    {
        move.movement = MovementKind::M07;
        move.controllable = false;
        move.landing = InvalidNodeId;
    }

    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
    REQUIRE(transaction->commit(
        map.revision,
        viewport.viewport_revision(),
        resources_authorizing(move.movement)));
    REQUIRE(transaction->mark_page_resolved());

    MoveObservation recollection;
    recollection.current_node = *make_stable_node_id(4, GridPosition { 3, 3 });
    recollection.floor = 4;
    recollection.action_points = 8;
    recollection.viewport_revision = viewport.viewport_revision() + 1;

    std::string error;
    REQUIRE_FALSE(transaction->observe(recollection, &error));
    REQUIRE(error == "next map observation does not match the committed move");

    recollection.renewed_same_floor_after_terminal = true;
    if (!move.controllable) {
        // The new map starts at a winding passage. It is not the M07 landing on the old map.
        recollection.landed_type = NodeType::Door;
        REQUIRE_FALSE(transaction->observe(recollection, &error));
        recollection.landed_type = NodeType::Final;
    }
    REQUIRE(transaction->observe(recollection, &error));

    RunState run;
    run.floor = 4;
    run.current_node = source.id;
    run.resources = resources_authorizing(move.movement);
    run.resources.action_points = 1;
    run.resources.white_model_birds = 1;
    REQUIRE(transaction->apply(run, &error));
    REQUIRE(run.current_node == recollection.current_node);
    REQUIRE(run.resources.action_points == 8);
    REQUIRE(run.resources.white_model_birds == 1);
}

TEST_CASE("BlackFlow completed one-time nodes become empty unless explicitly retained")
{
    REQUIRE(completed_node_becomes_empty(false));
    REQUIRE_FALSE(completed_node_becomes_empty(true));
    REQUIRE_FALSE(completed_node_becomes_empty(false, false));
    REQUIRE(completed_node_becomes_empty(true, true));
}

TEST_CASE("BlackFlow effective nodes count only first-time scoring landings")
{
    const NodeId current_node = 100;
    const std::unordered_set<NodeId> previously_entered_nodes { 101 };
    std::unordered_set<NodeId> effective_nodes;
    record_effective_landing(100, NodeType::Incident, current_node, previously_entered_nodes, effective_nodes);
    record_effective_landing(101, NodeType::Incident, current_node, previously_entered_nodes, effective_nodes);
    record_effective_landing(102, NodeType::Incident, current_node, previously_entered_nodes, effective_nodes);
    record_effective_landing(102, NodeType::Incident, current_node, previously_entered_nodes, effective_nodes);
    record_effective_landing(103, NodeType::Empty, current_node, previously_entered_nodes, effective_nodes);
    record_effective_landing(InvalidNodeId, NodeType::BattleNormal, current_node, previously_entered_nodes, effective_nodes);

    REQUIRE(effective_nodes == std::unordered_set<NodeId> { 102 });
}

TEST_CASE("BlackFlow ScrapShop counts as two effective nodes only on its first landing")
{
    std::unordered_set<NodeId> effective_nodes;
    const std::unordered_set<NodeId> previously_entered_nodes;

    REQUIRE(record_effective_landing(
                201,
                NodeType::ScrapShop,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 2);
    REQUIRE(record_effective_landing(
                201,
                NodeType::ScrapShop,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 0);
    REQUIRE(record_effective_landing(
                202,
                NodeType::Incident,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 1);
    REQUIRE(effective_nodes == std::unordered_set<NodeId> { 201, 202 });
}

TEST_CASE("BlackFlow weighted effective nodes include elite savage and floor-specific portals")
{
    REQUIRE(effective_node_weight_at_floor(NodeType::BattleElite, 1) == 2);
    REQUIRE(effective_node_weight_at_floor(NodeType::BattleSavage, 5) == 2);
    REQUIRE(effective_node_weight_at_floor(NodeType::Portal, 1) == 2);
    REQUIRE(effective_node_weight_at_floor(NodeType::Portal, 2) == 2);
    REQUIRE(effective_node_weight_at_floor(NodeType::Portal, 3) == 3);
    REQUIRE(effective_node_weight_at_floor(NodeType::Portal, 4) == 2);
    REQUIRE(effective_node_weight_at_floor(NodeType::Portal, 5) == 2);
    REQUIRE(effective_node_weight_at_floor(NodeType::BattleNormal, 3) == 1);
    REQUIRE(effective_node_weight_at_floor(NodeType::Portal, 3, "fruit_cache", MovementKind::M10) == 5);
}

TEST_CASE("BlackFlow movement panel full scan normalizes every initial scroll position")
{
    REQUIRE(MovementPanelMaximumSwipes >= 6);
    REQUIRE(movement_panel_scan_is_complete(true, true, true));
    REQUIRE_FALSE(movement_panel_scan_is_complete(true, true, false));
    REQUIRE_FALSE(movement_panel_scan_is_complete(true, false, true));

    constexpr int LastPage = 3;
    for (int initial_page = 0; initial_page <= LastPage; ++initial_page) {
        int page = initial_page;
        int upward_swipes = 0;
        bool top_anchor_seen = false;
        std::vector<int> inspected_pages;
        for (const MovementPanelScanAction action : movement_panel_full_scan_plan()) {
            switch (action) {
            case MovementPanelScanAction::ResetTowardStart:
                if (top_anchor_seen) {
                    break;
                }
                page = std::max(0, page - 1);
                ++upward_swipes;
                break;
            case MovementPanelScanAction::Inspect:
                inspected_pages.emplace_back(page);
                top_anchor_seen = top_anchor_seen || page == 0;
                break;
            case MovementPanelScanAction::AdvanceTowardEnd:
                page = std::min(LastPage, page + 1);
                break;
            }
        }

        for (int expected_page = 0; expected_page <= LastPage; ++expected_page) {
            REQUIRE(std::ranges::find(inspected_pages, expected_page) != inspected_pages.end());
        }
        REQUIRE(upward_swipes == initial_page);
    }
}

TEST_CASE("BlackFlow movement panel does not mistake repeated item kinds for the list end")
{
    const std::array previous {
        MovementPanelLayoutEntry { MovementKind::M03, 250 },
        MovementPanelLayoutEntry { MovementKind::M03, 390 },
        MovementPanelLayoutEntry { MovementKind::M08, 530 },
    };
    const std::array shifted {
        MovementPanelLayoutEntry { MovementKind::M03, 220 },
        MovementPanelLayoutEntry { MovementKind::M08, 360 },
        MovementPanelLayoutEntry { MovementKind::M08, 500 },
    };
    const std::array stable_with_ocr_jitter {
        MovementPanelLayoutEntry { MovementKind::M03, 253 },
        MovementPanelLayoutEntry { MovementKind::M03, 387 },
        MovementPanelLayoutEntry { MovementKind::M08, 534 },
    };

    REQUIRE_FALSE(movement_panel_layout_unchanged(previous, shifted));
    REQUIRE(movement_panel_layout_unchanged(previous, stable_with_ocr_jitter));
    REQUIRE(movement_panel_layout_overlaps(previous, shifted));
}

TEST_CASE("BlackFlow movement panel keeps searching through repeated-item pages")
{
    // 零件箱是规划加工品的权威信源。即使若干屏布局都由同一种加工品组成，
    // 选择面板也不能把“看起来没变”当作目标不存在，必须继续扫到安全上限。
    for (int completed_swipes = 0; completed_swipes < MovementPanelMaximumSwipes; ++completed_swipes) {
        REQUIRE(movement_panel_should_continue_target_search(false, completed_swipes));
    }
    REQUIRE_FALSE(movement_panel_should_continue_target_search(false, MovementPanelMaximumSwipes));
    REQUIRE_FALSE(movement_panel_should_continue_target_search(true, 0));
}

TEST_CASE("BlackFlow movement panel chooses the least-used instance of the requested kind")
{
    const std::array candidates {
        MovementPanelCandidate { MovementKind::M03, 0 },
        MovementPanelCandidate { MovementKind::M03, 3 },
        MovementPanelCandidate { MovementKind::M03, 1 },
        MovementPanelCandidate { MovementKind::M03, 2 },
        MovementPanelCandidate { MovementKind::M08, 1 },
    };

    REQUIRE(choose_movement_panel_candidate(candidates, MovementKind::M03) == 2);
    REQUIRE(choose_movement_panel_candidate(candidates, MovementKind::M08) == 4);
}

TEST_CASE("BlackFlow parts-box instances stay separate while planning charges are aggregated")
{
    RunResources resources;
    resources.movement_instances = {
        RunResources::MovementInstance { MovementKind::M03, 3, 0 },
        RunResources::MovementInstance { MovementKind::M03, 1, 1 },
        RunResources::MovementInstance { MovementKind::M03, 2, 2 },
        RunResources::MovementInstance { MovementKind::M08, 1, 3 },
    };

    rebuild_movement_aggregates(resources);

    REQUIRE(resources.movement_instances.size() == 4);
    REQUIRE(resources.movement_pieces.at(MovementKind::M03) == 3);
    REQUIRE(resources.movement_charges.at(MovementKind::M03) == 6);
    REQUIRE(resources.movement_pieces.at(MovementKind::M08) == 1);
    REQUIRE(resources.movement_charges.at(MovementKind::M08) == 1);
}

TEST_CASE("BlackFlow rejects a preview route that spends more instances than the parts box contains")
{
    PlannedRouteStep structural;
    structural.move.movement = MovementKind::M11;
    const std::vector steps { structural, structural, structural };

    REQUIRE_FALSE(planned_route_fits_movement_inventory(steps, { { MovementKind::M11, 1 } }));
    REQUIRE(planned_route_fits_movement_inventory(steps, { { MovementKind::M11, 3 } }));
}

TEST_CASE("BlackFlow exchanges an equivalent early structural principle for a later standard engine")
{
    MapSnapshot map;
    const auto add_node = [&](GridPosition position, NodeType type) {
        Node node;
        node.floor = 5;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : to_string(type);
        node.traversal = default_traversal_for(type);
        node.identity_revealed = true;
        node.identity_state = NodeIdentityState::Classified;
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId source = add_node({ 2, 2 }, NodeType::Empty);
    const NodeId first = add_node({ 1, 3 }, NodeType::HideInvisible);
    const NodeId walked = add_node({ 1, 4 }, NodeType::Incident);
    const NodeId last = add_node({ 2, 3 }, NodeType::HideInvisible);
    REQUIRE(map.upsert_edge({ first, walked, EdgeKnowledge::Confirmed, {} }));

    RunState run;
    run.floor = 5;
    run.current_node = source;
    run.resources.action_points = 3;
    run.resources.movement_charges.emplace(MovementKind::M11, 1);
    run.resources.movement_charges.emplace(MovementKind::M03, 1);

    RunState projected = run;
    std::vector<PlannedRouteStep> steps;
    const auto append = [&](MovementKind movement, NodeId target) {
        const auto actions = enumerate_move_actions(map, projected, GraphLayer::Confirmed);
        const auto found = std::ranges::find_if(actions, [&](const MoveAction& action) {
            return action.candidate.movement == movement && action.candidate.target == target &&
                   !action.candidate.bypass_final_on_completion;
        });
        REQUIRE(found != actions.end());
        MoveCandidate move = found->candidate;
        move.action_point_requirement = 1;
        const int before = projected.resources.action_points;
        std::string error;
        const auto outcome = project_move_outcome(map, projected, move, move.predicted_action_point_cost, &error);
        INFO(error);
        REQUIRE(outcome.has_value());
        steps.emplace_back(
            PlannedRouteStep {
                move,
                before,
                move.predicted_action_point_cost,
                outcome->action_point_gain,
                outcome->run.resources.action_points,
            });
        projected = outcome->run;
    };
    append(MovementKind::M11, first);
    append(MovementKind::Walk, walked);
    append(MovementKind::M03, last);

    REQUIRE(prefer_less_valuable_exchangeable_processing_steps(map, run, steps));
    REQUIRE(steps[0].move.movement == MovementKind::M03);
    REQUIRE(steps[0].move.target == first);
    REQUIRE(steps[1].move.movement == MovementKind::Walk);
    REQUIRE(steps[2].move.movement == MovementKind::M11);
    REQUIRE(steps[2].move.target == last);
    REQUIRE(steps.back().action_points_after == 0);

    // 标准引擎与报废假肢的移动、行动力和目标限制相同，此时按当前剩余次数估值：
    // 三次假肢应保留，一次引擎先消耗，而不是按加工品编号写死。
    run.resources.movement_charges.clear();
    run.resources.movement_charges.emplace(MovementKind::M02, 3);
    run.resources.movement_charges.emplace(MovementKind::M03, 1);
    projected = run;
    steps.clear();
    append(MovementKind::M02, first);
    append(MovementKind::Walk, walked);
    append(MovementKind::M03, last);
    REQUIRE(prefer_less_valuable_exchangeable_processing_steps(map, run, steps));
    REQUIRE(steps[0].move.movement == MovementKind::M03);
    REQUIRE(steps[2].move.movement == MovementKind::M02);
}

TEST_CASE("BlackFlow endpoint fallback cannot exhaust action points outside a real terminal")
{
    REQUIRE_FALSE(endpoint_fallback_candidate_is_safe(false, 1, 1, 0, false, false));
    REQUIRE(endpoint_fallback_candidate_is_safe(false, 1, 1, 0, true, false));
    REQUIRE_FALSE(endpoint_fallback_candidate_is_safe(false, 1, 1, 0, true, true));
    REQUIRE(endpoint_fallback_candidate_is_safe(false, 2, 1, 0, false, false));
    REQUIRE(endpoint_fallback_candidate_is_safe(true, 1, 1, 0, false, true));
}

TEST_CASE("BlackFlow movement panel advances one card without a kinetic fling")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    for (const std::string_view task_name : {
             "BlackFlow@Roguelike@MovementPanelSwipe",
             "BlackFlow@Roguelike@MovementPanelSwipeToStart",
         }) {
        const auto& task = tasks->at(std::string(task_name));
        const auto start = task.get("specificRect", std::vector<int> {});
        const auto end = task.get("rectMove", std::vector<int> {});
        const auto parameters = task.get("specialParams", std::vector<int> {});
        REQUIRE(start.size() == 4);
        REQUIRE(end.size() == 4);
        REQUIRE(parameters.size() >= 1);
        const int distance = std::abs(start[1] - end[1]);
        // 2026-09-02 现场：260 px / 200 ms 的手势让列表内容一次跳了约 690 px，
        // 前后两屏没有共同的完整加工品。行程限制在一张多一点，并把持续时间拉长，
        // 既保留相邻画面重叠，也避免 Android 把拖动解释成惯性甩动。
        REQUIRE(distance >= 160);
        REQUIRE(distance <= 200);
        REQUIRE(parameters[0] >= 300);
        REQUIRE(parameters[0] <= 400);
        REQUIRE(parameters.size() >= 2);
        REQUIRE(parameters[1] == 0);
        REQUIRE(parameters.size() >= 4);
        REQUIRE(parameters[3] == 0);
        REQUIRE(task.get("postDelay", 0) <= 350);
    }
}

TEST_CASE("BlackFlow movement inventory star slots stay anchored to the OCR name center")
{
    const auto require_center_near = [](const asst::Rect& slot, int expected_x, int expected_y) {
        REQUIRE(std::abs(slot.x + slot.width / 2 - expected_x) <= 2);
        REQUIRE(std::abs(slot.y + slot.height / 2 - expected_y) <= 2);
    };

    // 2026-08-30 的 1280x720 零件箱现场：1 次加工品。名称长度不同，
    // 星槽仍固定在名称框中心右侧，而不是固定在名称右端附近。
    require_center_near(movement_inventory_star_slot_rect(asst::Rect { 395, 259, 67, 19 }, 0), 462, 163);
    require_center_near(movement_inventory_star_slot_rect(asst::Rect { 403, 422, 51, 19 }, 0), 462, 324);

    // 2 次加工品的两颗星不是旧实现假定的 (+8,+12) 等距直线。
    const asst::Rect two_use_name { 394, 259, 69, 21 };
    require_center_near(movement_inventory_star_slot_rect(two_use_name, 0), 462, 163);
    require_center_near(movement_inventory_star_slot_rect(two_use_name, 1), 472, 170);

    // 3 次加工品沿用前三个离散槽位；最后一颗的纵向间距更大。
    const asst::Rect three_use_name { 387, 583, 83, 18 };
    require_center_near(movement_inventory_star_slot_rect(three_use_name, 0), 462, 485);
    require_center_near(movement_inventory_star_slot_rect(three_use_name, 1), 472, 493);
    require_center_near(movement_inventory_star_slot_rect(three_use_name, 2), 477, 508);
}

TEST_CASE("BlackFlow movement selection retries a stable frame after an adjacent item was loaded")
{
    REQUIRE(should_retry_movement_selection(MovementKind::M04, MovementKind::M05, true, 2));
    REQUIRE(should_retry_movement_selection(MovementKind::M04, std::nullopt, true, 1));
    // 装载相邻卡片会让列表自动居中，目标可能被临时挤出 OCR 区域；
    // 仍有次数时应回扫并重新定位，不能直接把整局判失败。
    REQUIRE(should_retry_movement_selection(MovementKind::M04, MovementKind::M05, false, 2));
    REQUIRE_FALSE(should_retry_movement_selection(MovementKind::M04, MovementKind::M04, true, 2));
    REQUIRE_FALSE(should_retry_movement_selection(MovementKind::M04, MovementKind::M05, true, 0));
}

TEST_CASE("BlackFlow movement selection never clicks a stale coordinate after the target leaves the stable frame")
{
    REQUIRE(
        movement_selection_click_decision(MovementKind::M06, false, MovementKind::M08) ==
        MovementSelectionClickDecision::ReacquireTarget);
    REQUIRE(
        movement_selection_click_decision(MovementKind::M06, false, std::nullopt) ==
        MovementSelectionClickDecision::ReacquireTarget);
    REQUIRE(
        movement_selection_click_decision(MovementKind::M06, true, MovementKind::M08) ==
        MovementSelectionClickDecision::ClickCurrentTarget);
    REQUIRE(
        movement_selection_click_decision(MovementKind::M06, false, MovementKind::M06) ==
        MovementSelectionClickDecision::AcceptAlreadyLoaded);
}

TEST_CASE("BlackFlow processing-item selection instability replans instead of terminating the run")
{
    REQUIRE(
        movement_selection_exhaustion_disposition(MovementKind::M08, true) ==
        MovementSelectionExhaustionDisposition::ReplanWithoutTarget);
    REQUIRE(
        movement_selection_exhaustion_disposition(MovementKind::Walk, true) ==
        MovementSelectionExhaustionDisposition::Fail);
    REQUIRE(
        movement_selection_exhaustion_disposition(MovementKind::M08, false) ==
        MovementSelectionExhaustionDisposition::Fail);
}

TEST_CASE("BlackFlow movement selection keeps a top-edge card click inside the visible panel")
{
    // 现场第三次定位喷气背包时，名称已经回弹到 y=201；旧逻辑再向上偏移 32
    // 会点到 y=169 的裁切区，导致装载标记始终不出现。
    const asst::Rect name_rect { 1136, 201, 111, 17 };
    const asst::Rect click_rect = movement_panel_selection_rect(name_rect);
    REQUIRE(click_rect.x == name_rect.x);
    REQUIRE(click_rect.width == name_rect.width);
    REQUIRE(click_rect.y >= 192);
    REQUIRE(click_rect.y <= name_rect.y);
    REQUIRE(click_rect.y + click_rect.height <= name_rect.y);
}

TEST_CASE("BlackFlow duel counts as two effective nodes while expedition and utility nodes do not")
{
    constexpr std::array excluded_types {
        NodeType::Final,
        NodeType::BattleBoss,
        NodeType::Sacrifice,
        NodeType::Light,
        NodeType::Door,
        NodeType::Employ,
        NodeType::Expedition,
    };
    std::unordered_set<NodeId> effective_nodes;
    const std::unordered_set<NodeId> previously_entered_nodes;
    NodeId id = 200;
    for (const NodeType type : excluded_types) {
        record_effective_landing(id++, type, InvalidNodeId, previously_entered_nodes, effective_nodes);
    }

    REQUIRE(effective_nodes.empty());
    REQUIRE(effective_node_weight(NodeType::Duel) == 2);
    REQUIRE(effective_node_weight(NodeType::Expedition) == 0);
    REQUIRE(record_effective_landing(
                id,
                NodeType::Duel,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 2);
    REQUIRE(effective_nodes == std::unordered_set<NodeId> { id });
}

TEST_CASE("BlackFlow floor three boss remains excluded from effective-node scoring")
{
    std::unordered_set<NodeId> effective_nodes;
    const std::unordered_set<NodeId> previously_entered_nodes;

    REQUIRE(effective_node_weight(NodeType::BattleBoss) == 0);
    REQUIRE(effective_node_weight(NodeType::BattleBoss, {}) == 0);
    REQUIRE(effective_node_weight(NodeType::BattleBoss, {}, MovementKind::Walk) == 0);
    REQUIRE(effective_node_weight(NodeType::BattleBoss, {}, MovementKind::M03) == 0);
    REQUIRE(record_effective_landing(
                291,
                NodeType::BattleBoss,
                {},
                MovementKind::Walk,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 0);
    REQUIRE(record_effective_landing(
                292,
                NodeType::BattleBoss,
                {},
                MovementKind::Walk,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 0);
    REQUIRE(effective_nodes.empty());
}

TEST_CASE("BlackFlow fruit-cache marker adds one effective-node weight to its node")
{
    std::unordered_set<NodeId> effective_nodes;
    const std::unordered_set<NodeId> previously_entered_nodes;

    REQUIRE(effective_node_weight(NodeType::Incident, "fruit_cache") == 2);
    REQUIRE(effective_node_weight(NodeType::Duel, "fruit_cache") == 3);
    REQUIRE(effective_node_weight(NodeType::Empty, "fruit_cache") == 1);
    REQUIRE(record_effective_landing(
                301,
                NodeType::Incident,
                "fruit_cache",
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 2);
    REQUIRE(record_effective_landing(
                301,
                NodeType::Incident,
                "fruit_cache",
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 0);
    REQUIRE(record_effective_landing(
                302,
                NodeType::Empty,
                "fruit_cache",
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 1);
}

TEST_CASE("BlackFlow Knot tentacle adds one effective-node point to its first distinct landing")
{
    std::unordered_set<NodeId> effective_nodes;
    const std::unordered_set<NodeId> previously_entered_nodes;

    REQUIRE(effective_node_weight(NodeType::Shop, {}, MovementKind::M10) == 2);
    REQUIRE(effective_node_weight(NodeType::ScrapShop, {}, MovementKind::M10) == 3);
    REQUIRE(record_effective_landing(
                401,
                NodeType::Shop,
                {},
                MovementKind::M10,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 2);
    REQUIRE(record_effective_landing(
                401,
                NodeType::Shop,
                {},
                MovementKind::M10,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 0);
    REQUIRE(record_effective_landing(
                402,
                NodeType::Shop,
                {},
                MovementKind::Walk,
                InvalidNodeId,
                previously_entered_nodes,
                effective_nodes) == 1);
}

TEST_CASE("BlackFlow fruit-cache marker template is available to map recognition")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto manifest =
        json::open(repository_root / "resource/roguelike/BlackFlow/map_perception/templates/manifest.json");
    REQUIRE(manifest.has_value());

    bool fruit_cache_found = false;
    for (const auto& entry : manifest->at("templates").as_array()) {
        if (entry.get("marker_type", std::string {}) != "fruit_cache") {
            continue;
        }
        fruit_cache_found = true;
        REQUIRE(entry.get("role", std::string {}) == "node_marker");
        REQUIRE(entry.get("threshold", 1.0) == 0.8);
        REQUIRE(std::filesystem::is_regular_file(
            repository_root / "resource/template/Roguelike/BlackFlow" / entry.get("file", std::string {})));
    }
    REQUIRE(fruit_cache_found);
}

TEST_CASE("BlackFlow confirmed topology-extra edges survive a later visual miss")
{
    NormalizedMap map;
    MapObservationBatch detected;
    detected.floor = 3;
    for (const GridPosition position : std::vector<GridPosition> { { 0, 0 }, { 0, 1 } }) {
        ObservedNode node;
        node.position = position;
        node.type = NodeType::Empty;
        node.identity_source = "map_topology_no_ocr_empty";
        node.confirmed_by_topology = true;
        detected.nodes.emplace_back(std::move(node));
    }
    detected.edges.emplace_back(
        ObservedEdge {
            { 0, 0 },
            { 0, 1 },
            EdgeKnowledge::Confirmed,
            EdgeEvidence { 0.91, true, false, "observed_extra_edge" },
        });
    REQUIRE(map.merge(detected));

    MapObservationBatch missed = detected;
    missed.coverage = ObservationCoverage::FullMap;
    missed.edges.clear();
    REQUIRE(map.merge(missed));

    const Node* first = map.snapshot().find_node(3, { 0, 0 });
    const Node* second = map.snapshot().find_node(3, { 0, 1 });
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    const Edge* edge = map.snapshot().find_edge(first->id, second->id);
    REQUIRE(edge != nullptr);
    REQUIRE(edge->knowledge == EdgeKnowledge::Confirmed);
    REQUIRE(edge->evidence.decision_source == "observed_extra_edge");
}

TEST_CASE("BlackFlow battle classification excludes duel")
{
    STATIC_REQUIRE(is_route_battle_node_type(NodeType::BattleNormal));
    STATIC_REQUIRE(is_route_battle_node_type(NodeType::BattleElite));
    STATIC_REQUIRE(is_route_battle_node_type(NodeType::BattleSavage));
    STATIC_REQUIRE(is_route_battle_node_type(NodeType::HideBattle));
    STATIC_REQUIRE(is_route_battle_node_type(NodeType::BattleBoss));
    REQUIRE_FALSE(is_route_battle_node_type(NodeType::Duel));
    REQUIRE_FALSE(is_combat_node_type(NodeType::Duel));
}

TEST_CASE("BlackFlow automation collection always allows resident settlements")
{
    const auto floor_one = automation_collection_forbidden_landing_types(1);
    REQUIRE_FALSE(floor_one.contains(NodeType::HideBattle));
    REQUIRE_FALSE(floor_one.contains(NodeType::BattleNormal));
    REQUIRE_FALSE(floor_one.contains(NodeType::BattleElite));
    REQUIRE_FALSE(floor_one.contains(NodeType::BattleSavage));

    const auto later_floor = automation_collection_forbidden_landing_types(2);
    REQUIRE(later_floor.contains(NodeType::HideBattle));
    REQUIRE(later_floor.contains(NodeType::BattleNormal));
    REQUIRE(later_floor.contains(NodeType::BattleElite));
    REQUIRE_FALSE(later_floor.contains(NodeType::BattleSavage));
}

TEST_CASE("BlackFlow automation collection avoids the floor-five boss but permits the floor-three exit")
{
    REQUIRE(automation_collection_forbidden_landing_types(5).contains(NodeType::BattleBoss));
    REQUIRE_FALSE(automation_collection_forbidden_landing_types(3).contains(NodeType::BattleBoss));
}

TEST_CASE("BlackFlow automation collection reserves full-map movement through the complete floor-four route")
{
    REQUIRE(automation_collection_reserved_full_map_charges(1) == 3);
    REQUIRE(automation_collection_reserved_full_map_charges(2) == 2);
    REQUIRE(automation_collection_reserved_full_map_charges(3) == 1);
    REQUIRE(automation_collection_reserved_full_map_charges(4) == 1);
    REQUIRE(automation_collection_reserved_full_map_charges(5) == 0);
}

TEST_CASE("BlackFlow automation strategy resource does not forbid resident settlements")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto strategy = json::open(repository_root / "resource/roguelike/BlackFlow/strategy.json");
    REQUIRE(strategy.has_value());

    bool rule_found = false;
    for (const auto& module : strategy->at("modules").as_array()) {
        if (module.get("id", std::string {}) != "automation_collection_route") {
            continue;
        }
        for (const auto& rule : module.at("rules").as_array()) {
            if (rule.get("id", std::string {}) != "automation_collection_avoid_regular_battles_after_floor1") {
                continue;
            }
            rule_found = true;
            for (const auto& condition : rule.at("candidate_if").at("any").as_array()) {
                REQUIRE(condition.get("value", std::string {}) != "battle_savage");
            }
        }
    }
    REQUIRE(rule_found);
}

TEST_CASE("BlackFlow automation collection requires the floor one eerie merchant")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto strategy = json::open(repository_root / "resource/roguelike/BlackFlow/strategy.json");
    REQUIRE(strategy.has_value());

    bool milestone_found = false;
    for (const auto& module : strategy->at("modules").as_array()) {
        if (module.get("id", std::string {}) != "automation_collection_route") {
            continue;
        }
        for (const auto& milestone : module.at("milestones").as_array()) {
            if (milestone.get("id", std::string {}) != "automation_collection_floor1_shop") {
                continue;
            }
            milestone_found = true;
            REQUIRE(milestone.get("enforcement", std::string {}) == "hard");
            const auto floor_window = milestone.get("floor_window", std::vector<int> {});
            REQUIRE(floor_window == std::vector<int> { 1, 1 });
            const auto node_types =
                milestone.at("selector").get("node_types", std::vector<std::string> {});
            REQUIRE(node_types == std::vector<std::string> { "shop" });
        }
    }
    REQUIRE(milestone_found);
}

TEST_CASE("BlackFlow roaming resident lookahead allows a different response for every outcome")
{
    MoveCandidate left_response;
    left_response.target = 10;
    left_response.landing = 10;
    left_response.path = { 10 };

    MoveCandidate right_response;
    right_response.target = 11;
    right_response.landing = 11;
    right_response.path = { 11 };

    const std::vector<std::vector<NodeId>> resident_positions { { 10, 11 } };
    const std::array responses { left_response, right_response };
    REQUIRE(every_roaming_resident_outcome_has_response(
        resident_positions,
        [&](const std::unordered_set<NodeId>& occupied) {
            return std::ranges::any_of(responses, [&](const MoveCandidate& response) {
                return !move_intersects_nodes(response, occupied);
            });
        }));

    const std::unordered_set<NodeId> pessimistic_union { 10, 11 };
    REQUIRE(std::ranges::none_of(responses, [&](const MoveCandidate& response) {
        return !move_intersects_nodes(response, pessimistic_union);
    }));
}

TEST_CASE("BlackFlow roaming residents stay or move one confirmed edge while respecting protected nodes")
{
    MapSnapshot map;
    const auto add_node = [&](int column, NodeType type, std::string name = {}) {
        const NodeId id = *make_stable_node_id(4, { 0, column });
        Node node;
        node.id = id;
        node.floor = 4;
        node.position = { 0, column };
        node.type = type;
        node.name = std::move(name);
        node.progress = NodeProgress::Active;
        node.traversal = default_traversal_for(type);
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId resident = add_node(1, NodeType::Incident);
    const NodeId open = add_node(2, NodeType::Empty);
    const NodeId shop = add_node(3, NodeType::Shop);
    const NodeId fate = add_node(4, NodeType::HideInvisible, "命运所指");
    const NodeId completed_empty = add_node(5, NodeType::Empty);
    for (const NodeId neighbor : { open, shop, fate, completed_empty }) {
        REQUIRE(map.upsert_edge({ resident, neighbor, EdgeKnowledge::Confirmed, {} }));
    }

    const std::unordered_set<NodeId> completed { completed_empty };
    const std::vector<NodeId> positions =
        roaming_resident_next_positions(map, resident, open, completed);
    REQUIRE(positions == std::vector<NodeId> { resident });

    const std::vector<NodeId> without_player_block =
        roaming_resident_next_positions(map, resident, InvalidNodeId, completed);
    std::vector<NodeId> expected { resident, open };
    std::ranges::sort(expected);
    REQUIRE(without_player_block == expected);
}

TEST_CASE("BlackFlow predicted resident settlement keeps its route-safe identity in a hidden preview")
{
    Node predicted;
    predicted.type = NodeType::BattleSavage;
    predicted.identity_from_prediction = true;
    predicted.prediction_rule = "initial_roaming_resident_settlement";

    REQUIRE(is_exact_predicted_resident_settlement(predicted));
    REQUIRE(is_resident_settlement(&predicted));
    REQUIRE(
        preview_landing_type_for_safety(&predicted, true, NodeType::HideBattle) == NodeType::BattleSavage);

    Node ordinary_hidden;
    ordinary_hidden.type = NodeType::HideBattle;
    REQUIRE_FALSE(is_resident_settlement(&ordinary_hidden));
    REQUIRE_FALSE(is_exact_predicted_resident_settlement(ordinary_hidden));
    REQUIRE(
        preview_landing_type_for_safety(&ordinary_hidden, true, NodeType::HideBattle) == NodeType::HideBattle);
}

TEST_CASE("BlackFlow terminal move cannot reveal nodes from its endpoint")
{
    MoveCandidate move;
    move.target = 102;
    move.landing = 102;
    move.path = { 101, 102 };

    REQUIRE_FALSE(move_endpoint_observation_available(true, 1));
    REQUIRE(move_reveal_origins(move, move.landing, false).empty());
}

TEST_CASE("BlackFlow action point exhaustion cannot reveal nodes from its endpoint")
{
    MoveCandidate move;
    move.target = 202;
    move.landing = 202;
    move.path = { 201, 202 };

    REQUIRE_FALSE(move_endpoint_observation_available(false, 0));
    REQUIRE(move_reveal_origins(move, move.landing, false).empty());
}

TEST_CASE("BlackFlow transparent walk path reveals only from its observable endpoint")
{
    MoveCandidate move;
    move.target = 303;
    move.landing = 303;
    move.path = { 301, 302, 303 };

    REQUIRE(move_endpoint_observation_available(false, 1));
    REQUIRE(move_reveal_origins(move, move.landing, true) == std::vector<NodeId> { 303 });
}

TEST_CASE("BlackFlow portal target reveals only from the final landing")
{
    MoveCandidate move;
    move.target = 302;
    move.landing = 399;

    REQUIRE(move_reveal_origins(move, move.landing, true) == std::vector<NodeId> { 399 });
}

TEST_CASE("BlackFlow reveal consistency keeps each linked event effect separate")
{
    MapSnapshot before;
    RunState run;
    run.floor = 3;
    const auto add_hidden = [&](GridPosition position, NodeType type) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.identity_state = NodeIdentityState::Hidden;
        node.identity_revealed = false;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(before.upsert_node(std::move(node)));
        return id;
    };
    const NodeId portal = add_hidden({ 0, 0 }, NodeType::Portal);
    const NodeId landing = add_hidden({ 0, 1 }, NodeType::Incident);
    const NodeId resident = add_hidden({ 4, 0 }, NodeType::BattleSavage);
    const NodeId employ = add_hidden({ 4, 1 }, NodeType::Employ);
    add_hidden({ 4, 2 }, NodeType::Incident);

    MoveCandidate move;
    move.target = portal;
    move.landing = landing;
    const auto lone_expected = expected_move_reveals(before, run, move, move.landing, true, { "独活-2" });
    const auto guard_expected = expected_move_reveals(before, run, move, move.landing, true, { "和平守卫者-2" });

    REQUIRE(lone_expected == std::unordered_set<NodeId> { landing, employ });
    REQUIRE(guard_expected == std::unordered_set<NodeId> { landing, resident });
    REQUIRE_FALSE(event_reveal_node_type("和平爱好者-2").has_value());
}

TEST_CASE("BlackFlow linked encounter transfer reveals from the returned-map landing")
{
    MapSnapshot map;
    RunState run;
    run.floor = 5;
    const auto add_hidden = [&](GridPosition position, NodeType type) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.identity_state = NodeIdentityState::Hidden;
        node.identity_revealed = false;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId event = add_hidden({ 1, 0 }, NodeType::HideInvisible);
    const NodeId event_neighbor = add_hidden({ 1, 1 }, NodeType::HideInvisible);
    const NodeId resident = add_hidden({ 4, 8 }, NodeType::BattleSavage);
    const NodeId resident_neighbor = add_hidden({ 4, 7 }, NodeType::HideInvisible);
    REQUIRE(map.upsert_edge({ event, event_neighbor, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ resident, resident_neighbor, EdgeKnowledge::Confirmed, {} }));

    MoveCandidate move;
    move.source = *make_stable_node_id(run.floor, GridPosition { 4, 5 });
    move.target = event;
    move.landing = event;

    const auto reveal_only =
        expected_linked_encounter_return_reveals(map, run, move, event, resident, false, true);
    const auto transferred =
        expected_linked_encounter_return_reveals(map, run, move, event, resident, true, true);

    REQUIRE(reveal_only == std::unordered_set<NodeId> { event, event_neighbor, resident });
    REQUIRE(transferred == std::unordered_set<NodeId> { event, resident, resident_neighbor });
    REQUIRE_FALSE(transferred.contains(event_neighbor));
}

TEST_CASE("BlackFlow linked encounter reveal crosses a winding passage after the event becomes empty")
{
    MapSnapshot map;
    RunState run;
    run.floor = 3;
    const auto add = [&](GridPosition position, NodeType type, bool revealed) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.identity_state = revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        node.identity_revealed = revealed;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };

    const NodeId event_neighbor = add({ 2, 0 }, NodeType::HideBattle, false);
    const NodeId event = add({ 1, 0 }, NodeType::Incident, true);
    const NodeId source = add({ 1, 1 }, NodeType::Empty, true);
    const NodeId first_door = add({ 1, 2 }, NodeType::Door, true);
    const NodeId clearing_1 = add({ 2, 2 }, NodeType::Empty, true);
    const NodeId clearing_2 = add({ 2, 3 }, NodeType::Empty, true);
    const NodeId clearing_3 = add({ 3, 3 }, NodeType::Empty, true);
    const NodeId clearing_4 = add({ 3, 4 }, NodeType::Empty, true);
    const NodeId second_door = add({ 3, 5 }, NodeType::Door, true);
    const NodeId landing = add({ 4, 5 }, NodeType::Employ, true);
    const NodeId landing_neighbor = add({ 4, 4 }, NodeType::HideInvisible, false);

    REQUIRE_FALSE(default_traversal_for(NodeType::Door).blocks_vision);
    REQUIRE(default_traversal_for(NodeType::Incident).blocks_vision);
    for (const auto [first, second] : {
             std::pair { event_neighbor, event },
             std::pair { event, source },
             std::pair { source, first_door },
             std::pair { first_door, clearing_1 },
             std::pair { clearing_1, clearing_2 },
             std::pair { clearing_2, clearing_3 },
             std::pair { clearing_3, clearing_4 },
             std::pair { clearing_4, second_door },
             std::pair { second_door, landing },
             std::pair { landing, landing_neighbor },
         }) {
        REQUIRE(map.upsert_edge({ first, second, EdgeKnowledge::Confirmed, {} }));
    }

    MoveCandidate move;
    move.source = source;
    move.target = event;
    move.landing = event;
    const auto expected =
        expected_linked_encounter_return_reveals(map, run, move, event, landing, true, true);

    REQUIRE(expected == std::unordered_set<NodeId> { event_neighbor, landing_neighbor });
}

TEST_CASE("BlackFlow reveal consistency attributes previously unknown event reveals after observation")
{
    MapSnapshot before;
    RunState run;
    run.floor = 3;
    const auto add_unknown = [&](GridPosition position) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = NodeType::Unknown;
        node.identity_state = NodeIdentityState::Hidden;
        node.identity_revealed = false;
        node.traversal = default_traversal_for(node.type);
        const NodeId id = node.id;
        REQUIRE(before.upsert_node(std::move(node)));
        return id;
    };
    const NodeId resident = add_unknown({ 2, 0 });
    const NodeId employ = add_unknown({ 2, 1 });
    const NodeId unrelated = add_unknown({ 2, 2 });

    MapSnapshot after = before;
    for (const auto [id, type] : { std::pair { resident, NodeType::BattleSavage },
                                  std::pair { employ, NodeType::Employ },
                                  std::pair { unrelated, NodeType::Incident } }) {
        Node revealed = *after.find_node(id);
        revealed.type = type;
        revealed.identity_state = NodeIdentityState::Classified;
        revealed.identity_revealed = true;
        revealed.traversal = default_traversal_for(type);
        REQUIRE(after.upsert_node(std::move(revealed)));
    }

    std::unordered_set<NodeId> expected;
    add_observed_event_reveal_expectations(before, run, after, { "和平守卫者-2" }, true, expected);

    REQUIRE(expected == std::unordered_set<NodeId> { resident });
}

TEST_CASE("BlackFlow initial floor reveal expectation combines entrance light eagle and mist rules")
{
    MapSnapshot map;
    const int floor = 2;
    const auto add = [&](GridPosition position, NodeType type, bool revealed, bool mist = false) {
        Node node;
        node.floor = floor;
        node.position = position;
        node.id = *make_stable_node_id(floor, position);
        node.type = type;
        node.identity_state = revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        node.identity_revealed = revealed;
        node.natural_reveal_suppressed = mist;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };
    const NodeId entrance = add({ 2, 2 }, NodeType::Empty, true);
    const NodeId connected = add({ 2, 3 }, NodeType::HideInvisible, true);
    const NodeId light = add({ 0, 0 }, NodeType::Light, true);
    const NodeId door = add({ 0, 1 }, NodeType::Door, true);
    const NodeId light_radius_two = add({ 0, 2 }, NodeType::HideBattle, true);
    const NodeId outside = add({ 4, 4 }, NodeType::Incident, false);
    const NodeId misted = add({ 1, 0 }, NodeType::Incident, false, true);
    REQUIRE(map.upsert_edge({ entrance, connected, EdgeKnowledge::Confirmed, {} }));

    const auto ordinary = expected_initial_floor_reveals(map, entrance, false);
    REQUIRE(ordinary.contains(connected));
    REQUIRE_FALSE(ordinary.contains(light));
    REQUIRE_FALSE(ordinary.contains(door));
    REQUIRE(ordinary.contains(light_radius_two));
    REQUIRE_FALSE(ordinary.contains(outside));
    // 初始羽瞰点与落点后的羽瞰点一样属于主动照亮；弥散虚雾只抑制
    // 沿连线传播的自然揭示，不能吞掉羽瞰点曼哈顿距离 2 的初始视野。
    REQUIRE(ordinary.contains(misted));
    REQUIRE(observed_initial_floor_reveals(map, entrance) ==
            std::unordered_set<NodeId> { connected, light_radius_two });

    const auto swaddled_eagle = expected_initial_floor_reveals(map, entrance, true);
    REQUIRE(swaddled_eagle.contains(outside));
    REQUIRE_FALSE(swaddled_eagle.contains(misted));
}

TEST_CASE("BlackFlow initial floor reveal propagates through transparent passage nodes")
{
    MapSnapshot map;
    const int floor = 4;
    const auto add = [&](GridPosition position, NodeType type, bool revealed) {
        Node node;
        node.floor = floor;
        node.position = position;
        node.id = *make_stable_node_id(floor, position);
        node.type = type;
        node.identity_state = revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        node.identity_revealed = revealed;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };

    const NodeId entrance = add({ 1, 3 }, NodeType::Empty, true);
    const NodeId passage = add({ 1, 2 }, NodeType::Door, true);
    const NodeId beyond_passage = add({ 2, 2 }, NodeType::Expedition, true);
    const NodeId opaque_neighbor = add({ 1, 4 }, NodeType::BattleNormal, true);
    const NodeId beyond_opaque = add({ 1, 5 }, NodeType::Incident, false);
    REQUIRE(map.upsert_edge({ entrance, passage, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ passage, beyond_passage, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ entrance, opaque_neighbor, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ opaque_neighbor, beyond_opaque, EdgeKnowledge::Confirmed, {} }));

    const auto expected = expected_initial_floor_reveals(map, entrance, false);
    REQUIRE_FALSE(expected.contains(passage));
    REQUIRE(expected.contains(beyond_passage));
    REQUIRE(expected.contains(opaque_neighbor));
    REQUIRE_FALSE(expected.contains(beyond_opaque));
}

TEST_CASE("BlackFlow initial floor reveal comparison ignores baseline-visible and hidden topology identities")
{
    MapSnapshot map;
    const int floor = 1;
    const auto add = [&](GridPosition position,
                         NodeType type,
                         std::string name,
                         bool visually_hidden = false,
                         std::string identity_source = "ocr") {
        Node node;
        node.floor = floor;
        node.position = position;
        node.id = *make_stable_node_id(floor, position);
        node.type = type;
        node.name = std::move(name);
        node.identity_state = NodeIdentityState::Classified;
        node.identity_revealed = true;
        node.visually_hidden = visually_hidden;
        node.identity_source = std::move(identity_source);
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };

    const NodeId entrance = add({ 1, 0 }, NodeType::Empty, "林间空地");
    const NodeId connected = add({ 1, 1 }, NodeType::BattleNormal, "作战");
    REQUIRE(map.upsert_edge({ entrance, connected, EdgeKnowledge::Confirmed, {} }));

    add({ 0, 2 }, NodeType::Door, "曲折密道");
    add({ 0, 3 }, NodeType::Incident, "命运所指");
    add({ 0, 4 }, NodeType::BattleBoss, "险路恶敌", false, "map_template_fixed_identity");
    add({ 2, 3 }, NodeType::Final, "险路尽头", false, "map_template_fixed_identity");
    add({ 2, 4 }, NodeType::Shop, "诡意行商", true, "map_template_fixed_identity");

    REQUIRE(observed_initial_floor_reveals(map, entrance) == std::unordered_set<NodeId> { connected });
}

TEST_CASE("BlackFlow linked encounter free transfer uses safe expected lexicographic benefit")
{
    const LinkedEncounterRouteValue baseline {
        .viable = true,
        .required_action_points = 4,
        .lexicographic_score = { -2, -2, 0, 5 },
        .lexicographic_score_labels = {
            "revealed_node_count",
            "effective_node_count",
            "persistent_processing_move_count",
            "route_length",
        },
    };
    LinkedEncounterRouteValue transfer = baseline;
    transfer.required_action_points = 3;
    transfer.lexicographic_score = { -2, -2, 0, 4 };
    REQUIRE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 0, 2, 2));

    // 行动力需求已经由可行性门槛兜底；只要仍然安全，路线期望分数更优即可。
    transfer.required_action_points = 5;
    REQUIRE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 0, 2, 2));

    transfer = baseline;
    REQUIRE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 1, 2, 2));
    REQUIRE_FALSE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 0, 2, 2));

    transfer = baseline;
    transfer.required_action_points = 3;
    transfer.lexicographic_score.back() = 4;
    REQUIRE_FALSE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 0, 3, 2));
    REQUIRE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 0, 2, 3));

    // 所有结果安全后按正常字典序比较期望；前项提升可以覆盖后项下降。
    transfer = baseline;
    transfer.lexicographic_score = { -3, -1, 0, 4 };
    REQUIRE(linked_encounter_free_transfer_is_stably_better(baseline, transfer, 0, 2, 2));
}

TEST_CASE("BlackFlow linked encounter return preserves both the original event and transferred landing")
{
    const int floor = 2;
    const NodeId event = *make_stable_node_id(floor, GridPosition { 1, 1 });
    const NodeId employ = *make_stable_node_id(floor, GridPosition { 2, 2 });

    MapSnapshot before;
    for (const auto [id, position, type] : {
             std::tuple { event, GridPosition { 1, 1 }, NodeType::Incident },
             std::tuple { employ, GridPosition { 2, 2 }, NodeType::HideInvisible },
         }) {
        Node node;
        node.id = id;
        node.floor = floor;
        node.position = position;
        node.type = type;
        node.progress = NodeProgress::Active;
        node.traversal = default_traversal_for(type);
        REQUIRE(before.upsert_node(std::move(node)));
    }

    MapSnapshot after = before;
    // 可控移动已经锁定原事件格；即使这一帧 OCR 还没把它刷新为空地，也不能把
    // 传送后的应急助力误写成原事件。
    Node revealed = *after.find_node(employ);
    revealed.type = NodeType::Employ;
    revealed.name = "应急助力";
    revealed.identity_revealed = true;
    revealed.traversal = default_traversal_for(NodeType::Employ);
    REQUIRE(after.upsert_node(std::move(revealed)));

    const auto resolved = resolve_linked_encounter_return(
        before,
        after,
        event,
        employ,
        NodeType::Employ,
        { employ });
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->event_node == event);
    REQUIRE(resolved->linked_node == employ);
    REQUIRE(resolved->linked_type == NodeType::Employ);
}

TEST_CASE("BlackFlow Xiaobajie linked encounter return finds the unique node that became empty")
{
    const int floor = 4;
    const NodeId event = *make_stable_node_id(floor, GridPosition { 1, 2 });
    const NodeId employ = *make_stable_node_id(floor, GridPosition { 3, 4 });
    const NodeId other = *make_stable_node_id(floor, GridPosition { 2, 3 });

    MapSnapshot before;
    for (const auto [id, position] : {
             std::pair { event, GridPosition { 1, 2 } },
             std::pair { employ, GridPosition { 3, 4 } },
             std::pair { other, GridPosition { 2, 3 } },
         }) {
        Node node;
        node.id = id;
        node.floor = floor;
        node.position = position;
        node.type = NodeType::HideInvisible;
        node.progress = NodeProgress::Active;
        node.traversal = default_traversal_for(node.type);
        REQUIRE(before.upsert_node(std::move(node)));
    }

    MapSnapshot after = before;
    Node cleared = *after.find_node(event);
    cleared.type = NodeType::Empty;
    // 当前观测里的空地本身仍可保持 Active；完成语义由事件生命周期回写。
    cleared.progress = NodeProgress::Active;
    cleared.traversal = default_traversal_for(NodeType::Empty);
    REQUIRE(after.upsert_node(std::move(cleared)));
    Node revealed = *after.find_node(employ);
    revealed.type = NodeType::Employ;
    revealed.traversal = default_traversal_for(NodeType::Employ);
    REQUIRE(after.upsert_node(std::move(revealed)));

    const auto resolved = resolve_linked_encounter_return(
        before,
        after,
        InvalidNodeId,
        employ,
        NodeType::Employ,
        { employ });
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->event_node == event);
    REQUIRE(resolved->linked_node == employ);

    Node also_cleared = *after.find_node(other);
    also_cleared.type = NodeType::Empty;
    also_cleared.progress = NodeProgress::Completed;
    also_cleared.traversal = default_traversal_for(NodeType::Empty);
    REQUIRE(after.upsert_node(std::move(also_cleared)));
    REQUIRE_FALSE(resolve_linked_encounter_return(
                      before,
                      after,
                      InvalidNodeId,
                      employ,
                      NodeType::Employ,
                      { employ })
                      .has_value());
}

TEST_CASE("BlackFlow move transaction accepts only an attributed linked encounter relocation")
{
    const int floor = 2;
    MapSnapshot map;
    Node source;
    source.floor = floor;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(floor, source.position);
    source.type = NodeType::Empty;
    source.traversal = default_traversal_for(source.type);
    REQUIRE(map.upsert_node(source));

    Node event = source;
    event.position = { 1, 1 };
    event.id = *make_stable_node_id(floor, event.position);
    event.type = NodeType::Incident;
    event.traversal = default_traversal_for(event.type);
    REQUIRE(map.upsert_node(event));
    const NodeId employ = *make_stable_node_id(floor, GridPosition { 2, 2 });

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { event.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        7);
    MoveCandidate move;
    move.action_id = "linked-encounter-transfer";
    move.source = source.id;
    move.target = event.id;
    move.landing = event.id;
    move.path = { event.id };
    move.movement = MovementKind::Walk;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { event.id };
    move.landing_action_point_gains.emplace(event.id, 0);
    move.controllable = true;

    auto make_transaction = [&]() {
        auto transaction = MoveTransaction::propose(move, map, viewport);
        REQUIRE(transaction.has_value());
        REQUIRE(transaction->record_preview(MovePreview { PreviewReachability::Reachable, 1 }));
        REQUIRE(transaction->commit(
            map.revision,
            viewport.viewport_revision(),
            resources_authorizing(move.movement)));
        REQUIRE(transaction->mark_page_resolved());
        return transaction;
    };
    MoveObservation returned;
    returned.current_node = employ;
    returned.floor = floor;
    returned.action_points = 2;
    returned.map_revision = map.revision + 1;
    returned.viewport_revision = viewport.viewport_revision() + 1;

    std::string error;
    auto unattributed = make_transaction();
    REQUIRE_FALSE(unattributed->observe(returned, &error));

    auto attributed = make_transaction();
    returned.linked_encounter_origin_node = event.id;
    REQUIRE(attributed->observe(returned, &error));
}

TEST_CASE("BlackFlow final encounter exposes both finish and pass-through planner actions")
{
    MapSnapshot map;
    Node start;
    start.floor = 1;
    start.position = { 0, 0 };
    start.id = *make_stable_node_id(start.floor, start.position);
    start.type = NodeType::Empty;
    start.identity_revealed = true;
    start.traversal = default_traversal_for(start.type);
    REQUIRE(map.upsert_node(start));

    Node final = start;
    final.position = { 0, 1 };
    final.id = *make_stable_node_id(final.floor, final.position);
    final.type = NodeType::Final;
    final.name = "险路尽头";
    final.traversal = default_traversal_for(final.type);
    REQUIRE(map.upsert_node(final));
    REQUIRE(map.upsert_edge({ start.id, final.id, EdgeKnowledge::Confirmed, {} }));

    Node beyond = start;
    beyond.position = { 0, 2 };
    beyond.id = *make_stable_node_id(beyond.floor, beyond.position);
    beyond.type = NodeType::Incident;
    beyond.name = "路过后的节点";
    beyond.traversal = default_traversal_for(beyond.type);
    REQUIRE(map.upsert_node(beyond));
    REQUIRE(map.upsert_edge({ final.id, beyond.id, EdgeKnowledge::Confirmed, {} }));

    RunState run;
    run.floor = 1;
    run.current_node = start.id;
    run.resources.action_points = 3;
    const auto moves = enumerate_move_actions(map, run, GraphLayer::Confirmed);
    const auto finish = std::ranges::find_if(moves, [&](const MoveAction& move) {
        return move.candidate.target == final.id && move.candidate.terminal_on_completion;
    });
    const auto pass = std::ranges::find_if(moves, [&](const MoveAction& move) {
        return move.candidate.target == final.id && move.candidate.bypass_final_on_completion;
    });
    REQUIRE(finish != moves.end());
    REQUIRE(pass != moves.end());
    REQUIRE_FALSE(pass->candidate.terminal_on_completion);
    REQUIRE(finish->candidate.action_id != pass->candidate.action_id);

    std::string projection_error;
    const auto passed = project_move_outcome(
        map,
        run,
        pass->candidate,
        pass->candidate.predicted_action_point_cost,
        &projection_error);
    REQUIRE(passed.has_value());
    REQUIRE_FALSE(passed->run.node_progress.contains(final.id));
    const auto onward = enumerate_move_actions(map, passed->run, GraphLayer::Confirmed);
    const auto leave_final = std::ranges::find_if(onward, [&](const MoveAction& move) {
        return move.candidate.target == beyond.id;
    });
    REQUIRE(leave_final != onward.end());

    const auto after_leaving = project_move_outcome(
        map,
        passed->run,
        leave_final->candidate,
        leave_final->candidate.predicted_action_point_cost,
        &projection_error);
    REQUIRE(after_leaving.has_value());
    const auto return_moves = enumerate_move_actions(map, after_leaving->run, GraphLayer::Confirmed);
    REQUIRE(std::ranges::any_of(return_moves, [&](const MoveAction& move) {
        return move.candidate.target == final.id && move.candidate.terminal_on_completion;
    }));
}

TEST_CASE("BlackFlow final pass intent dispatches through the final encounter page")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto execution = json::open(repository_root / "resource/roguelike/BlackFlow/node_execution.json");
    REQUIRE(execution.has_value());

    bool pass_route_found = false;
    for (const auto& route : execution->at("routes").as_array()) {
        if (route.get("page_intent", std::string {}) != "final.pass") {
            continue;
        }
        pass_route_found = true;
        REQUIRE(route.get("node_types", std::vector<std::string> {}) == std::vector<std::string> { "final" });
        REQUIRE(route.get("task", std::string {}) == "BlackFlow@Roguelike@Page-FinalEncounter");
        REQUIRE(route.get("completion_task", std::string {}) ==
                "BlackFlow@Roguelike@LegacyCompletion-NodePage");
    }
    REQUIRE(pass_route_found);
}

TEST_CASE("BlackFlow reveal consistency does not infer linked effects without the event title")
{
    MapSnapshot before;
    RunState run;
    run.floor = 2;
    const auto add_unknown = [&](GridPosition position) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = NodeType::Unknown;
        node.identity_state = NodeIdentityState::Hidden;
        node.identity_revealed = false;
        node.traversal = default_traversal_for(node.type);
        const NodeId id = node.id;
        REQUIRE(before.upsert_node(std::move(node)));
        return id;
    };
    const NodeId employ = add_unknown({ 3, 1 });
    const NodeId unrelated = add_unknown({ 3, 2 });

    MapSnapshot after = before;
    for (const auto [id, type] : { std::pair { employ, NodeType::Employ },
                                  std::pair { unrelated, NodeType::Incident } }) {
        Node revealed = *after.find_node(id);
        revealed.type = type;
        revealed.identity_state = NodeIdentityState::Classified;
        revealed.identity_revealed = true;
        revealed.traversal = default_traversal_for(type);
        REQUIRE(after.upsert_node(std::move(revealed)));
    }

    std::unordered_set<NodeId> event_expected;
    add_observed_event_reveal_expectations(before, run, after, {}, true, event_expected);
    REQUIRE(event_expected.empty());

    std::unordered_set<NodeId> non_event_expected;
    add_observed_event_reveal_expectations(before, run, after, {}, false, non_event_expected);
    REQUIRE(non_event_expected.empty());
}

TEST_CASE("BlackFlow reveal consistency ignores topology-only empty fallback under the HUD")
{
    MapSnapshot before;
    RunState run;
    run.floor = 4;

    Node hidden;
    hidden.floor = run.floor;
    hidden.position = { 0, 7 };
    hidden.id = *make_stable_node_id(hidden.floor, hidden.position);
    hidden.type = NodeType::HideBattle;
    hidden.identity_state = NodeIdentityState::Hidden;
    hidden.identity_revealed = false;
    hidden.traversal = default_traversal_for(hidden.type);
    REQUIRE(before.upsert_node(hidden));

    MapSnapshot after = before;
    Node weak_empty = hidden;
    weak_empty.type = NodeType::Empty;
    weak_empty.identity_state = NodeIdentityState::Classified;
    weak_empty.identity_revealed = true;
    weak_empty.identity_source = "map_topology_no_ocr_empty";
    weak_empty.detected_by_vision = true;
    weak_empty.traversal = default_traversal_for(weak_empty.type);
    REQUIRE(after.upsert_node(weak_empty));

    REQUIRE_FALSE(observed_move_reveals(before, run, after).contains(hidden.id));

    Node visual_empty = weak_empty;
    visual_empty.identity_source = "empty_template";
    REQUIRE(after.upsert_node(visual_empty));
    REQUIRE(observed_move_reveals(before, run, after).contains(hidden.id));
}

TEST_CASE("BlackFlow reveal consistency retries a transient topology-only empty expected node")
{
    REQUIRE(TransientRevealObservationMaximumAttempts >= 3);

    MapSnapshot after;
    Node transient;
    transient.floor = 1;
    transient.position = { 0, 1 };
    transient.id = *make_stable_node_id(transient.floor, transient.position);
    transient.type = NodeType::Empty;
    transient.name = std::string(EmptyNodeName);
    transient.identity_revealed = true;
    transient.identity_source = "map_topology_no_ocr_empty";
    REQUIRE(after.upsert_node(transient));

    REQUIRE(should_retry_transient_reveal_observation({ transient.id }, {}, after));
    REQUIRE_FALSE(should_retry_transient_reveal_observation({}, {}, after));

    transient.type = NodeType::BattleNormal;
    transient.name = "作战";
    transient.identity_source = "ocr";
    REQUIRE(after.upsert_node(transient));
    REQUIRE_FALSE(should_retry_transient_reveal_observation({ transient.id }, { transient.id }, after));
}

TEST_CASE("BlackFlow initial reveal retries OCR-empty fallback without changing empty semantics")
{
    MapSnapshot map;
    const int floor = 3;
    const auto add_node = [&](GridPosition position, NodeType type, bool revealed, std::string source) {
        Node node;
        node.floor = floor;
        node.position = position;
        node.id = *make_stable_node_id(floor, position);
        node.type = type;
        node.name = type == NodeType::Empty ? std::string(EmptyNodeName) : "未知的诡秘";
        node.identity_state = revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        node.identity_revealed = revealed;
        node.identity_source = std::move(source);
        node.traversal = default_traversal_for(type);
        REQUIRE(map.upsert_node(std::move(node)));
        return *make_stable_node_id(floor, position);
    };

    const NodeId entrance = add_node({ 0, 0 }, NodeType::Empty, true, "current_marker_empty");
    const NodeId weak_empty =
        add_node({ 0, 1 }, NodeType::Empty, true, "map_topology_no_ocr_empty");
    const NodeId hidden = add_node({ 0, 2 }, NodeType::HideInvisible, false, "ocr");
    REQUIRE(map.upsert_edge({ entrance, weak_empty, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ weak_empty, hidden, EdgeKnowledge::Confirmed, {} }));

    REQUIRE(initial_floor_reveal_observation_needs_ocr_retry(map, entrance, false));

    Node confirmed_empty = *map.find_node(weak_empty);
    confirmed_empty.identity_source = "empty_template";
    REQUIRE(map.upsert_node(std::move(confirmed_empty)));
    REQUIRE_FALSE(initial_floor_reveal_observation_needs_ocr_retry(map, entrance, false));
}

TEST_CASE("BlackFlow topology-only empty fallback does not contradict its deterministic identity")
{
    Node predicted;
    predicted.type = NodeType::BattleSavage;
    predicted.identity_from_prediction = true;
    predicted.prediction_rule = "initial_roaming_resident_settlement";

    ObservedNode observed;
    observed.type = NodeType::Empty;
    observed.identity_revealed = true;
    observed.identity_source = "map_topology_no_ocr_empty";

    REQUIRE_FALSE(deterministic_prediction_conflicts_with_observation(predicted, observed, true));
    REQUIRE_FALSE(deterministic_prediction_conflicts_with_observation(predicted, observed, false));

    observed.identity_source = "empty_template";
    REQUIRE(deterministic_prediction_conflicts_with_observation(predicted, observed, false));
    predicted.progress = NodeProgress::Completed;
    REQUIRE_FALSE(deterministic_prediction_conflicts_with_observation(predicted, observed, false));
}

TEST_CASE("BlackFlow reveal consistency reports missing and unexpected nodes")
{
    const RevealConsistencyResult result = compare_move_reveals({ 1, 2, 3 }, { 1, 3, 4 });
    REQUIRE_FALSE(result.consistent());
    REQUIRE(result.missing == std::vector<NodeId> { 2 });
    REQUIRE(result.unexpected == std::vector<NodeId> { 4 });
}

TEST_CASE("BlackFlow diffuse mist suppresses connected nodes inside the ideal domain")
{
    MapSnapshot map;
    RunState run;
    run.floor = 3;
    const auto add_node = [&](GridPosition position, NodeType type, bool revealed, bool suppressed) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.identity_state = revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        node.identity_revealed = revealed;
        node.natural_reveal_suppressed = suppressed;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };

    const NodeId landing = add_node({ 0, 0 }, NodeType::Empty, true, false);
    const NodeId mist_hidden = add_node({ 0, 1 }, NodeType::HideInvisible, false, true);
    const NodeId normal_hidden = add_node({ 1, 0 }, NodeType::HideBattle, false, false);
    const NodeId revealed_mist_empty = add_node({ 1, 1 }, NodeType::Empty, true, true);
    const NodeId behind_revealed_mist = add_node({ 1, 2 }, NodeType::HideInvisible, false, false);
    REQUIRE(map.upsert_edge({ landing, mist_hidden, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ landing, normal_hidden, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ landing, revealed_mist_empty, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ revealed_mist_empty, behind_revealed_mist, EdgeKnowledge::Confirmed, {} }));

    MoveCandidate move;
    move.movement = MovementKind::M08;
    move.source = normal_hidden;
    move.target = landing;
    move.landing = landing;
    move.controllable = true;

    const auto from_landing = expected_move_reveals(map, run, move, landing, true);
    REQUIRE_FALSE(from_landing.contains(mist_hidden));
    REQUIRE(from_landing.contains(normal_hidden));
    REQUIRE(from_landing.contains(behind_revealed_mist));

    move.target = mist_hidden;
    move.landing = mist_hidden;
    const auto direct = expected_move_reveals(map, run, move, mist_hidden, true);
    REQUIRE(direct.contains(mist_hidden));
}

TEST_CASE("BlackFlow reveal consistency keeps the entered light identity after it becomes empty")
{
    MapSnapshot map;
    RunState run;
    run.floor = 3;
    const auto add_node = [&](GridPosition position, NodeType type, bool revealed) {
        Node node;
        node.floor = run.floor;
        node.position = position;
        node.id = *make_stable_node_id(node.floor, node.position);
        node.type = type;
        node.identity_state = revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        node.identity_revealed = revealed;
        node.traversal = default_traversal_for(type);
        const NodeId id = node.id;
        REQUIRE(map.upsert_node(std::move(node)));
        return id;
    };

    // 回图自洽检查发生时，当前观测地图已把刚结算的羽瞰点显示为林间空地。
    const NodeId landing = add_node({ 4, 0 }, NodeType::Empty, true);
    const NodeId radius_three = add_node({ 3, 2 }, NodeType::HideInvisible, false);
    MoveCandidate move;
    move.movement = MovementKind::M11;
    move.landing = landing;

    const auto without_page_identity = expected_move_reveals(map, run, move, landing, true);
    REQUIRE_FALSE(without_page_identity.contains(radius_three));
    const auto with_page_identity =
        expected_move_reveals(map, run, move, landing, true, {}, NodeType::Light);
    REQUIRE(with_page_identity.contains(radius_three));
}

TEST_CASE("BlackFlow light actively reveals nodes inside diffuse mist")
{
    MapSnapshot map;
    RunState run;
    run.floor = 3;

    Node light;
    light.floor = run.floor;
    light.position = { 3, 4 };
    light.id = *make_stable_node_id(light.floor, light.position);
    light.type = NodeType::Light;
    light.identity_revealed = true;
    light.traversal = default_traversal_for(light.type);
    REQUIRE(map.upsert_node(light));

    Node mist_hidden;
    mist_hidden.floor = run.floor;
    mist_hidden.position = { 2, 2 };
    mist_hidden.id = *make_stable_node_id(mist_hidden.floor, mist_hidden.position);
    mist_hidden.type = NodeType::HideBattle;
    mist_hidden.identity_state = NodeIdentityState::Hidden;
    mist_hidden.identity_revealed = false;
    mist_hidden.natural_reveal_suppressed = true;
    mist_hidden.traversal = default_traversal_for(mist_hidden.type);
    REQUIRE(map.upsert_node(mist_hidden));

    MoveCandidate move;
    move.movement = MovementKind::Walk;
    move.landing = light.id;
    const auto revealed = expected_move_reveals(map, run, move, light.id, true);
    REQUIRE(revealed.contains(mist_hidden.id));
}

TEST_CASE("BlackFlow deterministic emergency prediction does not impersonate visual reveal")
{
    BlackFlowMapObservation observation;
    observation.floor = 3;
    observation.recognition_ok = true;
    observation.graph_connected = true;
    observation.current_marker_temporary_id = 1;

    PerceptionNodeObservation predicted;
    predicted.temporary_id = 1;
    predicted.position = { 0, 5 };
    predicted.exists = true;
    predicted.type = "battle_elite";
    predicted.displayed_name = "紧急作战";
    predicted.visually_hidden = true;
    predicted.identity_from_prediction = true;
    predicted.identity_source = "ideal_source_emergency_prediction";
    predicted.prediction_rule = "non_hopeful_ideal_source_is_emergency_battle";
    observation.nodes.emplace_back(predicted);

    BlackFlowObservationAdapter adapter;
    std::string error;
    const auto normalized = adapter.normalize(observation, &error);
    INFO(error);
    REQUIRE(normalized.has_value());
    REQUIRE(normalized->map.nodes.size() == 1);
    const ObservedNode& node = normalized->map.nodes.front();
    REQUIRE(node.type == NodeType::BattleElite);
    REQUIRE(node.identity_from_prediction == true);
    REQUIRE(node.identity_state == NodeIdentityState::Hidden);
    REQUIRE(node.identity_revealed == false);

    MapSnapshot map;
    Node semantic_prediction;
    semantic_prediction.id = *make_stable_node_id(3, predicted.position);
    semantic_prediction.floor = 3;
    semantic_prediction.position = predicted.position;
    semantic_prediction.type = NodeType::BattleElite;
    semantic_prediction.identity_revealed = false;
    semantic_prediction.identity_from_prediction = true;
    REQUIRE(map.upsert_node(std::move(semantic_prediction)));
    RunState run;
    run.floor = 3;
    REQUIRE_FALSE(initially_unknown_for_reveal(map, run, *make_stable_node_id(3, predicted.position)));
}

TEST_CASE("BlackFlow overlapping markers inherit roaming resident semantics when fewer than three are visible")
{
    BlackFlowMapObservation observation;
    observation.floor = 2;
    observation.recognition_ok = true;
    observation.graph_connected = true;
    observation.current_marker_temporary_id = 1;

    const auto add_node = [&](int id,
                              GridPosition position,
                              std::string type,
                              std::string displayed_name,
                              std::string marker,
                              std::string marker_name) {
        PerceptionNodeObservation node;
        node.temporary_id = id;
        node.position = position;
        node.exists = true;
        node.type = std::move(type);
        node.displayed_name = std::move(displayed_name);
        node.identity_source = "ocr";
        node.marker_type = std::move(marker);
        node.marker_display_name = std::move(marker_name);
        node.marker_score = 0.9;
        observation.nodes.emplace_back(std::move(node));
    };
    add_node(1, { 0, 0 }, "empty", "林间空地", "savage", "流窜居民");
    add_node(2, { 0, 1 }, "empty", "林间空地", "savage", "流窜居民");
    add_node(3, { 0, 2 }, "empty", "林间空地", "fruit_cache", "藏果地");
    add_node(4, { 0, 3 }, "empty", "林间空地", {}, {});
    add_node(5, { 0, 4 }, "incident", "不期而遇", "informant", "线人与线索");
    add_node(6, { 1, 0 }, "incident", "命运所指", "informant", "线人与线索");

    std::string error;
    const auto normalized = BlackFlowObservationAdapter {}.normalize(observation, &error);
    INFO(error);
    REQUIRE(normalized.has_value());
    REQUIRE(normalized->map.nodes.size() == 6);
    REQUIRE(normalized->map.nodes[2].marker_type == "fruit_cache");
    REQUIRE(normalized->map.nodes[2].marker_display_name == "藏果地");
    REQUIRE(normalized->map.nodes[2].marker_resident_overlap_possible == true);
    REQUIRE(normalized->map.nodes[3].marker_type.value_or("").empty());
    REQUIRE(normalized->map.nodes[4].marker_resident_overlap_possible == true);
    REQUIRE(normalized->map.nodes[4].name == "不期而遇");
    REQUIRE(normalized->map.nodes[4].marker_display_name == "线人与线索");
    REQUIRE(normalized->map.nodes[5].name == "命运所指");
    REQUIRE(normalized->map.nodes[5].fate_event == true);
    REQUIRE(normalized->map.nodes[5].marker_display_name == "谜题与谜底");
    REQUIRE(normalized->map.nodes[5].marker_resident_overlap_possible == true);

    observation.nodes[3].marker_type = "savage";
    observation.nodes[3].marker_display_name = "流窜居民";
    const auto complete_resident_set = BlackFlowObservationAdapter {}.normalize(observation, &error);
    INFO(error);
    REQUIRE(complete_resident_set.has_value());
    REQUIRE(complete_resident_set->map.nodes[2].marker_type == "fruit_cache");
    REQUIRE(complete_resident_set->map.nodes[2].marker_display_name == "藏果地");
    REQUIRE(complete_resident_set->map.nodes[2].marker_resident_overlap_possible == false);
    REQUIRE(complete_resident_set->map.nodes[4].marker_resident_overlap_possible == false);
    REQUIRE(complete_resident_set->map.nodes[5].marker_resident_overlap_possible == false);

    observation.nodes[0].marker_type.clear();
    observation.nodes[1].marker_type.clear();
    observation.nodes[3].marker_type.clear();
    const auto no_residents = BlackFlowObservationAdapter {}.normalize(observation, &error);
    REQUIRE(no_residents.has_value());
    REQUIRE(no_residents->map.nodes[4].marker_resident_overlap_possible == false);
}

TEST_CASE("BlackFlow battle preview on a revealed non-battle node confirms a roaming resident overlay")
{
    Node incident;
    incident.type = NodeType::Incident;
    incident.name = "不期而遇";
    incident.identity_revealed = true;
    incident.identity_source = "ocr";

    MovePreview preview;
    preview.displayed_type = NodeType::BattleNormal;
    preview.displayed_name = "作战";
    preview.identity_revealed = true;

    REQUIRE(preview_confirms_roaming_resident(incident, preview));
    incident.identity_source = "map_template_fixed_identity";
    REQUIRE_FALSE(preview_confirms_roaming_resident(incident, preview));
}

TEST_CASE("BlackFlow locks the resident settlement from unique initial resident evidence")
{
    MapObservationBatch map;
    map.floor = 2;
    const auto add_node = [&](GridPosition position, NodeType type, std::string marker = {}) {
        ObservedNode node;
        node.position = position;
        node.type = type;
        node.identity_revealed = type != NodeType::HideBattle;
        node.marker_type = std::move(marker);
        map.nodes.emplace_back(std::move(node));
    };
    const auto add_edge = [&](GridPosition first, GridPosition second) {
        map.edges.emplace_back(ObservedEdge { first, second, EdgeKnowledge::Confirmed, {} });
    };
    add_node({ 0, 0 }, NodeType::Empty);
    add_node({ 0, 1 }, NodeType::BattleNormal);
    add_node({ 0, 2 }, NodeType::Incident);
    add_node({ 0, 3 }, NodeType::Wish);
    add_node({ 0, 4 }, NodeType::HideBattle);
    add_node({ 1, 4 }, NodeType::Empty, "savage");
    add_edge({ 0, 0 }, { 0, 1 });
    add_edge({ 0, 1 }, { 0, 2 });
    add_edge({ 0, 2 }, { 0, 3 });
    add_edge({ 0, 3 }, { 0, 4 });
    add_edge({ 0, 4 }, { 1, 4 });

    const NodeId start = *make_stable_node_id(2, { 0, 0 });
    const ResidentSettlementPrediction prediction = predict_resident_settlement(map, start, false);
    REQUIRE(prediction.candidates == std::vector<GridPosition> { { 0, 4 } });
    REQUIRE(prediction.exact == GridPosition { 0, 4 });

    apply_exact_resident_settlement_prediction(map, prediction);
    const auto predicted = std::ranges::find(map.nodes, GridPosition { 0, 4 }, &ObservedNode::position);
    REQUIRE(predicted != map.nodes.end());
    REQUIRE(predicted->type == NodeType::BattleSavage);
    REQUIRE(predicted->identity_from_prediction == true);
    REQUIRE(predicted->identity_revealed == false);
    REQUIRE(predicted->visually_hidden == true);
    REQUIRE(predicted->prediction_rule == "initial_roaming_resident_settlement");
}

TEST_CASE("BlackFlow resident settlement distance bounds follow each floor rule")
{
    const auto candidates_at_distance = [](int floor, int candidate_distance) {
        MapObservationBatch map;
        map.floor = floor;
        for (int column = 0; column <= candidate_distance + 1; ++column) {
            ObservedNode node;
            node.position = { 0, column };
            node.type = column == 0 || column == candidate_distance + 1
                            ? NodeType::Empty
                            : (column == candidate_distance ? NodeType::HideBattle : NodeType::Incident);
            if (column == candidate_distance + 1) {
                node.marker_type = "savage";
            }
            map.nodes.emplace_back(std::move(node));
            if (column > 0) {
                map.edges.emplace_back(
                    ObservedEdge { { 0, column - 1 }, { 0, column }, EdgeKnowledge::Confirmed, {} });
            }
        }

        const NodeId start = *make_stable_node_id(floor, { 0, 0 });
        return predict_resident_settlement(map, start, false).candidates;
    };

    REQUIRE(candidates_at_distance(2, 7) == std::vector<GridPosition> { { 0, 7 } });
    REQUIRE(candidates_at_distance(2, 8).empty());
    REQUIRE(candidates_at_distance(4, 12) == std::vector<GridPosition> { { 0, 12 } });
    REQUIRE(candidates_at_distance(5, 12) == std::vector<GridPosition> { { 0, 12 } });
}

TEST_CASE("BlackFlow floor-two settlement keeps a distance-seven alternative uncertain")
{
    MapObservationBatch map;
    map.floor = 2;
    for (int column = 0; column <= 7; ++column) {
        ObservedNode node;
        node.position = { 0, column };
        node.type = column == 0
                        ? NodeType::Empty
                        : (column == 5 || column == 7 ? NodeType::HideBattle : NodeType::Incident);
        map.nodes.emplace_back(std::move(node));
        if (column > 0) {
            map.edges.emplace_back(
                ObservedEdge { { 0, column - 1 }, { 0, column }, EdgeKnowledge::Confirmed, {} });
        }
    }
    ObservedNode resident;
    resident.position = { 1, 5 };
    resident.type = NodeType::Empty;
    resident.marker_type = "savage";
    map.nodes.emplace_back(std::move(resident));
    map.edges.emplace_back(ObservedEdge { { 0, 5 }, { 1, 5 }, EdgeKnowledge::Confirmed, {} });

    const NodeId start = *make_stable_node_id(2, { 0, 0 });
    const ResidentSettlementPrediction prediction = predict_resident_settlement(map, start, false);
    REQUIRE(prediction.candidates == std::vector<GridPosition> { { 0, 5 }, { 0, 7 } });
    REQUIRE_FALSE(prediction.exact.has_value());
}

TEST_CASE("BlackFlow resident-settlement prediction never overwrites a later revealed identity")
{
    MapObservationBatch map;
    map.floor = 5;
    ObservedNode revealed;
    revealed.position = { 2, 4 };
    revealed.type = NodeType::BattleNormal;
    revealed.name = "作战";
    revealed.identity_revealed = true;
    revealed.identity_from_prediction = false;
    map.nodes.emplace_back(revealed);

    ResidentSettlementPrediction prediction;
    prediction.exact = GridPosition { 2, 4 };
    apply_exact_resident_settlement_prediction(map, prediction);

    REQUIRE(map.nodes.front().type == NodeType::BattleNormal);
    REQUIRE(map.nodes.front().identity_revealed == true);
    REQUIRE(map.nodes.front().identity_from_prediction == false);
}

TEST_CASE("BlackFlow rejects a resident-settlement prediction after authoritative contradiction")
{
    ResidentSettlementPrediction prediction;
    prediction.candidates = { { 3, 7 } };
    prediction.exact = GridPosition { 3, 7 };

    REQUIRE(reject_resident_settlement_prediction(prediction, { 3, 7 }));
    REQUIRE_FALSE(prediction.exact.has_value());

    MapObservationBatch next_observation;
    next_observation.floor = 4;
    ObservedNode still_hidden;
    still_hidden.position = { 3, 7 };
    still_hidden.type = NodeType::HideBattle;
    still_hidden.identity_revealed = false;
    next_observation.nodes.emplace_back(still_hidden);
    apply_exact_resident_settlement_prediction(next_observation, prediction);

    REQUIRE(next_observation.nodes.front().type == NodeType::HideBattle);
    REQUIRE_FALSE(next_observation.nodes.front().identity_from_prediction);
    REQUIRE_FALSE(reject_resident_settlement_prediction(prediction, { 3, 7 }));
}

TEST_CASE("BlackFlow resident settlement requires the same unique candidate in every overlap hypothesis")
{
    const std::vector<std::vector<GridPosition>> same {
        { { 3, 3 } },
        { { 3, 3 } },
    };
    const std::vector<std::vector<GridPosition>> split {
        { { 3, 3 } },
        { { 4, 4 } },
    };
    const std::vector<std::vector<GridPosition>> incomplete {
        { { 3, 3 } },
        {},
    };
    REQUIRE(resident_settlement_hypotheses_have_exact_consensus(same));
    REQUIRE_FALSE(resident_settlement_hypotheses_have_exact_consensus(split));
    REQUIRE_FALSE(resident_settlement_hypotheses_have_exact_consensus(incomplete));
}

TEST_CASE("BlackFlow two residents plus a fruit marker produce separate settlement hypotheses")
{
    MapObservationBatch map;
    map.floor = 5;
    const auto add_marker = [&](GridPosition position, std::string type, bool possible_overlap) {
        ObservedNode node;
        node.position = position;
        node.type = NodeType::Empty;
        node.marker_type = std::move(type);
        node.marker_resident_overlap_possible = possible_overlap;
        map.nodes.emplace_back(std::move(node));
    };
    add_marker({ 0, 1 }, "savage", false);
    add_marker({ 2, 2 }, "savage", false);
    add_marker({ 4, 3 }, "fruit_cache", true);

    const auto hypotheses = resident_marker_hypotheses(map);
    REQUIRE(hypotheses.size() == 2);
    REQUIRE(hypotheses[0] == std::vector<GridPosition> { { 0, 1 }, { 2, 2 } });
    REQUIRE(hypotheses[1] == std::vector<GridPosition> { { 0, 1 }, { 2, 2 }, { 4, 3 } });
}

TEST_CASE("BlackFlow floor-five fruit overlap keeps both historical settlement candidates uncertain")
{
    MapObservationBatch map;
    map.floor = 5;
    const auto add_node = [&](GridPosition position,
                              NodeType type,
                              std::string marker = {},
                              bool possible_overlap = false) {
        ObservedNode node;
        node.position = position;
        node.type = type;
        node.marker_type = std::move(marker);
        node.marker_resident_overlap_possible = possible_overlap;
        map.nodes.emplace_back(std::move(node));
    };
    const auto add_edge = [&](GridPosition first, GridPosition second) {
        map.edges.emplace_back(ObservedEdge { first, second, EdgeKnowledge::Confirmed, {} });
    };

    // Reduced topology from run-20260830-082400 floor 5. With only the two visible
    // residents, (3,3) is unique. If the fruit marker at (2,3) hides a resident,
    // (4,4) is unique instead, so neither position may be written back as certain.
    add_node({ 0, 2 }, NodeType::Empty);
    add_node({ 1, 2 }, NodeType::BattleNormal);
    add_node({ 2, 2 }, NodeType::HideInvisible);
    add_node({ 2, 3 }, NodeType::Empty, "fruit_cache", true);
    add_node({ 2, 4 }, NodeType::Empty);
    add_node({ 2, 5 }, NodeType::Empty, "savage");
    add_node({ 3, 5 }, NodeType::HideInvisible);
    add_node({ 3, 6 }, NodeType::Empty, "savage");
    add_node({ 4, 5 }, NodeType::HideInvisible);
    add_node({ 4, 4 }, NodeType::HideBattle);
    add_node({ 4, 3 }, NodeType::HideBattle);
    add_node({ 3, 3 }, NodeType::HideBattle);

    add_edge({ 0, 2 }, { 1, 2 });
    add_edge({ 1, 2 }, { 2, 2 });
    add_edge({ 2, 2 }, { 2, 3 });
    add_edge({ 2, 3 }, { 2, 4 });
    add_edge({ 2, 4 }, { 2, 5 });
    add_edge({ 2, 5 }, { 3, 5 });
    add_edge({ 3, 5 }, { 3, 6 });
    add_edge({ 3, 5 }, { 4, 5 });
    add_edge({ 4, 5 }, { 4, 4 });
    add_edge({ 4, 4 }, { 4, 3 });
    add_edge({ 4, 3 }, { 3, 3 });

    const NodeId start = *make_stable_node_id(5, { 0, 2 });
    const ResidentSettlementPrediction prediction = predict_resident_settlement(map, start, false);
    REQUIRE(prediction.hypothesis_count == 2);
    REQUIRE(prediction.initial_residents == std::vector<GridPosition> { { 2, 5 }, { 3, 6 } });
    REQUIRE(prediction.possible_overlap_residents == std::vector<GridPosition> { { 2, 3 } });
    REQUIRE(prediction.candidates == std::vector<GridPosition> { { 3, 3 }, { 4, 4 } });
    REQUIRE_FALSE(prediction.exact.has_value());
}

TEST_CASE("BlackFlow initial resident hypotheses ignore markers on non-empty nodes")
{
    MapObservationBatch map;
    map.floor = 5;
    const auto add_marker = [&](GridPosition position,
                                NodeType type,
                                std::string marker,
                                bool possible_overlap = false) {
        ObservedNode node;
        node.position = position;
        node.type = type;
        node.marker_type = std::move(marker);
        node.marker_resident_overlap_possible = possible_overlap;
        map.nodes.emplace_back(std::move(node));
    };
    add_marker({ 0, 0 }, NodeType::Empty, "savage");
    add_marker({ 0, 1 }, NodeType::Incident, "savage");
    add_marker({ 0, 2 }, NodeType::Shop, "fruit_cache", true);
    add_marker({ 0, 3 }, NodeType::Empty, "fruit_cache", true);

    const auto hypotheses = resident_marker_hypotheses(map);
    REQUIRE(hypotheses.size() == 2);
    REQUIRE(hypotheses[0] == std::vector<GridPosition> { { 0, 0 } });
    REQUIRE(hypotheses[1] == std::vector<GridPosition> { { 0, 0 }, { 0, 3 } });
}

TEST_CASE("BlackFlow Utopia effect expiration respects the Hopeful Soil exception")
{
    const std::optional<GridPosition> source = GridPosition { 2, 3 };
    REQUIRE(utopia_effect_expires_after_node_completion(true, "tilted-dune", source, { 2, 3 }));
    REQUIRE_FALSE(utopia_effect_expires_after_node_completion(true, "hopeful-soil", source, { 2, 3 }));
    REQUIRE_FALSE(utopia_effect_expires_after_node_completion(false, "tilted-dune", source, { 2, 3 }));
    REQUIRE_FALSE(utopia_effect_expires_after_node_completion(true, "tilted-dune", source, { 2, 2 }));
    REQUIRE_FALSE(utopia_effect_expires_after_node_completion(true, "tilted-dune", std::nullopt, { 2, 3 }));
}

TEST_CASE("BlackFlow floor four remembrance skips resident forest-count evidence")
{
    MapObservationBatch map;
    map.floor = 4;
    for (int column = 0; column <= 4; ++column) {
        ObservedNode node;
        node.position = { 0, column };
        node.type = column == 4 ? NodeType::HideBattle : NodeType::Incident;
        node.identity_revealed = column != 4;
        map.nodes.emplace_back(std::move(node));
        if (column > 0) {
            map.edges.emplace_back(
                ObservedEdge { { 0, column - 1 }, { 0, column }, EdgeKnowledge::Confirmed, {} });
        }
    }
    ObservedNode resident;
    resident.position = { 1, 4 };
    resident.type = NodeType::Empty;
    resident.identity_revealed = true;
    resident.marker_type = "savage";
    map.nodes.emplace_back(std::move(resident));
    map.edges.emplace_back(ObservedEdge { { 0, 4 }, { 1, 4 }, EdgeKnowledge::Confirmed, {} });
    ObservedNode extra_forest;
    extra_forest.position = { 1, 3 };
    extra_forest.type = NodeType::Empty;
    extra_forest.identity_revealed = true;
    map.nodes.emplace_back(std::move(extra_forest));
    map.edges.emplace_back(ObservedEdge { { 0, 4 }, { 1, 3 }, EdgeKnowledge::Confirmed, {} });

    const NodeId start = *make_stable_node_id(4, { 0, 0 });
    REQUIRE(predict_resident_settlement(map, start, false).candidates.empty());
    REQUIRE(predict_resident_settlement(map, start, true).exact == GridPosition { 0, 4 });
}

TEST_CASE("BlackFlow automation collection preserves processing items when revealed-node yield ties")
{
    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };

    PolicyCandidate battle_walk;
    battle_walk.move.action_id = "battle_walk";
    battle_walk.safe = true;
    battle_walk.revealed_node_count = 3;
    battle_walk.battle_count = 2;
    battle_walk.route_length = 8;
    battle_walk.risk_score = 10;

    PolicyCandidate peaceful_processing_move;
    peaceful_processing_move.move.action_id = "peaceful_processing_move";
    peaceful_processing_move.safe = true;
    peaceful_processing_move.revealed_node_count = 3;
    peaceful_processing_move.processing_move_count = 1;
    peaceful_processing_move.persistent_processing_move_count = 1;
    peaceful_processing_move.route_length = 1;

    const PolicyDecision decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { battle_walk, peaceful_processing_move });

    REQUIRE(decision.selected.has_value());
    REQUIRE(decision.selected->action_id == "battle_walk");
}

TEST_CASE("BlackFlow automation collection consumes floor-expiring processing items before route length")
{
    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };

    PolicyCandidate short_walk;
    short_walk.move.action_id = "short_walk";
    short_walk.safe = true;
    short_walk.revealed_node_count = 3;
    short_walk.effective_node_count = 2;
    short_walk.route_length = 1;

    PolicyCandidate expiring_processing_move = short_walk;
    expiring_processing_move.move.action_id = "expiring_processing_move";
    expiring_processing_move.processing_move_count = 1;
    expiring_processing_move.route_length = 8;

    const PolicyDecision decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { short_walk, expiring_processing_move });

    REQUIRE(decision.selected.has_value());
    REQUIRE(decision.selected->action_id == "expiring_processing_move");
}

TEST_CASE("BlackFlow automation collection uses semantic processing strength as final item tie break")
{
    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
        RoutePreference::IgnoreBattleTieBreaks,
        RoutePreference::OptimizeProcessingMoves,
    };

    PolicyCandidate use_strong_persistent;
    use_strong_persistent.move.action_id = "use_M08";
    use_strong_persistent.safe = true;
    use_strong_persistent.processing_move_count = 1;
    use_strong_persistent.persistent_processing_move_count = 1;
    use_strong_persistent.processing_move_counts[static_cast<std::size_t>(MovementKind::M08)] = 1;
    use_strong_persistent.route_length = 1;

    PolicyCandidate use_weak_persistent = use_strong_persistent;
    use_weak_persistent.move.action_id = "use_M01";
    use_weak_persistent.processing_move_counts = {};
    use_weak_persistent.processing_move_counts[static_cast<std::size_t>(MovementKind::M01)] = 1;

    const PolicyDecision preserve_decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { use_strong_persistent, use_weak_persistent });
    REQUIRE(preserve_decision.selected.has_value());
    REQUIRE(preserve_decision.selected->action_id == "use_M01");

    PolicyCandidate use_strong_expiring = use_strong_persistent;
    use_strong_expiring.move.action_id = "use_M07";
    use_strong_expiring.persistent_processing_move_count = 0;
    use_strong_expiring.processing_move_counts = {};
    use_strong_expiring.processing_move_counts[static_cast<std::size_t>(MovementKind::M07)] = 1;

    PolicyCandidate use_weak_expiring = use_strong_expiring;
    use_weak_expiring.move.action_id = "use_M04";
    use_weak_expiring.processing_move_counts = {};
    use_weak_expiring.processing_move_counts[static_cast<std::size_t>(MovementKind::M04)] = 1;

    const PolicyDecision consume_decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { use_strong_expiring, use_weak_expiring });
    REQUIRE(consume_decision.selected.has_value());
    REQUIRE(consume_decision.selected->action_id == "use_M04");

    PolicyCandidate use_remote = use_strong_persistent;
    use_remote.move.action_id = "use_remote";
    use_remote.processing_move_counts = {};
    use_remote.processing_move_counts[static_cast<std::size_t>(MovementKind::M12)] = 1;

    PolicyCandidate use_mother = use_remote;
    use_mother.move.action_id = "use_mother";
    use_mother.processing_move_counts = {};
    use_mother.processing_move_counts[static_cast<std::size_t>(MovementKind::M09)] = 1;

    const PolicyDecision remote_mother_decision = PolicyExecutor {}.choose(
        policy,
        FactStore {},
        MissionState {},
        RunState {},
        ResourceRegistry {},
        {},
        { use_remote, use_mother });
    REQUIRE(remote_mother_decision.selected.has_value());
    REQUIRE(remote_mother_decision.selected->action_id == "use_mother");
}

TEST_CASE("BlackFlow automation shop resume binding expires after the actual leave callback")
{
    REQUIRE(automation_shop_resume_binding_expires_on("AutomationShopLeave-Enter"));
    REQUIRE(automation_shop_resume_binding_expires_on("BlackFlow@Roguelike@AutomationShopLeave-Enter"));
    REQUIRE(automation_shop_resume_binding_expires_on("StageTraderLeaveConfirmCompleted"));
    REQUIRE(automation_shop_resume_binding_expires_on("BlackFlow@Roguelike@StageTraderLeaveConfirmCompleted"));
    REQUIRE_FALSE(automation_shop_resume_binding_expires_on("RecruitSkip"));
    REQUIRE_FALSE(automation_shop_resume_binding_expires_on("StageTraderLeave"));
}

TEST_CASE("BlackFlow shop recruitment resumes only after the store page is visible")
{
    REQUIRE(automation_shop_resume_base_task() == "BlackFlow@Roguelike@AutomationShopDecision-Enter");
    REQUIRE(automation_shop_resume_fallback_base_task() == "BlackFlow@Roguelike@AutomationShopResumeGeneric");

    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& resume_ready = tasks->at("BlackFlow@Roguelike@AutomationShopResumeReady");
    REQUIRE(resume_ready.get("algorithm", std::string {}) != "JustReturn");
    REQUIRE(resume_ready.get("template", std::string {}) == "Roguelike@StageTraderLeave.png");
    REQUIRE(resume_ready.get("templThreshold", 1.0) <= 0.79426);
    REQUIRE(resume_ready.get("action", std::string {}) == "DoNothing");
    REQUIRE(resume_ready.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@AutomationShopResumeAction" });

    const auto& resume = tasks->at("BlackFlow@Roguelike@TraderShoppingResume");
    REQUIRE(resume.get("template", std::string {}) == "Roguelike@StageTraderLeave.png");
    REQUIRE(resume.get("templThreshold", 1.0) <= 0.79426);
    REQUIRE(resume.get("action", std::string {}) == "DoNothing");
    REQUIRE(resume.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@AutomationShopResumeAction" });

    const auto& fallback = tasks->at("BlackFlow@Roguelike@AutomationShopResumeGeneric");
    REQUIRE(fallback.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(fallback.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@TraderRandomShopping" });
}

TEST_CASE("BlackFlow cultivation retries a start click swallowed by the shop dialogue")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string start_name = "BlackFlow@Roguelike@AutomationCultivateStartButton";
    const auto& start = tasks->at(start_name);
    const auto successors = start.get("next", std::vector<std::string> {});
    REQUIRE(successors == std::vector<std::string> {
                              "BlackFlow@Roguelike@AutomationCultivateAtMost",
                              "#self",
                          });
    REQUIRE(start.get("maxTimes", 0) == 3);
    REQUIRE(start.get("postDelay", 0) >= 600);
    REQUIRE(start.get("exceededNext", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@Roguelike@AutomationCultivateLeave-Enter" });

    const auto& modal = tasks->at("BlackFlow@Roguelike@AutomationCultivateAtMost");
    const auto reset_tasks = modal.get("reduceOtherTimes", std::vector<std::string> {});
    REQUIRE(std::ranges::find(reset_tasks, start_name + "*3") != reset_tasks.end());

    const auto start_roi = tasks->at("BlackFlow@Roguelike@CultivateStartButton").get(
        "roi",
        std::vector<int> {});
    REQUIRE(start_roi.size() == 4);
    // 数量弹窗顶部也写着“开始培育”；外层按钮 ROI 不得覆盖弹窗标题。
    REQUIRE(start_roi[1] > 200);
    REQUIRE(start_roi[0] + start_roi[2] < 700);
}

TEST_CASE("BlackFlow cultivation and door animations poll and click the shared skip label")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto& skip = tasks->at("BlackFlow@Roguelike@AnimationSkip");
    REQUIRE(skip.get("algorithm", std::string {}) == "OcrDetect");
    REQUIRE(skip.get("roi", std::vector<int> {}) == std::vector<int> { 1138, 32, 80, 32 });
    REQUIRE(skip.get("text", std::vector<std::string> {}) == std::vector<std::string> { "SKIP" });
    REQUIRE(skip.get("action", std::string {}) == "ClickSelf");

    const auto require_animation_poll = [&tasks](const std::string& wait_task, const std::string& skip_task) {
        const auto& wait = tasks->at(wait_task);
        const auto successors = wait.get("next", std::vector<std::string> {});
        REQUIRE(std::ranges::find(successors, skip_task) != successors.end());
        REQUIRE(wait.get("postDelay", 1000) <= 250);
    };

    require_animation_poll(
        "BlackFlow@Roguelike@CultivateConfirmCompleted",
        "BlackFlow@Roguelike@CultivateAnimationSkip");
    require_animation_poll(
        "BlackFlow@Roguelike@AutomationCultivateConfirmCompleted",
        "BlackFlow@Roguelike@AutomationCultivateAnimationSkip");
    const int floor = 3;
    MapSnapshot map;
    Node source;
    source.floor = floor;
    source.position = { 0, 0 };
    source.id = *make_stable_node_id(floor, source.position);
    source.type = NodeType::Empty;
    source.traversal = default_traversal_for(source.type);
    REQUIRE(map.upsert_node(source));

    Node door = source;
    door.position = { 0, 1 };
    door.id = *make_stable_node_id(floor, door.position);
    door.type = NodeType::Door;
    door.name = "曲折密道";
    door.traversal = default_traversal_for(door.type);
    REQUIRE(map.upsert_node(door));

    ViewportObservation viewport;
    viewport.replace(
        { NodeObservation { door.id, asst::Rect { 500, 200, 60, 60 }, std::nullopt, 1.0, 0.0 } },
        map.revision,
        1);
    MoveCandidate move;
    move.action_id = "walk-through-door";
    move.source = source.id;
    move.target = door.id;
    move.landing = door.id;
    move.path = { door.id };
    move.movement = MovementKind::Walk;
    move.predicted_action_point_cost = 1;
    move.possible_landings = { door.id };
    move.landing_node_types.emplace(door.id, NodeType::Door);
    move.landing_action_point_gains.emplace(door.id, 0);
    move.controllable = true;
    auto transaction = MoveTransaction::propose(move, map, viewport);
    REQUIRE(transaction.has_value());
    REQUIRE(transaction->record_preview(
        MovePreview { PreviewReachability::Reachable, 1, NodeType::Door, "曲折密道", true }));
    REQUIRE(move_confirmation_requires_door_animation_wait(*transaction));

    require_animation_poll(
        "BlackFlow@Roguelike@MoveConfirmDoorAnimationWait",
        "BlackFlow@Roguelike@MoveConfirmDoorAnimationSkip");
    const auto& map_visible = tasks->at("BlackFlow@Roguelike@MoveConfirmDoorMapVisible");
    REQUIRE(map_visible.get("next", std::vector<std::string> {}) == std::vector<std::string> {});

    for (const std::string reset_task : {
             "BlackFlow@Roguelike@AbandonConfirm",
             "BlackFlow@Roguelike@Stages-MapReady",
         }) {
        const auto resets = tasks->at(reset_task).get("reduceOtherTimes", std::vector<std::string> {});
        REQUIRE(std::ranges::find(resets, "BlackFlow@Roguelike@Page-Door-Wait*50") != resets.end());
    }

    REQUIRE(tasks->at("BlackFlow@Roguelike@CultivateAnimationSkip").get("baseTask", std::string {}) ==
            "BlackFlow@Roguelike@AnimationSkip");
    REQUIRE(tasks->at("BlackFlow@Roguelike@AutomationCultivateAnimationSkip").get("baseTask", std::string {}) ==
            "BlackFlow@Roguelike@AnimationSkip");
    REQUIRE(tasks->at("BlackFlow@Roguelike@MoveConfirmDoorAnimationSkip").get("baseTask", std::string {}) ==
            "BlackFlow@Roguelike@AnimationSkip");
}

TEST_CASE("BlackFlow move confirmation closes entry reward detail overlays before node dispatch")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const auto successors = tasks->at("BlackFlow@Roguelike@MovePreviewConfirmCompleted")
                                .get("next", std::vector<std::string> {});
    const std::string continue_overlay =
        "BlackFlow@Roguelike@MovePreviewConfirmCompleted@(BlackFlow@Roguelike@CloseCollectionContinue)";
    const std::string close_overlay =
        "BlackFlow@Roguelike@MovePreviewConfirmCompleted@(BlackFlow@Roguelike@CloseCollection)";
    const std::string completed = "BlackFlow@Roguelike@MovePreviewConfirmCompletedDone";

    const auto continue_it = std::ranges::find(successors, continue_overlay);
    const auto close_it = std::ranges::find(successors, close_overlay);
    const auto completed_it = std::ranges::find(successors, completed);
    REQUIRE(continue_it != successors.end());
    REQUIRE(close_it != successors.end());
    REQUIRE(completed_it != successors.end());
    REQUIRE(continue_it < close_it);
    REQUIRE(close_it < completed_it);
}

TEST_CASE("BlackFlow recruitment transitions continue the reveal screen without clicking top-right")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    const auto base_tasks = json::open(repository_root / "resource/tasks/Roguelike/base.json");
    REQUIRE(tasks.has_value());
    REQUIRE(base_tasks.has_value());

    const auto& fallback = base_tasks->at("Roguelike@RecruitWithoutButton");
    REQUIRE(fallback.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(fallback.get("specificRect", std::vector<int> {}) == std::vector<int> { 1180, 15, 85, 60 });

    const std::string continuation = "BlackFlow@Roguelike@RecruitWithoutButton";
    const auto confirm_successors =
        tasks->at("BlackFlow@Roguelike@ChooseOperConfirm").get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(confirm_successors, continuation) == confirm_successors.end());

    const auto& recruit_skip = tasks->at("BlackFlow@Roguelike@RecruitSkip");
    const auto skip_successors = recruit_skip.get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(skip_successors, continuation) != skip_successors.end());
    REQUIRE(recruit_skip.get("maskRange", std::vector<int> {}) == std::vector<int> { 80, 255 });
    REQUIRE(recruit_skip.get("templThreshold", 1.0) <= 0.67);

    const auto safe_rect = tasks->at(continuation).get("specificRect", std::vector<int> {});
    REQUIRE(safe_rect == std::vector<int> { 550, 300, 180, 120 });
    REQUIRE(safe_rect[0] + safe_rect[2] < 1100);

    const std::string transition_wait = "BlackFlow@Roguelike@RecruitWithoutButtonTransitionWait";
    const auto continuation_successors =
        tasks->at(continuation).get("next", std::vector<std::string> {});
    const auto encounter_result =
        std::ranges::find(continuation_successors, "BlackFlow@Roguelike@CloseEventAfterEncounter");
    const auto wait_entry = std::ranges::find(continuation_successors, transition_wait);
    REQUIRE(encounter_result != continuation_successors.end());
    REQUIRE(wait_entry != continuation_successors.end());
    REQUIRE(encounter_result < wait_entry);
    const auto map_entry =
        std::ranges::find(continuation_successors, "BlackFlow@Roguelike@MapPrepare-FloorEnterZoom");
    REQUIRE(map_entry != continuation_successors.end());
    REQUIRE(map_entry < wait_entry);

    const auto& passive_wait = tasks->at(transition_wait);
    REQUIRE(passive_wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(passive_wait.get("action", std::string {}) == "DoNothing");
    REQUIRE(passive_wait.get("postDelay", 0) >= 300);
    const auto wait_successors = passive_wait.get("next", std::vector<std::string> {});
    const auto wait_encounter_result =
        std::ranges::find(wait_successors, "BlackFlow@Roguelike@CloseEventAfterEncounter");
    const auto wait_self = std::ranges::find(wait_successors, transition_wait);
    REQUIRE(wait_encounter_result != wait_successors.end());
    REQUIRE(wait_self != wait_successors.end());
    REQUIRE(wait_encounter_result < wait_self);
    const auto wait_map_entry =
        std::ranges::find(wait_successors, "BlackFlow@Roguelike@MapPrepare-FloorEnterZoom");
    REQUIRE(wait_map_entry != wait_successors.end());
    REQUIRE(wait_map_entry < wait_self);

    const auto& start_explore_continuation =
        tasks->at("BlackFlow@StartExplore@Roguelike@RecruitWithoutButton");
    REQUIRE(start_explore_continuation.get("baseTask", std::string {}) == continuation);
    REQUIRE(start_explore_continuation.get("specificRect", std::vector<int> {}) == safe_rect);
    const auto start_explore_successors =
        start_explore_continuation.get("next", std::vector<std::string> {});
    const auto start_explore_skip = std::ranges::find(
        start_explore_successors,
        "BlackFlow@StartExplore@Roguelike@RecruitSkip");
    const auto start_explore_self = std::ranges::find(
        start_explore_successors,
        "BlackFlow@StartExplore@Roguelike@RecruitWithoutButton");
    REQUIRE(start_explore_skip != start_explore_successors.end());
    REQUIRE(start_explore_self != start_explore_successors.end());
    REQUIRE(start_explore_skip < start_explore_self);
    const auto start_explore_exceeded =
        start_explore_continuation.get("exceededNext", std::vector<std::string> {});
    REQUIRE(std::ranges::find(
                start_explore_exceeded,
                "BlackFlow@StartExplore@Roguelike@RecruitSkip") != start_explore_exceeded.end());

    const auto& start_explore_recruit_skip =
        tasks->at("BlackFlow@StartExplore@Roguelike@RecruitSkip");
    REQUIRE(start_explore_recruit_skip.get("baseTask", std::string {}) ==
            "BlackFlow@Roguelike@RecruitSkip");
    REQUIRE(start_explore_recruit_skip.get("template", std::string {}) == "RecruitSkip.png");
    REQUIRE(start_explore_recruit_skip.get("maskRange", std::vector<int> {}) ==
            std::vector<int> { 80, 255 });
    REQUIRE(start_explore_recruit_skip.get("templThreshold", 1.0) <= 0.67);

    const std::string animation_wait = "BlackFlow@StartExplore@Roguelike@RecruitAnimationWait";
    const auto start_confirm_successors =
        tasks->at("BlackFlow@StartExplore@Roguelike@ChooseOperConfirm").get("next", std::vector<std::string> {});
    REQUIRE(start_confirm_successors == std::vector<std::string> {
                                            "BlackFlow@StartExplore@Roguelike@RecruitSkip",
                                            animation_wait,
                                        });

    const auto& wait = tasks->at(animation_wait);
    REQUIRE(wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(wait.get("action", std::string {}) == "DoNothing");
    REQUIRE(wait.get("postDelay", 0) <= 250);
    REQUIRE(wait.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@StartExplore@Roguelike@RecruitSkip",
                animation_wait,
            });
    REQUIRE(wait.get("exceededNext", std::vector<std::string> {}) ==
            std::vector<std::string> { "BlackFlow@StartExplore@Roguelike@RecruitWithoutButton" });

    const std::string voucher_ready = "BlackFlow@StartExplore@Roguelike@CoreCharVoucherReady";
    const auto& ready = tasks->at(voucher_ready);
    REQUIRE(ready.get("template", std::string {}) == "BlackFlow@Roguelike@Recruit.png");
    REQUIRE(ready.get("roi", std::vector<int> {}) == std::vector<int> { 80, 470, 1120, 154 });
    REQUIRE(ready.get("action", std::string {}) == "DoNothing");
    REQUIRE(ready.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> {
                "BlackFlow@StartExplore@Roguelike@ChooseOperFlag",
                "BlackFlow@StartExplore@Roguelike@RecruitOther",
            });

    const auto require_ready_before_recruit_other = [&](const std::vector<std::string>& successors) {
        const auto ready_it = std::ranges::find(successors, voucher_ready);
        const auto other_it = std::ranges::find(
            successors,
            "BlackFlow@StartExplore@Roguelike@RecruitOther");
        REQUIRE(ready_it != successors.end());
        REQUIRE(other_it != successors.end());
        REQUIRE(ready_it < other_it);
    };
    require_ready_before_recruit_other(start_explore_recruit_skip.get("next", std::vector<std::string> {}));
    require_ready_before_recruit_other(start_explore_successors);
}

TEST_CASE("BlackFlow drop page classification recovers from a transient recruitment transition")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string choose_oper = "BlackFlow@Roguelike@ChooseOperFlag";
    const std::string safe_failure = "BlackFlow@Roguelike@RecoveryFailed";

    const auto get_drop_successors =
        tasks->at("BlackFlow@Roguelike@GetDropConfirmed").get("next", std::vector<std::string> {});
    const auto choose_from_drop = std::ranges::find(get_drop_successors, choose_oper);
    const auto drop_flag =
        std::ranges::find(get_drop_successors, "BlackFlow@Roguelike@DropsFlag");
    REQUIRE(choose_from_drop != get_drop_successors.end());
    REQUIRE(drop_flag != get_drop_successors.end());
    REQUIRE(choose_from_drop < drop_flag);

    // The persistent bottom navigation icon can classify a transitioning recruit page as DropsFlag.
    // Re-probe the page-specific confirmation button first once the next frame has stabilized.
    const auto classified_successors =
        tasks->at("BlackFlow@Roguelike@DropsFlag_default").get("next", std::vector<std::string> {});
    REQUIRE_FALSE(classified_successors.empty());
    REQUIRE(classified_successors.front() == choose_oper);

    // Reward/recruit page recognition exhaustion must preserve the active run.
    const auto& classifier = tasks->at("BlackFlow@Roguelike@DropsFlag");
    REQUIRE(classifier.get("onErrorNext", std::vector<std::string> {}) ==
            std::vector<std::string> { safe_failure });
    REQUIRE(classifier.get("exceededNext", std::vector<std::string> {}) ==
            std::vector<std::string> { safe_failure });
}

TEST_CASE("BlackFlow transient UI failures wait one minute before retrying the failed action")
{
    const std::filesystem::path repository_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(repository_root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());

    const std::string failure = "BlackFlow@Roguelike@RecoveryFailed";
    const std::string map_recovery_failure = "BlackFlow@Roguelike@RecoverMapFailed";
    const std::string wait = "BlackFlow@Roguelike@RecoveryRetryWait";
    const std::string retry_action = "BlackFlow@Roguelike@RecoveryRetryAction";

    REQUIRE(tasks->at(failure).get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { wait });
    REQUIRE(tasks->at(map_recovery_failure).get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { wait });

    const auto& retry_wait = tasks->at(wait);
    REQUIRE(retry_wait.get("algorithm", std::string {}) == "JustReturn");
    REQUIRE(retry_wait.get("postDelay", 0) == 60'000);
    REQUIRE(retry_wait.get("next", std::vector<std::string> {}) ==
            std::vector<std::string> { retry_action });

    const auto& action = tasks->at(retry_action);
    REQUIRE(action.get("algorithm", std::string {}) == "JustReturn");
    const auto default_successors = action.get("next", std::vector<std::string> {});
    REQUIRE(std::ranges::find(
                default_successors,
                "BlackFlow@Roguelike@Begin") == default_successors.end());
}

TEST_CASE("BlackFlow future random pool changes after a landing reveals its only hidden target")
{
    MapSnapshot map;
    const auto add = [&](GridPosition position, NodeType type, bool hidden = false) {
        Node n;
        n.floor = 1;
        n.position = position;
        n.id = *make_stable_node_id(1, position);
        n.type = type;
        n.traversal = default_traversal_for(type);
        n.visually_hidden = hidden;
        n.identity_revealed = !hidden;
        REQUIRE(map.upsert_node(n));
        return n.id;
    };
    const auto source = add({ 1, 2 }, NodeType::Empty);
    const auto battle = add({ 2, 1 }, NodeType::BattleNormal);
    const auto clearing = add({ 2, 2 }, NodeType::Empty);
    const auto hidden = add({ 2, 3 }, NodeType::HideInvisible, true);
    const auto shop = add({ 1, 3 }, NodeType::Shop);
    const auto endpoint = add({ 1, 4 }, NodeType::Final);
    REQUIRE(map.upsert_edge({ source, battle, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ battle, clearing, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ clearing, hidden, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ hidden, shop, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ shop, endpoint, EdgeKnowledge::Confirmed, {} }));
    RunState run;
    run.floor = 1;
    run.current_node = source;
    run.resources.action_points = 3;
    run.resources.movement_charges = { { MovementKind::M03, 2 }, { MovementKind::M07, 1 } };
    for (const bool compact : { false, true }) {
        INFO(compact);
        StateExpansionOptions options;
        options.use_compact_actions = compact;
        OnDemandStateGraph graph;
        REQUIRE(graph.initialize(map, run, options));
        const auto* actions = graph.actions(graph.initial_state());
        REQUIRE(actions != nullptr);
        const auto first = std::ranges::find_if(*actions, [&](const OnDemandSafetyAction& a) {
            return a.candidate.movement == MovementKind::M03 && a.candidate.target == battle;
        });
        REQUIRE(first != actions->end());
        const auto after = first->outcomes.front().successor;
        const auto* next = graph.actions(after);
        REQUIRE(next != nullptr);
        const auto random = std::ranges::find_if(*next, [](const OnDemandSafetyAction& a) {
            return a.candidate.movement == MovementKind::M07 && !a.candidate.bypass_final_on_completion;
        });
        REQUIRE(random != next->end());
        // 落点经林间空地传递视野，唯一诡秘已点亮；小八界不能继续把它当作唯一落点。
        REQUIRE(random->candidate.possible_landings.size() > 1);
        REQUIRE(
            std::ranges::find(random->candidate.possible_landings, shop) != random->candidate.possible_landings.end());
    }
}

TEST_CASE("BlackFlow inventory detail opening waits for motion and confirms the popup")
{
    int tick = 0;
    int clicks = 0;
    int first_click_tick = -1;
    int last_click_tick = -1;
    int dropped_clicks = 0;
    int popup_delay = 1;
    SECTION("the card is still sliding after scroll reset")
    {
    }
    SECTION("a stable click is lost and must be retried")
    {
        dropped_clicks = 1;
    }
    SECTION("a slow popup must not receive a second click")
    {
        popup_delay = 4;
    }
    const std::array<int, 5> positions { 524, 620, 780, 872, 872 };
    const bool opened = open_inventory_part_detail(
        [&]() -> std::optional<asst::Rect> { return asst::Rect { positions[std::min(tick, 4)], 258, 87, 22 }; },
        [&](const asst::Rect&) {
            ++clicks;
            if (first_click_tick < 0) {
                first_click_tick = tick;
            }
            if (tick >= 4 && clicks > dropped_clicks) {
                last_click_tick = tick;
            }
            return true;
        },
        [&]() { return last_click_tick >= 0 && tick - last_click_tick >= popup_delay; },
        [&]() {
            ++tick;
            return true;
        });
    REQUIRE(opened);
    REQUIRE(first_click_tick >= 4);
    REQUIRE(clicks == dropped_clicks + 1);
}

TEST_CASE("BlackFlow inventory detail opening stops on missing UI or interruption")
{
    int clicks = 0;
    int waits = 0;
    bool interrupted = false;
    bool moving = false;
    SECTION("the detail never appears")
    {
    }
    SECTION("the list never stops moving")
    {
        moving = true;
    }
    SECTION("the task is interrupted")
    {
        interrupted = true;
    }
    REQUIRE_FALSE(open_inventory_part_detail(
        [&]() -> std::optional<asst::Rect> { return asst::Rect { 524 + (moving ? waits * 10 : 0), 258, 87, 22 }; },
        [&](const asst::Rect&) {
            ++clicks;
            return true;
        },
        []() { return false; },
        [&]() {
            ++waits;
            return !interrupted;
        }));
    REQUIRE(clicks == (moving || interrupted ? 0 : 3));
    REQUIRE(waits <= 24);
}
