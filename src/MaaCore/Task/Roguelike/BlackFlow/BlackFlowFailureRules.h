#pragma once

#include <string_view>

namespace asst::blackflow
{
enum class FailureDisposition
{
    RestartRun,
    StopTask,
};

[[nodiscard]] constexpr std::string_view failure_next_action(FailureDisposition disposition) noexcept
{
    return disposition == FailureDisposition::RestartRun ? "restart_current_run" : "stop_task";
}
} // namespace asst::blackflow
