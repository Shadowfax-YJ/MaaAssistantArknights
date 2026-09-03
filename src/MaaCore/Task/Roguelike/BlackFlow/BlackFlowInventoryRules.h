#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include "BlackFlowModel.h"

namespace asst::blackflow
{
enum class InventoryPartCategory
{
    Concept,
    Natural,
    Processing,
    Unknown,
};

enum class InventoryDiscardBand
{
    BeforeQuotaExcess,
    QuotaExcess,
    Normal,
};

struct InventoryDiscardRank
{
    InventoryDiscardBand band = InventoryDiscardBand::Normal;
    int priority = std::numeric_limits<int>::max();
    // 只在 band 和 priority 相同（即同类加工品实例）时参与比较。
    // 未识别次数和非加工品排在可靠次数之后。
    int remaining_charges = std::numeric_limits<int>::max();

    auto operator<=>(const InventoryDiscardRank&) const = default;
};

// 同名加工品超出商店购买配额时，只把多出来的实例提前。
// “剩余次数少的先丢”同时决定哪些实例算作超额；未识别的次数由调用方传入
// numeric_limits<int>::max()，不会抢在可靠的低次数实例前面。
[[nodiscard]] inline std::vector<std::size_t>
    inventory_quota_excess_indices(std::span<const int> remaining_charges, std::size_t quota)
{
    if (remaining_charges.size() <= quota) {
        return {};
    }

    std::vector<std::size_t> indices(remaining_charges.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t lhs, std::size_t rhs) {
        return remaining_charges[lhs] < remaining_charges[rhs];
    });
    indices.resize(remaining_charges.size() - quota);
    return indices;
}

enum class InventoryScanAction
{
    InspectAllVisible,
    AdvanceTowardEnd,
    InspectNewRightColumn,
};

inline constexpr int InventoryMaximumSwipes = 4;
inline constexpr int InventoryRightEdgeReboundMinimumPixels = 48;

// 到达最右端后游戏列表会回弹，最右名称的中心会明显向左退。
// 这一帧仍与上一帧有较大像素差，不能只靠“画面未变”判定结束。
[[nodiscard]] constexpr bool inventory_scan_rebounded(
    std::optional<int> previous_rightmost_center,
    std::optional<int> current_rightmost_center) noexcept
{
    return previous_rightmost_center.has_value() && current_rightmost_center.has_value() &&
           *current_rightmost_center + InventoryRightEdgeReboundMinimumPixels < *previous_rightmost_center;
}

// 节点页面打开时，移动事务尚未回图应用；零件箱快照仍是移动前的状态。
// 事件路线评估和节点内商店都必须在副本上先投影本次入场所消耗的加工品，
// 并优先消耗剩余次数最少、当前已装载、零件箱位置靠前的实例。
[[nodiscard]] inline bool project_consumed_entry_processing_item(RunState& run, MovementKind movement)
{
    if (movement == MovementKind::Walk) {
        return true;
    }

    auto selected = run.resources.movement_instances.end();
    for (auto iter = run.resources.movement_instances.begin(); iter != run.resources.movement_instances.end(); ++iter) {
        if (iter->movement != movement || iter->remaining_charges <= 0) {
            continue;
        }
        if (selected == run.resources.movement_instances.end() ||
            std::tuple(iter->remaining_charges, !iter->loaded, iter->inventory_index) <
                std::tuple(selected->remaining_charges, !selected->loaded, selected->inventory_index)) {
            selected = iter;
        }
    }
    if (selected == run.resources.movement_instances.end()) {
        return false;
    }

    --selected->remaining_charges;
    if (selected->remaining_charges == 0) {
        const bool removed_loaded_instance = selected->loaded;
        run.resources.movement_instances.erase(selected);
        if (removed_loaded_instance) {
            run.active_movement.reset();
        }
    }
    rebuild_movement_aggregates(run.resources);
    return true;
}

// 零件名称 OCR 框只覆盖卡片底部文字；详情必须点击卡片图标。这里先集中这个换算，
// 避免清理流程继续把“名称文字中心”误当成可打开详情的位置。
[[nodiscard]] inline Rect inventory_part_detail_click_rect(const Rect& name_rect) noexcept
{
    // 卡片图标与名称同列，图标中心稳定地位于名称上方约 57 个逻辑像素。
    // 返回一个小点击框，既避开名称文字，也不碰相邻卡片。
    constexpr int ClickSize = 24;
    constexpr int IconCenterOffsetY = 57;
    const int center_x = name_rect.x + name_rect.width / 2;
    const int center_y = name_rect.y >= IconCenterOffsetY ? name_rect.y - IconCenterOffsetY : 0;
    return { center_x - ClickSize / 2, center_y - ClickSize / 2, ClickSize, ClickSize };
}

// 零件箱每次打开都默认位于最左端。先扫当前可见列，之后每次向右推进一列，
// 只计新进入视野的最右列；不再先做多余的“回到最左端”手势。
[[nodiscard]] constexpr std::array<InventoryScanAction, 9> inventory_full_scan_plan() noexcept
{
    return {
        InventoryScanAction::InspectAllVisible,     InventoryScanAction::AdvanceTowardEnd,
        InventoryScanAction::InspectNewRightColumn, InventoryScanAction::AdvanceTowardEnd,
        InventoryScanAction::InspectNewRightColumn, InventoryScanAction::AdvanceTowardEnd,
        InventoryScanAction::InspectNewRightColumn, InventoryScanAction::AdvanceTowardEnd,
        InventoryScanAction::InspectNewRightColumn,
    };
}
} // namespace asst::blackflow
