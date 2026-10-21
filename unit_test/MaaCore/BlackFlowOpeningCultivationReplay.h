#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowOpeningCultivation.h"

struct OpeningCultivationHarness
{
    OpeningCultivationObservation initial;
    std::vector<OpeningCultivationObservation> frames;
    std::vector<RunLogEvent> events;
    std::string fail_save;
    int samples = 0, clicks = 0, cancel_at = -1;
    bool click_ok = true, saved_image = false;

    bool run(json::object details)
    {
        details["observation_index"] = 1;
        return claim_opening_cultivation(
            initial, std::move(details),
            [&] { return frames.at(std::min(samples++, static_cast<int>(frames.size()) - 1)); },
            [&](std::string_view kind, json::object evidence, std::shared_ptr<cv::Mat> image) {
                if (kind == fail_save) {
                    return false;
                }
                if (kind == "observed") {
                    require(clicks == 0 && image && cv::norm(*image, initial.image, cv::NORM_INF) == 0,
                            "reward evidence was not saved before clicking");
                    saved_image = true;
                }
                if (kind == "confirming") {
                    require(saved_image, "confirmation preceded observation persistence");
                }
                events.emplace_back(opening_cultivation_event(kind, std::move(evidence), {}));
                return true;
            },
            [&](const Rect&) { ++clicks; return click_ok; },
            [](int delay) { require(delay == 250, "unexpected harvest wait"); return true; },
            [&] { return cancel_at >= 0 && samples >= cancel_at; });
    }
};

template <typename Test>
void opening_cultivation_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto image = MAA_NS::imread(
        repo / "unit_test/MaaCore/fixtures/blackflow-opening-cultivation/20261021/ingots-5.png");
    require(!image.empty(), "002 opening cultivation frame missing");
    for (const char* parent : { "Squad-EnterPoint", "SquadConfirm" }) {
        test((std::string("002 cultivation reward is reachable after ") + parent).c_str(), [&] {
            PipelineAnalyzer analyzer(image);
            analyzer.set_tasks(Task.get(std::string("BlackFlow@Roguelike@") + parent)->next);
            const auto result = analyzer.analyze();
            require(result && result->task_ptr->name == "BlackFlow@Roguelike@OpeningCultivationHarvest",
                    "opening cultivation card has no guarded claim branch");
        });
    }
    const auto initial = inspect_opening_cultivation(image);
    const auto details = opening_cultivation_details(image);
    test("harvest waits for the claim button before taking the OCR frame", [&] {
        int samples = 0;
        const auto ready = wait_for_opening_cultivation(
            [&] {
                auto frame = initial;
                if (++samples == 1) {
                    frame.confirm_button.reset();
                }
                return frame;
            }, [](int) { return true; }, [] { return false; });
        require(ready.confirm_button.has_value() && samples == 2, "button animation was treated as a fatal opening error");
    });
    test("harvest readiness wait is bounded on a missing button", [&] {
        int samples = 0;
        const auto ready = wait_for_opening_cultivation(
            [&] { ++samples; return OpeningCultivationObservation {}; },
            [](int) { return true; }, [] { return false; });
        require(!ready.confirm_button && samples == 12, "missing harvest UI waited forever");
    });
    test("002 native harvest OCR retains ingots and amount five", [&] {
        require(initial.page_visible && initial.confirm_button.has_value(), "harvest title or button not recognized");
        require(details.get("recognition_status", "") == "recognized", "reward observation incomplete");
        const auto& items = details.at("items").as_array();
        require(items.size() == 1 && items[0].get("name", "") == "源石锭" && items[0].get("amount", -1) == 5,
                "wrong cultivation reward content");
    });
    for (int index = 1; index <= 3; ++index) {
        const auto rewards = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-opening" /
                                           ("candidates-" + std::to_string(index) + ".jpg"));
        test("ordinary start reward cannot be claimed as cultivation", [&] {
            const auto observation = inspect_opening_cultivation(rewards);
            require(!observation.page_visible && !observation.confirm_button, "ordinary reward passed harvest page guard");
        });
    }
    test("harvest title is required even when the check button remains visible", [&] {
        auto no_title = image.clone();
        no_title(cv::Rect(550,14,220,45)).setTo(cv::Scalar::all(0));
        require(!inspect_opening_cultivation(no_title).page_visible, "generic check button accepted without title");
    });
    auto next = Task.get("BlackFlow@Roguelike@OpeningCultivationHarvest")->next;
    std::erase(next, "BlackFlow@Roguelike@OpeningCultivationHarvest");
    const auto successor = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-opening/candidates-1.jpg");
    PipelineAnalyzer analyzer(successor);
    analyzer.set_tasks(next);
    const auto recognized = analyzer.analyze();
    require(recognized && recognized->task_ptr->name == "BlackFlow@Roguelike@LastReward-EnterPoint", "normal reward successor is unreachable");
    OpeningCultivationObservation ready { .image = successor, .next_page_ready = true };
    test("harvest saves contents and confirms two successor frames before continuing", [&] {
        OpeningCultivationHarness h { .initial = initial, .frames = { {}, ready, ready } };
        require(h.run(details) && h.clicks == 1 && h.samples == 3, "harvest did not settle on successor");
        require(h.events.size() == 3 && h.events[1].phase == "before-click" &&
                    h.events.back().action == "start.cultivation.reward.confirmed" &&
                    h.events.back().details.get("claim_status", "") == "confirmed", "harvest lifecycle is incomplete");
        json::array shared;
        for (std::size_t index = 0; index < h.events.size(); ++index) {
            const auto& event = h.events[index];
            json::object row {
                { "schema_version", 1 }, { "sequence", index + 1 }, { "elapsed_ms", index + 1 },
                { "timestamp", "2026-10-21T06:31:56Z" }, { "action", event.action },
                { "phase", event.phase }, { "outcome", event.outcome }, { "level", std::string(to_string(event.level)) },
                { "task", event.task }, { "state", event.state }, { "details", event.details },
                { "floor", 0 }, { "map_generation", 0 },
            };
            if (index == 0) {
                row["image"] = json::object { { "path", "images/opening-cultivation.jpg" } };
            }
            shared.emplace_back(std::move(row));
        }
        std::ofstream("opening-cultivation-events.json") << json::value(std::move(shared)).format();
        require(MAA_NS::imwrite(std::filesystem::path("opening-cultivation.jpg"), image,
                               { cv::IMWRITE_JPEG_QUALITY, 76 }), "JPEG contract sample could not be saved");
    });
    for (const char* kind : { "observed", "confirming" }) {
        test((std::string("harvest save failure prevents clicks: ") + kind).c_str(), [&] {
            OpeningCultivationHarness h { .initial = initial, .frames = { ready }, .fail_save = kind };
            require(!h.run(details) && h.clicks == 0, "unrecorded cultivation claim was clicked");
        });
    }
    test("static harvest card stops after two attempts", [&] {
        OpeningCultivationHarness h { .initial = initial, .frames = { initial } };
        require(!h.run(details) && h.clicks == 2 && h.samples == 12, "harvest retries are unbounded");
        require(h.events.back().outcome == "failed", "static harvest claimed success");
    });
    test("missing button cannot claim cultivation", [&] {
        auto hidden = initial;
        hidden.confirm_button.reset();
        OpeningCultivationHarness h { .initial = hidden, .frames = { ready } };
        require(!h.run(details) && h.clicks == 0, "unverified button clicked");
    });
    test("unknown transition cannot establish cultivation receipt", [&] {
        OpeningCultivationHarness h { .initial = initial, .frames = { {} } };
        require(!h.run(details) && h.clicks == 1 && h.samples == 20, "unknown page accepted as receipt or clicked again");
    });
    test("interruption stops the pending claim", [&] {
        OpeningCultivationHarness h { .initial = initial, .frames = { {} }, .cancel_at = 1 };
        require(!h.run(details) && h.clicks == 1, "interruption ignored");
    });
    test("failed click cannot be recorded as confirmed", [&] {
        OpeningCultivationHarness h { .initial = initial, .frames = { ready }, .click_ok = false };
        require(!h.run(details) && h.events.back().outcome == "failed", "failed input accepted");
    });
    test("failed final persistence retains the pending claim", [&] {
        OpeningCultivationHarness h { .initial = initial, .frames = { ready }, .fail_save = "confirmed" };
        require(!h.run(details) && h.events.back().details.get("claim_status", "") == "unconfirmed", "failed final write accepted");
    });
    test("harvest OCR misses remain unknown alongside the saved image", [&] {
        const auto unreadable = opening_cultivation_details(cv::Mat::zeros(720,1280,CV_8UC3));
        require(unreadable.get("recognition_status", "") == "unreadable" && unreadable.at("items").as_array().empty(),
                "OCR miss invented an item or amount");
        OpeningCultivationHarness h { .initial = initial, .frames = { ready } };
        require(h.run(unreadable) && h.saved_image && h.events.front().details.get("recognition_status", "") == "unreadable",
                "unreadable content lost its original screenshot");
    });
}
