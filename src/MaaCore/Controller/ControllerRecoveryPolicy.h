#pragma once

#include <cstddef>
#include <optional>

namespace asst::controller_recovery
{

// Keep the existing tolerance for quick transient decode failures, but allow
// transport recovery on every attempt. The flag has no fast-path cost and only
// takes effect after the underlying adb command has failed or timed out.
inline constexpr std::size_t MaxScreencapAttempts = 21;
inline constexpr std::size_t FirstReconnectEnabledAttempt = 0;

[[nodiscard]] constexpr bool allow_reconnect_for_screencap_attempt(std::size_t attempt) noexcept
{
    return attempt >= FirstReconnectEnabledAttempt;
}

[[nodiscard]] constexpr bool should_reconnect_after_command(
    const std::optional<int>& exit_result,
    bool controller_inited,
    bool allow_reconnect,
    bool exiting) noexcept
{
    const bool command_failed = !exit_result.has_value() || exit_result.value() != 0;
    return command_failed && controller_inited && allow_reconnect && !exiting;
}

} // namespace asst::controller_recovery
