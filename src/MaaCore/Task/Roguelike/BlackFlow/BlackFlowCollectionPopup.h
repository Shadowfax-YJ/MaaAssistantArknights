#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BlackFlowModel.h"

namespace asst::blackflow
{
inline constexpr std::string_view CollectionPopupRootDirectory = "collection-popups";
inline constexpr std::string_view CollectionPopupRunLogAction = "collection.popup.capture";
inline constexpr std::string_view NodeEventRunLogAction = "node.event.capture";
inline constexpr std::string_view NodeGetDropRunLogAction = "node.get-drop.capture";
inline constexpr std::string_view NodeRecruitmentRunLogAction = "node.recruitment.capture";
inline constexpr std::string_view NodeStoreRunLogAction = "node.store.capture";
inline constexpr std::string_view NodeStorePurchaseRunLogAction = "node.store.purchase";

[[nodiscard]] inline constexpr bool is_node_evidence_run_log_action(std::string_view action) noexcept
{
    return action == CollectionPopupRunLogAction || action == NodeEventRunLogAction ||
           action == NodeGetDropRunLogAction || action == NodeRecruitmentRunLogAction ||
           action == NodeStoreRunLogAction ||
           action == NodeStorePurchaseRunLogAction;
}

[[nodiscard]] inline constexpr bool node_recruitment_page_task(std::string_view task) noexcept
{
    // ProcessTask callbacks may expose the fully-qualified resource key, a key
    // with its theme prefix stripped, or only the resolved task basename.
    return task == "ChooseOper" || task == "Roguelike@ChooseOper" ||
           task.ends_with("@Roguelike@ChooseOper");
}

[[nodiscard]] inline constexpr std::string_view recruitment_page_flag_task(std::string_view task) noexcept
{
    return task.find("StartExplore@Roguelike@ChooseOper") != std::string_view::npos
        ? "BlackFlow@StartExplore@Roguelike@ChooseOperFlag"
        : "BlackFlow@Roguelike@ChooseOperFlag";
}

enum class CollectionPopupButton
{
    Close,
    Continue,
};

enum class CollectionPopupSource
{
    None,
    StartReward,
    SquadReward,
    FloorEntry,
};

enum class NodeGetDropScreen
{
    Drop,
    Select,
};

[[nodiscard]] inline std::optional<NodeGetDropScreen> node_get_drop_screen(std::string_view task) noexcept
{
    if (task == "BlackFlow@Roguelike@GetDrop") {
        return NodeGetDropScreen::Drop;
    }
    if (task == "BlackFlow@Roguelike@GetDropSelect" ||
        task == "BlackFlow@Roguelike@GetDropSelectReward" ||
        task == "BlackFlow@Roguelike@GetDropTrophyReward") {
        return NodeGetDropScreen::Select;
    }
    return std::nullopt;
}

// TrophyReward 的 ProcessTask 命中框只是触发器，真正选择由专用插件按藏品优先级决定。
// 因此必须等专用插件拿到实际点击框后再采集，不能由通用 ProcessTask 插件提前保存。
[[nodiscard]] inline constexpr bool node_get_drop_uses_custom_selector(std::string_view task) noexcept
{
    return task == "BlackFlow@Roguelike@GetDropTrophyReward";
}

[[nodiscard]] inline constexpr std::string_view to_string(NodeGetDropScreen screen) noexcept
{
    return screen == NodeGetDropScreen::Select ? "select" : "drop";
}

// 普通 GetDrop 证据只属于战斗结算；GetDropSelectReward 还会由得偿所愿等
// 非战斗事件弹出，因此无论当前页面是否为战斗都必须保存。
[[nodiscard]] inline bool node_get_drop_should_capture(std::string_view task, bool combat_page) noexcept
{
    if (!node_get_drop_screen(task).has_value()) {
        return false;
    }
    return combat_page || task == "BlackFlow@Roguelike@GetDropSelectReward";
}

struct DropOptionSelection
{
    int option_count = 0;
    int selected_index_from_left = 0;
};

// 点击前用 MultiMatcher 找出同一排的全部按钮，再把 ProcessTask 实际命中的按钮
// 映射到按横坐标排序后的序号。当前界面只接受二选一或三选一。
[[nodiscard]] inline std::optional<DropOptionSelection> resolve_drop_option_selection(
    std::vector<Rect> option_buttons,
    const Rect& selected_button) noexcept
{
    if (option_buttons.size() < 2 || option_buttons.size() > 3) {
        return std::nullopt;
    }
    std::ranges::sort(option_buttons, {}, [](const Rect& rect) {
        return rect.x + rect.width / 2;
    });
    const int selected_center = selected_button.x + selected_button.width / 2;
    std::size_t nearest = 0;
    int nearest_distance = std::numeric_limits<int>::max();
    for (std::size_t index = 0; index < option_buttons.size(); ++index) {
        const int center = option_buttons[index].x + option_buttons[index].width / 2;
        const int distance = std::abs(center - selected_center);
        if (distance < nearest_distance) {
            nearest = index;
            nearest_distance = distance;
        }
    }
    return DropOptionSelection {
        static_cast<int>(option_buttons.size()),
        static_cast<int>(nearest + 1),
    };
}

[[nodiscard]] inline std::optional<CollectionPopupButton> collection_popup_button(std::string_view task) noexcept
{
    if (!task.starts_with("BlackFlow@") && task.find("BlackFlow@") == std::string_view::npos) {
        return std::nullopt;
    }
    if (task.find("CloseCollectionContinue") != std::string_view::npos) {
        return CollectionPopupButton::Continue;
    }
    if (task.find("StageEncounterSpecialClose") != std::string_view::npos ||
        task.find("CloseInterview") != std::string_view::npos ||
        task.find("CloseCollection") != std::string_view::npos) {
        return CollectionPopupButton::Close;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::string_view to_string(CollectionPopupButton button) noexcept
{
    return button == CollectionPopupButton::Continue ? "next" : "x";
}

[[nodiscard]] inline CollectionPopupSource collection_popup_source(std::string_view task) noexcept
{
    if (task.find("LastReward") != std::string_view::npos) {
        return CollectionPopupSource::StartReward;
    }
    if (task.find("SquadConfirm") != std::string_view::npos) {
        return CollectionPopupSource::SquadReward;
    }
    if (task.find("NextLevel") != std::string_view::npos) {
        return CollectionPopupSource::FloorEntry;
    }
    return CollectionPopupSource::None;
}

[[nodiscard]] inline bool collection_popup_needs_landing_resolution(std::string_view task) noexcept
{
    return task.find("EnteredPageClassificationRewardPrepare") != std::string_view::npos;
}

[[nodiscard]] inline std::filesystem::path collection_popup_regular_node_directory(int floor, NodeId node)
{
    return std::filesystem::path(CollectionPopupRootDirectory) / ("floor-" + std::to_string(floor)) /
           ("node-" + std::to_string(node));
}

[[nodiscard]] inline std::filesystem::path
    collection_popup_virtual_node_directory(int floor, std::string_view name, std::uint64_t page_revision = 0)
{
    std::string folder;
    if (name == "安眠一隅") {
        folder = "node-redacted-rest-corner";
    }
    else if (name == "追猎") {
        folder = "node-dangerous-enemy";
    }
    else {
        folder = "node-special-event";
    }
    if (page_revision != 0) {
        folder += "-p" + std::to_string(page_revision);
    }
    return std::filesystem::path(CollectionPopupRootDirectory) / ("floor-" + std::to_string(floor)) / folder;
}

[[nodiscard]] inline std::filesystem::path collection_popup_source_directory(CollectionPopupSource source, int floor)
{
    const std::filesystem::path sources = std::filesystem::path(CollectionPopupRootDirectory) / "sources";
    switch (source) {
    case CollectionPopupSource::StartReward:
        return sources / "start-reward";
    case CollectionPopupSource::SquadReward:
        return sources / "squad-reward";
    case CollectionPopupSource::FloorEntry:
        return sources / "floor-entry" / ("floor-" + std::to_string(floor));
    case CollectionPopupSource::None:
        break;
    }
    return std::filesystem::path(CollectionPopupRootDirectory) / "other";
}

[[nodiscard]] inline std::filesystem::path collection_popup_other_directory()
{
    return std::filesystem::path(CollectionPopupRootDirectory) / "other";
}
} // namespace asst::blackflow
