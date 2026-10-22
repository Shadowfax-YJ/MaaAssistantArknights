#pragma once

namespace
{
MapSnapshot reveal_guard_map()
{
    MapSnapshot map;
    for (int column = 0; column < 3; ++column) {
        Node node;
        node.floor = 4;
        node.position = { 0, column };
        node.id = *make_stable_node_id(4, node.position);
        node.type = column == 0 ? NodeType::Empty : NodeType::HideInvisible;
        node.identity_revealed = column == 0;
        node.traversal = default_traversal_for(node.type);
        REQUIRE(map.upsert_node(node));
        if (column > 0) {
            REQUIRE(map.upsert_edge(
                { *make_stable_node_id(4, { 0, column - 1 }), node.id, EdgeKnowledge::Confirmed, { } }));
        }
    }
    return map;
}
}

TEST_CASE("BlackFlow uncertain mist keeps only guaranteed natural reveal credit")
{
    const auto map = reveal_guard_map();
    const NodeId source = *make_stable_node_id(4, { 0, 0 });
    const NodeId hidden = *make_stable_node_id(4, { 0, 1 });
    RevealVisibility mist;
    mist.diffused_mist = true;
    const auto projected = reveal_visibility_map(map, mist);
    REQUIRE_FALSE(map.find_node(hidden)->natural_reveal_suppressed);
    REQUIRE(projected.find_node(hidden)->natural_reveal_suppressed);
    const auto expectation = make_reveal_expectation(map, mist, [&](const MapSnapshot& visibility) {
        return expected_initial_floor_reveals(visibility, source, false);
    });
    REQUIRE(expectation.required.empty());
    REQUIRE(expectation.possible == std::unordered_set<NodeId> { hidden });
    REQUIRE(compare_reveal_expectation(expectation, { }).consistent());
    REQUIRE(compare_reveal_expectation(expectation, { hidden }).consistent());
    REQUIRE_FALSE(compare_reveal_expectation(expectation, { *make_stable_node_id(4, { 0, 2 }) }).consistent());
}

TEST_CASE("BlackFlow uncertain mist cannot excuse a missing entered node or active light reveal")
{
    auto map = reveal_guard_map();
    const NodeId source = *make_stable_node_id(4, { 0, 0 });
    const NodeId hidden = *make_stable_node_id(4, { 0, 1 });
    const NodeId neighbor = *make_stable_node_id(4, { 0, 2 });
    RunState run;
    run.floor = 4;
    MoveCandidate move;
    move.source = source;
    move.target = move.landing = hidden;
    RevealVisibility mist;
    mist.diffused_mist = true;
    const auto expectation = make_reveal_expectation(map, mist, [&](const MapSnapshot& visibility) {
        return expected_move_reveals(visibility, run, move, hidden, true);
    });
    REQUIRE(expectation.required.contains(hidden));
    REQUIRE_FALSE(compare_reveal_expectation(expectation, { }).consistent());
    Node light = *map.find_node(hidden);
    light.type = NodeType::Light;
    light.traversal = default_traversal_for(light.type);
    REQUIRE(map.upsert_node(light));
    const auto lit = make_reveal_expectation(map, mist, [&](const MapSnapshot& visibility) {
        return expected_move_reveals(visibility, run, move, hidden, true);
    });
    REQUIRE(lit.required.contains(neighbor));
    REQUIRE_FALSE(compare_reveal_expectation(lit, { hidden }).consistent());
}

TEST_CASE("BlackFlow confirmed mist replaces stale masks and preserves transparent completed space")
{
    auto map = reveal_guard_map();
    const NodeId source = *make_stable_node_id(4, { 0, 0 });
    const NodeId middle = *make_stable_node_id(4, { 0, 1 });
    const NodeId beyond = *make_stable_node_id(4, { 0, 2 });
    Node empty = *map.find_node(middle);
    empty.type = NodeType::Empty;
    empty.identity_revealed = true;
    empty.traversal = default_traversal_for(empty.type);
    REQUIRE(map.upsert_node(empty));
    RevealVisibility mist;
    mist.diffused_mist = mist.domain_confirmed = true;
    mist.domain = { { 0, 1 } };
    const auto visibility = reveal_visibility_map(map, mist);
    REQUIRE(expected_initial_floor_reveals(visibility, source, false).contains(beyond));
    empty.identity_source = "map_topology_no_ocr_empty";
    REQUIRE(map.upsert_node(empty));
    REQUIRE_FALSE(expected_initial_floor_reveals(reveal_visibility_map(map, mist), source, false).contains(beyond));
    empty.identity_source = "empty_template";
    REQUIRE(map.upsert_node(empty));
    mist.domain.push_back({ 0, 2 });
    REQUIRE_FALSE(expected_initial_floor_reveals(reveal_visibility_map(map, mist), source, false).contains(beyond));
    mist.expired = true;
    REQUIRE(expected_initial_floor_reveals(reveal_visibility_map(map, mist), source, false).contains(beyond));
}

TEST_CASE("BlackFlow completed linked event accepts only its witnessed origin")
{
    auto before = reveal_guard_map();
    const NodeId event = *make_stable_node_id(4, { 0, 1 });
    const NodeId unrelated = *make_stable_node_id(4, { 0, 2 });
    auto after = before;
    for (const auto id : { event, unrelated }) {
        Node empty = *after.find_node(id);
        empty.type = NodeType::Empty;
        empty.identity_revealed = true;
        empty.identity_source = "map_topology_no_ocr_empty";
        REQUIRE(after.upsert_node(empty));
    }
    RunState run;
    run.floor = 4;
    auto observed = observed_move_reveals(before, run, after);
    REQUIRE(observed.empty());
    REQUIRE_FALSE(add_completed_linked_event_reveal(before, run, after, event, false, observed));
    REQUIRE(add_completed_linked_event_reveal(before, run, after, event, true, observed));
    REQUIRE(observed == std::unordered_set<NodeId> { event });
    Node resident = *before.find_node(event);
    resident.marker_type = "savage";
    REQUIRE(before.upsert_node(resident));
    observed.clear();
    REQUIRE_FALSE(add_completed_linked_event_reveal(before, run, after, event, true, observed));
}
