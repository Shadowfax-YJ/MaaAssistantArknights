#include "Task/Miscellaneous/BlackFlowStoreTrigger.hpp"

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

TEST_CASE("Stable BlackFlow entry starts the production cycle exactly on task start")
{
    const auto details = process_task_event("MiniGame@BlackFlow@Begin");

    CHECK(asst::should_start_black_flow_store_cycle(asst::AsstMsg::SubTaskStart, details));
    CHECK_FALSE(asst::should_start_black_flow_store_cycle(asst::AsstMsg::SubTaskCompleted, details));
}

TEST_CASE("Legacy investment and other process nodes cannot start the production cycle")
{
    CHECK_FALSE(
        asst::should_start_black_flow_store_cycle(
            asst::AsstMsg::SubTaskStart,
            process_task_event("BlackFlowTemporary@InvestSystem")));
    CHECK_FALSE(
        asst::should_start_black_flow_store_cycle(
            asst::AsstMsg::SubTaskStart,
            process_task_event("MiniGame@BlackFlow@ConfirmRefresh")));

    auto non_process = process_task_event("MiniGame@BlackFlow@Begin");
    non_process["subtask"] = "CustomTask";
    CHECK_FALSE(asst::should_start_black_flow_store_cycle(asst::AsstMsg::SubTaskStart, non_process));
}
