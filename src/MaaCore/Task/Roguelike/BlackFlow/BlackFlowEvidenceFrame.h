#pragma once

#include <opencv2/core.hpp>

namespace asst::blackflow
{
// Name anchors may approach the right edge. Include the card's name, artwork
// and price, clipping only the part outside the screenshot.
[[nodiscard]] inline cv::Rect store_card_evidence_roi(int x, int y, const cv::Mat& image)
{
    return cv::Rect(x - 8, y - 8, 188, 188) & cv::Rect(0, 0, image.cols, image.rows);
}

// Compare the content that will be acted on, not the animated HUD or merchant.
[[nodiscard]] inline bool evidence_frames_match(
    const cv::Mat& previous,
    const cv::Mat& current,
    cv::Rect roi,
    double maximum_mean_difference = 3.0)
{
    if (previous.empty() || current.empty() || previous.size() != current.size() || previous.type() != current.type() ||
        roi.empty() || (roi & cv::Rect(0, 0, current.cols, current.rows)) != roi) {
        return false;
    }
    const auto region = current(roi);
    return cv::norm(previous(roi), region, cv::NORM_L1) / (static_cast<double>(region.total()) * region.channels()) <=
           maximum_mean_difference;
}
} // namespace asst::blackflow
