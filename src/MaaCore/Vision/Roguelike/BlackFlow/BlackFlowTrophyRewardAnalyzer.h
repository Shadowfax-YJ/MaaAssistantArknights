#pragma once

#include <string>
#include <vector>

#include "Vision/VisionHelper.h"

namespace asst::blackflow
{
class BlackFlowTrophyRewardAnalyzer final : public VisionHelper
{
public:
    using VisionHelper::VisionHelper;
    ~BlackFlowTrophyRewardAnalyzer() override = default;

    struct Detection
    {
        Rect button_rect;
        Rect name_rect;
        std::string name;
        double name_score = 0.0;
        std::string category;
        double category_score = 0.0;
    };

    bool analyze();

    [[nodiscard]] const std::vector<Detection>& get_detections() const noexcept { return m_detections; }

private:
    std::vector<Detection> m_detections;
};
} // namespace asst::blackflow
