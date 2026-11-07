// Production TaskData, OCR and CustomTask; input images are captured failure frames.
#include "Assistant.h"
#include "Config/ResourceLoader.h"
#include "Config/TaskData.h"
#include "MaaUtils/ImageIo.h"
#include "Task/Interface/CustomTask.h"
#include "Task/Miscellaneous/BlackFlowSquadSwipe.hpp"
#include "Utils/WorkingDir.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/OCRer.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

using namespace asst;
namespace
{
int passed = 0, failed = 0;
void check(bool ok, const std::string& message)
{
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
    ok ? ++passed : ++failed;
}
std::string recognize(const cv::Mat& image, const TaskList& tasks)
{
    PipelineAnalyzer analyzer(image);
    analyzer.set_tasks(tasks);
    auto result = analyzer.analyze();
    return result ? result->task_ptr->name : "";
}
struct OcrProbe : OCRer
{
    using OCRer::OCRer;
    using OCRer::filter_and_replace_by_required_;
    using OCRer::postproc_replace_;
};
const std::string prefix = "MiniGame@BlackFlow@";
}

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    UserDir.set(std::filesystem::current_path());
    if (!ResourceLoader::get_instance().load(std::filesystem::path(argv[1]) / "resource")) { return 2; }
    const auto fixtures = std::filesystem::path(argv[2]);
    for (const auto* file : { "004-recruitment.png", "005-recruitment.png" }) {
        auto image = MAA_NS::imread(fixtures / file);
        if (image.empty()) { return 2; }
        for (const auto* stage : { "ChooseRecruitCombination", "ConfirmRecruitCombination", "WaitForRecruitment" }) {
            const auto task = Task.get(prefix + stage);
            check(task && recognize(image, task->next) == prefix + "GiveUpRecruit1",
                  std::string(file) + " resumes abandoned recruitment from " + stage);
        }
    }
    const auto confirm = Task.get(prefix + "ConfirmRecruitCombination");
    check(std::find(confirm->next.begin(), confirm->next.end(), prefix + "ChooseRecruitCombination") == confirm->next.end(),
          "confirmation never regresses to the stale combination title");
    const cv::Mat blank = cv::Mat::zeros(720, 1280, CV_8UC3);
    check(recognize(blank, Task.get(prefix + "StartExplore")->next) == prefix + "WaitForSquadPage",
          "unrecognized loading or blocked page waits without swiping");
    for (const auto& text : { "开拓者分队", "斤拓者分队", "午拓者分队", "指挥分队", "开拓者", "文明开化分队" }) {
        OcrProbe ocr(blank);
        ocr.set_task_info(prefix + "ChoosePioneerSquad");
        OCRer::Result observation;
        observation.text = text;
        ocr.postproc_replace_(observation);
        const bool expected = std::string(text).ends_with("拓者分队");
        check(ocr.filter_and_replace_by_required_(observation) == expected,
              "production OCR replacement/full-match: " + std::string(text));
    }
    const auto forward = Task.get(prefix + "SwipeToPioneerSquad");
    const auto backward = Task.get(prefix + "SwipeBackToPioneerSquad");
    check(forward->action == ProcessTaskAction::DoNothing && backward->action == ProcessTaskAction::DoNothing,
          "squad swipes use the plugin's recognized card anchor, never a fixed gap coordinate");
    auto gap_image = MAA_NS::imread(fixtures / "squad-gap.png");
    for (bool reverse : { false, true }) {
        PipelineAnalyzer analyzer(gap_image);
        analyzer.set_tasks({ prefix + (reverse ? "SwipeBackToPioneerSquad" : "SwipeToPioneerSquad") });
        auto result = analyzer.analyze();
        auto swipe = result ? black_flow_squad_swipe(result->rect, reverse) : std::nullopt;
        check(swipe.has_value(), "real stalled squad frame yields an in-screen card drag");
        if (swipe) {
            check(swipe->first.x >= result->rect.x && swipe->first.x < result->rect.x + result->rect.width &&
                  swipe->second.x - swipe->first.x == (reverse ? 560 : -560),
                  "drag starts under the recognized card name and respects search direction");
        }
    }
    auto network_image = MAA_NS::imread(fixtures / "005-network-400.png");
    for (const auto* stage : { "Begin", "StartExplore", "WaitForSquadPage", "ConfirmRecruitCombination", "WaitForFirstLayer" }) {
        check(recognize(network_image, Task.get(prefix + stage)->next) == prefix + "NetworkDisconnected",
              std::string("real error-400 dialog is explicitly identified from ") + stage);
    }
    check(forward->algorithm == AlgorithmType::OcrDetect, "swipe requires visible squad names");
    check(std::find(forward->exceeded_next.begin(), forward->exceeded_next.end(), prefix + "SwipeBackToPioneerSquad") != forward->exceeded_next.end(),
          "forward search exhaustion reverses direction");
    check(backward && backward->roi.x + backward->roi.width <= 640 && forward->roi.x >= 640,
          "opposite searches choose card anchors with room to drag");
    const auto start = Task.get(prefix + "StartExplore");
    for (const auto* name : { "SwipeToPioneerSquad", "SwipeBackToPioneerSquad", "WaitForSquadPage", "WaitForSquadPageBack", "WaitForRecruitment", "WaitForFirstLayer", "WaitForExploreRestart" }) {
        const auto task = Task.get(prefix + name);
        check(task && task->max_times > 0 && task->max_times <= 40 &&
              std::count(start->reduce_other_times.begin(), start->reduce_other_times.end(), prefix + name) >= task->max_times,
              std::string("bounded recovery resets each cycle: ") + name);
    }
    if (Task.get(prefix + "OpeningFailed")) {
        Assistant assistant;
        bool error_callback = false;
        CustomTask task([&](AsstMsg msg, const json::value& value, Assistant*) {
            if (msg == AsstMsg::SubTaskError && value.get("what", std::string()) == "BlackFlowOpeningFailed") {
                error_callback = true;
            }
        }, &assistant);
        check(task.set_params(json::object { { "task_names", json::array { prefix + "OpeningFailed" } } }),
              "production failure task registers");
        check(!task.run() && error_callback, "production CustomTask reports exhausted recovery as failure");
        CustomTask normal({}, &assistant);
        check(normal.set_params(json::object { { "task_names", json::array { "Stop" } } }) && normal.run(),
              "ordinary custom Stop remains successful");
    }
    else { check(false, "opening failure terminal exists"); }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
