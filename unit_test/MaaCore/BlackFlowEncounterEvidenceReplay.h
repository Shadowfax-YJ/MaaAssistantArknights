#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowEncounterEvidence.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.h"
#include "Utils/WorkingDir.hpp"

template <class Test>
void encounter_evidence_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    const auto fixture =
        json::open(repo / "unit_test/MaaCore/fixtures/blackflow-encounter/20261024/lifecycle-payloads.json").value();
    const auto root = std::filesystem::current_path() / "enc";
    const auto previous = UserDir.empty() ? std::filesystem::current_path() : UserDir.get();
    json::array exported;
    for (const auto& item : fixture.as_array()) {
        test("encounter actual Session port logger saves choices and exchange evidence", [&] {
            const auto directory = root / std::to_string(exported.size());
            std::filesystem::create_directories(directory);
            require(UserDir.set(directory), "test UserDir");
            auto session = std::make_shared<BlackFlowSession>();
            require(session->initialize("automation_collection") && session->set_current_floor(2), "initialize");
            require(session->update(map_frame(2, 1, 0, "incident")), "map");
            commit_move(*session, 2);
            require(session->mark_page_running(), "running");
            const auto payloads = item.at("payloads").as_array();
            const auto title = payloads.at(0).get("event_name", std::string());
            require(session->observe_page_content(title, "RoguelikeEvent"), "title");
            auto source = std::make_shared<BlackFlowMapObservationSource>();
            BlackFlowTaskPort port({ }, nullptr, "Roguelike", source);
            port.set_collection_popup_session(session);
            const cv::Mat image(720, 1280, CV_8UC3, cv::Scalar(16, 32, 64));
            for (const auto& payload : payloads) {
                auto details = payload.at("details").as_object();
                details["run_revision"] = session->run_revision();
                details["page_revision"] = session->page_context()->page_revision;
                std::string error;
                require(
                    record_encounter_evidence(
                        *session,
                        port,
                        title,
                        payload.get("phase", std::string()),
                        details,
                        payload.get("has_image", false) ? image : cv::Mat { },
                        &error),
                    error);
            }
            std::filesystem::path run;
            for (const auto& entry : std::filesystem::directory_iterator(directory / "debug/BlackFlow")) {
                if (entry.is_directory() && std::filesystem::exists(entry.path() / "run-events.jsonl")) {
                    run = entry.path();
                }
            }
            require(!run.empty(), "real run missing");
            std::ifstream stream(run / "run-events.jsonl");
            std::string line;
            json::array events;
            std::vector<json::value> rows;
            int confirmations = 0;
            for (; std::getline(stream, line);) {
                auto event = json::parse(line).value();
                rows.push_back(event);
                events.emplace_back(event);
                require(
                    event.get("transaction_id", std::string()) == session->page_context()->transaction_id,
                    "scope transaction lost");
                require(
                    event.get("details", "source_context", "node", std::uint64_t(0)) == session->page_context()->node,
                    "source node lost");
                if (event.get("action", std::string()) == "node.encounter.choice.confirmed") {
                    ++confirmations;
                }
                if (event.contains("image")) {
                    const auto member = event.get("image", "path", std::string());
                    require(member.starts_with("collection-popups/floor-2/node-"), "image left event node");
                    require(!MAA_NS::imread(run / member).empty(), "archived image unreadable");
                }
            }
            require(confirmations > 0, "actual choice missing");
            std::string error;
            require(validate_run_log_replay_stream(rows, &error), error);
            exported.emplace_back(json::object { { "case", item.at("case") }, { "events", std::move(events) } });
            UserDir.set(previous);
        });
    }
    UserDir.set(previous);
    std::ofstream(root / "producer-events.json") << json::value(exported).to_string();
    test("captured cargo immediate collection reward is a valid passive confirmation anchor", [&] {
        const auto image=MAA_NS::imread(repo/"unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg");
        require(!image.empty(),"cargo actual reward frame missing");
        Matcher matcher(image);matcher.set_task_info("BlackFlow@Roguelike@CloseCollection");
        require(matcher.analyze().has_value(),"actual cargo reward cannot be recognized");
    });
    test("encounter bridge rejects missing image stale scope and failed storage", [&] {
        BlackFlowSession session;
        require(
            session.initialize("automation_collection") && session.set_current_floor(2) &&
                    session.update(map_frame(2, 1, 0, "incident")),
            "initialize");
        commit_move(session, 2);
        require(session.mark_page_running() && session.observe_page_content("货从口出", "RoguelikeEvent"), "page");
        Port port;
        json::object details { { "choice_id", "choice-test" },
                               { "capture_kind", "encounter_choice" },
                               { "evidence_sequence", 1 },
                               { "run_revision", session.run_revision() },
                               { "page_revision", session.page_context()->page_revision } };
        require(!record_encounter_evidence(session, port, "货从口出", "attempt", details, { }), "empty image accepted");
        require(session.result() && session.result()->next_action == "stop_task", "save failure left runnable session");
        details["page_revision"] = session.page_context()->page_revision + 1;
        require(!record_encounter_evidence(session, port, "货从口出", "planned", details, { }), "stale page accepted");
        details["page_revision"] = session.page_context()->page_revision;
        details["run_revision"] = session.run_revision() + 1;
        require(!record_encounter_evidence(session, port, "货从口出", "planned", details, { }), "stale run accepted");
        details["run_revision"] = session.run_revision();
        require(
            !record_encounter_evidence(session, port, "货从口出", "invalid", details, { }),
            "unsupported phase accepted");
        BlackFlowTaskPort failed_port({ }, nullptr, "Roguelike", nullptr);
        failed_port.set_collection_popup_session(std::make_shared<BlackFlowSession>());
        require(
            !record_encounter_evidence(session, failed_port, "货从口出", "planned", details, { }),
            "failed log hidden");
    });
}
