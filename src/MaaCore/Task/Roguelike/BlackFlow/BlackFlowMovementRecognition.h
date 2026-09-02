#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "BlackFlowModel.h"
#include "Common/AsstTypes.h"

namespace cv
{
class Mat;
}

namespace asst::blackflow
{
enum class MovementPanelScanAction
{
    ResetTowardStart,
    Inspect,
    AdvanceTowardEnd,
};

enum class MovementSelectionClickDecision
{
    ClickCurrentTarget,
    AcceptAlreadyLoaded,
    ReacquireTarget,
};

enum class MovementSelectionExhaustionDisposition
{
    ReplanWithoutTarget,
    Fail,
};

struct MovementPanelCandidate
{
    MovementKind movement = MovementKind::Walk;
    std::optional<int> remaining_charges;
};

// 同类加工品按零件箱实例保留；装载时优先消耗剩余次数最少的实例。
// 没识别到次数的卡片排在有可靠次数的卡片之后，避免覆盖可验证的选择。
[[nodiscard]] inline std::optional<std::size_t> choose_movement_panel_candidate(
    std::span<const MovementPanelCandidate> candidates,
    MovementKind target) noexcept
{
    std::optional<std::size_t> best;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].movement != target ||
            (candidates[index].remaining_charges.has_value() && *candidates[index].remaining_charges <= 0)) {
            continue;
        }
        if (!best.has_value() ||
            (candidates[index].remaining_charges.has_value() &&
             (!candidates[*best].remaining_charges.has_value() ||
              *candidates[index].remaining_charges < *candidates[*best].remaining_charges))) {
            best = index;
        }
    }
    return best;
}

struct MovementInventoryStarSlot
{
    Rect rect;
    bool lit = false;
    int bright_pixels = 0;
};

// 零件箱星星以 OCR 名称框中心为锚。名称在卡片内居中排版，文字长度会改变
// 名称框左右端，不能拿右端作锚；1/2/3 次加工品分别使用下列槽位前缀。
// 三颗星也不是严格等距直线，因此使用现场标定的离散偏移。坐标按 MAA 的
// 1280x720 标准画面等比缩放。
[[nodiscard]] inline Rect movement_inventory_star_slot_rect(
    const Rect& name_rect,
    int slot,
    int image_width = 1280,
    int image_height = 720) noexcept
{
    static constexpr std::array<int, 3> SlotOffsetX { 34, 44, 49 };
    static constexpr std::array<int, 3> SlotOffsetY { -106, -98, -83 };
    const double scale_x = static_cast<double>(image_width) / 1280.0;
    const double scale_y = static_cast<double>(image_height) / 720.0;
    const int slot_index = std::clamp(slot, 0, static_cast<int>(SlotOffsetX.size()) - 1);
    const int center_x = name_rect.x + name_rect.width / 2 +
                         static_cast<int>(std::lround(SlotOffsetX[slot_index] * scale_x));
    const int center_y = name_rect.y + name_rect.height / 2 +
                         static_cast<int>(std::lround(SlotOffsetY[slot_index] * scale_y));
    const int radius_x = std::max(3, static_cast<int>(std::lround(5.0 * scale_x)));
    const int radius_y = std::max(3, static_cast<int>(std::lround(5.0 * scale_y)));
    return Rect { center_x - radius_x, center_y - radius_y, radius_x * 2 + 1, radius_y * 2 + 1 };
}

// 零件箱扫描和过载清理必须共用同一套星槽标定，否则同一张卡会在两条流程里
// 得到不同剩余次数。返回值同时保留每个槽位证据，供诊断日志绘制。
[[nodiscard]] std::optional<std::pair<int, std::vector<MovementInventoryStarSlot>>>
    recognize_movement_inventory_remaining_uses(
        const cv::Mat& image,
        const Rect& name_rect,
        int maximum_uses);

inline constexpr int MovementPanelMaximumSwipes = 8;

// 规划目标来自进入节点时的零件箱扫描，是选择面板搜索的权威信源。面板连续几屏
// 可能恰好都是同一种加工品，因此“布局看起来没变”只能作为滚动诊断，不能提前
// 结束目标搜索；找到目标或扫满安全上限才停。
[[nodiscard]] constexpr bool movement_panel_should_continue_target_search(
    bool target_located,
    int completed_swipes) noexcept
{
    return !target_located && completed_swipes < MovementPanelMaximumSwipes;
}

struct MovementPanelLayoutEntry
{
    MovementKind movement = MovementKind::Walk;
    int vertical_center = 0;
};

inline constexpr int MovementPanelLayoutJitterTolerance = 10;

// 到底后的重复截图允许少量 OCR 抖动；仅“加工品种类相同”不够，因为连续几屏
// 可能全是标准引擎/气垫底座。数量、顺序和纵坐标都稳定，才能证明手势没有再移动列表。
[[nodiscard]] inline bool movement_panel_layout_unchanged(
    std::span<const MovementPanelLayoutEntry> previous,
    std::span<const MovementPanelLayoutEntry> current) noexcept
{
    if (previous.empty() || previous.size() != current.size()) {
        return false;
    }
    for (std::size_t index = 0; index < previous.size(); ++index) {
        if (previous[index].movement != current[index].movement ||
            std::abs(previous[index].vertical_center - current[index].vertical_center) >
                MovementPanelLayoutJitterTolerance) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool movement_panel_layout_overlaps(
    std::span<const MovementPanelLayoutEntry> previous,
    std::span<const MovementPanelLayoutEntry> current) noexcept
{
    return std::ranges::any_of(previous, [&](const MovementPanelLayoutEntry& left) {
        return std::ranges::any_of(current, [&](const MovementPanelLayoutEntry& right) {
            return left.movement == right.movement;
        });
    });
}

[[nodiscard]] constexpr bool movement_panel_scan_is_complete(
    bool top_anchor_seen,
    bool end_anchor_seen,
    bool continuous) noexcept
{
    return top_anchor_seen && end_anchor_seen && continuous;
}

// 移动面板会围绕当前装载项打开。先检查当前画面；“徒步跋涉”是列表第一项，
// 一旦看见它，执行端就跳过后续向上手势。看不见时逐次向上并在每次滑动后复查，
// 最后再从顶部向下完整扫描。
[[nodiscard]] constexpr std::array<MovementPanelScanAction, 1 + 4 * MovementPanelMaximumSwipes>
    movement_panel_full_scan_plan() noexcept
{
    std::array<MovementPanelScanAction, 1 + 4 * MovementPanelMaximumSwipes> plan {};
    std::size_t index = 0;
    plan[index++] = MovementPanelScanAction::Inspect;
    for (int swipe = 0; swipe < MovementPanelMaximumSwipes; ++swipe) {
        plan[index++] = MovementPanelScanAction::ResetTowardStart;
        plan[index++] = MovementPanelScanAction::Inspect;
    }
    for (int swipe = 0; swipe < MovementPanelMaximumSwipes; ++swipe) {
        plan[index++] = MovementPanelScanAction::AdvanceTowardEnd;
        plan[index++] = MovementPanelScanAction::Inspect;
    }
    return plan;
}

// 点击后若装载到了相邻卡片，只要新画面仍能看见目标，就应使用新框重试；
// 这类列表惯性偏移不能直接升级为整局失败。
[[nodiscard]] constexpr bool should_retry_movement_selection(
    MovementKind target,
    std::optional<MovementKind> loaded,
    [[maybe_unused]] bool target_visible,
    int attempts_remaining) noexcept
{
    return attempts_remaining > 0 && (!loaded.has_value() || *loaded != target);
}

// 列表滑动和装载相邻卡片都会触发自动居中。只有稳定后的当前帧仍然看得到目标时，
// 才允许按该帧坐标点击；目标已经移出画面时必须重新扫描，不能复用旧坐标。
[[nodiscard]] constexpr MovementSelectionClickDecision movement_selection_click_decision(
    MovementKind target,
    bool target_visible_in_stable_frame,
    std::optional<MovementKind> loaded) noexcept
{
    if (loaded.has_value() && *loaded == target) {
        return MovementSelectionClickDecision::AcceptAlreadyLoaded;
    }
    if (target_visible_in_stable_frame) {
        return MovementSelectionClickDecision::ClickCurrentTarget;
    }
    return MovementSelectionClickDecision::ReacquireTarget;
}

// 加工品确实出现过、但滚动回弹导致三次都无法稳定点击时，丢弃本层对该加工品的
// 可用性证据并重新规划。徒步不能这样降级，否则规划器连基础移动也会失去。
[[nodiscard]] constexpr MovementSelectionExhaustionDisposition movement_selection_exhaustion_disposition(
    MovementKind target,
    bool target_seen) noexcept
{
    return target_seen && target != MovementKind::Walk
               ? MovementSelectionExhaustionDisposition::ReplanWithoutTarget
               : MovementSelectionExhaustionDisposition::Fail;
}

// 加工品名字位于卡片底边。只向上偏移少量距离，并把顶部卡片的点击位置限制在
// 可见内容区内；旧的 32 像素偏移会在目标回弹到 y=201 时点进 y=169 的裁切区。
[[nodiscard]] inline Rect movement_panel_selection_rect(const Rect& name_rect) noexcept
{
    constexpr int SafeOffsetAboveName = 9;
    constexpr int VisiblePanelTop = 192;
    const int selection_y = std::max(VisiblePanelTop, name_rect.y - SafeOffsetAboveName);
    const int selection_height = std::max(1, std::min(name_rect.height, name_rect.y - selection_y));
    return Rect {
        name_rect.x,
        selection_y,
        name_rect.width,
        selection_height,
    };
}

struct LoadedMovementRecognition
{
    MovementKind movement = MovementKind::Walk;
    Rect rect;
    double score = 0.0;
};

[[nodiscard]] std::optional<LoadedMovementRecognition> recognize_loaded_movement_with_evidence(const cv::Mat& image);
[[nodiscard]] std::optional<MovementKind> recognize_loaded_movement(const cv::Mat& image);
}
