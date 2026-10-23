// Exercise the production lifecycle callback and resource graph with injected return failures.
#include "Config/ResourceLoader.h"
#include "Config/TaskData.h"
#include "MaaUtils/ImageIo.h"
#include "Task/Roguelike/BlackFlow/BlackFlowLifecycleTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMovementRecognition.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTaskPluginBase.h"
#include "Task/Roguelike/RoguelikeDifficultySelectionTaskPlugin.h"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/OCRer.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace asst;
using namespace asst::blackflow;

struct DifficultyProbe : RoguelikeDifficultySelectionTaskPlugin
{
    using RoguelikeDifficultySelectionTaskPlugin::detect_blackflow_home_difficulty;
    using RoguelikeDifficultySelectionTaskPlugin::detect_blackflow_panel_difficulty;
    using RoguelikeDifficultySelectionTaskPlugin::RoguelikeDifficultySelectionTaskPlugin;
};

void require(bool ok, const std::string& message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

BlackFlowPerceptionSnapshot map_frame(int floor, int sequence, int current, std::string target = "empty")
{
    BlackFlowPerceptionSnapshot result;
    auto& o = result.observation;
    o.observation_id = "transition-replay-" + std::to_string(sequence);
    o.sequence = o.viewport_revision = sequence;
    o.floor = floor;
    o.floor_from_ocr = o.recognition_ok = o.graph_connected = true;
    o.current_marker_temporary_id = current;
    o.current_marker_score = 1;
    o.hud_action_points = floor == 3 ? 1 : 7;
    result.run.action_points = o.hud_action_points;
    result.run.active_movement = MovementKind::M01;
    result.run.movement_charges = std::unordered_map<MovementKind, int> { { MovementKind::M01, 1 } };
    for (int i = 0; i < 2; ++i) {
        PerceptionNodeObservation n;
        n.temporary_id = i;
        n.position = { 0, i };
        n.exists = n.detected_by_vision = true;
        n.confidence = 1;
        n.type = i ? target : "empty";
        n.icon_rect = { 400 + i * 120, 200, 60, 60 };
        o.nodes.push_back(n);
    }
    o.edges.push_back({ 0, 1, true, true, false, 1, "visual" });
    return result;
}

void commit_move(BlackFlowSession& s, int floor, bool random = false)
{
    std::string error;
    MoveCandidate m;
    m.action_id = "transition-last-move";
    m.source = *make_stable_node_id(floor, { 0, 0 });
    m.target = m.landing = *make_stable_node_id(floor, { 0, 1 });
    m.path = m.possible_landings = { m.target };
    m.landing_action_point_gains.emplace(m.target, 0);
    m.controllable = !random;
    if (random) {
        m.landing = InvalidNodeId;
    }
    m.movement = random ? MovementKind::M07 : MovementKind::M01;
    m.predicted_action_point_cost = 1;
    require(s.begin_transaction(m, &error), error);
    const auto preview = s.accept_preview({ PreviewReachability::Reachable, 1 }, &error);
    require(preview == PreviewDisposition::ReadyToCommit, "preview: " + error);
    EnteredPageObservation entered;
    entered.map_visible = s.map().snapshot().find_node(m.target)->type == NodeType::Empty;
    require(s.commit(entered, &error), error);
}

struct Port : IBlackFlowTaskPort
{
    bool succeeds = false;
    int attempts = 0;
    std::vector<RunLogEvent> events;
    std::vector<bool> captures;
    std::vector<std::shared_ptr<cv::Mat>> images;

    bool refresh(const BlackFlowObservationRequest&, BlackFlowPerceptionSnapshot&, std::string*) override
    {
        return false;
    }

    bool preview(const MoveCandidate&, const ViewportObservation&, MovePreview&, bool&, std::string*) override
    {
        return false;
    }

    bool inspect_battle(NodeId, const ViewportObservation&, BattleIntelPreview&, std::string*) override
    {
        return false;
    }

    MoveConfirmationStatus confirm(const MoveTransaction&, EnteredPageObservation&, std::string*) override
    {
        return { };
    }

    bool cleanup_open_inventory_if_overloaded(bool&, std::string*) override { return false; }

    bool cleanup_depart_inventory_overload(std::string*) override { return false; }

    bool resume_pending_tree_hole_return(int, std::string* error) override
    {
        ++attempts;
        if (error) {
            *error = "injected: return menu did not become ready";
        }
        return succeeds;
    }

    bool record_run_event(
        std::uint64_t,
        const RunLogEvent& event,
        std::shared_ptr<cv::Mat> image,
        bool capture,
        std::string*) override
    {
        events.push_back(event);
        captures.push_back(capture);
        images.push_back(std::move(image));
        return true;
    }
};

#include "BlackFlowDifficultyPanelReplay.h"
#include "BlackFlowDuelPreparationReplay.h"
#include "BlackFlowEmployReplay.h"
#include "BlackFlowEncounterEvidenceReplay.h"
#include "BlackFlowExpeditionEvidenceReplay.h"
#include "BlackFlowExpeditionPickerReplay.h"
#include "BlackFlowLinkedEncounterReplay.h"
#include "BlackFlowMerchantInventoryReplay.h"
#include "BlackFlowMovePreviewReplay.h"
#include "BlackFlowOpeningCultivationReplay.h"
#include "BlackFlowRevealReplay.h"
#include "BlackFlowStoreIdentityReplay.h"

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) {
        return 2;
    }
    require(ResourceLoader::get_instance().load(std::filesystem::path(argv[1]) / "resource"), "resources");
    int passed = 0, failed = 0;
    auto test = [&](const char* name, auto fn) {
        if (argc == 3 && std::string_view(name).find(argv[2]) == std::string_view::npos) { return; }
        try {
            fn();
            ++passed;
            std::cout << "PASS " << name << std::endl;
        }
        catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << name << ": " << e.what() << std::endl;
        }
    };

    struct DlcMovementImage
    {
        MovementKind movement;
        const char* filename;
        const char* task;
    };

    const std::array dlc_movement_images {
        DlcMovementImage { MovementKind::M13, "m13.png", "BlackFlow@Roguelike@MovementLoaded-M13" },
        DlcMovementImage { MovementKind::M14, "m14.png", "BlackFlow@Roguelike@MovementLoaded-M14" },
        DlcMovementImage { MovementKind::M15, "m15.png", "BlackFlow@Roguelike@MovementLoaded-M15" },
        DlcMovementImage { MovementKind::M16, "m16.png", "BlackFlow@Roguelike@MovementLoaded-M16" },
    };
    for (const auto& [movement, filename, task] : dlc_movement_images) {
        const std::string name = "captured DLC loaded movement " + std::string(to_string(movement));
        test(name.c_str(), [&] {
            const auto image = MAA_NS::imread(
                std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-dlc-movement" / filename);
            require(!image.empty(), "DLC movement screenshot missing");
            const auto recognized = recognize_loaded_movement_with_evidence(image);
            require(recognized.has_value() && recognized->movement == movement, "wrong loaded movement");
            for (const auto& [other, other_filename, other_task] : dlc_movement_images) {
                if (other == movement) {
                    continue;
                }
                Matcher matcher(image);
                matcher.set_task_info(other_task);
                require(!matcher.analyze().has_value(), "DLC movement icons cross-matched");
            }
            cv::Mat masked = image.clone();
            masked(cv::Rect(1162, 88, 94, 94)).setTo(cv::Scalar(0, 0, 0));
            require(!recognize_loaded_movement(masked).has_value(), "missing icon produced a loaded movement");
        });
    }
    auto config = std::make_shared<RoguelikeConfig>();
    require(config->verify_and_load_params(json::object { { "theme", "BlackFlow" }, { "mode", 30002 } }), "config");
    auto session = std::make_shared<BlackFlowSession>();
    auto port = std::make_shared<Port>();
    ProcessTask parent({ }, nullptr, "Roguelike");
    BlackFlowLifecycleTaskPlugin plugin({ }, nullptr, "Roguelike", config, nullptr, session, port);
    plugin.set_task_ptr(&parent);
    auto emit = [&](std::string task, AsstMsg message = AsstMsg::SubTaskStart, std::string previous = "") {
        require(
            plugin.verify(
                message,
                json::object { { "subtask", "ProcessTask" },
                               { "pre_task", previous },
                               { "details", json::object { { "task", task } } } }),
            "callback was not accepted");
        require(plugin.run(), "callback failed");
    };
    auto start = [&] {
        parent.set_enable(true);
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "initialize");
        require(session->set_current_floor(5), "outer floor");
        require(session->update(map_frame(5, 1, 0, "portal")), "outer map");
        commit_move(*session, 5);
        require(session->mark_page_running(), "portal page");
        session->record_portal_choice("木屑");
        NodeTaskResult result;
        result.kind = NodeTaskResultKind::PageCompleted;
        result.succeeded = true;
        require(session->apply_node_task_result(result, json::object { }), "portal completion");
        require(session->set_current_floor(6), "tree hole");
        emit("BlackFlow@Roguelike@TreeHoleLeaveConfirm", AsstMsg::SubTaskCompleted);
        port->attempts = 0;
        port->events.clear();
        port->captures.clear();
        port->succeeds = false;
    };
    test("tree-hole return stops after three failures and retains outer attribution", [&] {
        start();
        for (int i = 0; i < 3; ++i) {
            emit("BlackFlow@Roguelike@TreeHoleReturnResumeRetry");
        }
        require(session->terminated(), "return still retries indefinitely");
        require(session->result()->next_action == "stop_task", "unknown UI must not blindly abandon");
        require(session->in_tree_hole() && session->outer_floor() == 5, "failed return changed floor");
        require(port->attempts == 3, "wrong attempt count");
        emit("BlackFlow@Roguelike@StrategyTerminated");
        require(!parent.get_enable(), "terminal result did not stop the task");
    });
    test("return failures preserve the error and request an immediate frame", [&] {
        start();
        emit("BlackFlow@Roguelike@TreeHoleReturnResumeRetry");
        bool found = false;
        for (std::size_t i = 0; i < port->events.size(); ++i) {
            const auto& e = port->events[i];
            if (e.action != "recovery.tree_hole_return") {
                continue;
            }
            found =
                e.details.get("error", "").find("return menu") != std::string::npos && port->captures[i] &&
                run_log_image_capture_mode(e.action, e.phase, false, true) == RunLogImageCaptureMode::ImmediateSnapshot;
        }
        require(found, "failure evidence is missing or depends on a stable frame");
    });
    test("successful return resets the failure budget for the next tree hole", [&] {
        start();
        for (int i = 0; i < 2; ++i) {
            emit("BlackFlow@Roguelike@TreeHoleReturnResumeRetry");
        }
        port->succeeds = true;
        emit("BlackFlow@Roguelike@TreeHoleReturnResumeRetry");
        require(!session->in_tree_hole() && session->current_floor() == 5, "return did not restore outer floor");
        session->record_portal_choice("木屑");
        require(session->set_current_floor(6), "second tree hole");
        emit("BlackFlow@Roguelike@TreeHoleLeaveConfirm", AsstMsg::SubTaskCompleted);
        port->succeeds = false;
        emit("BlackFlow@Roguelike@TreeHoleReturnResumeRetry");
        require(!session->terminated(), "previous return failures leaked into new return");
    });
    test("post-event expanded inventory has a close route before node settlement", [&] {
        const auto image = MAA_NS::imread(std::filesystem::path(argv[1]) /
            "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/post-event-inventory.jpg");
        require(!image.empty() && movement_inventory_page_visible(image), "expanded inventory fixture missing");
        PipelineAnalyzer analyzer(image);
        analyzer.set_tasks(Task.get("BlackFlow@Roguelike@Stages")->next);
        const auto hit = analyzer.analyze();
        require(hit.has_value() && hit->task_ptr->name == "BlackFlow@Roguelike@MovementInventoryCloseAfterNode",
                "expanded inventory has no post-event recovery route");
        require(hit->task_ptr->action == ProcessTaskAction::ClickSelf, "inventory was not closed");
        const auto returned_map = MAA_NS::imread(std::filesystem::path(argv[1]) /
            "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/post-event-map.jpg");
        require(!returned_map.empty(), "returned map fixture missing");
        const auto close = Task.get("BlackFlow@Roguelike@CloseEventAfterEncounter");
        PipelineAnalyzer exit(returned_map); exit.set_tasks(close->next);
        const auto map_hit = exit.analyze();
        require(map_hit.has_value() && map_hit->task_ptr->name == "BlackFlow@Roguelike@Stages-MapReady" &&
                    map_hit->task_ptr->action == ProcessTaskAction::DoNothing,
                "returned map takes another event exit click");
        const auto observe = Task.get("BlackFlow@Roguelike@CloseEventAfterEncounterObserve");
        require(observe->action == ProcessTaskAction::DoNothing && observe->max_times == 4 &&
                    observe->reduce_other_times.empty(), "passive exit observation repeats clicks or resets itself");
        require(close->max_times == 3, "event exit clicks are unbounded");
        require(hit->task_ptr->next == std::vector<std::string> { "BlackFlow@Roguelike@MovementInventoryCloseAfterNode-Enter" },
                "inventory recovery bypasses node completion");

        require(Task.get("BlackFlow@Roguelike@Stages-MapReady")->next.front() ==
                    "BlackFlow@Roguelike@NodeCompletionAction", "inventory recovery bypasses node settlement");
        require(recovery_retry_task("BlackFlow@Roguelike@MovementInventoryCloseAfterNodeWait") ==
                    "BlackFlow@Roguelike@MovementInventoryCloseAfterNode-Enter", "recovery loses node settlement");
    });
    for (const char* file : {"walk-preview.jpg", "walk-preview-next.jpg", "processed-preview.png",
                             "legacy-preview.jpg", "battle-intel-preview.jpg", "zero-cost/charged-preview.jpg"}) {
        const auto name = std::string("inventory close resumes from actual node preview ") + file;
        test(name.c_str(), [&] {
            const auto preview = MAA_NS::imread(std::filesystem::path(argv[1]) /
                "unit_test/MaaCore/fixtures/blackflow-move-preview/20261009" / file);
            const auto map = MAA_NS::imread(std::filesystem::path(argv[1]) /
                "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/inventory-close-map.jpg");
            const auto expanded = MAA_NS::imread(std::filesystem::path(argv[1]) /
                "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/inventory-close-expanded.jpg");
            require(!preview.empty() && !map.empty() && !expanded.empty(), "return fixtures missing");
            // Real resource graph and image matching. Only the game response to
            // a dispatched click is supplied; this is not an original post-close recording.
            for (const bool after_node : {false, true}) {
                for (const int initial_state : {0, 1, 2}) { // map, preview, expanded box
                    for (const bool first_cancel_missed : {false, true}) {
                        std::string current = after_node ? "BlackFlow@Roguelike@MovementInventoryCloseAfterNode-Enter"
                                                         : "BlackFlow@Roguelike@MovementInventoryClose-Enter";
                        int state = initial_state, cancel_clicks = 0, collapse_clicks = 0;
                        bool reached_map = false;
                        for (int step = 0; step < 16; ++step) {
                            PipelineAnalyzer analyzer(state == 0 ? map : state == 1 ? preview : expanded);
                            analyzer.set_tasks(Task.get(current)->next);
                            const auto hit = analyzer.analyze();
                            require(hit.has_value(), "inventory return cannot classify an existing page");
                            const auto& task = hit->task_ptr;
                            require(task->name.find("RecoveryFailed") == std::string::npos &&
                                        task->name.find("RecoveryRetry") == std::string::npos,
                                    "recoverable preview waits a minute or stops the task");
                            if (task->action == ProcessTaskAction::ClickRect) {
                                require(state == 1 && task->specific_rect.x < 60 &&
                                            task->specific_rect.x + task->specific_rect.width < 100,
                                        "recovery clicked depart, inventory toggle or abandon");
                                if (++cancel_clicks > 1 || !first_cancel_missed) state = 0;
                            }
                            else if (task->action == ProcessTaskAction::ClickSelf) {
                                require(state == 2 && task->name.find("MovementInventoryClose") != std::string::npos,
                                        "recovery clicks after the box has closed");
                                ++collapse_clicks; state = 1;
                            }
                            if (task->name == "BlackFlow@Roguelike@MapPrepare-Ready" ||
                                task->name == "BlackFlow@Roguelike@Stages-MapReady") {
                                require(state == 0 && cancel_clicks == (initial_state == 0 ? 0 : first_cancel_missed ? 2 : 1) &&
                                            collapse_clicks == (initial_state == 2 ? 1 : 0),
                                        "recovery repeated a click after returning to the map");
                                require(task->next.front() == (after_node ? "BlackFlow@Roguelike@NodeCompletionAction"
                                                                         : "BlackFlow@Roguelike@Routing"),
                                        "recovery lost its original continuation");
                                reached_map = true; break;
                            }
                            current = task->name;
                        }
                        require(reached_map, "close-to-map loop only waits on node preview");
                    }
                }
            }
        });
    }
    test("after-node inventory return retains delayed collection popup handling", [&] {
        const auto image = MAA_NS::imread(std::filesystem::path(argv[1]) /
            "unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg");
        require(!image.empty(), "collection popup fixture missing");
        PipelineAnalyzer analyzer(image);
        analyzer.set_tasks(Task.get("BlackFlow@Roguelike@MovementInventoryCloseAfterNode-Enter")->next);
        const auto hit = analyzer.analyze();
        require(hit.has_value() && hit->task_ptr->name.find("CloseCollection") != std::string::npos &&
                    hit->task_ptr->action == ProcessTaskAction::ClickSelf,
                "inventory return discarded the existing delayed reward popup route");
    });
    test("inventory preview return invalidates the preserved viewport before resuming", [&] {
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "initialize");
        require(session->set_current_floor(2) && session->update(map_frame(2, 1, 0)), "map");
        session->mark_map_preserved_after_inventory();
        require(session->consume_map_preserved_after_inventory(), "preserved map baseline missing");
        const auto revision = session->map().snapshot().revision;
        for (const char* task : {"BlackFlow@Roguelike@MovementInventoryClosePreview",
                                 "BlackFlow@Roguelike@MovementInventoryCloseAfterNodePreview"}) {
            session->mark_map_preserved_after_inventory();
            emit(task);
            require(!session->consume_map_preserved_after_inventory(), "unexpected preview reused old map coordinates");
            require(session->map().snapshot().revision == revision && !session->terminated(),
                    "closing a preview changed map facts or terminated the run");
        }
    });
    test("inventory close recovery rechecks the page and stops after three failures", [&] {
        port->events.clear(); port->captures.clear(); port->images.clear();
        parent.set_enable(true);
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "initialize");
        for (int i = 0; i < 3; ++i) {
            emit("BlackFlow@Roguelike@RecoveryFailed", AsstMsg::SubTaskStart,
                 "BlackFlow@Roguelike@MovementInventoryCloseTransitionWait");
            if (i < 2) {
                require(Task.get("BlackFlow@Roguelike@RecoveryRetryAction")->next.front() ==
                            "BlackFlow@Roguelike@MovementInventoryClose-Enter", "recovery repeats a passive wait");
                require(!session->terminated(), "recovery stopped before its budget");
                // Successful passive wait callbacks must never refill the budget.
                (void)plugin.verify(AsstMsg::SubTaskCompleted, json::object {
                    { "subtask", "ProcessTask" }, { "details", json::object {
                        { "task", "BlackFlow@Roguelike@MovementInventoryCloseTransitionWait" } } } });
            }
        }
        require(session->terminated() && session->result()->outcome == "inventory_close_recovery_exhausted",
                "inventory close still recovers indefinitely");
        require(!parent.get_enable(), "exhausted recovery did not stop the task");
        require(port->events.back().action == "recovery.inventory_close" && port->captures.back(),
                "close failure screenshot was not requested");
        require(run_log_image_capture_mode("recovery.inventory_close", "failed", false, true) ==
                    RunLogImageCaptureMode::ImmediateSnapshot, "failure capture waits for a stable screen");
        json::array events;
        for (const auto& event : port->events) {
            if (event.action != "recovery.inventory_close") continue;
            events.emplace_back(json::object {
                { "schema_version", 1 }, { "level", std::string(to_string(event.level)) },
                { "action", event.action }, { "phase", event.phase }, { "outcome", event.outcome },
                { "task", event.task }, { "state", json::object { { "floor", 0 } } }, { "details", event.details },
            });
        }
        require(events.size() == 3, "failure diagnostic sequence is incomplete");
        std::ofstream("inventory-close-events.json") << json::value(events).format();
        const auto expected = json::open(std::filesystem::path(argv[1]) /
            "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/inventory-close-events.json");
        require(expected.has_value() && *expected == json::value(events), "shared recovery diagnostic sample drifted");

    });
    test("inventory close budget resets only after recognized map progress or a new run", [&] {
        const auto fail_close = [&](std::string task) {
            emit("BlackFlow@Roguelike@RecoveryFailed", AsstMsg::SubTaskStart, std::move(task));
        };
        parent.set_enable(true);
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "initialize");
        for (int cycle = 0; cycle < 2; ++cycle) {
            for (int i = 0; i < 2; ++i) fail_close("BlackFlow@Roguelike@MovementInventoryClose");
            require(!session->terminated(), "prior successful close consumed this episode budget");
            (void)plugin.verify(AsstMsg::SubTaskCompleted, json::object {
                { "subtask", "ProcessTask" }, { "details", json::object {
                    { "task", "BlackFlow@Roguelike@Stages-MapReady" } } } });
        }
        for (int i = 0; i < 2; ++i) fail_close("BlackFlow@Roguelike@MovementInventoryCloseAfterNode");
        require(!session->terminated(), "after-node close inherited old episode budget");
        require(Task.get("BlackFlow@Roguelike@RecoveryRetryAction")->next.front() ==
                    "BlackFlow@Roguelike@MovementInventoryCloseAfterNode-Enter", "after-node retry bypassed completion");
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "new run");
        fail_close("BlackFlow@Roguelike@MovementInventoryCloseTransitionWait");
        require(!session->terminated(), "new run inherited old failures");
    });
    test("refresh retries opening the dialog and has a finite failure route", [&] {
        const auto refresh = Task.get("BlackFlow@Roguelike@AutomationShopRefresh");
        require(refresh->next.size() == 2 && refresh->next.back() == refresh->name, "missed opening has no retry");
        require(
            refresh->max_times == 3 && !refresh->exceeded_next.empty() && !refresh->on_error_next.empty(),
            "opening is unbounded");
        const auto confirm = Task.get("BlackFlow@Roguelike@AutomationShopRefreshConfirm");
        require(
            std::ranges::find(confirm->next, confirm->name) == confirm->next.end(),
            "payment confirmation repeats blindly");
    });
    test("captured missed-refresh page reaches the bounded retry", [&] {
        const cv::Mat image = MAA_NS::imread(
            std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-recovery/shop-refresh-not-open.jpg");
        require(!image.empty(), "shop fixture is missing");
        const auto refresh = Task.get("BlackFlow@Roguelike@AutomationShopRefresh");
        PipelineAnalyzer analyzer(image);
        analyzer.set_tasks(refresh->next);
        const auto match = analyzer.analyze();
        require(match.has_value() && match->task_ptr->name == refresh->name, "captured shop still has no successor");
        Matcher shop(image);
        shop.set_task_info("BlackFlow@Roguelike@AutomationShopPurchaseSettleConfirmed");
        require(shop.analyze().has_value(), "captured shop cannot establish the payment baseline");
        OCRer wallet(image);
        wallet.set_task_info("BlackFlow@Roguelike@StageTraderInvest-Wallet");
        wallet.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);
        wallet.set_use_char_model(true);
        const auto amounts = wallet.analyze();
        require(
            amounts.has_value() && std::ranges::any_of(*amounts, [](const auto& row) { return row.text == "25"; }),
            "captured wallet is not read as 25");
    });
    test("captured 002 pending refresh is recognized as network loading", [&] {
        const auto image = MAA_NS::imread(
            std::filesystem::path(argv[1]) /
            "unit_test/MaaCore/fixtures/blackflow-recovery/shop-refresh-network-pending.jpg");
        require(!image.empty(), "pending refresh fixture is missing");
        OCRer loading(image);
        loading.set_task_info("LoadingText");
        require(loading.analyze().has_value(), "captured network wait was not recognized");
        Matcher shop(image);
        shop.set_task_info("BlackFlow@Roguelike@AutomationShopPurchaseSettleConfirmed");
        require(shop.analyze().has_value(), "fixture no longer reproduces a visible shop under network loading");
        OCRer wallet(image);
        wallet.set_task_info("BlackFlow@Roguelike@StageTraderInvest-Wallet");
        wallet.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);
        wallet.set_use_char_model(true);
        const auto amounts = wallet.analyze();
        require(
            amounts.has_value() && std::ranges::any_of(*amounts, [](const auto& row) { return row.text == "31"; }),
            "captured pending wallet is not read as 31");
    });
    test("captured ordinary shop does not trigger network loading wait", [&] {
        const auto image = MAA_NS::imread(
            std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-recovery/shop-refresh-not-open.jpg");
        require(!image.empty(), "shop fixture is missing");
        OCRer loading(image);
        loading.set_task_info("LoadingText");
        require(!loading.analyze().has_value(), "ordinary shelf was mistaken for network loading");
    });
    test("initial core failure is recorded as an incomplete run", [&] {
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "initialize");
        plugin.initial_core_recruitment_failed();
        require(
            session->terminated() && session->result()->next_action == "stop_task",
            "core failure was not terminal");
        require(session->result()->outcome == "initial_core_recruitment_failed", "incorrect result");
        require(port->events.back().action == "recovery.initial_core_recruitment", "failure log missing");
    });
    for (int difficulty : { 5, 6 }) {
        test(("captured home badge " + std::to_string(difficulty)).c_str(), [&] {
            const auto image = MAA_NS::imread(
                std::filesystem::path(argv[1]) /
                ("unit_test/MaaCore/fixtures/blackflow-difficulty/home-" + std::to_string(difficulty) + ".jpg"));
            require(!image.empty(), "difficulty screenshot missing");
            DifficultyProbe selection({ }, nullptr, "Roguelike", config, nullptr);
            require(selection.detect_blackflow_home_difficulty(image) == difficulty, "wrong applied difficulty");
        });
    }
    json::array difficulty_events;
    for (int observed : { 6, 5, -1 }) {
        test(("difficulty evidence preserves observed " + std::to_string(observed)).c_str(), [&] {
            require(
                plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }),
                "initialize");
            require(
                port->events.back().details.get("difficulty_verification", "") == "required",
                "missing declaration");
            const cv::Mat image(720, 1280, CV_8UC3, cv::Scalar(50, 60, 70));
            require(plugin.record_difficulty_verification(6, observed, observed == 6, image), "recording failed");
            const auto& event = port->events.back();
            require(event.details.get("target_difficulty", -1) == 6, "target missing");
            require(event.details.get("observed_difficulty", -1) == observed, "observed was replaced by target");
            require(session->terminated() == (observed != 6), "verification failure did not stop collection");
            require(port->images.back() && port->images.back()->data == image.data, "verification frame replaced");
            difficulty_events.emplace_back(
                json::object {
                    { "schema_version", 1 },
                    { "action", event.action },
                    { "phase", event.phase },
                    { "outcome", event.outcome },
                    { "task", event.task },
                    { "floor", 0 },
                    { "state", json::object { { "floor", 0 } } },
                    { "details", event.details },
                });
        });
    }
    std::ofstream("difficulty-events.json") << json::value(difficulty_events).format();
    test("shared difficulty contract matches production events", [&] {
        const auto expected =
            json::open(std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-difficulty/events.json");
        require(expected.has_value() && *expected == json::value(difficulty_events), "shared fixture drifted");
    });
    test("opening candidate pages cannot pass squad confirmation gate", [&] {
        const auto fixtures = std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-opening";
        for (int i = 1; i <= 3; ++i) {
            const auto image = MAA_NS::imread(fixtures / ("candidates-" + std::to_string(i) + ".jpg"));
            require(!image.empty(), "opening fixture missing");
            PipelineAnalyzer guarded(image);
            guarded.set_tasks({ "BlackFlow@Roguelike@SquadConfirm" });
            require(!guarded.analyze().has_value(), "reward page passed squad page guard");
            PipelineAnalyzer reward(image);
            reward.set_tasks({ "BlackFlow@Roguelike@LastReward-EnterPoint" });
            require(reward.analyze().has_value(), "reward page did not enter logged selection flow");
        }
    });
    merchant_inventory_regressions(std::filesystem::path(argv[1]), test);
    store_identity_regressions(std::filesystem::path(argv[1]), test);
    encounter_evidence_regressions(std::filesystem::path(argv[1]), test);
    move_preview_regressions(std::filesystem::path(argv[1]), test);
    employ_capture_regressions(std::filesystem::path(argv[1]), test);
    expedition_picker_regressions(std::filesystem::path(argv[1]), test);
    expedition_evidence_regressions(std::filesystem::path(argv[1]), test);
    opening_cultivation_regressions(std::filesystem::path(argv[1]), test);
    linked_encounter_regressions(std::filesystem::path(argv[1]), test);
    reveal_session_regressions(std::filesystem::path(argv[1]), test);
    duel_preparation_regressions(std::filesystem::path(argv[1]), test);
    difficulty_panel_regressions(std::filesystem::path(argv[1]), config, test);
    test("split title updates lifecycle attribution from floor two to three", [&] {
        require(plugin.load_params(json::object { { "blackflow_strategy", "automation_collection" } }), "initialize");
        require(session->set_current_floor(2), "previous floor");
        require(session->update(map_frame(2, 1, 0)), "previous map");
        const auto image = MAA_NS::imread(
            std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-transitions/floor-three-split.png");
        require(!image.empty(), "split title fixture missing");
        PipelineAnalyzer analyzer(image);
        analyzer.set_tasks({ "BlackFlow@Roguelike@NextLevel" });
        const auto result = analyzer.analyze();
        require(result.has_value(), "third-floor title not recognized");
        const auto& title = std::get<OCRer::Result>(result->result);
        require(title.text == "血色空脉", "wrong canonical floor name");
        require(
            plugin.verify(
                AsstMsg::SubTaskCompleted,
                json::object {
                    { "subtask", "ProcessTask" },
                    { "details",
                      json::object { { "task", result->task_ptr->name }, { "result", title.to_json() } } } }),
            "floor callback rejected");
        require(plugin.run(), "floor callback failed");
        require(session->current_floor() == 3, "recognized floor retained the previous floor");
        // The run/map snapshot advances when the next observation is merged, after title confirmation.
        require(session->update(map_frame(3, 2, 0)), "new-floor observation rejected");
        require(session->run().floor == 3, "map/log attribution retained the previous floor");
        require(!session->terminated(), "floor recognition terminated collection");
    });
    std::cout << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
