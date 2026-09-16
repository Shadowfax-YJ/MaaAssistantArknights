// Real Session, TaskData and image recognizers; only game observations are supplied.
#include "Config/ResourceLoader.h"
#include "Config/TaskData.h"
#include "MaaUtils/ImageIo.h"
#include "Task/Roguelike/BlackFlow/BlackFlowLifecycleRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowSession.h"
#include "Utils/WorkingDir.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace asst;
using namespace asst::blackflow;

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

BlackFlowSession pending_move(bool pursuit, std::string target = "empty", int floor = 3, bool random = false)
{
    BlackFlowSession s;
    std::string error;
    require(s.initialize("automation_collection", &error), error);
    require(s.set_current_floor(floor, &error), error);
    auto initial = map_frame(floor, 1, 0, target);
    initial.run.action_points = initial.observation.hud_action_points = 1;
    if (random) {
        initial.run.active_movement = MovementKind::M07;
        initial.run.movement_charges->emplace(MovementKind::M07, 1);
    }
    require(s.update(initial, &error), error);
    commit_move(s, floor, random);
    if (target == "empty") {
        require(!s.page_context(), "empty landing must not fabricate a node page");
    }
    if (pursuit) {
        s.mark_floor_three_pursuit_battle_pending();
    }
    return s;
}

std::string recognize(const cv::Mat& image, const std::vector<std::string>& tasks)
{
    PipelineAnalyzer analyzer(image);
    analyzer.set_tasks(tasks);
    auto result = analyzer.analyze();
    return result ? result->task_ptr->name : "";
}

// Execute resource-defined page decisions and count clicks, without a controller.
// Recognition is real, and clicks switch between the captured small/large map frames.
void check_floor_recovery(const cv::Mat& small, const cv::Mat& large, bool zoomed, const char* recovery_entry)
{
    auto entry = Task.get(recovery_entry);
    require(entry != nullptr, "floor recheck entry is missing");
    std::vector<std::string> next = entry->next;
    int zoom_in = 0, zoom_out = 0;
    for (int step = 0; step < 12; ++step) {
        auto name = recognize(zoomed ? large : small, next);
        require(!name.empty(), "floor recovery cannot classify captured map");
        if (name == "BlackFlow@Roguelike@NextLevel") {
            require(zoom_in == 0, "floor recheck enlarged an already prepared map");
            require(zoom_out <= 1, "floor recheck repeated zoom-out");
            return;
        }
        auto task = Task.get(name);
        if (task->action == ProcessTaskAction::ClickSelf) {
            if (zoomed) {
                ++zoom_out;
            }
            else {
                ++zoom_in;
            }
            zoomed = !zoomed;
        }
        next = task->next;
    }
    throw std::runtime_error("floor recovery did not reach floor recognition");
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        return 2;
    }
    UserDir.set(std::filesystem::current_path());
    require(ResourceLoader::get_instance().load(std::filesystem::path(argv[1]) / "resource"), "resource load failed");
    const auto fixtures = std::filesystem::path(argv[2]);
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
    test("empty landing pursuit advances into floor four", [&] {
        auto s = pending_move(true);
        std::string e;
        s.clear_current_floor();
        require(s.set_current_floor(4, &e), e);
        require(s.update(map_frame(4, 2, 0), &e), "fourth-floor observation rejected: " + e);
        require(
            s.run().floor == 4 && s.run().current_node == *make_stable_node_id(4, { 0, 0 }),
            "old location retained");
        require(!s.transaction() && !s.page_context(), "old movement retained");
        require(s.run().resources.action_points == 7, "new floor HUD ignored");
        require(s.update(map_frame(4, 3, 0), &e), "next observation rejected: " + e);
    });
    test("empty landing without pursuit cannot authorize arbitrary cross-floor move", [&] {
        auto s = pending_move(false);
        std::string e;
        s.clear_current_floor();
        require(s.set_current_floor(4, &e), e);
        require(!s.update(map_frame(4, 2, 0), &e), "unconfirmed cross-floor move accepted");
        require(s.transaction() != nullptr && s.run().floor == 3, "failed observation mutated state");
    });
    test("same-floor empty landing still reconciles", [&] {
        auto s = pending_move(false);
        std::string e;
        auto after = map_frame(3, 2, 1);
        after.run.action_points = after.observation.hud_action_points = 0;
        require(s.update(after, &e), "same-floor landing rejected: " + e);
        require(
            !s.transaction() && s.run().current_node == *make_stable_node_id(3, { 0, 1 }),
            "same-floor move retained");
    });
    test("ordinary node page pursuit still reconciles", [&] {
        auto s = pending_move(true, "scrap_shop");
        std::string e;
        require(s.page_context().has_value(), "node page missing");
        s.clear_current_floor();
        require(s.set_current_floor(4, &e), e);
        require(s.update(map_frame(4, 2, 0), &e), "node-page pursuit rejected: " + e);
    });
    test("random empty landing pursuit advances into floor four", [&] {
        auto s = pending_move(true, "empty", 3, true);
        std::string e;
        s.clear_current_floor();
        require(s.set_current_floor(4, &e), e);
        require(s.update(map_frame(4, 2, 0), &e), "random empty landing pursuit rejected: " + e);
        require(!s.transaction() && s.run().floor == 4, "random movement was not reconciled");
    });
    test("pursuit cannot skip a floor", [&] {
        auto s = pending_move(true);
        std::string e;
        s.clear_current_floor();
        require(s.set_current_floor(5, &e), e);
        require(!s.update(map_frame(5, 2, 0), &e), "pursuit authorized a two-floor jump");
    });
    test("pursuit requires lifecycle confirmation of the next floor", [&] {
        auto s = pending_move(true);
        std::string e;
        require(!s.update(map_frame(4, 2, 0), &e), "unconfirmed next-floor observation accepted");
    });
    test("cancelled pursuit transaction cannot authorize its replacement", [&] {
        auto s = pending_move(true);
        std::string e;
        s.cancel_transaction();
        commit_move(s, 3);
        s.clear_current_floor();
        require(s.set_current_floor(4, &e), e);
        require(!s.update(map_frame(4, 2, 0), &e), "old pursuit authorization leaked into new move");
    });
    test("pursuit authorization does not leak into a reset run", [&] {
        auto s = pending_move(true);
        std::string e;
        s.reset_run();
        require(s.set_current_floor(3, &e), e);
        require(s.update(map_frame(3, 1, 0), &e), e);
        commit_move(s, 3);
        s.clear_current_floor();
        require(s.set_current_floor(4, &e), e);
        require(!s.update(map_frame(4, 2, 0), &e), "old run authorized a new-run transition");
    });
    for (const char* file : { "recruit-caster.jpg", "recruit-supporter.jpg" }) {
        test(file, [&] {
            auto image = MAA_NS::imread(fixtures / file);
            require(!image.empty(), "fixture missing");
            require(
                recognize(image, { "BlackFlow@Roguelike@GetDropConfirmed" }).empty(),
                "recruitment matched a stale reward button");
            const auto resume = std::string(recovery_retry_task("BlackFlow@Roguelike@GetDropConfirmedAction"));
            auto entry = Task.get(resume);
            require(entry != nullptr, "reward recovery entry missing");
            require(recognize(image, { resume }) == resume, "recovery entrance excludes recruitment page");
            require(
                recognize(image, entry->next) == "BlackFlow@Roguelike@ChooseOperFlag",
                "recovery does not hand off to recruitment");
            Task.set_task_base(
                "BlackFlow@Roguelike@GetDropConfirmedAction",
                "BlackFlow@Roguelike@GetDropConfirmedContinue");
            require(
                recognize(image, Task.get("BlackFlow@Roguelike@GetDropConfirmedAction")->next) ==
                    "BlackFlow@Roguelike@ChooseOperFlag",
                "runtime action alias did not reach recruitment");
        });
    }
    test("reward recovery still accepts an unchanged reward page", [&] {
        auto image = MAA_NS::imread(fixtures.parent_path() / "blackflow-interaction/stuck-before.jpg");
        require(!image.empty(), "fixture missing");
        auto entry = Task.get(std::string(recovery_retry_task("BlackFlow@Roguelike@GetDropConfirmedAction")));
        const auto name = recognize(image, entry->next);
        require(name == "BlackFlow@Roguelike@DropsFlag", "unchanged reward page did not reach reward dispatcher");
        require(
            !recognize(image, { "BlackFlow@Roguelike@GetDropConfirmed" }).empty(),
            "original reward button no longer recognized");
    });
    test("closed movement panel has positive map evidence", [&] {
        auto image = MAA_NS::imread(fixtures / "panel-closed.jpg");
        require(!image.empty(), "fixture missing");
        require(
            recognize(image, { "BlackFlow@Roguelike@MovementPanelTitle" }).empty(),
            "closed panel title falsely recognized");
        require(
            !recognize(image, { "BlackFlow@Roguelike@MapPrepare-FloorEnterZoom" }).empty(),
            "closed panel has no map gate");
    });
    test("open movement panel still has its title", [&] {
        auto image = MAA_NS::imread(fixtures / "panel-open.jpg");
        require(!image.empty(), "fixture missing");
        require(
            !recognize(image, { "BlackFlow@Roguelike@MovementPanelTitle" }).empty(),
            "open panel title not recognized");
    });
    auto small = MAA_NS::imread(fixtures / "floor-four-small.jpg");
    auto large = MAA_NS::imread(fixtures / "floor-four-large.jpg");
    test("floor recovery keeps prepared map small", [&] { check_floor_recovery(small, large, false, argv[3]); });
    test("floor recovery handles enlarged map", [&] { check_floor_recovery(small, large, true, argv[3]); });
    std::cout << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
