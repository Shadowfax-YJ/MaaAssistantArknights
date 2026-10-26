#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/AsstTypes.h"

namespace cv
{
class Mat;
}

namespace asst::blackflow::perception
{
struct SacrificeItemList
{
    std::vector<Rect> cards;
    std::string empty_evidence;
    std::string observed_text;
    bool operator==(const SacrificeItemList&) const = default;
};

// nullopt is unknown, never an empty list. Empty cards require independent positive UI evidence.
std::optional<SacrificeItemList> sacrifice_item_list(const cv::Mat& image);
std::vector<Rect> sacrifice_visible_items(const cv::Mat& image);
std::optional<Rect> sacrifice_preferred_part(const cv::Mat& image, const std::vector<Rect>& cards);
bool sacrifice_item_selected(const cv::Mat& image, const Rect& card);
std::string sacrifice_selected_name(const cv::Mat& image);
}
