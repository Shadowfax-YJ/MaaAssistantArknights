#pragma once

#include <optional>

namespace cv
{
class Mat;
}

namespace asst::blackflow::perception
{
[[nodiscard]] std::optional<int> recognize_ingots(const cv::Mat& image);
}
