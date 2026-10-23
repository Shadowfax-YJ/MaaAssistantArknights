#pragma once

#include <optional>
#include <utility>

#include "Common/AsstTypes.h"

namespace asst
{
// The gaps between squad cards do not accept drags. Anchor inside a recognized card instead.
inline std::optional<std::pair<Point, Point>> black_flow_squad_swipe(const Rect& name, bool backwards)
{
    if (name.empty() || name.y < 440 || name.y + name.height > 495) {
        return std::nullopt;
    }
    const int x = name.x + name.width / 2;
    const int end_x = x + (backwards ? 560 : -560);
    if (x < 40 || x > 1240 || end_x < 40 || end_x > 1240) {
        return std::nullopt;
    }
    return std::pair { Point { x, 350 }, Point { end_x, 350 } };
}
}
