#pragma once

#include <string_view>

namespace asst::blackflow
{
[[nodiscard]] constexpr std::string_view initial_collection_task(bool automation_collection) noexcept
{
    return automation_collection ? "BlackFlow@Roguelike@CollectionBegin" : "BlackFlow@Roguelike@Begin";
}

// 地图恢复的等待可能跨越移动转场，重试时先识别实际页面；其他动作保留原位重试。
[[nodiscard]] constexpr std::string_view recovery_retry_task(std::string_view failed_task) noexcept
{
    if (failed_task == "BlackFlow@Roguelike@TreeHoleReturnResumeAction") {
        return "BlackFlow@Roguelike@TreeHoleReturnResumeRetry";
    }
    return failed_task.starts_with("BlackFlow@Roguelike@RecoverMap") ? "BlackFlow@Roguelike@RecoverMap-Enter"
                                                                     : failed_task;
}

enum class AbandonResetDisposition
{
    DeferUntilStartExplore,
    FinishAndStop,
};

enum class StartExploreRunDisposition
{
    KeepInitialRun,
    FinishAndStartNext,
};

[[nodiscard]] constexpr AbandonResetDisposition abandon_reset_disposition(bool stop_after_abandon) noexcept
{
    return stop_after_abandon ? AbandonResetDisposition::FinishAndStop
                              : AbandonResetDisposition::DeferUntilStartExplore;
}

[[nodiscard]] constexpr StartExploreRunDisposition start_explore_run_disposition(
    bool start_explore_seen,
    bool run_has_progress) noexcept
{
    return !start_explore_seen && !run_has_progress ? StartExploreRunDisposition::KeepInitialRun
                                                    : StartExploreRunDisposition::FinishAndStartNext;
}
} // namespace asst::blackflow
