#include <catch2/catch_test_macros.hpp>

#include "Task/Miscellaneous/BlackFlowStoreScreenshotTrigger.hpp"

TEST_CASE("Store overview task start triggers a persistent screenshot")
{
    const json::value details = json::object {
        { "subtask", "ProcessTask" },
        { "details", json::object { { "task", "BlackFlowTemporary@InvestSystem" } } },
    };

    REQUIRE(asst::should_capture_black_flow_store_overview(asst::AsstMsg::SubTaskStart, details));
}

TEST_CASE("Store overview task completion does not trigger another screenshot")
{
    const json::value details = json::object {
        { "subtask", "ProcessTask" },
        { "details", json::object { { "task", "BlackFlowTemporary@InvestSystem" } } },
    };

    REQUIRE_FALSE(asst::should_capture_black_flow_store_overview(asst::AsstMsg::SubTaskCompleted, details));
}

TEST_CASE("Non-process subtask does not trigger a store screenshot")
{
    const json::value details = json::object {
        { "subtask", "CustomTask" },
        { "details", json::object { { "task", "BlackFlowTemporary@InvestSystem" } } },
    };

    REQUIRE_FALSE(asst::should_capture_black_flow_store_overview(asst::AsstMsg::SubTaskStart, details));
}

TEST_CASE("Other process task does not trigger a store screenshot")
{
    const json::value details = json::object {
        { "subtask", "ProcessTask" },
        { "details", json::object { { "task", "BlackFlowTemporary@InvestEntry" } } },
    };

    REQUIRE_FALSE(asst::should_capture_black_flow_store_overview(asst::AsstMsg::SubTaskStart, details));
}
