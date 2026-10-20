#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowEmployCapture.h"

template <typename Test>
void employ_capture_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-employ";
    const auto frames = json::open(root / "frames.json");
    require(frames.has_value(), "employ fixture manifest missing");
    cv::Mat clear, blocked;
    for (const auto& item : frames->as_array()) {
        const std::string file = item.at("file").as_string();
        const auto frame = MAA_NS::imread(root / file);
        require(!frame.empty(), "employ fixture missing: " + file);
        const bool expected = item.at("clear").as_boolean();
        (expected ? clear : blocked) = frame;
        test(("employ native page visibility " + file).c_str(), [&] {
            require(employ_capture_frame_is_clear(frame) == expected, "wrong clean/obscured classification");
        });
        if (!expected) {
            test(("employ remaining second toast " + file).c_str(), [&] {
                auto partial = MAA_NS::imread(root / "run-20261016-165116-426279-3995.jpg");
                frame(cv::Rect(935, 180, 345, 105)).copyTo(partial(cv::Rect(935, 180, 345, 105)));
                require(!employ_capture_frame_is_clear(partial), "partly dismissed toast accepted");
            });
        }
    }
    test("employ empty or different page is not clear", [&] {
        require(!employ_capture_frame_is_clear({ }), "empty frame accepted");
        require(!employ_capture_frame_is_clear(cv::Mat::zeros(720, 1280, CV_8UC3)), "blank page accepted");
    });
    test("employ remaining lower toast cannot pass exposed top instruction", [&] {
        auto stacked = clear.clone();
        // Synthetic partial dismissal: retain a real second toast on an otherwise clean page.
        blocked(cv::Rect(935, 180, 345, 105)).copyTo(stacked(cv::Rect(935, 180, 345, 105)));
        require(!employ_capture_frame_is_clear(stacked), "lower toast accepted");
    });
    auto run_sequence = [&](std::vector<bool> visible, int cancel_after, int expected_samples, bool succeeds) {
        cv::Mat result;
        int samples = 0, index = 0, waits = 0;
        bool current_clear = false;
        const bool ok = capture_unobstructed_employ_frame(
            [&] {
                current_clear = visible.at(std::min(index++, static_cast<int>(visible.size()) - 1));
                return current_clear ? clear.clone() : blocked.clone();
            },
            [&](const cv::Mat&) { return current_clear; },
            [&](int ms) {
                require(ms == 250, "unexpected probe interval");
                ++waits;
                return true;
            },
            [&] { return cancel_after >= 0 && index >= cancel_after; },
            result,
            samples);
        require(ok == succeeds && samples == expected_samples, "wrong capture boundary");
        require(result.empty() != succeeds, "failed capture returned a usable image");
        if (ok) {
            require(cv::norm(result, clear, cv::NORM_INF) == 0, "saved a different frame");
            require(waits >= 2, "did not wait for consecutive clean frames");
        }
    };
    test("employ static toast times out without a screenshot", [&] { run_sequence({ false }, -1, 40, false); });
    test("employ waits for notification disappearance", [&] { run_sequence({ false, false, true }, -1, 5, true); });
    test("employ reappearing toast resets the clear streak", [&] {
        run_sequence({ true, true, false, true }, -1, 6, true);
    });
    test("employ cancellation never returns a capture", [&] { run_sequence({ true }, 2, 2, false); });
}
