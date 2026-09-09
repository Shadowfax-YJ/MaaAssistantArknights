#include "SacrificeRecognition.h"

#include "Config/TaskData.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Vision/OCRer.h"

namespace asst::blackflow::perception
{
bool sacrifice_item_selected(const cv::Mat& image, const Rect& card)
{
    const Rect edge { card.x + 10, card.y + 93, 82, 10 };
    if (image.empty() || edge.x < 0 || edge.y < 0 || edge.x + edge.width > image.cols ||
        edge.y + edge.height > image.rows) {
        return false;
    }
    cv::Mat hsv, selected;
    cv::cvtColor(make_roi(image, edge), hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(65, 85, 90), cv::Scalar(100, 255, 255), selected);
    return static_cast<double>(cv::countNonZero(selected)) / selected.total() >= 0.30;
}

std::vector<Rect> sacrifice_visible_items(const cv::Mat& image)
{
    OCRer title(image);
    title.set_task_info("BlackFlow@Roguelike@SacrificePicker");
    if (!title.analyze()) {
        return {};
    }
    const auto first = Task.get("BlackFlow@Roguelike@SacrificeFirstItem")->specific_rect;
    std::vector<Rect> cards;
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 5; ++column) {
            const Rect card { first.x + column * 113, first.y + row * 113, 102, 104 };
            if (card.x + 102 > image.cols || card.y + 110 > image.rows) {
                continue;
            }
            const auto inside = make_roi(gray, Rect { card.x + 14, card.y + 88, 74, 11 });
            const auto outside = make_roi(gray, Rect { card.x + 14, card.y + 106, 74, 4 });
            cv::Mat dark;
            cv::threshold(inside, dark, 65, 255, cv::THRESH_BINARY_INV);
            // 空白背景没有卡片的黑色底边；选中卡片的底边则会变成青色高亮。
            if ((static_cast<double>(cv::countNonZero(dark)) / dark.total() >= 0.55 &&
                 cv::mean(outside)[0] - cv::mean(inside)[0] >= 12) ||
                sacrifice_item_selected(image, card)) {
                cards.push_back(card);
            }
        }
    }
    return cards;
}

std::string sacrifice_selected_name(const cv::Mat& image)
{
    OCRer label(image);
    label.set_task_info("BlackFlow@Roguelike@SacrificeSelectedLabel");
    if (!label.analyze()) {
        return {};
    }
    OCRer name(image);
    name.set_task_info("BlackFlow@Roguelike@SacrificeSelectedName");
    if (!name.analyze() || name.get_result().size() != 1) {
        return {};
    }
    return name.get_result().front().text;
}
}
