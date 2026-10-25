#include "BlackFlowMovementRecognition.h"
#include "BlackFlowEvidenceFrame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <regex>
#include <string_view>
#include <utility>

#include <opencv2/core.hpp>

#include "Vision/Matcher.h"
#include "Vision/OCRer.h"

namespace asst::blackflow
{
bool movement_inventory_page_visible(const cv::Mat& image)
{
    if (image.empty()) {
        return false;
    }
    Matcher opened(image);
    opened.set_task_info("BlackFlow@Roguelike@MovementInventoryOpened");
    if (!opened.analyze().has_value()) {
        return false;
    }
    OCRer contents(image);
    contents.set_task_info("BlackFlow@Roguelike@MovementInventoryContents");
    return contents.analyze().has_value();
}

std::optional<InventoryCapacity> inventory_capacity_from_ocr(std::span<const TextRect> values)
{
    const std::regex fraction(R"((^|[^0-9-])([0-9]{1,2})\s*/\s*([0-9]{1,2})([^0-9]|$))");
    std::optional<InventoryCapacity> result;
    for (const auto& value : values) {
        if (value.score < 0.85) {
            continue;
        }
        std::smatch match;
        if (!std::regex_search(value.text, match, fraction)) {
            continue;
        }
        InventoryCapacity current { std::stoi(match[2].str()), std::stoi(match[3].str()) };
        if (current.limit <= 0 || (result && *result != current)) {
            return std::nullopt;
        }
        result = current;
    }
    return result;
}

InventoryOverloadObservation recognize_inventory_overload(const cv::Mat& image)
{
    InventoryOverloadObservation result;
    if (image.empty()) {
        return result;
    }
    for (const auto* task : { "LoadingText",
                              "BlackFlow@Roguelike@DataSyncConfirm",
                              "BlackFlow@Roguelike@MovementInventoryDiscardButton",
                              "BlackFlow@Roguelike@MovementInventoryEquippedDiscardConfirm" }) {
        OCRer overlay(image);
        overlay.set_task_info(task);
        if (overlay.analyze()) {
            return result;
        }
    }
    for (const auto* task : { "BlackFlow@Roguelike@CloseCollection",
                              "BlackFlow@Roguelike@CloseCollectionContinue",
                              "BlackFlow@Roguelike@SacrificeInventoryDetailsClose" }) {
        Matcher overlay(image);
        overlay.set_task_info(task);
        if (overlay.analyze()) {
            return result;
        }
    }
    if (movement_inventory_page_visible(image)) {
        result.page = InventoryOverloadPage::Inventory;
    }
    else {
        Matcher opened(image);
        opened.set_task_info("BlackFlow@Roguelike@MovementInventoryOpened");
        if (opened.analyze()) {
            return result;
        }
        for (const auto* task : { "BlackFlow@Roguelike@MapPrepare-Ready", "BlackFlow@Roguelike@MapPrepare-ZoomOut" }) {
            Matcher map(image);
            map.set_task_info(task);
            if (map.analyze()) {
                result.page = InventoryOverloadPage::Map;
            }
        }
    }
    if (result.page == InventoryOverloadPage::Unknown) {
        return result;
    }
    OCRer capacity(image);
    capacity.set_task_info(
        result.page == InventoryOverloadPage::Inventory ? "BlackFlow@Roguelike@SacrificeInventoryCount"
                                                        : "BlackFlow@Roguelike@MovementInventoryMapCapacity");
    (void)capacity.analyze();
    result.capacity_ocr = capacity.get_result();
    result.capacity = inventory_capacity_from_ocr(result.capacity_ocr);
    OCRer banner(image);
    banner.set_task_info("BlackFlow@Roguelike@MovementInventoryOverloadedBanner");
    result.banner = banner.analyze().has_value();
    if (result.page == InventoryOverloadPage::Map) {
        OCRer prompt(image);
        prompt.set_task_info("BlackFlow@Roguelike@MovementInventoryResetExpand");
        result.banner = prompt.analyze().has_value() || result.banner;
    }
    // An old banner and a new count may coexist during the discard animation.
    if (result.capacity && result.capacity->count <= result.capacity->limit && result.banner) {
        result.capacity.reset();
    }
    return result;
}

bool inventory_reset_can_confirm_early(
    const cv::Mat& previous,
    const cv::Mat& current,
    std::span<const TextRect> previous_names,
    std::span<const TextRect> current_names)
{
    if (!inventory_name_layout_unchanged(previous_names, current_names)) {
        return false;
    }
    bool has_item_anchor = false;
    for (const auto& name : current_names) {
        if (name.text == "待收集零件") {
            continue;
        }
        has_item_anchor = true;
        if (!inventory_name_is_complete(name.rect, current.cols, current.rows)) {
            return false;
        }
    }
    // Exclude the animated box and HUD, but include the entire scrolling list:
    // matching only names would miss a different item instance with the same label.
    // Duplicate names do not invalidate unchanged positions AND unchanged list pixels.
    const cv::Rect list(
        current.cols * 360 / 1280,
        current.rows * 140 / 720,
        current.cols * 860 / 1280,
        current.rows * 475 / 720);
    return has_item_anchor && evidence_frames_match(previous, current, list, 1.0);
}

std::optional<std::pair<int, std::vector<MovementInventoryStarSlot>>>
    recognize_movement_inventory_remaining_uses(
        const cv::Mat& image,
        const Rect& name_rect,
        int maximum_uses)
{
    constexpr int BrightChannelMinimum = 200;
    if (image.empty() || maximum_uses <= 0 || image.channels() < 3) {
        return std::nullopt;
    }

    std::vector<MovementInventoryStarSlot> slots;
    slots.reserve(static_cast<std::size_t>(maximum_uses));
    int remaining_uses = 0;
    const int minimum_bright_pixels = std::max(
        3,
        static_cast<int>(std::lround(
            5.0 * static_cast<double>(image.cols) / 1280.0 * static_cast<double>(image.rows) / 720.0)));
    const cv::Rect image_bounds(0, 0, image.cols, image.rows);
    for (int slot = 0; slot < maximum_uses; ++slot) {
        const Rect expected = movement_inventory_star_slot_rect(name_rect, slot, image.cols, image.rows);
        const cv::Rect requested(expected.x, expected.y, expected.width, expected.height);
        const cv::Rect actual = requested & image_bounds;
        if (actual != requested || actual.empty()) {
            return std::nullopt;
        }

        int bright_pixels = 0;
        for (int y = actual.y; y < actual.y + actual.height; ++y) {
            const uchar* row = image.ptr<uchar>(y);
            for (int x = actual.x; x < actual.x + actual.width; ++x) {
                const uchar* pixel = row + x * image.channels();
                if (pixel[0] > BrightChannelMinimum && pixel[1] > BrightChannelMinimum &&
                    pixel[2] > BrightChannelMinimum) {
                    ++bright_pixels;
                }
            }
        }
        const bool lit = bright_pixels >= minimum_bright_pixels;
        remaining_uses += lit ? 1 : 0;
        slots.emplace_back(MovementInventoryStarSlot { expected, lit, bright_pixels });
    }
    return std::pair { remaining_uses, std::move(slots) };
}

std::optional<LoadedMovementRecognition> recognize_loaded_movement_with_evidence(const cv::Mat& image)
{
    struct LoadedTask
    {
        MovementKind movement;
        std::string_view task;
    };

    static constexpr std::array LoadedTasks = {
        LoadedTask { MovementKind::Walk, "BlackFlow@Roguelike@MovementLoaded-Walk" },
        LoadedTask { MovementKind::M01, "BlackFlow@Roguelike@MovementLoaded-M01" },
        LoadedTask { MovementKind::M02, "BlackFlow@Roguelike@MovementLoaded-M02" },
        LoadedTask { MovementKind::M03, "BlackFlow@Roguelike@MovementLoaded-M03" },
        // M04、M12 暂无实录图标模板；这些加工品仍由选择面板中的
        // 名称和“装载中”标记阳性复核，不能用其他加工品的图标冒充。
        // LoadedTask { MovementKind::M04, "BlackFlow@Roguelike@MovementLoaded-M04" },
        LoadedTask { MovementKind::M05, "BlackFlow@Roguelike@MovementLoaded-M05" },
        LoadedTask { MovementKind::M06, "BlackFlow@Roguelike@MovementLoaded-M06" },
        LoadedTask { MovementKind::M07, "BlackFlow@Roguelike@MovementLoaded-M07" },
        LoadedTask { MovementKind::M08, "BlackFlow@Roguelike@MovementLoaded-M08" },
        LoadedTask { MovementKind::M09, "BlackFlow@Roguelike@MovementLoaded-M09" },
        LoadedTask { MovementKind::M10, "BlackFlow@Roguelike@MovementLoaded-M10" },
        LoadedTask { MovementKind::M11, "BlackFlow@Roguelike@MovementLoaded-M11" },
        // LoadedTask { MovementKind::M12, "BlackFlow@Roguelike@MovementLoaded-M12" },
        LoadedTask { MovementKind::M13, "BlackFlow@Roguelike@MovementLoaded-M13" },
        LoadedTask { MovementKind::M14, "BlackFlow@Roguelike@MovementLoaded-M14" },
        LoadedTask { MovementKind::M15, "BlackFlow@Roguelike@MovementLoaded-M15" },
        LoadedTask { MovementKind::M16, "BlackFlow@Roguelike@MovementLoaded-M16" },
    };

    std::optional<LoadedMovementRecognition> best;
    double best_score = 0.0;
    for (const LoadedTask& loaded : LoadedTasks) {
        Matcher matcher(image);
        matcher.set_task_info(std::string(loaded.task));
        const auto result = matcher.analyze();
        if (result.has_value() && result->score > best_score) {
            best = LoadedMovementRecognition { loaded.movement, result->rect, result->score };
            best_score = result->score;
        }
    }
    return best;
}

std::optional<MovementKind> recognize_loaded_movement(const cv::Mat& image)
{
    const auto recognition = recognize_loaded_movement_with_evidence(image);
    return recognition.has_value() ? std::optional(recognition->movement) : std::nullopt;
}
} // namespace asst::blackflow
