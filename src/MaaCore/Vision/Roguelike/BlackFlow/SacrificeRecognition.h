#pragma once

#include <string>
#include <vector>

#include "Common/AsstTypes.h"

namespace cv
{
class Mat;
}

namespace asst::blackflow::perception
{
std::vector<Rect> sacrifice_visible_items(const cv::Mat& image);
bool sacrifice_item_selected(const cv::Mat& image, const Rect& card);
std::string sacrifice_selected_name(const cv::Mat& image);
}
