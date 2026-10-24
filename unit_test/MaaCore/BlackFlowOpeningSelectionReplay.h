#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowOpeningSelection.h"

struct OpeningRolesHarness
{
    int clicks = 0, swipes = 0, samples = 0;
    bool wrong = false, bypass = false, stuck = false, unknown = false, click_ok = true, cancelled = false;
    bool target_visible = true;
    std::string fail_save;
    std::vector<std::string> kinds;
    OpeningRolesObservation current;

    OpeningRolesObservation observe()
    {
        ++samples;
        current = {};
        current.image = cv::Mat(720, 1280, CV_8UC3, cv::Scalar(clicks, swipes, 0));
        if (unknown) return current;
        if (clicks >= 2 && !stuck || bypass) {
            current.voucher_page = true;
            current.vouchers = { { { 290, 315, 125, 30 }, .99, wrong ? "近卫招募券" : "重装招募券" },
                                 { { 575, 315, 125, 30 }, .99, wrong ? "辅助招募券" : "术师招募券" },
                                 { { 865, 315, 125, 30 }, .99, "医疗招募券" } };
        }
        else {
            current.candidates = { { { 600, 450, 150, 30 }, .99, target_visible || swipes ? "坚不可摧" : "取长补短" } };
            if (clicks) current.prompt = Rect { 610, 538, 130, 24 };
        }
        return current;
    }
    bool run()
    {
        return select_opening_roles(
            [&] { return observe(); },
            [&](std::string_view kind, json::object details, std::shared_ptr<cv::Mat> image) {
                if (kind == fail_save) return false;
                require(image && !image->empty() && image->at<cv::Vec3b>(0,0)[0] == clicks,
                        "opening diagnostic did not preserve the decision frame");
                kinds.emplace_back(kind);
                if (kind == "roles.verified") require(opening_vouchers_match(current), "verification preceded actual vouchers");
                return true;
            },
            [&](const Point&) { ++clicks; return click_ok; },
            [&] { ++swipes; return true; },
            [](int) { return true; },
            [&] { return cancelled; });
    }
};

template <typename Test>
void opening_selection_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-opening/20261024";
    const auto wrong = MAA_NS::imread(root / "wrong-vouchers.jpg");
    const auto support = MAA_NS::imread(root / "last-support.jpg");
    for (int index = 1; index <= 3; ++index) {
        test(("opening original support header is recognized " + std::to_string(index)).c_str(), [&] {
            const auto image = MAA_NS::imread(root.parent_path() / ("candidates-" + std::to_string(index) + ".jpg"));
            OCRer titles(image);
            titles.set_task_info("BlackFlow@Roguelike@AutomationCollectionPreferredStartReward");
            const auto detected = titles.analyze();
            require(detected && !detected->empty(), "original support titles missing");
            const auto transition = inspect_opening_reward_transition(image, detected->front().text);
            require(transition.same_reward_page && !transition.destination_ready, "original reward header cannot authorize confirmation");
        });
    }
    test("002 opening actual wrong vouchers cannot pass group verification", [&] {
        const auto frame = inspect_opening_roles(wrong);
        require(frame.voucher_page && frame.vouchers.size() == 3, "actual initial recruitment page not recognized");
        require(!opening_vouchers_match(frame) && frame.candidates.empty(), "actual wrong vouchers accepted as target combo");
    });
    test("002 last support page cannot be mistaken for a recruitment group", [&] {
        require(inspect_opening_roles(support).candidates.empty(), "support text matched a combo");
        const auto transition = inspect_opening_reward_transition(support, "空间租赁");
        require(transition.same_reward_page && !transition.destination_ready, "original support falsely completed");
        const auto after = inspect_opening_reward_transition(wrong, "空间租赁");
        require(!after.same_reward_page && after.destination_ready, "actual recruitment transition not detected");
    });
    test("opening selects and confirms target group before verifying vouchers", [] {
        OpeningRolesHarness h;
        require(h.run() && h.clicks == 2 && h.swipes == 0, "normal combo could not be selected");
        require(h.kinds == std::vector<std::string>{"roles.observed","roles.selecting","roles.confirming","roles.verified"},
                "selection evidence lifecycle incomplete");
    });
    test("opening finds target combo without the default-card picture", [] {
        OpeningRolesHarness h; h.target_visible = false;
        require(h.run() && h.swipes == 1 && h.clicks == 2, "combo OCR did not recover an offscreen target");
    });
    test("opening wrong voucher receipt fails after one confirmation", [] {
        OpeningRolesHarness h; h.wrong = true;
        require(!h.run() && h.clicks == 2 && h.kinds.back() == "failed", "wrong combo continued or received stale clicks");
    });
    test("opening skipped combo stops before any voucher click", [] {
        OpeningRolesHarness h; h.bypass = true; h.wrong = true;
        require(!h.run() && h.clicks == 0 && h.samples == 2, "bypass was accepted or waited for a missing voucher");
    });
    test("opening unknown page never authorizes clicks or swipes", [] {
        OpeningRolesHarness h; h.unknown = true;
        require(!h.run() && h.clicks == 0 && h.swipes == 0 && h.samples == 48, "unknown page clicked or waited forever");
    });
    test("opening stuck confirmation never receives another blind click", [] {
        OpeningRolesHarness h; h.stuck = true;
        require(!h.run() && h.clicks == 2 && h.samples == 48, "stuck combo clicked repeatedly");
    });
    for (const auto* kind : {"roles.observed","roles.selecting","roles.confirming","roles.verified"}) {
        test((std::string("opening evidence failure prevents completion: ") + kind).c_str(), [&] {
            OpeningRolesHarness h; h.fail_save = kind;
            require(!h.run(), "missing evidence reported success");
            require(h.clicks <= (std::string_view(kind) == "roles.confirming" ? 1 : std::string_view(kind) == "roles.verified" ? 2 : 0),
                    "click preceded evidence persistence");
        });
    }
    test("opening rejected device input cannot establish a combo", [] {
        OpeningRolesHarness h; h.click_ok = false;
        require(!h.run() && h.clicks == 1, "failed touch was retried blindly");
    });
    test("opening cancellation never starts selection", [] {
        OpeningRolesHarness h; h.cancelled = true;
        require(!h.run() && h.clicks == 0, "cancelled task clicked");
    });
    test("opening failure persists its actual frame and incomplete strategy result", [&] {
        auto config = std::make_shared<RoguelikeConfig>();
        require(config->verify_and_load_params(json::object{{"theme","BlackFlow"},{"mode",30002}}), "config");
        auto session = std::make_shared<BlackFlowSession>();
        auto port = std::make_shared<Port>();
        BlackFlowLifecycleTaskPlugin lifecycle({}, nullptr, "Roguelike", config, nullptr, session, port);
        require(lifecycle.load_params(json::object{{"blackflow_strategy","automation_collection"}}), "lifecycle");
        auto details = opening_roles_details(inspect_opening_roles(wrong));
        details["failure_reason"] = "roles_selection_bypassed";
        require(lifecycle.record_opening_evidence("failed", details, std::make_shared<cv::Mat>(wrong)), "save failure");
        require(session->result() && !session->result()->succeeded && session->result()->next_action == "stop_task",
                "opening failure lost its strategy result");
        require(port->images.back() && cv::norm(*port->images.back(),wrong,cv::NORM_INF) == 0,
                "failure image was dropped");
        const auto& event = port->events.back();
        json::array shared { json::object {
            {"schema_version",1},{"sequence",1},{"elapsed_ms",1},{"timestamp","2026-10-24T04:53:56Z"},
            {"action",event.action},{"phase",event.phase},{"outcome",event.outcome},
            {"level",std::string(to_string(event.level))},{"task",event.task},{"state",event.state},
            {"details",event.details},{"floor",0},{"map_generation",0},
            {"image",json::object{{"path","images/wrong-vouchers.jpg"}}},
        } };
        std::ofstream("opening-diagnostic-events.json") << json::value(std::move(shared)).format();
    });
}
