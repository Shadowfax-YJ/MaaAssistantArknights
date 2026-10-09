#pragma once
#include "MaaUtils/NoWarningCV.hpp"

// Captured game frames replay the production recognizers, without issuing device input.
template <typename Test>
void move_preview_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-move-preview/20261009";
    const auto image = MAA_NS::imread(root / "walk-preview.jpg");
    require(!image.empty(), "walk preview fixture missing");
    test("new walking button is visible to native preview sampler", [&] {
        Matcher analyzer(image);
        analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewEnter");
        require(analyzer.analyze().has_value(), "preview controls are not visible");
    });
    test("new walking preview can enter confirmation resource graph", [&] {
        PipelineAnalyzer analyzer(image);
        analyzer.set_tasks({ "BlackFlow@Roguelike@MovePreviewConfirm" });
        require(analyzer.analyze().has_value(), "confirm button is not recognized");
    });
    test("new walking preview retains exact action point cost", [&] {
        OCRer analyzer(image);
        analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewCost");
        const auto result = analyzer.analyze();
        require(result.has_value() && !result->empty(), "cost is not recognized");
        require(parse_move_preview_action_point_cost(result->front().text) == -1, "wrong preview cost");
    });
    test("processed depart button is visible to native preview sampler", [&] {
        const auto processed = MAA_NS::imread(root / "processed-preview.png");
        require(!processed.empty(), "processed preview fixture missing");
        Matcher analyzer(processed);
        analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewEnter");
        require(analyzer.analyze().has_value(), "processed preview controls are not visible");
    });
    for (const char* filename : {
             "walk-preview.jpg", "walk-preview-next.jpg", "battle-intel-preview.jpg", "processed-preview.png",
             "legacy-preview.jpg", "zero-cost/charged-preview.jpg" }) {
        const auto frame = MAA_NS::imread(root / filename);
        require(!frame.empty(), std::string("missing preview: ") + filename);
        for (const char* task : {
                 "MovePreviewEnter", "MovePreviewConfirm", "MovePreviewConfirmObserve", "ResumeMovePreviewEnter",
                 "HuntedConfirmDestination", "HuntedDepart", "HuntedDepartObserve", "StageEncounterBattleDepart",
                 "StageEncounterBattleDepartObserve", "StageEnterBattleAgain", "StageEnterBattleAgainObserve" }) {
            const std::string name = std::string(filename) + " recognizes " + task;
            test(name.c_str(), [&] {
                PipelineAnalyzer analyzer(frame);
                analyzer.set_tasks({ std::string("BlackFlow@Roguelike@") + task });
                require(analyzer.analyze().has_value(), "depart entry is not recognized");
            });
        }
        const std::string name = std::string(filename) + " retains exact cost -1";
        test(name.c_str(), [&] {
            OCRer analyzer(frame);
            analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewCost");
            const auto result = analyzer.analyze();
            require(result.has_value() && !result->empty(), "cost is not recognized");
            require(parse_move_preview_action_point_cost(result->front().text) == -1, "wrong preview cost");
        });
    }
    const auto processed = MAA_NS::imread(root / "processed-preview.png");
    auto recognizes_depart = [](const cv::Mat& frame) {
        Matcher analyzer(frame);
        analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewEnter");
        return analyzer.analyze().has_value();
    };
    // These independent frames show -0 after M07 selection. A cropped zero must
    // neither disappear from OCR nor turn into a different action-point cost.
    for (const char* filename : {
             "001560-move-preview-failed.jpg", "001587-move-preview-failed.jpg", "001614-move-preview-failed.jpg",
             "001641-move-preview-failed.jpg", "001668-move-preview-failed.jpg", "001695-move-preview-failed.jpg",
             "001722-move-preview-failed.jpg", "001749-move-preview-failed.jpg" }) {
        const std::string name = std::string(filename) + " retains exact processed cost zero";
        test(name.c_str(), [&] {
            const auto frame = MAA_NS::imread(root / "zero-cost" / filename);
            require(!frame.empty(), "zero-cost preview fixture missing");
            require(recognizes_depart(frame), "zero-cost processed button missing");
            OCRer analyzer(frame);
            analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewCost");
            const auto result = analyzer.analyze();
            require(result.has_value() && result->size() == 1, "zero cost is not recognized");
            require(parse_move_preview_action_point_cost(result->front().text) == 0, "zero cost misread");
        });
    }
    for (const char* filename : {
             "walk-preview.jpg", "processed-preview.png", "legacy-preview.jpg",
             "zero-cost/001587-move-preview-failed.jpg" }) {
        const std::string name = std::string(filename) + " missing cost is rejected despite visible button";
        test(name.c_str(), [&] {
            auto frame = MAA_NS::imread(root / filename);
            require(!frame.empty(), "missing-cost source fixture missing");
            // Synthetic negative: remove only the physical fee text, leaving the
            // title and depart label intact. Missing evidence is not a free move.
            frame(cv::Rect(1074, 540, 44, 36)).setTo(cv::Scalar::all(0));
            require(recognizes_depart(frame), "cost removal changed button visibility");
            OCRer analyzer(frame);
            analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewCost");
            require(!analyzer.analyze().has_value(), "missing cost recognized as a valid fee");
        });
    }
    for (const auto& frame : { image, processed }) {
        const char* label = frame.cols == 1280 ? "walk" : "processed";
        const std::string name = std::string(label) + " survives standard 1280x720 capture normalization";
        test(name.c_str(), [&] {
            cv::Mat normalized;
            cv::resize(frame, normalized, cv::Size(1280, 720));
            require(recognizes_depart(normalized), "normalized depart button missing");
            OCRer analyzer(normalized);
            analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewCost");
            const auto cost = analyzer.analyze();
            require(cost.has_value() && !cost->empty() &&
                        parse_move_preview_action_point_cost(cost->front().text) == -1,
                    "normalized preview cost changed");
        });
        const std::string jpeg_name = std::string(label) + " survives JPEG quality 45";
        test(jpeg_name.c_str(), [&] {
            std::vector<uchar> buffer;
            require(cv::imencode(".jpg", frame, buffer, { cv::IMWRITE_JPEG_QUALITY, 45 }), "JPEG encode");
            require(recognizes_depart(cv::imdecode(buffer, cv::IMREAD_COLOR)), "compressed depart button missing");
        });
        for (double gain : { 0.8, 1.15 }) {
            const std::string brightness_name = std::string(label) + " brightness gain " + std::to_string(gain);
            test(brightness_name.c_str(), [&] {
                cv::Mat adjusted;
                frame.convertTo(adjusted, -1, gain);
                require(recognizes_depart(adjusted), "brightness-adjusted depart button missing");
            });
        }
        const std::string absent_name = std::string(label) + " missing button is rejected";
        test(absent_name.c_str(), [&] {
            auto absent = frame.clone();
            absent(cv::Rect(1122, 520, 153, 65)).setTo(cv::Scalar::all(0));
            require(!recognizes_depart(absent), "absent button recognized");
        });
    }
    for (const auto& relative : {
             "blackflow-move-preview/20261009/map-before-preview.jpg",
             "blackflow-transitions/recruit-caster.jpg",
             "blackflow-transitions/recruit-supporter.jpg",
             "blackflow-recovery/20260926/inventory-open.jpg",
             "blackflow-recovery/20260926/purchase-before-wallet-9.jpg",
             "blackflow-difficulty/home-6.jpg" }) {
        test(relative, [&] {
            const auto frame = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures" / relative);
            require(!frame.empty(), "negative fixture missing");
            PipelineAnalyzer analyzer(frame);
            analyzer.set_tasks({
                "BlackFlow@Roguelike@MovePreviewEnter", "BlackFlow@Roguelike@MovePreviewConfirm",
                "BlackFlow@Roguelike@ResumeMovePreviewEnter", "BlackFlow@Roguelike@HuntedDepart",
                "BlackFlow@Roguelike@StageEncounterBattleDepart", "BlackFlow@Roguelike@StageEnterBattleAgain" });
            require(!analyzer.analyze().has_value(), "non-preview page recognized as depart button");
        });
    }
    test("unreachable button remains blocked rather than reachable", [&] {
        auto blocked = image.clone();
        blocked(cv::Rect(1122, 520, 153, 65)).setTo(cv::Scalar::all(0));
        const auto button = MAA_NS::imread(
            repo / "resource/template/Roguelike/BlackFlow/BlackFlow@Roguelike@MovePreviewCannotEnter.png");
        require(!button.empty(), "blocked template missing");
        button.copyTo(blocked(cv::Rect(1147, 541, button.cols, button.rows)));
        require(!recognizes_depart(blocked), "unreachable button recognized as reachable");
        Matcher analyzer(blocked);
        analyzer.set_task_info("BlackFlow@Roguelike@MovePreviewCannotEnter");
        require(analyzer.analyze().has_value(), "unreachable control lost");
    });
}
