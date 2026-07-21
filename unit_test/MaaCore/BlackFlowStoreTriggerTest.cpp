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

TEST_CASE("BlackFlow store collection starts only at the stable investment-system seam")
{
    const auto details = process_task_event("MiniGame@BlackFlow@InvestSystem");

    CHECK(asst::should_start_black_flow_store_cycle(asst::AsstMsg::SubTaskStart, details));
    CHECK_FALSE(asst::should_start_black_flow_store_cycle(asst::AsstMsg::SubTaskCompleted, details));
    CHECK_FALSE(
        asst::should_start_black_flow_store_cycle(
            asst::AsstMsg::SubTaskStart,
            process_task_event("MiniGame@BlackFlow@Begin")));
}

TEST_CASE("Legacy and unrelated process nodes cannot start the stable store collection")
{
    CHECK_FALSE(
        asst::should_start_black_flow_store_cycle(
            asst::AsstMsg::SubTaskStart,
            process_task_event("BlackFlowTemporary@InvestSystem")));
    CHECK_FALSE(
        asst::should_start_black_flow_store_cycle(
            asst::AsstMsg::SubTaskStart,
            process_task_event("MiniGame@BlackFlow@ConfirmRefresh")));

    auto non_process = process_task_event("MiniGame@BlackFlow@InvestSystem");
    non_process["subtask"] = "CustomTask";
    CHECK_FALSE(asst::should_start_black_flow_store_cycle(asst::AsstMsg::SubTaskStart, non_process));
}
