#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <opencv2/core.hpp>

#include "Vision/Roguelike/BlackFlow/PerceptionNode.h"

namespace asst::blackflow::perception
{
struct IdealCoordinate
{
    int row = -1;
    int column = -1;

    bool operator==(const IdealCoordinate&) const noexcept = default;
};

struct IdealDomainRecognition
{
    std::string status = "skipped";
    std::string reason;
    std::string ideology;
    std::string policy;
    std::optional<IdealCoordinate> source;
    std::vector<IdealCoordinate> domain;
    std::vector<IdealCoordinate> observed_domain;
    double score_margin = 0.0;
    bool heads_agree = false;
};

// 移植自 lubiao-wiki 的冻结版理想源/理想域节点特征与四个岭回归头。
// 理念和方针由 MAA 主动打开实托邦弹层 OCR 得到，本类不再识别 HUD 图标和难度。
class IdealDomainRecognizer
{
public:
    bool load(const std::filesystem::path& model_path, std::string& error);
    [[nodiscard]] IdealDomainRecognition recognize(
        const cv::Mat& canonical_bgr,
        const std::vector<Node>& nodes,
        int difficulty,
        std::string_view ideology,
        std::string_view policy) const;
    [[nodiscard]] bool loaded() const noexcept { return m_loaded; }

private:
    struct RidgeHead
    {
        std::vector<float> mean;
        std::vector<float> scale;
        std::vector<float> coefficient;
    };

    std::unordered_map<std::string, RidgeHead> m_heads;
    bool m_loaded = false;
};
} // namespace asst::blackflow::perception
