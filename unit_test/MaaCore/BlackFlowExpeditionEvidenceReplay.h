#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowExpeditionEvidence.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.h"
#include "Utils/WorkingDir.hpp"
#include <map>

template <class Test>
void expedition_evidence_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    const auto root = std::filesystem::current_path() / "exp";
    const auto fixture =
        json::open(repo / "unit_test/MaaCore/fixtures/blackflow-expedition/20261024/lifecycle-payloads.json").value();
    std::map<std::string, json::array> cases;
    for (const auto& item : fixture.as_array()) {
        cases[item.get("details", "expedition_id", std::string())].emplace_back(item);
    }
    json::array exported;
    const auto previous_user_directory = UserDir.empty() ? std::filesystem::current_path() : UserDir.get();
    for (const auto& [id, payloads] : cases) {
        test("expedition actual Session port logger stores replayable node images and lifecycle", [&] {
            const auto case_root = root / std::to_string(exported.size());
            std::filesystem::create_directories(case_root);
            require(UserDir.set(case_root), "isolated test directory");
            auto session = std::make_shared<BlackFlowSession>();
            require(session->initialize("automation_collection"), "initialize expedition");
            require(session->set_current_floor(2), "floor");
            require(session->update(map_frame(2, 1, 0, "expedition")), "map");
            commit_move(*session, 2);
            require(session->mark_page_running(), "running expedition");
            require(session->observe_page_content("未涉足之树", "RoguelikeEvent"), "event name");
            auto source = std::make_shared<BlackFlowMapObservationSource>();
            BlackFlowTaskPort port({ }, nullptr, "Roguelike", source);
            port.set_collection_popup_session(session);
            const cv::Mat image(720, 1280, CV_8UC3, cv::Scalar(8, 16, 32));
            for (const auto& payload : payloads) {
                auto details = payload.at("details").as_object();
                details["run_revision"] = session->run_revision();
                details["page_revision"] = session->page_context()->page_revision;
                const std::string phase = payload.get("phase", std::string());
                std::string error;
                require(
                    record_expedition_evidence(
                        *session,
                        port,
                        phase,
                        details,
                        payload.get("has_image", false) ? image : cv::Mat { },
                        &error),
                    error);
            }
            std::filesystem::path run;
            for (const auto& entry : std::filesystem::directory_iterator(case_root / "debug/BlackFlow")) {
                if (entry.is_directory() && std::filesystem::exists(entry.path() / "run-events.jsonl") &&
                    (run.empty() || std::filesystem::last_write_time(entry.path() / "run-events.jsonl") >
                                        std::filesystem::last_write_time(run / "run-events.jsonl"))) {
                    run = entry.path();
                }
            }
            require(!run.empty(), "no actual archive directory");
            std::ifstream stream(run / "run-events.jsonl");
            std::string line;
            std::vector<json::value> rows;
            json::array events;
            int captures = 0, confirmations = 0;
            for (; std::getline(stream, line);) {
                const auto row = json::parse(line).value();
                rows.push_back(row);
                events.emplace_back(row);
                require(
                    row.get("transaction_id", std::string()) == session->page_context()->transaction_id,
                    "transaction lost");
                require(
                    row.get("details", "source_context", "node", std::uint64_t(0)) == session->page_context()->node,
                    "node scope lost");
                if (row.get("action", std::string()) == "node.expedition.dispatch_confirmed") {
                    ++confirmations;
                }
                if (row.contains("image")) {
                    const auto member = row.get("image", "path", std::string());
                    require(
                        member.starts_with("collection-popups/floor-2/node-"),
                        "image left its expedition node directory");
                    require(!MAA_NS::imread(run / member).empty(), "archive image unreadable");
                    ++captures;
                }
            }
            require(confirmations == 1 && captures >= 3, "missing or duplicate expedition evidence");
            std::ifstream readable(run / "run.log");
            const std::string readable_text((std::istreambuf_iterator<char>(readable)), { });
            require(
                readable_text.find("selected_option_index_from_top=") != std::string::npos &&
                    readable_text.find("operator_name=") != std::string::npos &&
                    readable_text.find("elite_before=1") != std::string::npos,
                "readable archive log omitted choice, operator or pre-dispatch elite");
            std::string error;
            require(validate_run_log_replay_stream(rows, &error), error);
            exported.emplace_back(json::object { { "case", id }, { "events", std::move(events) } });
            UserDir.set(previous_user_directory);
        });
    }
    UserDir.set(previous_user_directory);
    std::ofstream(root / "producer-events.json") << json::value(std::move(exported)).to_string();
    test("expedition bridge rejects missing frames and stale contexts", [&] {
        BlackFlowSession session;
        require(session.initialize("automation_collection"), "initialize");
        require(session.set_current_floor(2) && session.update(map_frame(2, 1, 0, "expedition")), "map");
        commit_move(session, 2);
        require(session.mark_page_running(), "running");
        require(session.observe_page_content("未涉足之树", "RoguelikeEvent"), "title");
        Port port;
        json::object details { { "run_revision", session.run_revision() },
                               { "page_revision", session.page_context()->page_revision },
                               { "expedition_id", "test-expedition" },
                               { "expedition_phase", "dispatch_prepared" },
                               { "evidence_sequence", 1 } };
        require(
            !record_expedition_evidence(session, port, "dispatch_prepared", details, { }),
            "missing image accepted");
        require(
            session.result().has_value() && session.result()->next_action == "stop_task",
            "failed evidence left a complete/runnable session");
        details["expedition_phase"] = "choice_planned";
        details["page_revision"] = session.page_context()->page_revision + 1;
        require(!record_expedition_evidence(session, port, "choice_planned", details, { }), "stale page accepted");
        details["page_revision"] = session.page_context()->page_revision;
        details["run_revision"] = session.run_revision() + 1;
        require(!record_expedition_evidence(session, port, "choice_planned", details, { }), "stale run accepted");
        details["run_revision"] = session.run_revision();
        require(
            !record_expedition_evidence(session, port, "unsupported_phase", details, { }),
            "unknown phase accepted");
        BlackFlowTaskPort failed_port({ }, nullptr, "Roguelike", nullptr);
        failed_port.set_collection_popup_session(std::make_shared<BlackFlowSession>());
        require(!record_expedition_evidence(session, failed_port, "choice_planned", details, { }), "failed log hidden");
        require(port.events.empty(), "invalid evidence was persisted");
    });
}
