#include "Task/Miscellaneous/BlackFlowDataCollectionTrigger.hpp"
#include "Task/Miscellaneous/BlackFlowDataCollectionPlan.hpp"

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
            asst::AsstMsg::SubTaskCompleted,
            process_task_event("MiniGame@BlackFlow@ProcessingItemReady")) ==
        asst::BlackFlowCaptureTarget::ProcessingItemPopup);
    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskCompleted,
            process_task_event("MiniGame@BlackFlow@PartsBoxReady")) == asst::BlackFlowCaptureTarget::PartsBox);
}

TEST_CASE("BlackFlow collection waits for opening capture tasks to complete")
{
    const auto processing_item = process_task_event("MiniGame@BlackFlow@ProcessingItemReady");
    CHECK(
        asst::black_flow_capture_target(asst::AsstMsg::SubTaskStart, processing_item) ==
        asst::BlackFlowCaptureTarget::None);
    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskCompleted,
            process_task_event("BlackFlowTemporary@InvestSystem")) == asst::BlackFlowCaptureTarget::None);

    CHECK(
        asst::black_flow_capture_target(
            asst::AsstMsg::SubTaskStart,
            process_task_event("MiniGame@BlackFlow@PartsBoxReady")) == asst::BlackFlowCaptureTarget::None);

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

TEST_CASE("BlackFlow opening snapshots share one cycle directory and keep distinct labels")
{
    const std::optional group_id = asst::OpeningCaptureGroupId { "2026.08.03-20.00.00.123" };

    const auto processing_item =
        asst::black_flow_capture_plan(asst::BlackFlowCaptureTarget::ProcessingItemPopup, group_id);
    const auto parts_box = asst::black_flow_capture_plan(asst::BlackFlowCaptureTarget::PartsBox, group_id);

    CHECK(processing_item.relative_dir == parts_box.relative_dir);
    CHECK(
        processing_item.relative_dir ==
        std::filesystem::path("debug/roguelike/black_flow/opening_cycles") / group_id->value);
    CHECK(processing_item.suffix == "processing-item");
    CHECK(parts_box.suffix == "parts-box");
    CHECK(processing_item.group_role == asst::BlackFlowCaptureGroupRole::BeginsGroup);
    CHECK(parts_box.group_role == asst::BlackFlowCaptureGroupRole::RequiresGroup);
    CHECK(processing_item.image_source == asst::BlackFlowCaptureImageSource::FreshScreenshot);
    CHECK(parts_box.image_source == asst::BlackFlowCaptureImageSource::FreshScreenshot);
}
