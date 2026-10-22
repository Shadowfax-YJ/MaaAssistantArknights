#pragma once

// Captured map/state observations drive production Session merging and move reconciliation.
inline BlackFlowPerceptionSnapshot
    reveal_replay_frame(const json::value& data, NodeId current, int sequence, int action_points)
{
    BlackFlowPerceptionSnapshot result;
    auto& observation = result.observation;
    observation.floor = data.get("floor", 4);
    observation.observation_id = "captured-reveal-" + std::to_string(sequence);
    observation.sequence = observation.viewport_revision = sequence;
    observation.floor_from_ocr = observation.recognition_ok = observation.graph_connected = true;
    observation.hud_action_points = action_points;
    observation.current_marker_score = 1;
    observation.utopia_status = data.get("utopia_status", std::string { });
    observation.utopia_ideology = data.get("utopia_ideology", std::string { });
    observation.utopia_policy = data.get("utopia_policy", std::string { });
    observation.utopia_reason = data.get("utopia_reason", std::string { });
    observation.ideal_source_heads_agree = data.get("ideal_source_heads_agree", false);
    if (data.contains("ideal_source")) {
        observation.ideal_source =
            GridPosition { data.at("ideal_source").get("row", 0), data.at("ideal_source").get("column", 0) };
    }
    for (const auto& p : data.at("ideal_domain").as_array()) {
        observation.ideal_domain.push_back({ p.get("row", 0), p.get("column", 0) });
    }
    result.run.action_points = action_points;
    result.run.active_movement = MovementKind::M03;
    result.run.movement_charges = std::unordered_map<MovementKind, int> { { MovementKind::M03, 5 } };
    for (const auto& spec : movement_specs()) {
        if (to_string(spec.kind) == data.get("movement", std::string {})) {
            result.run.active_movement = spec.kind;
            if (spec.kind != MovementKind::Walk) {
                (*result.run.movement_charges)[spec.kind] = 5;
            }
        }
    }
    std::unordered_map<NodeId, int> temporary_ids;
    for (const auto& value : data.at("map_nodes").as_array()) {
        PerceptionNodeObservation node;
        node.temporary_id = static_cast<int>(observation.nodes.size());
        const NodeId id = value.get("id", NodeId { });
        temporary_ids.emplace(id, node.temporary_id);
        if (id == current) {
            observation.current_marker_temporary_id = node.temporary_id;
        }
        node.position = { value.get("row", 0), value.get("column", 0) };
        node.exists = node.detected_by_vision = node.confirmed_by_topology = true;
        node.confidence = 1;
        node.type = value.get("node_type", std::string { });
        node.displayed_name = value.get("node_name", std::string { });
        node.identity_source = value.get("identity_source", std::string { });
        node.identity_from_topology = value.get("identity_from_topology", false);
        node.identity_from_prediction = value.get("identity_from_prediction", false);
        node.prediction_rule = value.get("prediction_rule", std::string { });
        node.visually_hidden = value.get("visually_hidden", false);
        node.natural_reveal_suppressed = value.get("natural_reveal_suppressed", false);
        node.marker_type = value.get("marker_type", std::string { });
        node.marker_score = value.get("marker_score", 0.0);
        node.icon_rect = { 100 + node.position.column * 100, 100 + node.position.row * 100, 40, 40 };
        observation.nodes.push_back(std::move(node));
    }
    for (const auto& edge : data.at("map_edges").as_array()) {
        observation.edges.push_back(
            { temporary_ids.at(edge.get("first", NodeId { })),
              temporary_ids.at(edge.get("second", NodeId { })),
              true,
              true,
              false,
              1,
              "captured" });
    }
    return result;
}

template <typename Test>
void reveal_session_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto fixtures = json::open(repo / "unit_test/MaaCore/fixtures/blackflow-reveal/20261022/captured-cases.json");
    require(fixtures.has_value(), "captured reveal cases missing");
    json::array contract_events;
    for (const auto& fixture : fixtures->as_array()) {
        const std::string name = "captured reveal Session " + fixture.get("name", std::string { });
        test(name.c_str(), [&] {
            BlackFlowSession session;
            std::string error;
            require(session.initialize("automation_collection", &error), error);
            const auto& after = fixture.at("after");
            require(session.set_current_floor(after.get("floor", 4), &error), error);
            const bool initial = !fixture.contains("before");
            if (!initial) {
                const auto& before = fixture.at("before");
                const int points = fixture.at("run").get("action_points", 7);
                const NodeId source = after.get("source", InvalidNodeId);
                require(session.update(reveal_replay_frame(before, source, 1, points), &error), error);
                (void)session.take_telemetry_events();
                MoveCandidate move;
                move.action_id = "captured-reveal-move";
                move.source = source;
                move.target = move.landing = after.get("selected_target", InvalidNodeId);
                move.possible_landings = { move.target };
                move.landing_action_point_gains.emplace(move.target, 0);
                for (const auto& spec : movement_specs()) {
                    if (to_string(spec.kind) == after.get("movement", std::string { })) {
                        move.movement = spec.kind;
                    }
                }
                for (const auto& node : before.at("move").at("path").as_array()) {
                    move.path.push_back(node.get("id", InvalidNodeId));
                }
                move.controllable = true;
                move.predicted_action_point_cost = 1;
                require(session.begin_transaction(move, &error), "begin: " + error);
                require(
                    session.accept_preview({ PreviewReachability::Reachable, 1 }, &error) ==
                        PreviewDisposition::ReadyToCommit,
                    "preview: " + error);
                EnteredPageObservation entered;
                const bool linked = after.get("entered_event_node", InvalidNodeId) != InvalidNodeId;
                entered.classified_type = *node_type_from_string(after.get("entered_type", std::string { }));
                if (entered.classified_type == NodeType::Incident) {
                    entered.event_name = after.get("entered_name", std::string { });
                }
                const bool committed = session.commit(entered, &error);
                require(committed, "commit: " + error);
                require(session.mark_page_running(&error), error);
                for (const auto& content : after.at("observed_page_contents").as_array()) {
                    require(session.observe_page_content(content.as_string(), "captured", &error), error);
                }
                if (linked) {
                    (void)session.preferred_encounter_choice("和平守卫者-2");
                }
                NodeTaskResult completed;
                completed.kind = NodeTaskResultKind::PageCompleted;
                completed.succeeded = true;
                completed.node.repeatable =
                    entered.classified_type == NodeType::Shop || entered.classified_type == NodeType::ScrapShop;
                require(session.apply_node_task_result(completed, json::object { }, &error), error);
            }
            const NodeId current =
                initial ? after.get("entrance", InvalidNodeId) : after.get("actual_landing", InvalidNodeId);
            const int points = initial ? 7 : fixture.at("run").get("action_points", 7) - 1;
            require(session.update(reveal_replay_frame(after, current, 2, points), &error), "return: " + error);
            bool comparison_seen = false;
            for (const auto& event : session.take_telemetry_events()) {
                require(
                    !(event.what == "BlackFlowRoutingWarning" &&
                      event.details.get("code", "") == "reveal_consistency_mismatch"),
                    "false reveal warning");
                if (event.what == "BlackFlowRevealConsistency" || event.what == "BlackFlowInitialRevealConsistency") {
                    contract_events.emplace_back(
                        json::object {
                            { "schema_version", 1 },
                            { "action", "telemetry." + event.what },
                            { "sequence", contract_events.size() + 1 },
                            { "elapsed_ms", contract_events.size() + 1 },
                            { "timestamp", "2026-10-22T00:00:00Z" },
                            { "level", "INFO" },
                            { "floor", after.get("floor", 4) },
                            { "phase", "observed" },
                            { "outcome", "recorded" },
                            { "details", event.details },
                        });
                    comparison_seen = true;
                    require(event.details.at("missing_revealed_nodes").as_array().empty(), "false missing reveal");
                    const bool unknown_mist = after.get("utopia_ideology", "") == "diffused-mist" &&
                                              after.get("utopia_status", "") == "abstained";
                    require(
                        event.details.get("consistency", "") == (unknown_mist ? "未确定" : "一致"),
                        "uncertainty was hidden or invented");
                    if (after.get("entered_event_node", InvalidNodeId) != InvalidNodeId) {
                        require(event.details.get("completed_event_reveal_confirmed", false), "page witness missing");
                    }
                }
            }
            require(comparison_seen, "reveal comparison was skipped");
            if (after.get("entered_event_node", InvalidNodeId) != InvalidNodeId) {
                const Node* note =
                    session.exploration_notebook().snapshot().find_node(after.get("entered_event_node", InvalidNodeId));
                require(
                    note != nullptr && note->name == "和平守卫者" && note->progress == NodeProgress::Completed,
                    "event notebook regressed");
            }
        });
    }
    std::ofstream("reveal-events.json") << json::value(contract_events).format();
}
