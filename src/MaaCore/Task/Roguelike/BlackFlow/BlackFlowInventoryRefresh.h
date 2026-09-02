#pragma once

namespace asst::blackflow
{
enum class MovementInventoryRefreshEvent
{
    RunStarted,
    FloorEntered,
    NodeCompleted,
    ObservationApplied,
};

[[nodiscard]] constexpr bool next_movement_inventory_refresh_state(
    bool current,
    MovementInventoryRefreshEvent event) noexcept
{
    switch (event) {
    case MovementInventoryRefreshEvent::RunStarted:
    case MovementInventoryRefreshEvent::FloorEntered:
    case MovementInventoryRefreshEvent::NodeCompleted:
        return true;
    case MovementInventoryRefreshEvent::ObservationApplied:
        return false;
    }
    return current;
}
} // namespace asst::blackflow
