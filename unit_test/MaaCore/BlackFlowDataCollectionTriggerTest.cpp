#include "Task/Miscellaneous/BlackFlowDataCollectionTrigger.hpp"

#include <catch2/catch_test_macros.hpp>

namespace
{
json::value process_task_event(std::string task)
{
    return json::object {
        { "subtask", "ProcessTask" },
        { "details", json::object { { "task", std::move(task) } } },
    };
}
} // namespace

TEST_CASE("BlackFlow collection classifies the stable capture seams")
{
    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskStart,
            process_task_event("BlackFlowTemporary@InvestSystem")) == asst::BlackFlowCaptureTarget::StoreOverview);
    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskStart,
            process_task_event("MiniGame@BlackFlow@ProcessingItemReady")) ==
        asst::BlackFlowCaptureTarget::ProcessingItemPopup);
    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskStart,
            process_task_event("MiniGame@BlackFlow@PartsBoxReady")) == asst::BlackFlowCaptureTarget::PartsBox);
}

TEST_CASE("BlackFlow collection ignores completion events and unrelated tasks")
{
    const auto processing_item = process_task_event("MiniGame@BlackFlow@ProcessingItemReady");
    CHECK(
        asst::black_flow_capture_target(asst::AsstMsg::SubTaskCompleted, processing_item) ==
        asst::BlackFlowCaptureTarget::None);
    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskCompleted,
            process_task_event("BlackFlowTemporary@InvestSystem")) == asst::BlackFlowCaptureTarget::None);

    for (const std::string_view task : {
             "MiniGame@BlackFlow@Begin",
             "MiniGame@BlackFlow@CloseProcessingItem",
             "MiniGame@BlackFlow@PartsBoxReadyish",
         }) {
        CHECK(
            asst::black_flow_capture_target(asst::AsstMsg::SubTaskStart, process_task_event(std::string(task))) ==
            asst::BlackFlowCaptureTarget::None);
    }

    auto non_process = process_task_event("MiniGame@BlackFlow@PartsBoxReady");
    non_process["subtask"] = "CustomTask";
    CHECK(
        asst::black_flow_capture_target(asst::AsstMsg::SubTaskStart, non_process) ==
        asst::BlackFlowCaptureTarget::None);
}
