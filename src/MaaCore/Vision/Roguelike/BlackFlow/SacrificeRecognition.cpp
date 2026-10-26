#include "SacrificeRecognition.h"

#include <regex>

#include "Config/TaskData.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"

namespace asst::blackflow::perception
{
std::optional<SacrificeItemList> sacrifice_item_list(const cv::Mat& image)
{
    if (image.empty() || image.cols < 1280 || image.rows < 720) {
        return std::nullopt;
    }
    Matcher title(image);
    title.set_task_info("BlackFlow@Roguelike@SacrificePicker");
    if (!title.analyze()) {
        return std::nullopt;
    }
    auto cards = sacrifice_visible_items(image);
    if (!cards.empty()) {
        return SacrificeItemList { std::move(cards), {}, {} };
    }
    // Failure to detect card borders is not evidence of an empty inventory. Require the
    // loaded picker controls plus an explicit empty message or a readable zero inventory.
    for (const auto task : { "LoadingText", "BlackFlow@Roguelike@DataSyncConfirm" }) {
        OCRer overlay(image);
        overlay.set_task_info(task);
        if (overlay.analyze()) {
            return std::nullopt;
        }
    }
    OCRer back(image);
    back.set_task_info("BlackFlow@Roguelike@SacrificeBack");
    OCRer heading(image);
    heading.set_task_info("BlackFlow@Roguelike@SacrificeListHeading");
    if (!back.analyze() || !heading.analyze() || heading.get_result().size() != 1) {
        return std::nullopt;
    }
    const auto& list_heading = heading.get_result().front().text;
    const bool parts = list_heading == "零件列表";
    if (!parts && list_heading != "藏品列表" && list_heading != "收藏品列表") {
        return std::nullopt;
    }
    OCRer empty(image);
    empty.set_task_info("BlackFlow@Roguelike@SacrificeEmptyList");
    if (empty.analyze() && empty.get_result().size() == 1 && empty.get_result().front().score >= 0.95) {
        return SacrificeItemList { {}, "explicit_empty_text", empty.get_result().front().text };
    }
    OCRer count(image);
    count.set_task_info(
        parts ? "BlackFlow@Roguelike@SacrificeInventoryHudCount" : "BlackFlow@Roguelike@SacrificeRelicHudCount");
    if (count.analyze() && count.get_result().size() == 1 && count.get_result().front().score >= 0.95) {
        static const std::regex zero_parts(R"(^\s*0\s*/\s*[1-9][0-9]{0,2}\s*$)");
        static const std::regex zero_relics(R"(^\s*0\s*$)");
        const auto& text = count.get_result().front().text;
        if (std::regex_match(text, parts ? zero_parts : zero_relics)) {
            return SacrificeItemList { {}, parts ? "zero_inventory_count" : "zero_relic_count", text };
        }
    }
    return std::nullopt;
}

std::optional<Rect> sacrifice_preferred_part(const cv::Mat& image, const std::vector<Rect>& cards)
{
    if (image.empty()) {
        return std::nullopt;
    }
    for (const auto& card : cards) {
        Matcher icon(image);
        icon.set_task_info("BlackFlow@Roguelike@SacrificeThinGrass");
        icon.set_roi(card);
        if (!icon.analyze()) {
            continue;
        }
        // Apply the part rule only to the parts picker, never the relic picker.
        OCRer heading(image);
        heading.set_task_info("BlackFlow@Roguelike@SacrificeListHeading");
        if (heading.analyze() && heading.get_result().size() == 1 &&
            heading.get_result().front().text == "零件列表") {
            return card;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

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
    Matcher title(image);
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
