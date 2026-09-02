#pragma once

namespace asst::blackflow
{
enum class AbandonResetDisposition
{
    DeferUntilStartExplore,
    FinishAndStop,
};

[[nodiscard]] constexpr AbandonResetDisposition abandon_reset_disposition(bool stop_after_abandon) noexcept
{
    return stop_after_abandon ? AbandonResetDisposition::FinishAndStop
                              : AbandonResetDisposition::DeferUntilStartExplore;
}
} // namespace asst::blackflow
