#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <set>

#include <meojson/json.hpp>

#include "Task/Roguelike/BlackFlow/BlackFlowModel.h"
#include "Task/Roguelike/BlackFlow/BlackFlowPlanner.h"
#include "Task/Roguelike/BlackFlow/BlackFlowPlannerRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRevealSemantics.h"
#include "Task/Roguelike/BlackFlow/BlackFlowStateSpace.h"

using namespace asst::blackflow;

namespace
{
Node add_node(MapSnapshot& map, GridPosition position, NodeType type)
{
    Node node;
    node.floor = 1;
    node.position = position;
    node.id = *make_stable_node_id(1, position);
    node.type = type;
    node.traversal = default_traversal_for(type);
    node.identity_revealed = true;
    node.identity_state = NodeIdentityState::Classified;
    REQUIRE(map.upsert_node(node));
    return node;
}
}

TEST_CASE("BlackFlow DLC movement resources cover all four new processing items")
{
    REQUIRE(ProcessingMovementSlotCount == 17);
    const auto tasks_path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                            "resource/tasks/Roguelike/BlackFlow.json";
    const auto tasks = json::open(tasks_path);
    REQUIRE(tasks.has_value());
    const std::array kinds { MovementKind::M13, MovementKind::M14, MovementKind::M15, MovementKind::M16 };
    for (const MovementKind kind : kinds) {
        const MovementSpec* movement = find_movement_spec(kind);
        REQUIRE(movement != nullptr);
        CHECK(movement->action_point_cost == 1);
        CHECK(movement->initial_charges == (kind == MovementKind::M14 ? 2 : 1));
        CHECK_FALSE(movement->expires_on_floor_end);
        CHECK(std::ranges::count(ProcessingMovementStrengthOrder, kind) == 1);
        for (const std::string key :
             { "MovementInventoryItems", "MovementInventoryAllItems", "MovementPanelItems", "TrophyRewardNames" }) {
            const auto names = tasks->at("BlackFlow@Roguelike@" + key).at("text").as_array();
            CHECK(std::ranges::any_of(names, [&](const auto& name) { return name.as_string() == movement->name; }));
        }
    }
}

TEST_CASE("BlackFlow DLC processing ranges respect geometry and displayed node types")
{
    MapSnapshot map;
    const Node source = add_node(map, { 2, 2 }, NodeType::Empty);
    const Node row = add_node(map, { 2, 8 }, NodeType::Incident);
    const Node column = add_node(map, { 8, 2 }, NodeType::BattleNormal);
    const Node diagonal = add_node(map, { 3, 3 }, NodeType::BattleElite);
    const auto& wheel = *find_movement_spec(MovementKind::M14);
    CHECK(is_in_geometric_range(map, source.id, row.id, wheel));
    CHECK(is_in_geometric_range(map, source.id, column.id, wheel));
    CHECK_FALSE(is_in_geometric_range(map, source.id, diagonal.id, wheel));
    const auto& seat = *find_movement_spec(MovementKind::M13);
    for (const NodeType type : { NodeType::Final, NodeType::BattleBoss, NodeType::Evacuate }) {
        CHECK(node_type_allowed(seat, type));
    }
    CHECK_FALSE(node_type_allowed(seat, NodeType::Incident));
    CHECK_FALSE(node_type_allowed(seat, movement_visible_node_type(NodeType::Final, true)));
    const auto& horn = *find_movement_spec(MovementKind::M15);
    CHECK(is_in_geometric_range(map, source.id, diagonal.id, horn));
    CHECK_FALSE(is_in_geometric_range(map, source.id, column.id, horn));
    CHECK(node_type_allowed(horn, NodeType::HideBattle));
    CHECK_FALSE(node_type_allowed(horn, NodeType::Incident));
    const auto& express = *find_movement_spec(MovementKind::M16);
    CHECK(node_type_allowed(express, NodeType::Incident));
    CHECK_FALSE(node_type_allowed(express, NodeType::HideInvisible));
    CHECK_FALSE(node_type_allowed(express, NodeType::Shop));
}

TEST_CASE("BlackFlow DLC movement actions agree in both state spaces")
{
    MapSnapshot map;
    const Node source = add_node(map, { 2, 2 }, NodeType::Empty);
    const Node event = add_node(map, { 2, 3 }, NodeType::Incident);
    add_node(map, { 2, 8 }, NodeType::Final);
    add_node(map, { 3, 3 }, NodeType::BattleNormal);
    add_node(map, { 8, 2 }, NodeType::Rest);
    REQUIRE(map.upsert_edge({ source.id, event.id, EdgeKnowledge::Confirmed, { } }));
    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 10;
    for (const MovementKind kind : { MovementKind::M13, MovementKind::M14, MovementKind::M15, MovementKind::M16 }) {
        run.resources.movement_charges.emplace(kind, find_movement_spec(kind)->initial_charges);
    }
    std::set<std::string> expected;
    for (const auto& action : enumerate_move_actions(map, run)) {
        expected.emplace(action.candidate.action_id);
    }
    for (const bool compact : { false, true }) {
        StateExpansionOptions options;
        options.use_compact_actions = compact;
        OnDemandStateGraph graph;
        std::string error;
        REQUIRE(graph.initialize(map, run, options, &error));
        const auto* actions = graph.actions(graph.initial_state(), &error);
        REQUIRE(actions != nullptr);
        std::set<std::string> actual;
        for (const auto& action : *actions) {
            actual.emplace(action.candidate.action_id);
        }
        CHECK(actual == expected);
    }
}

TEST_CASE("BlackFlow express converts an adjacent event without being pruned as walking")
{
    MapSnapshot map;
    const Node source = add_node(map, { 0, 0 }, NodeType::Empty);
    const Node event = add_node(map, { 0, 1 }, NodeType::Incident);
    REQUIRE(map.upsert_edge({ source.id, event.id, EdgeKnowledge::Confirmed, { } }));
    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 4;
    run.resources.movement_charges.emplace(MovementKind::M16, 2);
    const auto generated = enumerate_move_actions(map, run);
    const auto move =
        std::ranges::find(generated, MovementKind::M16, [](const auto& a) { return a.candidate.movement; });
    REQUIRE(move != generated.end());
    CHECK(move_landing_type(move->candidate, event.id) == NodeType::Shop);
    CHECK_FALSE(move_preview_updates_target_identity(move->candidate));
    CHECK_FALSE(move_lands_on_forbidden_node_type(map, move->candidate, { NodeType::Incident }));
    CHECK(move_lands_on_forbidden_node_type(map, move->candidate, { NodeType::Shop }));
    const auto projected = project_move_outcome(map, run, move->candidate, 1);
    REQUIRE(projected.has_value());
    CHECK(projected->run.projected_shops.contains(event.id));
    CHECK_FALSE(projected->run.node_progress.contains(event.id));

    for (const bool compact : { false, true }) {
        Milestone merchant;
        merchant.id = "converted-merchant";
        merchant.floor_begin = 1;
        merchant.floor_end = 1;
        merchant.selector.node_types = { NodeType::Shop };
        merchant.selector.node_names = { "诡意行商" };
        MissionState mission;
        mission.set_status(merchant.id, MilestoneStatus::Available);
        FactStore facts;
        auto goal = SafetyGoalProgram::compile(std::vector { merchant }, mission, facts, { merchant.id });
        REQUIRE(goal.has_value());
        REQUIRE(goal->has_binding_goals());
        StateExpansionOptions options;
        options.use_compact_actions = compact;
        options.safety_goal = &*goal;
        options.safety_goal_facts = &facts;
        OnDemandStateGraph graph;
        REQUIRE(graph.initialize(map, run, options));
        const auto* actions = graph.actions(graph.initial_state());
        REQUIRE(actions != nullptr);
        const auto express =
            std::ranges::find(*actions, MovementKind::M16, [](const auto& a) { return a.candidate.movement; });
        REQUIRE(express != actions->end());
        REQUIRE(express->outcomes.size() == 1);
        const auto next_id = express->outcomes.front().successor;
        const auto next = graph.state(next_id);
        CHECK(goal->milestone_progress(next.goal_progress_id, merchant.id) == 1);
        const auto mask = graph.node_mask(event.id);
        REQUIRE(mask.has_value());
        CHECK((next.projected_shops & *mask) != 0);
        CHECK_FALSE(graph.is_completed(next_id, event.id));
        CHECK(next.movement_charges[static_cast<std::size_t>(MovementKind::M16)] == 1);
        const auto* after = graph.actions(next_id);
        REQUIRE(after != nullptr);
        CHECK(std::ranges::none_of(*after, [](const auto& a) { return a.candidate.movement == MovementKind::M16; }));
        PlannerState unconverted = next;
        unconverted.projected_shops = 0;
        CHECK_FALSE(unconverted == next);
        CHECK(PlannerStateHash { }(unconverted) != PlannerStateHash { }(next));
    }
}

TEST_CASE("BlackFlow Lorentz eye reveals the whole map only on a processing light landing")
{
    MapSnapshot map;
    const Node source = add_node(map, { 0, 0 }, NodeType::Empty);
    const Node light = add_node(map, { 0, 1 }, NodeType::Light);
    const Node next_light = add_node(map, { 0, 2 }, NodeType::Light);
    Node far = add_node(map, { 4, 5 }, NodeType::HideBattle);
    far.identity_revealed = false;
    far.visually_hidden = true;
    far.natural_reveal_suppressed = true;
    REQUIRE(map.upsert_node(far));
    add_node(map, { 0, 7 }, NodeType::Final);
    REQUIRE(map.upsert_edge({ source.id, light.id, EdgeKnowledge::Confirmed, {} }));
    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 8;
    run.resources.lorentz_eye_available = true;
    run.resources.movement_charges[MovementKind::M14] = 2;
    MoveCandidate move;
    move.source = source.id;
    move.target = move.landing = light.id;
    move.movement = MovementKind::Walk;
    CHECK_FALSE(expected_move_reveals(map, run, move, light.id, true).contains(far.id));
    move.movement = MovementKind::M14;
    CHECK(expected_move_reveals(map, run, move, light.id, true).contains(far.id));
    CHECK_FALSE(expected_move_reveals(map, run, move, light.id, false).contains(far.id));
    CHECK_FALSE(expected_move_reveals(map, run, move, source.id, true).contains(far.id));
    RunState spent = run;
    spent.resources.lorentz_eye_available = false;
    CHECK_FALSE(expected_move_reveals(map, spent, move, light.id, true).contains(far.id));
    const auto projected = project_move_outcome(map, run, move, 1);
    REQUIRE(projected.has_value());
    CHECK_FALSE(projected->run.resources.lorentz_eye_available);
    CHECK(projected->run.revealed_nodes.contains(far.id));
    CHECK(run.resources.lorentz_eye_available);
    CHECK_FALSE(map.find_node(far.id)->identity_revealed);
    MapSnapshot completed = map;
    Node completed_light = light;
    completed_light.type = NodeType::Empty;
    REQUIRE(completed.upsert_node(completed_light));
    RunState finalized = run;
    finalized.consumed_one_time_nodes.emplace(light.id);
    CHECK(expected_move_reveals(completed, finalized, move, light.id, true, {}, NodeType::Light).contains(far.id));

    std::optional<PlannerState> expected;
    for (const bool compact : { false, true }) {
        StateExpansionOptions options;
        options.use_compact_actions = compact;
        OnDemandStateGraph graph;
        REQUIRE(graph.initialize(map, run, options));
        const auto* actions = graph.actions(graph.initial_state());
        REQUIRE(actions != nullptr);
        const auto action = std::ranges::find_if(*actions, [&](const auto& a) {
            return a.candidate.movement == MovementKind::M14 && a.candidate.target == light.id;
        });
        REQUIRE(action != actions->end());
        REQUIRE(action->outcomes.size() == 1);
        const auto id = action->outcomes.front().successor;
        const auto state = graph.state(id);
        CHECK_FALSE(state.lorentz_eye_available);
        CHECK((state.revealed_hidden_battles & *graph.node_mask(far.id)) != 0);
        CHECK((state.newly_revealed_nodes & *graph.node_mask(far.id)) != 0);
        if (expected.has_value()) {
            CHECK(state == *expected);
        }
        expected = state;
        const auto* after = graph.actions(id);
        REQUIRE(after != nullptr);
        const auto second = std::ranges::find_if(*after, [&](const auto& a) {
            return a.candidate.movement == MovementKind::M14 && a.candidate.target == next_light.id;
        });
        REQUIRE(second != after->end());
        CHECK_FALSE(graph.state(second->outcomes.front().successor).lorentz_eye_available);
    }
}

TEST_CASE("BlackFlow exploration chooses a processing light landing when the Lorentz eye reveals distant nodes")
{
    MapSnapshot map;
    const Node source = add_node(map, { 0, 0 }, NodeType::Empty);
    const Node light = add_node(map, { 0, 1 }, NodeType::Light);
    const Node exit = add_node(map, { 0, 2 }, NodeType::Final);
    REQUIRE(map.upsert_edge({ source.id, light.id, EdgeKnowledge::Confirmed, {} }));
    REQUIRE(map.upsert_edge({ light.id, exit.id, EdgeKnowledge::Confirmed, {} }));
    for (const GridPosition position : { GridPosition { 5, 5 }, { 5, 6 }, { 6, 5 } }) {
        Node hidden = add_node(map, position, NodeType::HideInvisible);
        hidden.identity_revealed = false;
        hidden.identity_state = NodeIdentityState::Hidden;
        hidden.visually_hidden = true;
        REQUIRE(map.upsert_node(hidden));
    }
    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 2;
    run.resources.movement_charges[MovementKind::M14] = 1;
    ResolvedPolicy policy;
    policy.route_preferences = {
        RoutePreference::MaximizeRevealedNodes,
        RoutePreference::MaximizeEffectiveNodes,
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
    for (const bool eye : { false, true }) {
        run.resources.lorentz_eye_available = eye;
        const auto plan = BlackFlowPlanner {}.plan(request);
        INFO(plan.error);
        CAPTURE(eye);
        REQUIRE(plan.decision.selected.has_value());
        CHECK(plan.decision.selected->target == light.id);
        CHECK(plan.decision.selected->movement == (eye ? MovementKind::M14 : MovementKind::Walk));
        CHECK(run.resources.lorentz_eye_available == eye);
    }
}

TEST_CASE("BlackFlow extra merchant reveals are only a possible duck board explanation")
{
    MapSnapshot map;
    const Node source = add_node(map, { 0, 0 }, NodeType::Empty);
    const Node shop = add_node(map, { 4, 5 }, NodeType::Shop);
    const Node scrap_shop = add_node(map, { 5, 5 }, NodeType::ScrapShop);
    const Node other = add_node(map, { 5, 4 }, NodeType::BattleNormal);
    const Node employ = add_node(map, { 4, 4 }, NodeType::Employ);
    const auto expected = expected_initial_floor_reveals(map, source.id, false);
    const auto observed = observed_initial_floor_reveals(map, source.id);
    const auto consistency = compare_move_reveals(expected, observed);
    REQUIRE_FALSE(consistency.consistent());
    const auto explanation = unexpected_merchant_reveals(map, consistency.unexpected);
    CHECK(explanation.size() == 2);
    CHECK(std::ranges::find(explanation, shop.id) != explanation.end());
    CHECK(std::ranges::find(explanation, scrap_shop.id) != explanation.end());
    CHECK(std::ranges::find(explanation, other.id) == explanation.end());
    CHECK(std::ranges::find(explanation, employ.id) == explanation.end());
    CHECK_FALSE(expected.contains(shop.id));
    CHECK_FALSE(expected.contains(scrap_shop.id));
    // 说明不修改预期/实际集合，不据此认定持有，也不掩盖其他额外揭示。
    CHECK(compare_move_reveals(expected, observed).unexpected == consistency.unexpected);
    CHECK(unexpected_merchant_reveals(map, {}).empty());
}

TEST_CASE("BlackFlow multi-use processing items retain their observed charges")
{
    MapSnapshot map;
    const Node source = add_node(map, { 0, 0 }, NodeType::Empty);
    const Node target = add_node(map, { 0, 1 }, NodeType::Incident);
    add_node(map, { 0, 7 }, NodeType::Final);
    RunState run;
    run.floor = 1;
    run.current_node = source.id;
    run.resources.action_points = 8;
    run.resources.movement_instances = {
        { MovementKind::M14, 2, 0, true }, { MovementKind::M14, 2, 1, false }
    };
    rebuild_movement_aggregates(run.resources);
    CHECK(run.resources.movement_charges.at(MovementKind::M14) == 4);
    for (const bool compact : { false, true }) {
        StateExpansionOptions options;
        options.use_compact_actions = compact;
        OnDemandStateGraph graph;
        REQUIRE(graph.initialize(map, run, options));
        CHECK(graph.state(graph.initial_state()).movement_charges[static_cast<std::size_t>(MovementKind::M14)] == 4);
        const auto* actions = graph.actions(graph.initial_state());
        REQUIRE(actions != nullptr);
        const auto action = std::ranges::find_if(*actions, [&](const auto& a) {
            return a.candidate.movement == MovementKind::M14 && a.candidate.target == target.id;
        });
        REQUIRE(action != actions->end());
        CHECK(graph.state(action->outcomes.front().successor)
                  .movement_charges[static_cast<std::size_t>(MovementKind::M14)] == 3);
        const auto projected = project_move_outcome(map, run, action->candidate, 1);
        REQUIRE(projected.has_value());
        CHECK(projected->run.resources.movement_charges.at(MovementKind::M14) == 3);
        CHECK(run.resources.movement_charges.at(MovementKind::M14) == 4);
    }
}

TEST_CASE("BlackFlow squad random reveals require a processing move and respect the maximum")
{
    MapSnapshot before;
    for (int i = 0; i < 4; ++i) {
        Node node = add_node(before, { 4, i }, NodeType::HideInvisible);
        node.identity_revealed = false;
        REQUIRE(before.upsert_node(node));
    }
    MapSnapshot after = before;
    int count = 0;
    for (const auto& [id, node] : before.nodes()) {
        Node visible = node;
        visible.identity_revealed = ++count <= 3;
        REQUIRE(after.upsert_node(visible));
    }
    RunState run;
    run.floor = 1;
    MoveCandidate move;
    move.movement = MovementKind::M14;
    std::unordered_set<NodeId> expected;
    add_observed_random_reveal_expectations(before, run, after, move, true, expected);
    CHECK(expected.empty());
    run.resources.random_reveals_on_processing_move = 3;
    move.movement = MovementKind::Walk;
    add_observed_random_reveal_expectations(before, run, after, move, true, expected);
    CHECK(expected.empty());
    move.movement = MovementKind::M14;
    add_observed_random_reveal_expectations(before, run, after, move, true, expected);
    CHECK(expected.size() == 3);
    for (const auto& [id, node] : before.nodes()) {
        Node visible = node;
        visible.identity_revealed = true;
        REQUIRE(after.upsert_node(visible));
    }
    expected.clear();
    add_observed_random_reveal_expectations(before, run, after, move, true, expected);
    CHECK(expected.empty());
}
