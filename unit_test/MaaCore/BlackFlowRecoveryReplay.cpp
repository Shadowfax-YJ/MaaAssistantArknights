// Exercise the production lifecycle callback and resource graph with injected return failures.
#include "Config/ResourceLoader.h"
#include "Config/TaskData.h"
#include "MaaUtils/ImageIo.h"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"
#include "Task/Roguelike/BlackFlow/BlackFlowLifecycleTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTaskPluginBase.h"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include "Task/Roguelike/RoguelikeDifficultySelectionTaskPlugin.h"

using namespace asst;
using namespace asst::blackflow;

struct DifficultyProbe : RoguelikeDifficultySelectionTaskPlugin
{
    using RoguelikeDifficultySelectionTaskPlugin::detect_blackflow_home_difficulty;
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

struct Port final : IBlackFlowTaskPort
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
        return {};
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

    bool record_run_event(std::uint64_t, const RunLogEvent& event, std::shared_ptr<cv::Mat> image, bool capture, std::string*)
        override
    {
        events.push_back(event);
        captures.push_back(capture);
        images.push_back(std::move(image));
        return true;
    }
};

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    require(ResourceLoader::get_instance().load(std::filesystem::path(argv[1]) / "resource"), "resources");
    int passed = 0, failed = 0;
    auto test = [&](const char* name, auto fn) {
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
    auto config = std::make_shared<RoguelikeConfig>();
    require(config->verify_and_load_params(json::object { { "theme", "BlackFlow" }, { "mode", 30002 } }), "config");
    auto session = std::make_shared<BlackFlowSession>();
    auto port = std::make_shared<Port>();
    ProcessTask parent({}, nullptr, "Roguelike");
    BlackFlowLifecycleTaskPlugin plugin({}, nullptr, "Roguelike", config, nullptr, session, port);
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
        require(session->apply_node_task_result(result, json::object {}), "portal completion");
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
        require(amounts.has_value() && std::ranges::any_of(*amounts, [](const auto& row) { return row.text == "25"; }),
                "captured wallet is not read as 25");
    });
    test("captured 002 pending refresh is recognized as network loading", [&] {
        const auto image = MAA_NS::imread(
            std::filesystem::path(argv[1]) / "unit_test/MaaCore/fixtures/blackflow-recovery/shop-refresh-network-pending.jpg");
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
        require(amounts.has_value() && std::ranges::any_of(*amounts, [](const auto& row) { return row.text == "31"; }),
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
        require(session->terminated() && session->result()->next_action == "stop_task", "core failure was not terminal");
        require(session->result()->outcome == "initial_core_recruitment_failed", "incorrect result");
        require(port->events.back().action == "recovery.initial_core_recruitment", "failure log missing");
    });
    for (int difficulty : { 5, 6 }) {
        test(("captured home badge " + std::to_string(difficulty)).c_str(), [&] {
            const auto image = MAA_NS::imread(
                std::filesystem::path(argv[1]) /
                ("unit_test/MaaCore/fixtures/blackflow-difficulty/home-" + std::to_string(difficulty) + ".jpg"));
            require(!image.empty(), "difficulty screenshot missing");
            DifficultyProbe selection({}, nullptr, "Roguelike", config, nullptr);
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
    std::cout << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
