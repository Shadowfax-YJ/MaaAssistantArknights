#pragma once

#include "BlackFlowStoreOrchestrator.hpp"

#include <optional>
#include <string_view>

namespace asst
{
struct BlackFlowStoreTaskPolicy
{
    std::string_view expected_terminal;
    int retry_times = 20;
    std::optional<int> times_limit;
};

inline constexpr std::optional<BlackFlowStoreTaskPolicy>
    black_flow_store_task_policy(std::string_view task_name) noexcept
{
    using namespace black_flow_store_tasks;
    if (task_name == EnterFreshExploration || task_name == ObserveStorePage) {
        return BlackFlowStoreTaskPolicy { .expected_terminal = StorePageReady };
    }
    if (task_name == OpenRefreshDialog) {
        return BlackFlowStoreTaskPolicy { .expected_terminal = RefreshDialogVisible };
    }
    if (task_name == ConfirmRefresh) {
        return BlackFlowStoreTaskPolicy {
            .expected_terminal = ConfirmRefresh,
            .retry_times = 0,
            .times_limit = 1,
        };
    }
    if (task_name == SafeExit) {
        return BlackFlowStoreTaskPolicy { .expected_terminal = StartExploreEntryVisible };
    }
    return std::nullopt;
}
} // namespace asst
