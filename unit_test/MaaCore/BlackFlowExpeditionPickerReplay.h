#pragma once
#include "MaaUtils/NoWarningCV.hpp"

// Replay the installed collector's actual battle departure, whose background map
// contains a node named "先行一步" inside the former picker OCR region.
template <typename Test>
void expedition_picker_regressions(const std::filesystem::path& repo, Test&& test)
{
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-expedition/20261020";
    const auto battle = MAA_NS::imread(root / "battle-depart-with-expedition-label.png");
    require(!battle.empty(), "expedition false-positive screenshot missing");
    const auto picker_image = MAA_NS::imread(root / "picker-unselected.png");
    require(!picker_image.empty(), "captured expedition picker missing");
    test("native title matcher accepts the captured expedition picker", [&] {
        const auto task = Task.get("BlackFlow@Roguelike@ExpeditionPicker");
        require(task != nullptr && task->algorithm == AlgorithmType::MatchTemplate, "picker still uses title OCR");
        Matcher picker(picker_image);
        picker.set_task_info("BlackFlow@Roguelike@ExpeditionPicker");
        require(picker.analyze().has_value(), "real expedition picker title was rejected");
    });
    test("real expedition picker remains reachable in event settlement", [&] {
        const auto reward = Task.get("BlackFlow@Roguelike@StageEncounterReward");
        require(reward != nullptr, "event settlement task missing");
        PipelineAnalyzer continuation(picker_image);
        continuation.set_tasks(reward->next);
        const auto result = continuation.analyze();
        require(
            result.has_value() && result->task_ptr->name == "BlackFlow@Roguelike@ExpeditionPicker",
            "real expedition picker was bypassed by event settlement");
    });
    test("picker recognition does not depend on operator selection contents", [&] {
        auto changed = picker_image.clone();
        changed(cv::Rect(0, 190, 1280, 530)).setTo(cv::Scalar(0, 0, 0));
        Matcher picker(changed);
        picker.set_task_info("BlackFlow@Roguelike@ExpeditionPicker");
        require(picker.analyze().has_value(), "picker identity depended on variable selection contents");
    });
    test("picker recognition requires its actual title decoration", [&] {
        auto hidden = picker_image.clone();
        hidden(cv::Rect(230, 120, 200, 70)).setTo(cv::Scalar(0, 0, 0));
        PipelineAnalyzer picker(hidden);
        picker.set_tasks({ "BlackFlow@Roguelike@ExpeditionPicker" });
        require(!picker.analyze().has_value(), "missing picker title was accepted");
    });
    test("background expedition node cannot identify an operator picker", [&] {
        PipelineAnalyzer picker(battle);
        picker.set_tasks({ "BlackFlow@Roguelike@ExpeditionPicker" });
        require(!picker.analyze().has_value(), "background map label passed the expedition picker guard");
        Matcher direct_picker(battle);
        direct_picker.set_task_info("BlackFlow@Roguelike@ExpeditionPicker");
        require(!direct_picker.analyze().has_value(), "native title matcher accepted the background map label");
    });
    test("battle departure remains reachable behind background expedition node", [&] {
        const auto reward = Task.get("BlackFlow@Roguelike@StageEncounterReward");
        require(reward != nullptr, "event settlement task missing");
        PipelineAnalyzer continuation(battle);
        continuation.set_tasks(reward->next);
        const auto result = continuation.analyze();
        require(result.has_value(), "battle departure continuation was not recognized");
        require(
            result->task_ptr->name == "BlackFlow@Roguelike@StageEncounterBattleDepart",
            "background expedition branch preempted battle departure");
    });
}
