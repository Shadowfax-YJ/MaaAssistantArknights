#pragma once

#include "Config/Roguelike/RoguelikeCopilotConfig.h"
#include "Task/Roguelike/BlackFlow/BlackFlowDuelPreparationEvidence.h"
#include "Task/Roguelike/RoguelikeBattleTaskPlugin.h"

namespace asst
{
struct DuelPreparationReplayAccess
{
    static void load(RoguelikeBattleTaskPlugin& probe, std::string stage)
    {
        probe.clear();
        require(!probe.m_duel_preparation_tier.has_value(), "last duel tier survived clear");
        probe.m_stage_name = stage;
        probe.m_preparation_deploy_plan = RoguelikeCopilot.get_stage_data(stage)->preparation->deploy_plan;
    }

    static bool choose(RoguelikeBattleTaskPlugin& probe, int tier) { return probe.select_duel_preparation_tier(tier); }
};
}

template <class Test>
void duel_preparation_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    const auto fixture = json::open(repo / "unit_test/MaaCore/fixtures/blackflow-duel/preparation.json").value();
    const auto counter_dir = repo / "unit_test/MaaCore/fixtures/blackflow-duel/20261030";
    const auto counters = json::open(counter_dir / "counters.json").value();
    for (const auto& sample : counters.at("cases").as_array()) {
        const auto name = "native duel completed counter with scenery noise: " + sample.at("name").as_string();
        test(name.c_str(), [&] {
            const auto image = MAA_NS::imread(counter_dir / sample.at("image").as_string());
            require(!image.empty(), "original duel frame missing");
            OCRer start(image);
            start.set_task_info("BlackFlow@Roguelike@BattlePreparationStart");
            OCRer counter(image);
            counter.set_task_info("BlackFlow@Roguelike@BattlePreparationDeployCount");
            OCRer cancel(image);
            cancel.set_task_info("BlackFlow@Roguelike@BattlePreparationDeployCancel");
            require(start.analyze().has_value() && counter.analyze().has_value() && !cancel.analyze(),
                    "original image is not a completed preparation page");
            require(select_preparation_deployment_count(counter.get_result()) == std::pair { 2, 2 },
                    "native counter recognition lost the completed deployment");
            // Stored frames are captured after the failing OCR read; retain the actual
            // runtime results as well, so image timing/compression cannot hide the bug.
            std::vector<TextRect> runtime;
            for (const auto& row : sample.at("ocr").as_array()) {
                const auto& r = row.at("rect");
                runtime.emplace_back(
                    Rect { r[0].as_integer(), r[1].as_integer(), r[2].as_integer(), r[3].as_integer() },
                    row.at("score").as_double(), row.at("text").as_string());
            }
            require(runtime.size() > 1 && select_preparation_deployment_count(runtime) == std::pair { 2, 2 },
                    "recorded noise rejected the valid counter");
        });
    }
    test("native duel choice resets between battles and rejects changing a selected tier", [&] {
        auto config = std::make_shared<RoguelikeConfig>();
        RoguelikeBattleTaskPlugin probe({ }, nullptr, "Roguelike", config, nullptr);
        int saved = 0;
        probe.set_duel_preparation_observer([&](auto phase, auto details, const auto& image) {
            require(phase == "selected" && image.empty(), "choice is not an unplayed selection");
            ++saved;
            return true;
        });
        for (const std::string stage : { "搏杀", "共斗" }) {
            for (int tier = 0; tier < 3; ++tier) {
                DuelPreparationReplayAccess::load(probe, stage);
                require(!DuelPreparationReplayAccess::choose(probe, -1), "invalid tier accepted");
                require(DuelPreparationReplayAccess::choose(probe, tier), "valid native selection rejected");
                require(DuelPreparationReplayAccess::choose(probe, tier), "same tier retry rejected");
                require(!DuelPreparationReplayAccess::choose(probe, (tier + 1) % 3), "retry changed tier");
            }
        }
        require(saved == 6, "retries duplicated selection records");
    });
    test("duel evidence bridge preserves the running node, transaction and pre-start image", [&] {
        BlackFlowSession session;
        require(session.initialize("automation_collection"), "initialize");
        require(session.set_current_floor(4), "floor");
        require(session.update(map_frame(4, 1, 0, "duel")), "map");
        commit_move(session, 4);
        require(session.mark_page_running(), "duel running");
        struct DuelPort : Port
        {
            std::string phase;
            json::object details;
            cv::Mat image;
            bool succeeds = true;
            bool capture_event_detail(
                std::string_view,
                std::string_view p,
                json::object d,
                const cv::Mat& i,
                std::string*) override
            {
                phase = p;
                details = std::move(d);
                image = i.clone();
                return succeeds;
            }
        } port;
        const auto data = fixture[0].at("details").as_object();
        const auto before = fixture[1].at("details").as_object();
        require(record_duel_preparation_evidence(session, port, "selected", data, { }), "selection not saved");
        require(
            port.events.size() == 1 && port.events[0].action == "node.battle.duel.select",
            "wrong selection action");
        require(!port.events[0].transaction_id.empty(), "selection lost transaction attribution");
        require(port.events[0].state.get("page", "node_type", std::string()) == "duel", "duel identity changed");
        require(
            !record_duel_preparation_evidence(session, port, "before-start", before, { }),
            "empty screenshot accepted");
        const cv::Mat frame(720, 1280, CV_8UC3, cv::Scalar(5, 10, 15));
        require(
            record_duel_preparation_evidence(session, port, "before-start", before, frame),
            "screenshot not forwarded");
        require(port.phase == "duel-before-start" && !port.image.empty(), "capture phase or image lost");
        require(
            port.details.get("capture_kind", std::string()) == "duel_preparation_before_start",
            "screenshot role lost");
        port.succeeds = false;
        require(
            !record_duel_preparation_evidence(session, port, "before-start", before, frame),
            "capture failure was hidden");
    });
}
