#pragma once

#include <cmath>
#include <map>
#include <set>
#include <regex>
#include <span>
#include <opencv2/imgproc.hpp>
#include "BlackFlowMovementRecognition.h"

namespace asst::blackflow
{
using CivilizationInventorySlots = std::map<std::pair<int, int>, TextRect>;

inline CivilizationInventorySlots civilization_inventory_slots(std::span<const TextRect> names, const cv::Mat& image, bool empty_slots = false)
{
    CivilizationInventorySlots result;
    std::set<std::pair<int, int>> ambiguous;
    for (const auto& name : names) {
        const int x = name.rect.x + name.rect.width / 2;
        const int y = name.rect.y + name.rect.height / 2;
        const int first_y = empty_slots ? 236 : 270;
        const int row = (y - first_y + 25) / 162;
        if (x < 348 || x >= (empty_slots ? image.cols : 1190) || row < 0 || row >= 3 || std::abs(y - (first_y + row * 162)) > 30 ||
            name.score < 0.85 || !inventory_name_is_complete(name.rect, image.cols, image.rows)) {
            continue;
        }
        const auto slot = std::pair { x < 760 ? 0 : 1, row };
        if (ambiguous.contains(slot)) continue;
        const auto [existing, inserted] = result.emplace(slot, name);
        if (!inserted && existing->second.text != name.text) {
            result.erase(existing);
            ambiguous.insert(slot);
        }
    }
    return result;
}

inline bool civilization_slot_layout_equal(const CivilizationInventorySlots& a, const CivilizationInventorySlots& b)
{
    if (a.size() != b.size()) return false;
    for (const auto& [slot, name] : a) {
        const auto other = b.find(slot);
        if (other == b.end() || other->second.text != name.text ||
            std::abs(name.rect.x * 2 + name.rect.width - other->second.rect.x * 2 - other->second.rect.width) > 6 ||
            std::abs(name.rect.y * 2 + name.rect.height - other->second.rect.y * 2 - other->second.rect.height) > 6) return false;
    }
    return true;
}

inline double civilization_card_description_similarity(
    const cv::Mat& before, const TextRect& a, const cv::Mat& after, const TextRect& b)
{
    // Static description text excludes the animated HUD and card aura. Search
    // a few pixels around the OCR anchor to absorb box/JPEG jitter.
    const cv::Rect source(a.rect.x + a.rect.width / 2 + 55, a.rect.y + a.rect.height / 2 - 77, 220, 53);
    const cv::Rect target(b.rect.x + b.rect.width / 2 + 49, b.rect.y + b.rect.height / 2 - 82, 232, 63);
    if ((source & cv::Rect(0,0,before.cols,before.rows)) != source ||
        (target & cv::Rect(0,0,after.cols,after.rows)) != target) return 0;
    cv::Mat old_gray, new_gray, scores;
    cv::cvtColor(before(source), old_gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(after(target), new_gray, cv::COLOR_BGR2GRAY);
    cv::matchTemplate(new_gray, old_gray, scores, cv::TM_CCOEFF_NORMED);
    double maximum = 0; cv::minMaxLoc(scores, nullptr, &maximum);
    return std::isfinite(maximum) ? maximum : 0;
}

inline bool civilization_inventory_advanced_one_column(
    const cv::Mat& before, const CivilizationInventorySlots& previous,
    const cv::Mat& after, const CivilizationInventorySlots& current)
{
    for (int row = 0; row < 3; ++row) {
        const auto old_right = previous.find({1,row}), new_left = current.find({0,row});
        if (old_right == previous.end() || new_left == current.end() ||
            old_right->second.text != new_left->second.text ||
            civilization_card_description_similarity(before, old_right->second, after, new_left->second) < 0.88) return false;
    }
    bool changed = previous.size() != current.size();
    for (const auto& [slot, name] : previous) {
        const auto next = current.find(slot);
        if (next == current.end() || next->second.text != name.text ||
            civilization_card_description_similarity(before, name, after, next->second) < 0.90) {
            changed = true;
        }
    }
    // Identical names still represent separate instances, but a completely
    // indistinguishable viewport cannot prove that the swipe moved at all.
    return changed;
}

inline bool civilization_inventory_page_complete(
    const CivilizationInventorySlots& names, const CivilizationInventorySlots& empty, int page, int total)
{
    for (int column = 0; column < 2; ++column) for (int row = 0; row < 3; ++row) {
        const auto key = std::pair { column, row };
        const bool occupied = (page + column) * 3 + row < total;
        if (occupied ? !names.contains(key) || empty.contains(key) : names.contains(key) || !empty.contains(key)) return false;
    }
    return true;
}
} // namespace asst::blackflow
