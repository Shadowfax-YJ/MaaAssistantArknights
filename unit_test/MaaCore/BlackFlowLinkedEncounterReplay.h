#pragma once

#include "Task/Roguelike/BlackFlow/BlackFlowEncounterRules.h"

// Replay the real Session choice provider with the recorded map and controlled marker variants.
template <typename Test>
void linked_encounter_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto path = repo / "unit_test/MaaCore/fixtures/blackflow-linked-encounter/20261021/map.json";
    const auto document = json::open(path);
    require(document.has_value(), "linked encounter map fixture missing");
    const auto& data = *document;
    const GridPosition camp_position { 3, 1 };

    auto frame = [&](int floor) {
        BlackFlowPerceptionSnapshot result;
        auto& observation = result.observation;
        observation.observation_id = "003-lone-survivor-replay-1";
        observation.sequence = observation.viewport_revision = 1;
        observation.floor = floor;
        observation.floor_from_ocr = observation.recognition_ok = observation.graph_connected = true;
        observation.hud_action_points = 5;
        observation.current_marker_score = 1;
        result.run.action_points = 5;
        result.run.active_movement = MovementKind::Walk;
        result.run.movement_charges = std::unordered_map<MovementKind, int> { { MovementKind::M01, 1 } };
        for (const auto& value : data.at("nodes").as_array()) {
            PerceptionNodeObservation node;
            node.temporary_id = static_cast<int>(observation.nodes.size());
            node.position = { value.get("row", 0), value.get("column", 0) };
            if (node.position == GridPosition { 2, 6 }) {
                observation.current_marker_temporary_id = node.temporary_id;
            }
            node.exists = node.detected_by_vision = node.confirmed_by_topology = true;
            node.confidence = 1;
            node.type = value.get("type", std::string {});
            node.displayed_name = value.get("name", std::string {});
            node.marker_type = value.get("marker_type", std::string {});
            node.marker_score = node.marker_type.empty() ? 0 : 1;
            node.visually_hidden = value.get("visually_hidden", false);
            node.icon_rect = { 100 + node.position.column * 100, 100 + node.position.row * 100, 40, 40 };
            observation.nodes.push_back(std::move(node));
        }
        for (const auto& edge : data.at("edges").as_array()) {
            observation.edges.push_back({ edge[0].as_integer(), edge[1].as_integer(), true, true, false, 1, "recorded" });
        }
        return result;
    };
    const auto node_at = [](BlackFlowPerceptionSnapshot& snapshot, GridPosition position)
        -> PerceptionNodeObservation& {
        const auto found = std::ranges::find_if(snapshot.observation.nodes, [&](const auto& node) {
            return node.position == position;
        });
        require(found != snapshot.observation.nodes.end(), "fixture node missing");
        return *found;
    };
    const auto enter_event = [&](BlackFlowSession& session, int floor) {
        std::string error;
        MoveCandidate move;
        move.action_id = "003-recorded-walk-to-lone-survivor";
        move.source = *make_stable_node_id(floor, { 2, 6 });
        move.target = move.landing = *make_stable_node_id(floor, { 2, 5 });
        move.path = move.possible_landings = { move.landing };
        move.movement = MovementKind::Walk;
        move.controllable = true;
        move.predicted_action_point_cost = 1;
        move.landing_action_point_gains.emplace(move.landing, 0);
        require(session.begin_transaction(move, &error), "begin recorded event: " + error);
        require(
            session.accept_preview({ PreviewReachability::Reachable, 1 }, &error) == PreviewDisposition::ReadyToCommit,
            "preview recorded event: " + error);
        EnteredPageObservation entered;
        entered.classified_type = NodeType::Incident;
        entered.event_name = LoneSurvivorEventName;
        require(session.commit(entered, &error), "commit recorded event: " + error);
    };
    const auto choice = [&](BlackFlowPerceptionSnapshot snapshot, bool clear_resident_in_new_observation = false) {
        BlackFlowSession session;
        std::string error;
        const int floor = snapshot.observation.floor;
        require(session.initialize("automation_collection", &error), "initialize: " + error);
        require(session.set_current_floor(floor, &error), "floor: " + error);
        require(session.update(snapshot, &error), "recorded map: " + error);
        if (clear_resident_in_new_observation) {
            auto& camp = node_at(snapshot, camp_position);
            camp.marker_type.clear();
            camp.marker_score = 0;
            snapshot.observation.observation_id = "003-lone-survivor-replay-2";
            snapshot.observation.sequence = snapshot.observation.viewport_revision = 2;
            require(session.update(snapshot, &error), "updated map: " + error);
        }
        enter_event(session, floor);
        return session.preferred_encounter_choice(LoneSurvivorFollowupEventName);
    };

    test("003 recorded resident on Emergency Aid forbids Lone Survivor transfer", [&] {
        require(choice(frame(4)) == 2U, "selected transfer into the recorded resident");
    });
    for (const int floor : { 2, 3 }) {
        test(("Lone Survivor resident restriction also applies on floor " + std::to_string(floor)).c_str(), [&] {
            require(choice(frame(floor)) == 2U, "ordinary resident allowance overrode the event restriction");
        });
    }
    test("resident on hidden Emergency Aid forbids Lone Survivor transfer", [&] {
        auto snapshot = frame(4);
        auto& camp = node_at(snapshot, camp_position);
        camp.type = "hide_invisible";
        camp.displayed_name = "未知的诡秘";
        camp.visually_hidden = true;
        require(choice(snapshot) == 2U, "selected transfer while an unknown mystery held a resident");
    });
    test("resident on any unknown mystery blocks transfer even with a known safe Emergency Aid", [&] {
        auto snapshot = frame(4);
        node_at(snapshot, camp_position).marker_type.clear();
        const auto hidden = std::ranges::find_if(snapshot.observation.nodes, [](const auto& node) {
            return node.type == "hide_invisible";
        });
        require(hidden != snapshot.observation.nodes.end(), "unknown mystery control missing");
        hidden->marker_type = "savage";
        require(choice(snapshot) == 2U, "only checked the known transfer target");
    });
    test("safe Emergency Aid preserves beneficial Lone Survivor transfer", [&] {
        auto snapshot = frame(4);
        node_at(snapshot, camp_position).marker_type.clear();
        require(choice(snapshot) == 1U, "unrelated resident markers disabled safe transfer");
    });
    test("nonresident marker on Emergency Aid preserves Lone Survivor transfer", [&] {
        auto snapshot = frame(4);
        node_at(snapshot, camp_position).marker_type = "fruit";
        require(choice(snapshot) == 1U, "nonresident marker was treated as a resident");
    });
    test("latest map observation clears the Lone Survivor transfer restriction", [&] {
        require(choice(frame(4), true) == 1U, "historical resident marker remained a permanent veto");
    });
}
