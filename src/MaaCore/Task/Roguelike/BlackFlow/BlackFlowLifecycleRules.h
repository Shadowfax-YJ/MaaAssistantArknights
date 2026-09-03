#pragma once

namespace asst::blackflow
{
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
