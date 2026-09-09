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
    // 从可见干员的职业交集确认招募券类别，不依赖精英化和等级识别是否成功。
    std::optional<battle::Role> get_detected_role() const;

private:
    int match_elite(const Rect& raw_roi);
    static int match_level(const cv::Mat& image, const Rect& raw_roi);

    std::vector<battle::roguelike::Recruitment> m_result;
    std::unordered_set<std::string> m_detected_names;
};
}
