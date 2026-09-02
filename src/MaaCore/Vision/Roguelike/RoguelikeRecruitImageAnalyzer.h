#pragma once
#include "Vision/VisionHelper.h"

#include "Common/AsstBattleDef.h"

#include <unordered_set>

namespace asst
{
class RoguelikeRecruitImageAnalyzer final : public VisionHelper
{
public:
    using VisionHelper::VisionHelper;
    virtual ~RoguelikeRecruitImageAnalyzer() noexcept override = default;

    bool analyze();

    const auto& get_result() const noexcept { return m_result; }
    const auto& get_detected_names() const noexcept { return m_detected_names; }

private:
    int match_elite(const Rect& raw_roi);
    static int match_level(const cv::Mat& image, const Rect& raw_roi);

    std::vector<battle::roguelike::Recruitment> m_result;
    std::unordered_set<std::string> m_detected_names;
};
}
