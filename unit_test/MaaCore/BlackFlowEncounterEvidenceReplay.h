#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowEncounterEvidence.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.h"
#include "Utils/WorkingDir.hpp"
#include "Vision/Roguelike/BlackFlow/SacrificeRecognition.h"

template <class Test>
void encounter_evidence_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    auto fixture =
        json::open(repo / "unit_test/MaaCore/fixtures/blackflow-encounter/20261024/lifecycle-payloads.json").value();
    const auto decline_root = repo / "unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-decline";
    fixture.as_array().emplace_back(json::open(decline_root / "choice-payloads.json").value());
    const auto empty_root = repo / "unit_test/MaaCore/fixtures/blackflow-sacrifice/20261025";
    const auto empty_payloads = json::open(empty_root / "lifecycle-payloads.json").value();
    for (const auto& item : empty_payloads.as_array()) {
        fixture.as_array().emplace_back(item);
    }
    const auto generic_root = repo / "unit_test/MaaCore/fixtures/blackflow-encounter/20261025";
    const auto generic_payloads = json::open(generic_root / "lifecycle-payloads.json").value();
    for(const auto& item:generic_payloads.as_array()) fixture.as_array().emplace_back(item);
    const auto civ_payloads = json::open(std::filesystem::current_path()/"civilization-payloads.json").value();
    for(const auto& item:civ_payloads.as_array())fixture.as_array().emplace_back(item);
    const auto root = std::filesystem::current_path() / "enc";
    const auto previous = UserDir.empty() ? std::filesystem::current_path() : UserDir.get();
    json::array exported;
    json::array empty_exported;
    json::array generic_exported;
    json::array civ_exported;
    int case_index = 0;
    for (const auto& item : fixture.as_array()) {
        test("encounter actual Session port logger saves choices and exchange evidence", [&] {
            const auto directory = root / std::to_string(case_index++);
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
            const bool decline = item.get("case", std::string()) == "cargo-decline-ending";
            const cv::Mat image = decline ? MAA_NS::imread(decline_root / "evidence-frame.jpg")
                                          : cv::Mat(720, 1280, CV_8UC3, cv::Scalar(16, 32, 64));
            require(!image.empty(), "encounter evidence image missing");
            const cv::Mat before = decline ? MAA_NS::imread(decline_root / "options.jpg") : image;
            require(!before.empty(), "encounter candidate image missing");
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
                        payload.get("has_image", false)
                            ? payload.get("phase", std::string()) == "confirmed" ? image : before
                            : cv::Mat { },
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
            const bool inventory_only=item.get("case",std::string()).starts_with("civilization-");
            require(inventory_only ? confirmations==0 : confirmations>0,"scan evidence became a choice or choice receipt disappeared");
            std::string error;
            require(validate_run_log_replay_stream(rows, &error), error);
            if (decline) {
                require(!session->terminated() && session->page_context()->stage == PageExecutionStage::Running,
                        "decline confirmation completed or stopped the node before the original ending flow");
                json::array shared { json::object {
                    { "case", item.at("case") }, { "events", std::move(events) },
                    { "evidence_frames", json::object {
                        { "candidates", "options.jpg" }, { "attempt", "options.jpg" },
                        { "confirmed", "evidence-frame.jpg" } } },
                } };
                std::ofstream(root / "cargo-decline-producer-events.json") << json::value(shared).to_string();
            }
            else if (item.get("case", std::string()) == "sacrifice-confirmed-empty" ||
                     item.get("case", std::string()) == "sacrifice-unconfirmed-list") {
                empty_exported.emplace_back(json::object { { "case", item.at("case") }, { "events", std::move(events) } });
            }
            else if(inventory_only) {
                civ_exported.emplace_back(json::object{{"case",item.at("case")},{"events",std::move(events)}});
            }
            else if(item.get("case",std::string()).starts_with("generic-")) {
                generic_exported.emplace_back(json::object{{"case",item.at("case")},{"events",std::move(events)}});
            }
            else {
                exported.emplace_back(json::object { { "case", item.at("case") }, { "events", std::move(events) } });
            }
            UserDir.set(previous);
        });
    }
    UserDir.set(previous);
    test("direct event result uses actual port logger and current choice reference without inventing an award",[&]{
        const auto directory=root/"direct-receipt";std::filesystem::create_directories(directory);require(UserDir.set(directory),"UserDir");
        auto session=std::make_shared<BlackFlowSession>();
        require(session->initialize("automation_collection")&&session->set_current_floor(2)&&session->update(map_frame(2,1,0,"incident")),"initialize");
        commit_move(*session,2);require(session->mark_page_running()&&session->observe_page_content("得偿","RoguelikeEvent"),"running");
        BlackFlowTaskPort port({},nullptr,"Roguelike",std::make_shared<BlackFlowMapObservationSource>());port.set_collection_popup_session(session);
        auto image=MAA_NS::imread(repo/"unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg");
        json::object details{{"capture_kind","encounter_choice"},{"choice_id","direct-choice"},{"candidate_capture_id","direct-candidate"},
            {"evidence_sequence",1},{"run_revision",session->run_revision()},{"page_revision",session->page_context()->page_revision}};
        std::string error;require(record_encounter_evidence(*session,port,"得偿","candidates",details,image,&error),error);
        require(port.capture_reward_result("BlackFlow@Roguelike@StageEncounterResult",{{"receipt_status","event_stage_observed"}},image,&error),error);
        require(!port.capture_reward_result("test",{},cv::Mat{},&error),"missing result frame accepted");
        json::array rows;
        for(const auto& entry:std::filesystem::directory_iterator(directory/"debug/BlackFlow"))if(entry.is_directory()){
            std::ifstream stream(entry.path()/"run-events.jsonl");std::string line;while(std::getline(stream,line))rows.emplace_back(json::parse(line).value());
        }
        const auto& result=rows.at(rows.size()-1);require(result.get("action",std::string())=="node.reward.result","actual result not logged");
        require(result.get("details","choice_reference","choice_id",std::string())=="direct-choice","result linked to another choice");
        require(result.get("details","acquisition_status",std::string())=="unresolved","raw result became acquisition");
        generic_exported.emplace_back(json::object{{"case","direct-event-result"},{"events",std::move(rows)}});
        UserDir.set(previous);
    });
    test("changed event title receipt keeps the same actual node and transaction scope",[&]{
        BlackFlowSession session;require(session.initialize("automation_collection")&&session.set_current_floor(2)&&session.update(map_frame(2,1,0,"incident")),"initialize");
        commit_move(session,2);require(session.mark_page_running()&&session.observe_page_content("第二阶段","RoguelikeEvent"),"running");
        Port port;cv::Mat image(720,1280,CV_8UC3,cv::Scalar::all(30));
        json::object d{{"capture_kind","encounter_choice"},{"choice_id","first-stage"},{"result_event_name","第二阶段"},
            {"confirmation_basis","next_event_candidate_page"},{"evidence_sequence",1},
            {"run_revision",session.run_revision()},{"page_revision",session.page_context()->page_revision}};
        require(record_encounter_evidence(session,port,"第一阶段","confirmed",d,image),"new title rejected the actual old source");
        d["page_revision"]=session.page_context()->page_revision+1;
        require(!record_encounter_evidence(session,port,"第一阶段","confirmed",d,image),"title exception bypassed stale page guard");
    });
    std::ofstream(root / "civilization-producer-events.json") << json::value(civ_exported).to_string();
    std::ofstream(root / "generic-producer-events.json") << json::value(generic_exported).to_string();
    std::ofstream(root / "producer-events.json") << json::value(exported).to_string();
    std::ofstream(root / "sacrifice-empty-producer-events.json") << json::value(empty_exported).to_string();
    test("sacrifice captured real picker keeps all five cards", [&] {
        const auto image = MAA_NS::imread(empty_root / "picker-with-items.jpg");
        require(!image.empty(), "real picker image missing");
        const auto list = perception::sacrifice_item_list(image);
        require(list && list->cards.size() == 5 && list->empty_evidence.empty(), "real cards marked empty");
    });
    test("sacrifice damaged card detection with nonzero inventory remains unknown", [&] {
        auto image = MAA_NS::imread(empty_root / "picker-with-items.jpg");
        require(!image.empty(), "real picker image missing");
        make_roi(image, Rect { 680, 175, 600, 350 }).setTo(cv::Scalar(40,40,40));
        require(!perception::sacrifice_item_list(image), "detection failure became empty inventory");
    });
    test("sacrifice empty and wrong page images remain unknown", [&] {
        require(!perception::sacrifice_item_list({}), "empty capture became empty inventory");
        const auto image = MAA_NS::imread(decline_root / "evidence-frame.jpg");
        require(!image.empty() && !perception::sacrifice_item_list(image), "event ending became empty picker");
        require(!perception::sacrifice_item_list(cv::Mat(720,1280,CV_8UC3,cv::Scalar())), "black capture became empty picker");
    });
    test("captured cargo immediate collection reward is a valid passive confirmation anchor", [&] {
        const auto image=MAA_NS::imread(repo/"unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg");
        require(!image.empty(),"cargo actual reward frame missing");
        Matcher matcher(image);matcher.set_task_info("BlackFlow@Roguelike@CloseCollection");
        require(matcher.analyze().has_value(),"actual cargo reward cannot be recognized");
    });
    test("002 captured cargo decline uses the existing event ending and leaves map return guarded", [&] {
        const auto image = MAA_NS::imread(decline_root / "evidence-frame.jpg");
        const auto options = MAA_NS::imread(decline_root / "options.jpg");
        require(!image.empty() && !options.empty(), "002 decline frame missing");
        Matcher ending(image);
        ending.set_task_info("BlackFlow@Roguelike@CloseEventAfterEncounter");
        require(ending.analyze().has_value(), "actual ending not recognized");
        Matcher before(options);
        before.set_task_info("BlackFlow@Roguelike@CloseEventAfterEncounter");
        require(!before.analyze(), "original options falsely matched the ending");
        auto hidden = image.clone();
        hidden(cv::Rect(567,584,145,136)).setTo(cv::Scalar::all(0));
        Matcher no_button(hidden);
        no_button.set_task_info("BlackFlow@Roguelike@CloseEventAfterEncounter");
        require(!no_button.analyze(), "event name alone established an ending");
        std::string current = "BlackFlow@Roguelike@StageEncounterJudgeOption";
        TaskConstPtr close;
        for (int step = 0; step < 6; ++step) {
            PipelineAnalyzer dispatcher(image);
            dispatcher.set_tasks(Task.get(current)->next);
            const auto hit = dispatcher.analyze();
            require(hit.has_value(), "ending dispatch could not recognize the actual page");
            if (hit->task_ptr->name == "BlackFlow@Roguelike@CloseEventAfterEncounter") {
                close = hit->task_ptr;
                break;
            }
            require(hit->task_ptr->action == ProcessTaskAction::DoNothing &&
                        hit->task_ptr->algorithm == AlgorithmType::JustReturn,
                    "ending was diverted into " + hit->task_ptr->name);
            current = hit->task_ptr->name;
        }
        require(close != nullptr, "choice confirmation did not reach the original ending dispatcher");
        // The map response is a separate captured frame, not an original
        // post-click recording of this stopped run.
        const auto map = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/post-event-map.jpg");
        require(!map.empty(), "map return frame missing");
        PipelineAnalyzer returned(map);
        returned.set_tasks(close->next);
        const auto after = returned.analyze();
        require(after && after->task_ptr->action == ProcessTaskAction::DoNothing &&
                    after->task_ptr->name == "BlackFlow@Roguelike@Stages-MapReady",
                "map after exit received another ending click");
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
    test("cargo unresolved receipt retains a failed StopTask result for the outer task", [&] {
        BlackFlowSession session;
        require(session.initialize("automation_collection") && session.set_current_floor(2) &&
                    session.update(map_frame(2, 1, 0, "incident")), "initialize");
        commit_move(session, 2);
        require(session.mark_page_running() && session.observe_page_content("货从口出", "RoguelikeEvent"), "page");
        Port port;
        json::object details {
            { "capture_kind", "encounter_choice" }, { "choice_id", "cargo-decline-unresolved" },
            { "evidence_sequence", 1 }, { "run_revision", session.run_revision() },
            { "page_revision", session.page_context()->page_revision },
            { "reason", "cargo_choice_transition_not_confirmed" },
        };
        require(record_encounter_evidence(session, port, "货从口出", "unresolved", details, {}), "failure receipt not saved");
        require(session.result() && !session.result()->succeeded &&
                    session.result()->next_action == "stop_task" && session.result()->outcome == "encounter_choice_unresolved",
                "unresolved choice could be reported as task completion");
    });
}
