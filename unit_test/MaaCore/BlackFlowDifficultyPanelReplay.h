#pragma once

template <typename Test>
void difficulty_panel_regressions(
    const std::filesystem::path& repo,
    const std::shared_ptr<RoguelikeConfig>& config,
    Test&& test)
{
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-difficulty";
    DifficultyProbe selection({}, nullptr, "Roguelike", config, nullptr);
    cv::Mat base;
    for (const char* file : { "base-panel-first.jpg", "base-panel-retry.jpg" }) {
        test((std::string("001 captured base difficulty panel ") + file).c_str(), [&] {
            base = MAA_NS::imread(root / "20261021" / file);
            require(!base.empty(), "base panel fixture missing");
            require(selection.detect_blackflow_panel_difficulty(base) == 0, "base label did not resolve to 0");
            require(selection.detect_blackflow_home_difficulty(base) == -1, "panel was accepted as applied home difficulty");
        });
    }
    test("difficulty heading alone cannot imply base difficulty", [&] {
        auto heading = base.clone();
        heading(cv::Rect(430, 365, 195, 55)).setTo(cv::Scalar::all(0));
        require(selection.detect_blackflow_panel_difficulty(heading) == -1, "unreadable selected row became 0");
    });
    test("missing difficulty panel cannot authorize base difficulty", [&] {
        auto missing = base.clone();
        missing(cv::Rect(1064, 294, 216, 170)).setTo(cv::Scalar::all(0));
        require(selection.detect_blackflow_panel_difficulty(missing) == -1, "label alone passed the panel guard");
    });
    test("empty difficulty image remains unknown", [&] {
        require(selection.detect_blackflow_panel_difficulty({}) == -1, "empty image became 0");
    });
    test("captured numeric glyph remains readable in the selected panel row", [&] {
        // Controlled geometry: move the original frame's upper-row glyph 1 into the selected row.
        auto numeric = base.clone();
        numeric(cv::Rect(400, 300, 225, 160)).setTo(cv::Scalar::all(0));
        base(cv::Rect(480, 170, 100, 115)).copyTo(numeric(cv::Rect(480, 320, 100, 115)));
        require(selection.detect_blackflow_panel_difficulty(numeric) == 1, "numeric row was not recognized");
    });
    for (const int difficulty : { 5, 6 }) {
        test(("home badge is not a selected panel row " + std::to_string(difficulty)).c_str(), [&] {
            const auto home = MAA_NS::imread(root / ("home-" + std::to_string(difficulty) + ".jpg"));
            require(selection.detect_blackflow_panel_difficulty(home) == -1, "home image passed panel recognition");
        });
    }
}
