#include "BlackFlowMovementRecognition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

#include <opencv2/core.hpp>

#include "Vision/Matcher.h"

namespace asst::blackflow
{
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
        // M04（重弹簧）、M12（简易遥控器）缺少 Movement 模板与 MovementLoaded 任务
        // LoadedTask { MovementKind::M04, "BlackFlow@Roguelike@MovementLoaded-M04" },
        LoadedTask { MovementKind::M05, "BlackFlow@Roguelike@MovementLoaded-M05" },
        LoadedTask { MovementKind::M06, "BlackFlow@Roguelike@MovementLoaded-M06" },
        LoadedTask { MovementKind::M07, "BlackFlow@Roguelike@MovementLoaded-M07" },
        LoadedTask { MovementKind::M08, "BlackFlow@Roguelike@MovementLoaded-M08" },
        LoadedTask { MovementKind::M09, "BlackFlow@Roguelike@MovementLoaded-M09" },
        LoadedTask { MovementKind::M10, "BlackFlow@Roguelike@MovementLoaded-M10" },
        LoadedTask { MovementKind::M11, "BlackFlow@Roguelike@MovementLoaded-M11" },
        // LoadedTask { MovementKind::M12, "BlackFlow@Roguelike@MovementLoaded-M12" },
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
