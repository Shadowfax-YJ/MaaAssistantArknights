#include "BlackFlowTaskPort.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "BlackFlowMovementRecognition.h"
#include "BlackFlowAutomationStoreRules.h"
#include "BlackFlowInventoryRules.h"
#include "BlackFlowCollectionPopup.h"
#include "BlackFlowCollectionPopupTaskPlugin.h"
#include "BlackFlowSession.h"

#include "Vision/Roguelike/BlackFlow/BlackFlowFloor.h"

#include "Config/Roguelike/BlackFlow/BlackFlowNodeExecutionConfig.h"
#include "Config/Roguelike/RoguelikeCopilotConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/AbstractTask.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Utils/StringMisc.hpp"
#include "Vision/Matcher.h"
#include "Vision/MultiMatcher.h"
#include "Vision/OCRer.h"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view CurrentActionPointsTask = "BlackFlow@Roguelike@CurrentActionPoints";
constexpr std::string_view CurrentIngotsTask = "BlackFlow@Roguelike@CurrentIngots";
constexpr std::string_view FloorFiveViewportSwipeLeftTask = "BlackFlow@Roguelike@MapViewportFloor5SwipeLeft";
constexpr std::string_view MapCaptureStabilityWaitTask = "BlackFlow@Roguelike@MapCaptureStabilityWait";
constexpr std::string_view MapReadyTask = "BlackFlow@Roguelike@MapPrepare-Ready";
constexpr std::string_view UtopiaPanelToggleTask = "BlackFlow@Roguelike@UtopiaPanelToggle";
constexpr std::string_view UtopiaPanelPolicyTask = "BlackFlow@Roguelike@UtopiaPanelPolicy";
constexpr std::string_view UtopiaPanelIdeologyTask = "BlackFlow@Roguelike@UtopiaPanelIdeology";
constexpr std::string_view UtopiaMapRefreshTask = "BlackFlow@Roguelike@UtopiaMapRefresh-Exit";
constexpr std::string_view UtopiaMapRefreshCompletedTask = "BlackFlow@Roguelike@UtopiaMapRefresh-Completed";
constexpr std::string_view MovementPanelTitleTask = "BlackFlow@Roguelike@MovementPanelTitle";
constexpr std::string_view MovePreviewEnterTask = "BlackFlow@Roguelike@MovePreviewEnter";
constexpr std::string_view MovePreviewCannotEnterTask = "BlackFlow@Roguelike@MovePreviewCannotEnter";
constexpr std::string_view MovePreviewCostTask = "BlackFlow@Roguelike@MovePreviewCost";
constexpr std::string_view MovePreviewDisplayedNameTask = "BlackFlow@Roguelike@MovePreviewDisplayedName";
constexpr std::string_view MovePreviewDisplayedCategoryTask =
    "BlackFlow@Roguelike@MovePreviewDisplayedCategory";
constexpr std::string_view MovePreviewStageNameTask = "BlackFlow@Roguelike@MovePreviewStageName";
constexpr std::string_view MovePreviewConfirmTask = "BlackFlow@Roguelike@MovePreviewConfirm";
constexpr std::string_view MoveConfirmDoorAnimationWaitTask =
    "BlackFlow@Roguelike@MoveConfirmDoorAnimationWait";
constexpr std::string_view InventorySwipeTask = "BlackFlow@Roguelike@MovementInventorySwipe";
constexpr std::string_view InventorySwipeToStartTask = "BlackFlow@Roguelike@MovementInventorySwipeToStart";
constexpr std::string_view InventoryOverloadPromptTask =
    "BlackFlow@Roguelike@MovementInventoryOverloadPrompt";
constexpr std::string_view InventoryOverloadOpenTask = "BlackFlow@Roguelike@MovementInventoryOverloadOpen";
constexpr std::string_view InventoryOverloadedBannerTask =
    "BlackFlow@Roguelike@MovementInventoryOverloadedBanner";
constexpr std::string_view InventoryAllItemsTask = "BlackFlow@Roguelike@MovementInventoryAllItems";
constexpr std::string_view InventoryDiscardPriorityTask =
    "BlackFlow@Roguelike@MovementInventoryDiscardPriority";
constexpr std::string_view InventoryDiscardBeforeQuotaExcessTask =
    "BlackFlow@Roguelike@MovementInventoryDiscardBeforeQuotaExcess";
constexpr std::string_view InventoryDiscardButtonTask = "BlackFlow@Roguelike@MovementInventoryDiscardButton";
constexpr std::string_view InventoryEquippedDiscardConfirmTask =
    "BlackFlow@Roguelike@MovementInventoryEquippedDiscardConfirm";
constexpr std::string_view InventoryDiscardDetailWaitTask =
    "BlackFlow@Roguelike@MovementInventoryDiscardDetailWait";
constexpr std::string_view InventoryOverloadCloseTask = "BlackFlow@Roguelike@MovementInventoryOverloadClose";
constexpr std::string_view InventoryNaturalPriorityTask = "BlackFlow@Roguelike@InventoryNaturalPriority";
constexpr std::string_view InventoryConceptPriorityTask = "BlackFlow@Roguelike@InventoryConceptPriority";
constexpr std::string_view EnteredPageClassificationTask = "BlackFlow@Roguelike@EnteredPageClassification";
constexpr std::string_view EnteredPageClassificationCombatTask =
    "BlackFlow@Roguelike@EnteredPageClassificationCombat";
constexpr std::string_view EnteredPageClassificationCombatOperatorConfirmTask =
    "BlackFlow@Roguelike@EnteredPageClassificationCombatOperatorConfirm";
constexpr std::string_view EnteredPageClassificationCombatOperatorBackTask =
    "BlackFlow@Roguelike@EnteredPageClassificationCombatOperatorBack";
constexpr std::string_view EnteredPageClassificationRetryWaitTask =
    "BlackFlow@Roguelike@EnteredPageClassificationRetryWait";
constexpr std::string_view EnteredPageClassificationEncounterPrepareTask =
    "BlackFlow@Roguelike@EnteredPageClassificationEncounterPrepare";
constexpr std::string_view EnteredPageClassificationRewardPrepareTask =
    "BlackFlow@Roguelike@EnteredPageClassificationRewardPrepare";
constexpr std::string_view StageEncounterOcrTask = "BlackFlow@Roguelike@StageEncounterOcr";
constexpr int EnteredPageClassificationRetryTimes = 2;
constexpr int EnteredPageRewardPrepareTimes = 3;
constexpr int EnteredPageCombatOperatorMaximumBacks = 2;
constexpr int MapCaptureStabilityAttempts = 8;
constexpr double MapCaptureMaximumMeanDifference = 3.0;
const cv::Rect BattlePreviewMapComparisonRoi { 170, 120, 910, 500 };
constexpr int RunLogCaptureStabilityAttempts = 16;
constexpr int RunLogCaptureStabilityInterval = 150;
constexpr int InventoryNewRightColumnMinimumX = 760;
constexpr double InventoryUnchangedFrameMaximumDifference = 3.0;
constexpr int InventoryMaximumDiscardAttempts = 16;

void set_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::optional<int> recognize_integer(const cv::Mat& image, std::string_view task_name)
{
    const auto task = Task.get<OcrTaskInfo>(task_name);
    if (task == nullptr) {
        return std::nullopt;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task);
    if (const auto number_task = Task.get<OcrTaskInfo>("NumberOcrReplace"); number_task != nullptr) {
        analyzer.set_replace(number_task->replace_map);
    }
    analyzer.set_use_char_model(true);
    const auto results = analyzer.analyze();
    if (!results.has_value() || results->size() != 1) {
        return std::nullopt;
    }
    int value = 0;
    if (!utils::chars_to_number(results->front().text, value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> recognize_move_preview_action_point_cost(const cv::Mat& image)
{
    const auto strict = recognize_integer(image, MovePreviewCostTask);
    if (strict.has_value() && *strict <= 0 && *strict >= -9) {
        return strict;
    }

    const auto task = Task.get(std::string(MovePreviewCostTask));
    OCRer analyzer(image, task->roi);
    analyzer.set_task_info(task);
    if (!analyzer.analyze()) {
        return std::nullopt;
    }
    for (const auto& result : analyzer.get_result()) {
        if (const auto parsed = parse_move_preview_action_point_cost(result.text); parsed.has_value()) {
            Log.info("Recovered move preview action point cost from OCR", result.text, *parsed);
            return parsed;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
    recognize_text(const cv::Mat& image, std::string_view task_name, const std::vector<std::string>& required = {})
{
    const auto task = Task.get<OcrTaskInfo>(task_name);
    if (task == nullptr) {
        return std::nullopt;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task);
    if (!required.empty()) {
        analyzer.set_required(required);
    }
    const auto results = analyzer.analyze();
    if (!results.has_value()) {
        return std::nullopt;
    }
    const std::vector<std::string>& whitelist = required.empty() ? task->text : required;
    for (const auto& result : *results) {
        if (std::ranges::find(whitelist, result.text) != whitelist.end()) {
            return result.text;
        }
    }
    return std::nullopt;
}

struct MovePreviewIdentity
{
    std::string name;
    NodeType type = NodeType::Unknown;
};

std::optional<MovePreviewIdentity> recognize_move_preview_identity(const cv::Mat& image)
{
    // 已揭示战斗的大标题是具体关卡名（例如“急不可耐”），节点类型写在上方小字“作战”中；
    // 未揭示战斗的小字同样是“作战”，必须让大标题“未知的凶戾”覆盖它。
    static const std::vector<std::string> HiddenPreviewNames { "未知的凶戾", "未知的诡秘" };
    const auto hidden_displayed_name = recognize_text(image, MovePreviewDisplayedNameTask, HiddenPreviewNames);
    const auto displayed_category =
        recognize_text(image, MovePreviewDisplayedCategoryTask, BlackFlowNodeExecution.preview_names());
    const auto displayed_title = hidden_displayed_name.has_value()
                                     ? hidden_displayed_name
                                     : recognize_text(
                                           image,
                                           MovePreviewDisplayedNameTask,
                                           BlackFlowNodeExecution.preview_names());
    const auto displayed_name =
        hidden_displayed_name.has_value() ? hidden_displayed_name
        : displayed_category.has_value()  ? displayed_category
                                          : displayed_title;
    if (!displayed_name.has_value()) {
        return std::nullopt;
    }
    const auto displayed_type = BlackFlowNodeExecution.preview_node_type(*displayed_name);
    if (!displayed_type.has_value()) {
        return std::nullopt;
    }
    return MovePreviewIdentity { *displayed_name, *displayed_type };
}

bool recognizes_text_fragment(const cv::Mat& image, std::string_view task_name, std::string_view fragment)
{
    const auto task = Task.get<OcrTaskInfo>(task_name);
    if (task == nullptr) {
        return false;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task);
    const auto results = analyzer.analyze();
    return results.has_value() && std::ranges::any_of(*results, [&](const auto& result) {
               return result.text == fragment || result.text.find(fragment) != std::string::npos;
           });
}

bool matches_template(const cv::Mat& image, std::string_view task_name)
{
    Matcher analyzer(image);
    analyzer.set_task_info(std::string(task_name));
    return analyzer.analyze().has_value();
}

MovePreviewFrameState classify_move_preview_frame(const cv::Mat& image)
{
    if (matches_template(image, MovePreviewEnterTask)) {
        return MovePreviewFrameState::Reachable;
    }
    if (matches_template(image, MovePreviewCannotEnterTask)) {
        return MovePreviewFrameState::Blocked;
    }
    return MovePreviewFrameState::Missing;
}

cv::Mat move_preview_stability_region(const cv::Mat& image)
{
    if (image.empty()) {
        return {};
    }
    // 只比较会承载分类、标题、关卡名和行动力消耗的右侧预览区域。地图上的粒子与
    // “居民”动画不应阻止预览识别，但预览文字自身仍在滑入时必须继续等待。
    const cv::Rect requested(900, 90, 380, 510);
    const cv::Rect bounds(0, 0, image.cols, image.rows);
    const cv::Rect actual = requested & bounds;
    return actual.empty() ? cv::Mat {} : image(actual);
}

struct CollectionPopupDestination
{
    std::filesystem::path directory;
    json::object attribution;
};

int collection_popup_floor(const BlackFlowSession& session)
{
    return session.current_floor().value_or(session.run().floor);
}

const Node* collection_popup_node(const BlackFlowSession& session, NodeId node)
{
    if (const Node* current = session.map().snapshot().find_node(node); current != nullptr) {
        return current;
    }
    return session.exploration_notebook().snapshot().find_node(node);
}

json::object serialize_node_battle(const NodeBattleRecord& battle)
{
    json::object result {
        { "stage_name", battle.stage_name },
    };
    if (battle.total_kills.has_value()) {
        result["total_kills"] = *battle.total_kills;
    }
    return result;
}

void add_battle_attribution(json::object& attribution, const std::optional<NodeBattleRecord>& battle)
{
    if (!battle.has_value()) {
        return;
    }
    attribution["battle"] = serialize_node_battle(*battle);
    if (battle->total_kills.has_value()) {
        attribution["battle_total_kills"] = *battle->total_kills;
    }
}

CollectionPopupDestination regular_collection_popup_destination(
    const BlackFlowSession& session,
    NodeId node,
    int fallback_floor,
    std::string_view evidence)
{
    const Node* metadata = collection_popup_node(session, node);
    const int floor = metadata == nullptr ? fallback_floor : metadata->floor;
    json::object attribution {
        { "kind", "node" },
        { "evidence", std::string(evidence) },
        { "floor", floor },
        { "node", node },
        { "node_name", metadata == nullptr ? std::string() : metadata->name },
        { "node_type", metadata == nullptr ? std::string("unclassified") : std::string(to_string(metadata->type)) },
    };
    if (metadata != nullptr) {
        attribution["row"] = metadata->position.row;
        attribution["column"] = metadata->position.column;
        add_battle_attribution(attribution, metadata->battle);
    }
    return { collection_popup_regular_node_directory(floor, node), std::move(attribution) };
}

CollectionPopupDestination virtual_collection_popup_destination(
    int floor,
    std::string name,
    std::uint64_t page_revision,
    std::string_view evidence)
{
    std::string type = "special_event";
    std::string type_display;
    if (name == "安眠一隅") {
        type = "redacted";
        type_display = "■■■■";
    }
    else if (name == "追猎") {
        type = "battle_boss";
        type_display = "险路恶敌";
    }
    return {
        collection_popup_virtual_node_directory(floor, name, page_revision),
        json::object {
            { "kind", "abstract_node" },
            { "evidence", std::string(evidence) },
            { "floor", floor },
            { "node_name", std::move(name) },
            { "node_type", std::move(type) },
            { "node_type_display", std::move(type_display) },
            { "page_revision", page_revision },
        },
    };
}

CollectionPopupDestination pursuit_collection_popup_destination(
    int floor,
    std::string stage_name,
    std::optional<int> total_kills)
{
    CollectionPopupDestination result {
        collection_popup_virtual_node_directory(floor, "追猎", 0),
        json::object {
            { "kind", "abstract_node" },
            { "evidence", "pursuit_lifecycle" },
            { "entry_kind", "pursuit" },
            { "floor", floor },
            { "node_name", std::move(stage_name) },
            { "node_type", "battle_boss" },
            { "node_type_display", "险路恶敌" },
            { "page_revision", 0 },
        },
    };
    if (!result.attribution.get("node_name", std::string()).empty()) {
        add_battle_attribution(
            result.attribution,
            NodeBattleRecord { result.attribution.get("node_name", std::string()), total_kills });
    }
    return result;
}

std::optional<CollectionPopupDestination> resolve_collection_popup_destination(
    const BlackFlowSession& session,
    std::string_view task,
    bool allow_current_landing)
{
    const int floor = collection_popup_floor(session);
    const CollectionPopupSource source = collection_popup_source(task);
    if (source != CollectionPopupSource::None) {
        std::string source_name;
        switch (source) {
        case CollectionPopupSource::StartReward:
            source_name = "start_reward";
            break;
        case CollectionPopupSource::SquadReward:
            source_name = "squad_reward";
            break;
        case CollectionPopupSource::FloorEntry:
            source_name = "floor_entry";
            break;
        case CollectionPopupSource::None:
            break;
        }
        return CollectionPopupDestination {
            collection_popup_source_directory(source, floor),
            json::object {
                { "kind", "source" },
                { "source", std::move(source_name) },
                { "floor", floor },
            },
        };
    }

    if (const auto& page = session.page_context(); page.has_value()) {
        if (const auto entering_floor = collection_popup_pending_floor_entry(
                task,
                page->floor,
                page->node_type,
                page->page_intent,
                page->changes_floor);
            entering_floor.has_value()) {
            return CollectionPopupDestination {
                collection_popup_source_directory(CollectionPopupSource::FloorEntry, *entering_floor),
                json::object {
                    { "kind", "source" },
                    { "source", "floor_entry" },
                    { "evidence", "exit_page_precedes_next_level_commit" },
                    { "floor", *entering_floor },
                    { "previous_floor", page->floor },
                    { "previous_node", page->node },
                    { "previous_node_name", page->node_name },
                    { "previous_node_type", std::string(to_string(page->node_type)) },
                },
            };
        }
    }

    if (floor <= 0 && session.run().current_node == InvalidNodeId && session.transaction() == nullptr) {
        return CollectionPopupDestination {
            collection_popup_source_directory(CollectionPopupSource::StartReward, floor),
            json::object {
                { "kind", "source" },
                { "source", "start_reward" },
                { "evidence", "before_first_floor" },
                { "floor", floor },
            },
        };
    }
    if (session.run().current_node != InvalidNodeId && floor > 0 &&
        static_cast<int>(session.run().current_node >> 48U) != floor) {
        return CollectionPopupDestination {
            collection_popup_source_directory(CollectionPopupSource::FloorEntry, floor),
            json::object {
                { "kind", "source" },
                { "source", "floor_entry" },
                { "evidence", "recognized_floor_precedes_map_rebuild" },
                { "floor", floor },
            },
        };
    }

    if (const auto pursuit_floor = session.collection_popup_pursuit_floor(); pursuit_floor.has_value()) {
        return pursuit_collection_popup_destination(
            *pursuit_floor,
            session.collection_popup_pursuit_stage_name(),
            session.collection_popup_pursuit_total_kills());
    }

    if (const auto& page = session.page_context(); page.has_value()) {
        if (!page->has_landing && !page->node_name.empty()) {
            CollectionPopupDestination destination = virtual_collection_popup_destination(
                page->floor,
                page->node_name,
                page->page_revision,
                "page_without_map_landing");
            add_battle_attribution(destination.attribution, page->battle);
            return destination;
        }
        if (page->node != InvalidNodeId) {
            CollectionPopupDestination destination =
                regular_collection_popup_destination(session, page->node, page->floor, "page_context");
            add_battle_attribution(destination.attribution, page->battle);
            return destination;
        }
    }

    if (const MoveTransaction* transaction = session.transaction(); transaction != nullptr) {
        const MoveCandidate& proposal = transaction->proposal();
        if (proposal.controllable) {
            const NodeId landing = proposal.landing == InvalidNodeId ? proposal.target : proposal.landing;
            if (landing != InvalidNodeId) {
                return regular_collection_popup_destination(session, landing, floor, "controllable_landing");
            }
        }
        else {
            // 小八界在回图前可能始终无法把同类型事件页唯一绑定到一个候选格；这一整个
            // 页面生命周期内的弹窗都必须延迟，不能退回到移动前的 current_node。
            return std::nullopt;
        }
    }

    const NodeId current = session.run().current_node;
    if (current != InvalidNodeId &&
        (allow_current_landing || session.run().visited_nodes.contains(current))) {
        return regular_collection_popup_destination(session, current, floor, "current_node");
    }
    return std::nullopt;
}

bool node_evidence_is_combat_page(const BlackFlowSession& session)
{
    if (session.collection_popup_pursuit_floor().has_value()) {
        return true;
    }
    if (const auto& page = session.page_context(); page.has_value()) {
        return is_combat_node_type(page->node_type) || page->battle.has_value();
    }
    if (const MoveTransaction* transaction = session.transaction(); transaction != nullptr) {
        const MoveCandidate& proposal = transaction->proposal();
        const NodeId landing = proposal.landing == InvalidNodeId ? proposal.target : proposal.landing;
        if (const Node* node = collection_popup_node(session, landing); node != nullptr) {
            return is_combat_node_type(node->type);
        }
    }
    if (const Node* node = collection_popup_node(session, session.run().current_node); node != nullptr) {
        return is_combat_node_type(node->type);
    }
    return false;
}

std::string normalize_inventory_name(std::string_view name)
{
    std::string result(name);
    if (result.starts_with("“")) {
        result.erase(0, std::string("“").size());
    }
    if (result.ends_with("”")) {
        result.erase(result.size() - std::string("”").size());
    }
    return result;
}

std::optional<InventoryPartCategory> inventory_part_category(std::string_view recognized_name)
{
    const std::string name = normalize_inventory_name(recognized_name);
    for (const MovementSpec& movement : movement_specs()) {
        if (movement.kind != MovementKind::Walk && name == normalize_inventory_name(movement.name)) {
            return InventoryPartCategory::Processing;
        }
    }

    const auto category_from_priority_task = [&](std::string_view task_name, InventoryPartCategory category)
        -> std::optional<InventoryPartCategory> {
        const auto task = Task.get<OcrTaskInfo>(task_name);
        if (task == nullptr) {
            return std::nullopt;
        }
        for (const std::string& configured_name : task->text) {
            if (name == normalize_inventory_name(configured_name)) {
                return category;
            }
        }
        return std::nullopt;
    };
    if (const auto category =
            category_from_priority_task(InventoryConceptPriorityTask, InventoryPartCategory::Concept);
        category.has_value()) {
        return category;
    }
    return category_from_priority_task(InventoryNaturalPriorityTask, InventoryPartCategory::Natural);
}

std::optional<std::size_t> inventory_part_discard_priority(std::string_view recognized_name)
{
    const auto task = Task.get<OcrTaskInfo>(InventoryDiscardPriorityTask);
    if (task == nullptr) {
        return std::nullopt;
    }
    const std::string name = normalize_inventory_name(recognized_name);
    for (std::size_t index = 0; index < task->text.size(); ++index) {
        if (name == normalize_inventory_name(task->text[index])) {
            return index;
        }
    }
    return std::nullopt;
}

bool inventory_part_precedes_quota_excess(std::string_view recognized_name)
{
    const auto task = Task.get<OcrTaskInfo>(InventoryDiscardBeforeQuotaExcessTask);
    if (task == nullptr) {
        return false;
    }
    const std::string name = normalize_inventory_name(recognized_name);
    return std::ranges::any_of(task->text, [&](const std::string& configured_name) {
        return name == normalize_inventory_name(configured_name);
    });
}

std::optional<int> recognize_inventory_part_valuation(const cv::Mat& image, const Rect& name_rect)
{
    const auto task = Task.get<OcrTaskInfo>("NumberOcrReplace");
    if (task == nullptr || image.empty()) {
        return std::nullopt;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task);
    // 零件卡片的估价位于名称上方、同一卡片右上角。逐卡局部 OCR，避免把容量、总估价
    // 或相邻卡片的数字误绑到当前自然物。
    Rect valuation_roi = name_rect.move({ 275, -135, 165, 105 });
    valuation_roi.x = std::clamp(valuation_roi.x, 0, image.cols);
    valuation_roi.y = std::clamp(valuation_roi.y, 0, image.rows);
    valuation_roi.width = std::clamp(valuation_roi.width, 0, image.cols - valuation_roi.x);
    valuation_roi.height = std::clamp(valuation_roi.height, 0, image.rows - valuation_roi.y);
    if (valuation_roi.empty()) {
        return std::nullopt;
    }
    analyzer.set_roi(valuation_roi);
    analyzer.set_replace(task->replace_map);
    analyzer.set_use_char_model(true);
    const auto results = analyzer.analyze();
    if (!results.has_value()) {
        return std::nullopt;
    }
    for (const auto& result : *results) {
        int value = 0;
        if (utils::chars_to_number(result.text, value) && value >= 0) {
            return value;
        }
    }
    return std::nullopt;
}

cv::Mat inventory_card_region(const cv::Mat& image)
{
    if (image.empty()) {
        return {};
    }
    const cv::Rect requested(348, 132, 930, 495);
    const cv::Rect bounds(0, 0, image.cols, image.rows);
    const cv::Rect actual = requested & bounds;
    return actual.empty() ? cv::Mat {} : image(actual);
}

bool map_capture_candidate_is_unobstructed(const cv::Mat& image, std::string& rejection_reason)
{
    if (!matches_template(image, MapReadyTask)) {
        rejection_reason = "zoomed-out map HUD is not visible";
        return false;
    }
    constexpr std::array KnownOcclusionTasks {
        MovePreviewEnterTask,
        MovePreviewCannotEnterTask,
        std::string_view("BlackFlow@Roguelike@MovementInventoryClose"),
        std::string_view("BlackFlow@Roguelike@CloseCollection"),
    };
    for (const std::string_view task : KnownOcclusionTasks) {
        if (matches_template(image, task)) {
            rejection_reason = "known overlay is still visible: " + std::string(task);
            return false;
        }
    }
    if (recognize_text(image, MovementPanelTitleTask).has_value()) {
        rejection_reason = "movement selection panel is still visible";
        return false;
    }
    rejection_reason.clear();
    return true;
}

Rect shrink_to_center(const Rect& rect, double ratio)
{
    const int width = std::max(1, static_cast<int>(rect.width * ratio));
    const int height = std::max(1, static_cast<int>(rect.height * ratio));
    return Rect { rect.x + (rect.width - width) / 2, rect.y + (rect.height - height) / 2, width, height };
}

} // namespace

class BlackFlowTaskPort::ProcessTaskContext final : public AbstractTask
{
public:
    ProcessTaskContext(
        const AsstCallback& callback,
        Assistant* inst,
        std::string_view task_chain,
        BlackFlowCollectionPopupTaskPlugin::Capture popup_capture) :
        AbstractTask(callback, inst, task_chain)
    {
        set_retry_times(0);
        register_plugin<BlackFlowCollectionPopupTaskPlugin>(std::move(popup_capture));
    }

    bool execute(std::vector<std::string> tasks, std::string* error)
    {
        m_tasks = std::move(tasks);
        m_last_task.clear();
        m_last_image.reset();
        const bool succeeded = AbstractTask::run();
        if (!succeeded) {
            set_error(error, "ProcessTask did not reach a successful terminal task");
        }
        return succeeded;
    }

    [[nodiscard]] const std::string& last_task() const noexcept { return m_last_task; }

    [[nodiscard]] std::shared_ptr<cv::Mat> last_image() const noexcept { return m_last_image; }

    [[nodiscard]] cv::Mat capture() const { return ctrler()->get_image(); }

    bool capture_stable_move_preview(
        cv::Mat& image,
        MovePreviewFrameState& state,
        int& sampled_frames,
        double& mean_difference,
        const std::function<bool(const cv::Mat&, MovePreviewFrameState)>& semantics_are_stable,
        std::string* error)
    {
        sampled_frames = 0;
        mean_difference = -1.0;
        std::optional<cv::Mat> previous;
        MovePreviewFrameState previous_state = MovePreviewFrameState::Missing;
        std::string last_rejection = "preview controls are not visible";

        for (int attempt = 0; attempt < MovePreviewStabilityAttempts; ++attempt) {
            if (attempt != 0) {
                sleep(MovePreviewStabilityInterval);
                if (need_exit()) {
                    set_error(error, "move preview stability capture was interrupted");
                    return false;
                }
            }

            cv::Mat current = capture();
            if (current.empty()) {
                set_error(error, "move preview stability capture returned an empty frame");
                return false;
            }
            ++sampled_frames;

            const MovePreviewFrameState current_state = classify_move_preview_frame(current);
            if (current_state == MovePreviewFrameState::Missing) {
                previous.reset();
                previous_state = MovePreviewFrameState::Missing;
                last_rejection = "preview controls are not visible";
                continue;
            }

            const cv::Mat previous_region =
                previous.has_value() ? move_preview_stability_region(*previous) : cv::Mat {};
            const cv::Mat current_region = move_preview_stability_region(current);
            if (!previous_region.empty() && previous_region.size() == current_region.size() &&
                previous_region.type() == current_region.type()) {
                const double denominator =
                    static_cast<double>(current_region.total()) * static_cast<double>(current_region.channels());
                mean_difference = cv::norm(previous_region, current_region, cv::NORM_L1) / denominator;
                if (move_preview_frame_is_stable(previous_state, current_state, mean_difference)) {
                    if (semantics_are_stable(current, current_state)) {
                        image = std::move(current);
                        state = current_state;
                        return true;
                    }
                    last_rejection = "preview pixels are stable but semantic content is not yet stable";
                }
                else {
                    last_rejection =
                        previous_state != current_state
                            ? "preview state changed between consecutive frames"
                            : "preview is still animating (mean frame difference " + std::to_string(mean_difference) +
                                  ")";
                }
            }
            else {
                mean_difference = -1.0;
                last_rejection = "waiting for a second comparable preview frame";
            }

            previous = std::move(current);
            previous_state = current_state;
        }

        set_error(
            error,
            "move preview did not stabilize after " + std::to_string(sampled_frames) + " samples: " + last_rejection);
        return false;
    }

    bool capture_stable_frame(cv::Mat& image, int& sampled_frames, double& mean_difference, std::string* error)
    {
        sampled_frames = 0;
        mean_difference = -1.0;
        cv::Mat previous = capture();
        if (previous.empty()) {
            set_error(error, "run-log stable capture returned an empty first frame");
            return false;
        }
        ++sampled_frames;
        for (int attempt = 1; attempt < RunLogCaptureStabilityAttempts; ++attempt) {
            sleep(RunLogCaptureStabilityInterval);
            if (need_exit()) {
                set_error(error, "run-log stable capture was interrupted");
                return false;
            }
            cv::Mat current = capture();
            if (current.empty()) {
                set_error(error, "run-log stable capture returned an empty frame");
                return false;
            }
            ++sampled_frames;
            if (previous.size() == current.size() && previous.type() == current.type()) {
                const double denominator =
                    static_cast<double>(current.total()) * static_cast<double>(current.channels());
                mean_difference = cv::norm(previous, current, cv::NORM_L1) / denominator;
                if (run_log_frame_is_stable(mean_difference)) {
                    image = std::move(current);
                    return true;
                }
            }
            else {
                mean_difference = -1.0;
            }
            previous = std::move(current);
        }
        set_error(
            error,
            "run-log frame did not stabilize after " + std::to_string(sampled_frames) +
                " samples (last mean difference " + std::to_string(mean_difference) + ")");
        return false;
    }

    bool capture_stable_map(cv::Mat& image, std::string* error)
    {
        std::optional<cv::Mat> previous;
        std::string last_rejection = "no frame sampled";
        for (int attempt = 0; attempt < MapCaptureStabilityAttempts; ++attempt) {
            std::string wait_error;
            if (!execute({ std::string(MapCaptureStabilityWaitTask) }, &wait_error)) {
                set_error(error, "map capture stability wait failed: " + wait_error);
                return false;
            }
            cv::Mat current = capture();
            if (!map_capture_candidate_is_unobstructed(current, last_rejection)) {
                previous.reset();
                continue;
            }
            if (previous.has_value() && previous->size() == current.size() && previous->type() == current.type()) {
                const double denominator =
                    static_cast<double>(current.total()) * static_cast<double>(current.channels());
                const double mean_difference = cv::norm(*previous, current, cv::NORM_L1) / denominator;
                if (mean_difference <= MapCaptureMaximumMeanDifference) {
                    image = std::move(current);
                    return true;
                }
                last_rejection = "map is still animating (mean frame difference " +
                                 std::to_string(mean_difference) + ")";
            }
            previous = std::move(current);
        }
        set_error(
            error,
            "map capture did not become stable and unobstructed after " +
                std::to_string(MapCaptureStabilityAttempts) + " checks: " + last_rejection);
        return false;
    }

    bool click(const Rect& rect) const { return ctrler()->click(rect); }

protected:
    bool _run() override
    {
        ProcessTask process(*this, m_tasks);
        const bool succeeded = process.run();
        m_last_task = process.get_last_task_name();
        if (const auto& hit = process.get_last_hit(); hit != nullptr && hit->image != nullptr) {
            m_last_image = hit->image;
        }
        return succeeded;
    }

private:
    std::vector<std::string> m_tasks;
    std::string m_last_task;
    std::shared_ptr<cv::Mat> m_last_image;
};

class BlackFlowTaskPort::CollectionPopupCaptureState
{
public:
    struct Pending
    {
        std::string task;
        std::string button;
        cv::Mat image;
        int sampled_frames = 0;
        double mean_difference = -1.0;
    };

    struct PendingNodeEvidence
    {
        std::string action;
        std::string task;
        std::string phase;
        cv::Mat image;
        json::object details;
    };

    std::vector<Pending> pending;
    std::vector<PendingNodeEvidence> pending_node_evidence;
};

BlackFlowTaskPort::BlackFlowTaskPort(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain,
    std::shared_ptr<IBlackFlowMapObservationSource> map_source) :
    m_task_context(std::make_unique<ProcessTaskContext>(
        callback,
        inst,
        task_chain,
        [this](std::string_view task, std::string* error) {
            return capture_collection_popup(task, error);
        })),
    m_collection_popup_state(std::make_unique<CollectionPopupCaptureState>()),
    m_map_source(std::move(map_source))
{
}

BlackFlowTaskPort::~BlackFlowTaskPort() = default;

bool BlackFlowTaskPort::refresh(
    const BlackFlowObservationRequest& request,
    BlackFlowPerceptionSnapshot& snapshot,
    std::string* error)
{
    try {
        if (m_map_source == nullptr || m_task_context == nullptr) {
            set_error(error, "BlackFlow map observation source is not attached");
            return false;
        }
        if (!perception::floor_profile(request.floor).has_value()) {
            set_error(error, "NextLevel recognized an unsupported floor: " + std::to_string(request.floor));
            return false;
        }

        const auto capture_start = std::chrono::steady_clock::now();
        cv::Mat image;
        if (m_pending_stable_map_image != nullptr && !m_pending_stable_map_image->empty()) {
            image = std::move(*m_pending_stable_map_image);
            m_pending_stable_map_image.reset();
        }
        else if (!m_task_context->capture_stable_map(image, error)) {
            return false;
        }

        BlackFlowObservationRequest current_request = request;
        bool map_refreshed = false;
        if (request.inspect_utopia) {
            UtopiaPanelObservation utopia;
            if (!inspect_utopia_for_generation(
                    request.map_generation,
                    utopia,
                    image,
                    map_refreshed,
                    error)) {
                return false;
            }
            current_request.utopia_ideology = std::move(utopia.ideology);
            current_request.utopia_policy = std::move(utopia.policy);
        }

        if (const auto viewport = perception::floor_viewport_profile(request.floor);
            viewport.has_value() &&
            should_normalize_map_viewport(
                viewport->before_every_capture,
                request.viewport_already_normalized,
                map_refreshed)) {
            for (int swipe = 0; swipe < viewport->swipe_left_count; ++swipe) {
                std::string swipe_error;
                if (!m_task_context->execute({ std::string(FloorFiveViewportSwipeLeftTask) }, &swipe_error)) {
                    set_error(
                        error,
                        "floor " + std::to_string(request.floor) + " map viewport left-swipe " +
                            std::to_string(swipe + 1) + "/" + std::to_string(viewport->swipe_left_count) +
                            " failed: " + swipe_error);
                    return false;
                }
            }
            if (!m_task_context->capture_stable_map(image, error)) {
                return false;
            }
        }
        else if (viewport.has_value() && viewport->before_every_capture &&
                 request.viewport_already_normalized && !map_refreshed) {
            Log.info(
                "BlackFlow reuses the normalized map viewport after closing the parts box",
                "floor",
                request.floor);
        }
        current_request.capture_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - capture_start)
                .count();

        BlackFlowPerceptionSnapshot next;
        std::string perception_error;
        if (!m_map_source
                 ->recognize(image, current_request, next.observation, next.observed_facts, &perception_error)) {
            snapshot = std::move(next);
            set_error(
                error,
                "map_model_failed_after_next_level_floor: floor " + std::to_string(request.floor) + ": " +
                    perception_error);
            Log.warn(
                "BlackFlow map model failed after NextLevel floor recognition",
                "floor",
                request.floor,
                "error",
                perception_error);
            return false;
        }
        const auto action_points = recognize_action_points(image);
        const auto ingots = recognize_integer(image, CurrentIngotsTask);
        const auto movement = recognize_loaded_movement(image);
        next.run = make_map_hud_run_observation(
            action_points,
            ingots.has_value() && *ingots >= 0 && *ingots <= 999 ? ingots : std::nullopt,
            movement);
        if (action_points.has_value()) {
            next.observation.hud_action_points = *action_points;
        }
        m_last_stable_map_image = std::make_unique<cv::Mat>(image.clone());
        snapshot = std::move(next);
        return true;
    }
    catch (const std::exception& exception) {
        set_error(error, "BlackFlow map refresh failed: " + std::string(exception.what()));
        return false;
    }
    catch (...) {
        set_error(error, "BlackFlow map refresh failed: unknown exception");
        return false;
    }
}

bool BlackFlowTaskPort::preview(
    const MoveCandidate& candidate,
    const ViewportObservation& viewport,
    MovePreview& preview,
    bool& panel_open,
    std::string* error)
{
    panel_open = false;
    if (m_task_context == nullptr) {
        set_error(error, "BlackFlow ProcessTask context is not attached");
        return false;
    }
    const auto click_rect =
        viewport.clickable_rect(candidate.target, viewport.map_revision(), viewport.viewport_revision());
    if (!click_rect.has_value()) {
        set_error(error, "target node has no current viewport rectangle");
        return false;
    }
    // 缩到图标框中心一半再点，边缘落空会让预览面板弹不出来。
    if (!m_task_context->click(shrink_to_center(*click_rect, 0.5))) {
        set_error(error, "target node click failed");
        return false;
    }
    panel_open = true;

    cv::Mat image;
    MovePreviewFrameState preview_state = MovePreviewFrameState::Missing;
    int sampled_frames = 0;
    double mean_difference = -1.0;
    std::optional<int> displayed_cost;
    std::optional<MovePreviewIdentity> displayed_identity;
    MovePreviewSemanticStability semantic_stability;
    const auto semantics_are_stable = [&](const cv::Mat& current, MovePreviewFrameState current_state) {
        if (current_state == MovePreviewFrameState::Blocked) {
            return true;
        }
        const auto current_cost = recognize_move_preview_action_point_cost(current);
        const auto current_identity = recognize_move_preview_identity(current);
        if (!current_cost.has_value() || *current_cost > 0 || *current_cost < -9 || !current_identity.has_value()) {
            semantic_stability.reset();
            return false;
        }
        const std::string signature = current_identity->name + "|" +
                                      std::to_string(static_cast<int>(current_identity->type)) + "|" +
                                      std::to_string(*current_cost);
        if (!semantic_stability.observe(signature)) {
            return false;
        }
        displayed_cost = current_cost;
        displayed_identity = current_identity;
        return true;
    };
    if (!m_task_context->capture_stable_move_preview(
            image,
            preview_state,
            sampled_frames,
            mean_difference,
            semantics_are_stable,
            error)) {
        return false;
    }
    Log.trace("BlackFlow move preview stabilized", "samples", sampled_frames, "mean difference", mean_difference);
    if (preview_state == MovePreviewFrameState::Blocked) {
        preview.reachability = PreviewReachability::Blocked;
        preview.exact_action_point_cost = candidate.predicted_action_point_cost;
        return true;
    }
    if (preview_state != MovePreviewFrameState::Reachable) {
        set_error(error, "move preview did not identify a reachable or blocked state");
        return false;
    }
    if (!displayed_cost.has_value() || *displayed_cost > 0 || *displayed_cost < -9) {
        set_error(error, "move preview action point cost OCR failed");
        return false;
    }
    if (!displayed_identity.has_value()) {
        set_error(error, "move preview displayed name OCR failed");
        return false;
    }

    preview.reachability = PreviewReachability::Reachable;
    preview.exact_action_point_cost = -*displayed_cost;
    preview.displayed_type = displayed_identity->type;
    preview.displayed_name = displayed_identity->name;
    preview.identity_revealed =
        displayed_identity->type != NodeType::HideInvisible && displayed_identity->type != NodeType::HideBattle;
    return true;
}

bool BlackFlowTaskPort::inspect_battle(
    NodeId node,
    const ViewportObservation& viewport,
    BattleIntelPreview& intel,
    std::string* error)
{
    intel = {};
    if (m_task_context == nullptr) {
        set_error(error, "BlackFlow ProcessTask context is not attached");
        return false;
    }
    const auto click_rect = viewport.clickable_rect(node, viewport.map_revision(), viewport.viewport_revision());
    if (!click_rect.has_value()) {
        set_error(error, "battle intel target has no current viewport rectangle");
        return false;
    }
    m_pending_stable_map_image.reset();
    m_battle_preview_map_reference =
        m_last_stable_map_image == nullptr || m_last_stable_map_image->empty()
            ? nullptr
            : std::make_unique<cv::Mat>(m_last_stable_map_image->clone());
    if (!m_task_context->click(shrink_to_center(*click_rect, 0.5))) {
        m_battle_preview_map_reference.reset();
        set_error(error, "battle intel target click failed");
        return false;
    }
    intel.panel_open = true;
    cv::Mat image;
    MovePreviewFrameState preview_state = MovePreviewFrameState::Missing;
    int sampled_frames = 0;
    double mean_difference = -1.0;
    std::optional<MovePreviewIdentity> displayed_identity;
    std::optional<std::string> stage_name;
    MovePreviewSemanticStability semantic_stability;
    const auto semantics_are_stable = [&](const cv::Mat& current, MovePreviewFrameState) {
        const auto current_identity = recognize_move_preview_identity(current);
        if (!current_identity.has_value()) {
            semantic_stability.reset();
            return false;
        }

        std::optional<std::string> current_stage_name;
        if (is_battle_intel_preview_type(current_identity->type)) {
            current_stage_name =
                recognize_text(current, MovePreviewStageNameTask, RoguelikeCopilot.get_stage_names("BlackFlow"));
            if (!current_stage_name.has_value()) {
                semantic_stability.reset();
                return false;
            }
        }
        const std::string signature = current_identity->name + "|" +
                                      std::to_string(static_cast<int>(current_identity->type)) + "|" +
                                      current_stage_name.value_or("");
        if (!semantic_stability.observe(signature)) {
            return false;
        }
        displayed_identity = current_identity;
        stage_name = std::move(current_stage_name);
        return true;
    };
    if (!m_task_context->capture_stable_move_preview(
            image,
            preview_state,
            sampled_frames,
            mean_difference,
            semantics_are_stable,
            error)) {
        return false;
    }
    Log.trace(
        "BlackFlow battle intel preview stabilized",
        "samples",
        sampled_frames,
        "mean difference",
        mean_difference);
    if (preview_state != MovePreviewFrameState::Reachable && preview_state != MovePreviewFrameState::Blocked) {
        set_error(error, "battle intel preview did not reach a stable preview state");
        return false;
    }
    if (!displayed_identity.has_value()) {
        set_error(error, "battle intel preview identity OCR failed");
        return false;
    }
    intel.displayed_type = displayed_identity->type;
    intel.displayed_name = displayed_identity->name;
    if (!is_battle_intel_preview_type(displayed_identity->type)) {
        set_error(
            error,
            "battle intel preview identity mismatch: expected a normal or emergency battle, displayed " +
                displayed_identity->name);
        return false;
    }
    intel.target_verified = true;
    intel.stage_name = std::move(stage_name);
    if (!intel.stage_name.has_value()) {
        set_error(error, "battle intel stage name OCR failed");
        return false;
    }
    return true;
}

bool BlackFlowTaskPort::check_battle_preview_map(BattlePreviewMapCheck& check, std::string* error)
{
    check = {};
    if (m_battle_preview_map_reference == nullptr || m_battle_preview_map_reference->empty()) {
        return true;
    }
    if (m_task_context == nullptr) {
        set_error(error, "battle preview map check has no ProcessTask context");
        m_battle_preview_map_reference.reset();
        return false;
    }

    cv::Mat current;
    if (!m_task_context->capture_stable_map(current, error)) {
        m_battle_preview_map_reference.reset();
        return false;
    }
    m_pending_stable_map_image = std::make_unique<cv::Mat>(current.clone());

    const cv::Mat& previous = *m_battle_preview_map_reference;
    if (previous.size() == current.size() && previous.type() == current.type() &&
        BattlePreviewMapComparisonRoi.x >= 0 && BattlePreviewMapComparisonRoi.y >= 0 &&
        BattlePreviewMapComparisonRoi.x + BattlePreviewMapComparisonRoi.width <= current.cols &&
        BattlePreviewMapComparisonRoi.y + BattlePreviewMapComparisonRoi.height <= current.rows) {
        const cv::Mat previous_map = previous(BattlePreviewMapComparisonRoi);
        const cv::Mat current_map = current(BattlePreviewMapComparisonRoi);
        const double denominator =
            static_cast<double>(current_map.total()) * static_cast<double>(current_map.channels());
        check.mean_difference = cv::norm(previous_map, current_map, cv::NORM_L1) / denominator;
    }
    check.disposition = battle_preview_map_frame_is_unchanged(check.mean_difference)
                            ? BattlePreviewMapDisposition::Unchanged
                            : BattlePreviewMapDisposition::Changed;
    if (check.disposition == BattlePreviewMapDisposition::Unchanged) {
        m_last_stable_map_image = std::move(m_pending_stable_map_image);
    }
    Log.info(
        "BlackFlow battle preview map cache checked",
        "mean difference",
        check.mean_difference,
        "disposition",
        check.disposition == BattlePreviewMapDisposition::Unchanged ? "reuse" : "rebuild");
    m_battle_preview_map_reference.reset();
    return true;
}

bool BlackFlowTaskPort::cleanup_overloaded_inventory(bool inventory_already_open, std::string* error)
{
    struct ObservedPart
    {
        std::string name;
        Rect name_rect;
        InventoryPartCategory category = InventoryPartCategory::Unknown;
        std::optional<int> remaining_charges;
        InventoryDiscardRank rank;
        std::optional<int> current_valuation;
        int scan_page = 0;
    };

    if (m_task_context == nullptr) {
        set_error(error, "parts-box overload cleanup has no ProcessTask context");
        return false;
    }
    if (!inventory_already_open &&
        !m_task_context->execute({ std::string(InventoryOverloadOpenTask) }, error)) {
        set_error(error, "parts-box overload prompt could not be confirmed");
        return false;
    }

    const auto inventory_is_overloaded = [&]() {
        return recognizes_text_fragment(
            m_task_context->capture(),
            InventoryOverloadedBannerTask,
            "当前零件数量过多");
    };
    if (!inventory_is_overloaded()) {
        set_error(error, "parts-box opened but the overload banner was not recognized");
        return false;
    }

    const auto run_task = [&](std::string_view task_name, std::string_view failure) {
        std::string task_error;
        if (m_task_context->execute({ std::string(task_name) }, &task_error)) {
            return true;
        }
        set_error(error, std::string(failure) + (task_error.empty() ? std::string {} : ": " + task_error));
        return false;
    };

    const auto analyze_parts = [&](const cv::Mat& image, int minimum_name_x, int scan_page) {
        std::vector<ObservedPart> parts;
        const auto task = Task.get<OcrTaskInfo>(InventoryAllItemsTask);
        if (task == nullptr || image.empty()) {
            return parts;
        }
        OCRer analyzer(image);
        analyzer.set_task_info(task);
        analyzer.set_required(task->text);
        const auto results = analyzer.analyze();
        if (!results.has_value()) {
            return parts;
        }
        for (const auto& result : *results) {
            if (result.rect.x + result.rect.width / 2 < minimum_name_x) {
                continue;
            }
            const auto category = inventory_part_category(result.text);
            if (!category.has_value()) {
                continue;
            }
            const auto discard_priority = inventory_part_discard_priority(result.text);
            if (!discard_priority.has_value()) {
                continue;
            }
            const std::optional<int> valuation = *category == InventoryPartCategory::Natural
                                                     ? recognize_inventory_part_valuation(image, result.rect)
                                                     : std::nullopt;
            std::optional<int> remaining_charges;
            if (*category == InventoryPartCategory::Processing) {
                const std::string normalized_name = normalize_inventory_name(result.text);
                const auto movement = std::ranges::find_if(movement_specs(), [&](const MovementSpec& spec) {
                    return spec.kind != MovementKind::Walk &&
                           normalized_name == normalize_inventory_name(spec.name);
                });
                if (movement != movement_specs().end()) {
                    const auto recognition = recognize_movement_inventory_remaining_uses(
                        image,
                        result.rect,
                        movement->initial_charges);
                    if (recognition.has_value()) {
                        remaining_charges = recognition->first;
                    }
                }
            }
            // 前置组始终最先丢弃；超配额加工品要等完整扫描后才能确定，
            // 因此先保留普通档，扫描结束后再只提升真正超出配额的实例。
            const InventoryDiscardRank rank {
                inventory_part_precedes_quota_excess(result.text)
                    ? InventoryDiscardBand::BeforeQuotaExcess
                    : InventoryDiscardBand::Normal,
                static_cast<int>(*discard_priority),
                remaining_charges.value_or(std::numeric_limits<int>::max()),
            };
            parts.emplace_back(ObservedPart {
                normalize_inventory_name(result.text),
                result.rect,
                *category,
                remaining_charges,
                rank,
                valuation,
                scan_page,
            });
        }
        return parts;
    };

    const auto reset_to_start = [&]() {
        for (int swipe = 0; swipe < InventoryMaximumSwipes; ++swipe) {
            if (!run_task(InventorySwipeToStartTask, "parts-box could not scroll back to the first columns")) {
                return false;
            }
        }
        return true;
    };

    bool first_scan_after_open = true;
    const auto scan_all_parts = [&]() -> std::optional<std::vector<ObservedPart>> {
        // 零件箱刚打开时天然位于最左端；只有完成过一次横向扫描后才需要复位。
        if (!std::exchange(first_scan_after_open, false) && !reset_to_start()) {
            return std::nullopt;
        }
        std::vector<ObservedPart> parts;
        std::optional<cv::Mat> previous;
        std::optional<int> previous_rightmost_center;
        for (int scan_page = 0; scan_page <= InventoryMaximumSwipes; ++scan_page) {
            if (scan_page > 0 && !run_task(InventorySwipeTask, "parts-box could not scroll to the next column")) {
                return std::nullopt;
            }
            const cv::Mat image = m_task_context->capture();
            if (previous.has_value() && previous->size() == image.size() && previous->type() == image.type()) {
                const cv::Mat previous_cards = inventory_card_region(*previous);
                const cv::Mat current_cards = inventory_card_region(image);
                const double denominator =
                    static_cast<double>(current_cards.total()) * static_cast<double>(current_cards.channels());
                const double difference = cv::norm(previous_cards, current_cards, cv::NORM_L1) / denominator;
                if (difference <= InventoryUnchangedFrameMaximumDifference) {
                    break;
                }
            }
            auto page = analyze_parts(image, 0, scan_page);
            std::optional<int> rightmost_center;
            for (const ObservedPart& part : page) {
                const int center = part.name_rect.x + part.name_rect.width / 2;
                rightmost_center = std::max(rightmost_center.value_or(center), center);
            }
            if (scan_page > 0 && inventory_scan_rebounded(previous_rightmost_center, rightmost_center)) {
                Log.info(
                    "BlackFlow inventory scan reached the right edge and rebounded",
                    "previous rightmost center",
                    *previous_rightmost_center,
                    "current rightmost center",
                    *rightmost_center);
                break;
            }
            if (scan_page > 0) {
                std::erase_if(page, [](const ObservedPart& part) {
                    return part.name_rect.x + part.name_rect.width / 2 < InventoryNewRightColumnMinimumX;
                });
            }
            parts.insert(
                parts.end(),
                std::make_move_iterator(page.begin()),
                std::make_move_iterator(page.end()));
            if (rightmost_center.has_value()) {
                previous_rightmost_center = rightmost_center;
            }
            previous = image.clone();
        }
        return parts;
    };

    const auto apply_quota_excess_ranks = [](std::vector<ObservedPart>& parts) {
        std::map<std::string, std::vector<std::size_t>> candidates_by_name;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const ObservedPart& part = parts[index];
            if (part.category == InventoryPartCategory::Processing &&
                part.rank.band == InventoryDiscardBand::Normal) {
                candidates_by_name[part.name].emplace_back(index);
            }
        }

        for (const auto& [name, candidates] : candidates_by_name) {
            const std::optional<std::size_t> quota = automation_store_purchase_quota(name);
            if (!quota.has_value() || candidates.size() <= *quota) {
                continue;
            }
            std::vector<int> remaining_charges;
            remaining_charges.reserve(candidates.size());
            for (const std::size_t index : candidates) {
                remaining_charges.emplace_back(
                    parts[index].remaining_charges.value_or(std::numeric_limits<int>::max()));
            }
            for (const std::size_t local_index : inventory_quota_excess_indices(remaining_charges, *quota)) {
                parts[candidates[local_index]].rank.band = InventoryDiscardBand::QuotaExcess;
            }
        }
    };

    for (int discard_attempt = 0; discard_attempt < InventoryMaximumDiscardAttempts && inventory_is_overloaded();
         ++discard_attempt) {
        auto parts = scan_all_parts();
        if (!parts.has_value()) {
            return false;
        }
        apply_quota_excess_ranks(*parts);
        // 上一次丢弃完成后，零件箱容量和顶部提示可能不会在 800 ms 内同时刷新。
        // 完整扫描本身会持续数秒，因此必须在真正选择并丢弃下一件之前重新读取现场；
        // 否则会拿循环入口处的短暂旧提示作出不可逆的额外丢弃。
        if (!inventory_is_overloaded()) {
            Log.info("BlackFlow inventory overload cleared while scanning; skipping the pending discard");
            break;
        }
        const auto selected = std::ranges::min_element(
            *parts,
            {},
            [](const ObservedPart& part) { return part.rank; });
        if (selected == parts->end()) {
            set_error(error, "parts-box remains overloaded but no known discardable part was recognized across the list");
            return false;
        }

        const std::string selected_name = selected->name;
        const int selected_page = selected->scan_page;
        const InventoryDiscardRank selected_rank = selected->rank;
        const std::optional<int> selected_remaining_charges = selected->remaining_charges;
        const Rect selected_name_rect = selected->name_rect;
        if (!reset_to_start()) {
            return false;
        }
        for (int page = 0; page < selected_page; ++page) {
            if (!run_task(InventorySwipeTask, "parts-box could not return to the selected discard page")) {
                return false;
            }
        }

        std::optional<Rect> current_rect;
        for (int recognition_attempt = 0; recognition_attempt < 3 && !current_rect.has_value(); ++recognition_attempt) {
            const auto visible = analyze_parts(m_task_context->capture(), 0, selected_page);
            const auto found = std::ranges::min_element(visible, {}, [&](const ObservedPart& part) {
                const int center_distance =
                    std::abs((part.name_rect.x + part.name_rect.width / 2) -
                             (selected_name_rect.x + selected_name_rect.width / 2)) +
                    std::abs((part.name_rect.y + part.name_rect.height / 2) -
                             (selected_name_rect.y + selected_name_rect.height / 2));
                return std::tuple {
                    part.name == selected_name ? 0 : 1,
                    part.remaining_charges == selected_remaining_charges ? 0 : 1,
                    center_distance,
                };
            });
            if (found != visible.end() && found->name == selected_name) {
                current_rect = found->name_rect;
                break;
            }
            if (recognition_attempt + 1 < 3 &&
                !run_task(EnteredPageClassificationRetryWaitTask, "parts-box item recognition retry wait failed")) {
                return false;
            }
        }
        if (!current_rect.has_value() || !m_task_context->click(inventory_part_detail_click_rect(*current_rect))) {
            set_error(error, "selected discard part could not be found or clicked after restoring its scroll page");
            return false;
        }
        // 点击卡片后详情弹层有展开动画。直接执行丢弃按钮 OCR 时，现场日志表明识别会在
        // 约 0.1 秒内发生，此时画面仍是零件卡片正文，必然找不到“丢弃”。先显式等待弹层稳定，
        // 再识别并点击按钮；任务上的 preDelay 只发生在按钮已经识别之后，不能代替这里。
        if (!run_task(InventoryDiscardDetailWaitTask, "selected parts-box item detail did not become ready")) {
            return false;
        }
        Log.info(
            "BlackFlow discarding overloaded inventory part",
            selected_name,
            "discard band",
            static_cast<int>(selected_rank.band),
            "discard priority",
            selected_rank.priority,
            "remaining charges",
            selected_rank.remaining_charges == std::numeric_limits<int>::max()
                ? std::string("not applicable or unrecognized")
                : std::to_string(selected_rank.remaining_charges),
            "current valuation",
            selected->current_valuation.has_value() ? std::to_string(*selected->current_valuation)
                                                    : std::string("not applicable or unrecognized"),
            "scan page",
            selected_page);
        if (!run_task(InventoryDiscardButtonTask, "selected parts-box item could not be discarded")) {
            return false;
        }
        // 正在装载的加工品会在首次点击“丢弃”后追加一次确认。普通零件会直接回到
        // 零件箱，因此先在确认按钮的限定区域内识别，只有弹窗确实存在时才点击。
        if (recognizes_text_fragment(
                m_task_context->capture(),
                InventoryEquippedDiscardConfirmTask,
                "确认")) {
            Log.info("BlackFlow confirms discarding equipped processing item", selected_name);
            if (!run_task(
                    InventoryEquippedDiscardConfirmTask,
                    "equipped processing item discard confirmation could not be completed")) {
                return false;
            }
        }
    }

    if (inventory_is_overloaded()) {
        set_error(
            error,
            "parts-box remains overloaded after " + std::to_string(InventoryMaximumDiscardAttempts) +
                " discard attempts");
        return false;
    }
    return run_task(InventoryOverloadCloseTask, "parts-box could not return to routing after cleanup");
}

bool BlackFlowTaskPort::cleanup_open_inventory_if_overloaded(bool& cleanup_performed, std::string* error)
{
    cleanup_performed = false;
    if (m_task_context == nullptr) {
        set_error(error, "parts-box overload check has no ProcessTask context");
        return false;
    }
    if (!recognizes_text_fragment(
            m_task_context->capture(),
            InventoryOverloadedBannerTask,
            "当前零件数量过多")) {
        return true;
    }
    if (!cleanup_overloaded_inventory(true, error)) {
        return false;
    }
    cleanup_performed = true;
    return true;
}

bool BlackFlowTaskPort::confirm(
    const MoveTransaction& transaction,
    EnteredPageObservation& entered_page,
    std::string* error)
{
    if (m_task_context == nullptr || transaction.stage() != MoveTransactionStage::Previewed ||
        !transaction.preview().has_value() || transaction.preview()->reachability != PreviewReachability::Reachable) {
        set_error(error, "move confirmation requires a reachable previewed transaction");
        return false;
    }
    if (!m_task_context->execute({ std::string(MovePreviewConfirmTask) }, error)) {
        return false;
    }
    if (!move_confirmation_left_preview(m_task_context->last_task())) {
        set_error(
            error,
            "move confirmation did not leave the preview after its retry limit: " +
                m_task_context->last_task());
        return false;
    }
    entered_page = {};
    const cv::Mat confirmed_image = m_task_context->capture();
    if (recognize_text(confirmed_image, InventoryOverloadPromptTask).has_value()) {
        entered_page.inventory_overloaded = true;
        if (!cleanup_overloaded_inventory(false, error)) {
            return false;
        }
        entered_page.inventory_cleanup_performed = true;
        return true;
    }
    if (move_confirmation_requires_door_animation_wait(transaction) &&
        !m_task_context->execute({ std::string(MoveConfirmDoorAnimationWaitTask) }, error)) {
        const std::string detail = error == nullptr ? std::string {} : *error;
        set_error(
            error,
            detail.empty() ? "winding-passage animation did not return to the map"
                           : "winding-passage animation did not return to the map: " + detail);
        return false;
    }
    // 定向移动的预览标题就是实际落点；随机移动点击的节点只负责触发预览，实际落点可能
    // 是另一种页面，因此无论标题是否已揭示都必须重新识别确认后的现场。
    if (transaction.proposal().controllable && transaction.preview()->identity_revealed) {
        return true;
    }
    return classify_entered_page(confirmed_image, entered_page, error);
}

void BlackFlowTaskPort::reset_run()
{
    m_utopia_generation.reset();
    m_utopia_observation = {};
    m_last_stable_map_image.reset();
    m_battle_preview_map_reference.reset();
    m_pending_stable_map_image.reset();
    if (m_collection_popup_state != nullptr) {
        m_collection_popup_state->pending.clear();
        m_collection_popup_state->pending_node_evidence.clear();
    }
    if (m_map_source != nullptr) {
        m_map_source->reset_run();
    }
}

void BlackFlowTaskPort::configure_diagnostics(const DiagnosticSettings& settings)
{
    if (m_map_source != nullptr) {
        m_map_source->configure_diagnostics(settings);
    }
}

void BlackFlowTaskPort::set_collection_popup_session(std::weak_ptr<BlackFlowSession> session)
{
    m_collection_popup_session = std::move(session);
}

bool BlackFlowTaskPort::persist_node_evidence_capture(
    std::string_view action,
    std::string task,
    std::string phase,
    const cv::Mat& image,
    const std::filesystem::path& relative_directory,
    json::object attribution,
    json::object details,
    std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    attribution["directory"] = relative_directory.generic_string();
    if (action == CollectionPopupRunLogAction) {
        details["collection_popup_directory"] = relative_directory.generic_string();
    }
    else {
        details["node_evidence_directory"] = relative_directory.generic_string();
    }
    details["attribution"] = std::move(attribution);
    json::object state = session->run_log_state();
    const RunLogEvent event {
        .level = RunLogLevel::Info,
        .action = std::string(action),
        .phase = std::move(phase),
        .outcome = "captured",
        .task = std::move(task),
        .transaction_id = state.get("transaction_id", std::string()),
        .state = std::move(state),
        .details = std::move(details),
    };
    return record_run_event(
        session->run_revision(),
        event,
        std::make_shared<cv::Mat>(image.clone()),
        false,
        error);
}

bool BlackFlowTaskPort::persist_collection_popup_capture(
    const std::string& task,
    std::string_view button,
    const cv::Mat& image,
    int sampled_frames,
    double mean_difference,
    const std::filesystem::path& relative_directory,
    json::object attribution,
    bool deferred,
    std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    attribution["directory"] = relative_directory.generic_string();
    json::object state = session->run_log_state();
    const RunLogEvent event {
        .level = RunLogLevel::Info,
        .action = std::string(CollectionPopupRunLogAction),
        .phase = "before-click",
        .outcome = "captured",
        .task = task,
        .transaction_id = state.get("transaction_id", std::string()),
        .state = std::move(state),
        .details = json::object {
            { "button", std::string(button) },
            { "collection_popup_directory", relative_directory.generic_string() },
            { "deferred_attribution", deferred },
            { "attribution", std::move(attribution) },
            { "image_capture",
              json::object {
                  { "mode", "stable_before_click" },
                  { "status", "captured" },
                  { "sampled_frames", sampled_frames },
                  { "mean_difference", mean_difference },
              } },
        },
    };
    return record_run_event(
        session->run_revision(),
        event,
        std::make_shared<cv::Mat>(image.clone()),
        false,
        error);
}

bool BlackFlowTaskPort::capture_collection_popup(std::string_view task, std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    const auto button = collection_popup_button(task);
    if (!button.has_value()) {
        return true;
    }
    if (m_task_context == nullptr || m_collection_popup_state == nullptr) {
        set_error(error, "collection popup capture context is not attached");
        return false;
    }

    if (!m_collection_popup_state->pending.empty()) {
        std::string resolve_error;
        if (!resolve_pending_collection_popups(&resolve_error)) {
            set_error(error, "failed to resolve an earlier popup: " + resolve_error);
            return false;
        }
    }
    if (!m_collection_popup_state->pending.empty() && session->transaction() == nullptr) {
        std::string flush_error;
        if (!flush_pending_collection_popups(&flush_error)) {
            set_error(error, "failed to flush an unresolved earlier popup: " + flush_error);
            return false;
        }
    }

    cv::Mat captured;
    int sampled_frames = 0;
    double mean_difference = -1.0;
    if (!m_task_context->capture_stable_frame(captured, sampled_frames, mean_difference, error)) {
        return false;
    }
    const std::string_view template_task = *button == CollectionPopupButton::Continue
                                               ? "BlackFlow@Roguelike@CloseCollectionContinue"
                                               : "BlackFlow@Roguelike@CloseCollection";
    if (!matches_template(captured, template_task)) {
        set_error(
            error,
            "stable frame no longer contains the recognized collection popup button: " +
                std::string(to_string(*button)));
        return false;
    }

    const auto destination = resolve_collection_popup_destination(*session, task, false);
    if (!destination.has_value() && session->transaction() != nullptr &&
        !session->transaction()->proposal().controllable) {
        m_collection_popup_state->pending.emplace_back(
            CollectionPopupCaptureState::Pending {
                std::string(task),
                std::string(to_string(*button)),
                std::move(captured),
                sampled_frames,
                mean_difference,
            });
        return true;
    }

    const CollectionPopupDestination resolved = destination.value_or(
        CollectionPopupDestination {
            collection_popup_other_directory(),
            json::object {
                { "kind", "other" },
                { "reason", "no_node_or_explicit_source" },
                { "floor", collection_popup_floor(*session) },
            },
        });
    return persist_collection_popup_capture(
        std::string(task),
        to_string(*button),
        captured,
        sampled_frames,
        mean_difference,
        resolved.directory,
        resolved.attribution,
        false,
        error);
}

bool BlackFlowTaskPort::capture_event_page(
    std::string_view event_name,
    const cv::Mat& stitched_image,
    std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    const auto& page = session->page_context();
    if (!page.has_value() || page->stage != PageExecutionStage::Running ||
        is_combat_node_type(page->node_type)) {
        return true;
    }
    if (stitched_image.empty()) {
        set_error(error, "stitched encounter capture is empty");
        return false;
    }

    const auto destination = resolve_collection_popup_destination(*session, {}, true);
    if (!destination.has_value() && session->transaction() != nullptr &&
        !session->transaction()->proposal().controllable && m_collection_popup_state != nullptr) {
        m_collection_popup_state->pending_node_evidence.emplace_back(
            CollectionPopupCaptureState::PendingNodeEvidence {
                std::string(NodeEventRunLogAction),
                "RoguelikeEvent",
                "after-scroll",
                stitched_image.clone(),
                json::object {
                    { "event_name", std::string(event_name) },
                    { "capture_kind", "stitched_event_page" },
                    { "source_width", stitched_image.cols },
                    { "source_height", stitched_image.rows },
                },
            });
        return true;
    }
    const CollectionPopupDestination resolved = destination.value_or(
        CollectionPopupDestination {
            collection_popup_other_directory(),
            json::object {
                { "kind", "other" },
                { "reason", "event_page_has_no_resolved_node" },
                { "floor", collection_popup_floor(*session) },
            },
        });
    return persist_node_evidence_capture(
        NodeEventRunLogAction,
        "RoguelikeEvent",
        "after-scroll",
        stitched_image,
        resolved.directory,
        resolved.attribution,
        json::object {
            { "event_name", std::string(event_name) },
            { "capture_kind", "stitched_event_page" },
            { "source_width", stitched_image.cols },
            { "source_height", stitched_image.rows },
        },
        error);
}

bool BlackFlowTaskPort::capture_get_drop(
    std::string_view task,
    std::optional<Rect> selected_button,
    std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    const bool pursuit_loot_preclick = pursuit_loot_click_task(task);
    if (pursuit_loot_preclick) {
        if (!session->collection_popup_pursuit_floor().has_value()) {
            return true;
        }
        if (m_task_context == nullptr) {
            set_error(error, "pursuit loot-entry capture context is not attached");
            return false;
        }
        const cv::Mat captured = m_task_context->capture();
        if (captured.empty()) {
            set_error(error, "pursuit loot-entry capture returned an empty frame");
            return false;
        }
        const auto destination = resolve_collection_popup_destination(*session, task, true);
        const CollectionPopupDestination resolved = destination.value_or(
            CollectionPopupDestination {
                collection_popup_virtual_node_directory(
                    *session->collection_popup_pursuit_floor(),
                    "追猎"),
                json::object {
                    { "kind", "virtual_node" },
                    { "floor", *session->collection_popup_pursuit_floor() },
                    { "node_name", "追猎" },
                },
            });
        return persist_node_evidence_capture(
            CollectionPopupRunLogAction,
            std::string(task),
            "loot-preclick",
            captured,
            resolved.directory,
            resolved.attribution,
            json::object {
                { "capture_kind", "immediate_before_first_loot_click" },
                { "entry_kind", "pursuit" },
            },
            error);
    }
    const bool recruitment_page = node_recruitment_page_task(task);
    const auto screen = node_get_drop_screen(task);
    if (!recruitment_page &&
        (!screen.has_value() || !node_get_drop_should_capture(task, node_evidence_is_combat_page(*session)))) {
        return true;
    }
    if (m_task_context == nullptr) {
        set_error(error, "node page capture context is not attached");
        return false;
    }

    cv::Mat captured;
    int sampled_frames = 0;
    double mean_difference = -1.0;
    if (!m_task_context->capture_stable_frame(captured, sampled_frames, mean_difference, error)) {
        return false;
    }
    std::vector<Rect> option_buttons;
    if (recruitment_page) {
        if (!matches_template(captured, recruitment_page_flag_task(task))) {
            set_error(error, "stable frame no longer contains the recognized recruitment page");
            return false;
        }
    }
    else if (*screen == NodeGetDropScreen::Select) {
        MultiMatcher matcher(captured);
        matcher.set_task_info(std::string(task));
        if (const auto hits = matcher.analyze(); hits.has_value()) {
            option_buttons.reserve(hits->size());
            for (const auto& hit : *hits) {
                if (selected_button.has_value()) {
                    const int button_center_y = hit.rect.y + hit.rect.height / 2;
                    const int selected_center_y = selected_button->y + selected_button->height / 2;
                    if (std::abs(button_center_y - selected_center_y) >
                        (std::max)(hit.rect.height, selected_button->height)) {
                        continue;
                    }
                }
                option_buttons.emplace_back(hit.rect);
            }
        }
    }
    else if (!matches_template(captured, task)) {
        set_error(error, "stable frame no longer contains the recognized GetDrop screen");
        return false;
    }

    const auto destination = resolve_collection_popup_destination(*session, task, true);
    json::object details {
        { "screen", recruitment_page ? std::string("recruitment") : std::string(to_string(*screen)) },
        { "capture_kind", "stable_before_click" },
        { "capture_stability",
          json::object {
              { "sampled_frames", sampled_frames },
              { "mean_difference", mean_difference },
        } },
    };
    if (!recruitment_page && *screen == NodeGetDropScreen::Select) {
        std::ranges::sort(option_buttons, {}, [](const Rect& rect) {
            return rect.x + rect.width / 2;
        });
        json::array button_rects;
        for (const Rect& rect : option_buttons) {
            button_rects.emplace_back(json::array { rect.x, rect.y, rect.width, rect.height });
        }
        details["detected_option_count"] = static_cast<int>(option_buttons.size());
        details["option_button_rects_from_left"] = std::move(button_rects);
        if (selected_button.has_value()) {
            details["selected_button_rect"] = json::array {
                selected_button->x,
                selected_button->y,
                selected_button->width,
                selected_button->height,
            };
        }

        const auto selection = selected_button.has_value()
            ? resolve_drop_option_selection(option_buttons, *selected_button)
            : std::nullopt;
        if (selection.has_value()) {
            details["option_count"] = selection->option_count;
            details["selected_option_index_from_left"] = selection->selected_index_from_left;
            details["selection_resolution"] = "multi_match_sorted_x";
        }
        else {
            details["selection_resolution"] = "unavailable";
            details["selection_resolution_reason"] = !selected_button.has_value()
                ? "process_task_hit_rect_missing"
                : "detected_option_count_is_not_two_or_three";
        }
    }
    const std::string_view action =
        recruitment_page ? NodeRecruitmentRunLogAction : NodeGetDropRunLogAction;
    if (!destination.has_value() && session->transaction() != nullptr &&
        !session->transaction()->proposal().controllable && m_collection_popup_state != nullptr) {
        m_collection_popup_state->pending_node_evidence.emplace_back(
            CollectionPopupCaptureState::PendingNodeEvidence {
                std::string(action),
                std::string(task),
                recruitment_page ? "entered" : "before-click",
                std::move(captured),
                std::move(details),
            });
        return true;
    }
    const CollectionPopupDestination resolved = destination.value_or(
        CollectionPopupDestination {
            collection_popup_other_directory(),
            json::object {
                { "kind", "other" },
                { "reason", recruitment_page ? "recruitment_page_has_no_resolved_node"
                                               : "combat_drop_has_no_resolved_node" },
                { "floor", collection_popup_floor(*session) },
            },
        });
    return persist_node_evidence_capture(
        action,
        std::string(task),
        recruitment_page ? "entered" : "before-click",
        captured,
        resolved.directory,
        resolved.attribution,
        std::move(details),
        error);
}

bool BlackFlowTaskPort::capture_store_page(
    std::string_view store_kind,
    std::string_view capture_phase,
    int refresh_index,
    const cv::Mat* captured_image,
    std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    if (m_task_context == nullptr) {
        set_error(error, "store capture context is not attached");
        return false;
    }

    cv::Mat captured;
    int sampled_frames = 0;
    double mean_difference = -1.0;
    if (captured_image != nullptr) {
        if (captured_image->empty()) {
            set_error(error, "provided stitched store capture is empty");
            return false;
        }
        captured = captured_image->clone();
    }
    else if (!m_task_context->capture_stable_frame(captured, sampled_frames, mean_difference, error)) {
        return false;
    }
    const auto destination = resolve_collection_popup_destination(*session, {}, true);
    const CollectionPopupDestination resolved = destination.value_or(
        CollectionPopupDestination {
            collection_popup_other_directory(),
            json::object {
                { "kind", "other" },
                { "reason", "store_page_has_no_resolved_node" },
                { "floor", collection_popup_floor(*session) },
            },
        });
    return persist_node_evidence_capture(
        NodeStoreRunLogAction,
        "BlackFlowAutomationStore",
        std::string(capture_phase),
        captured,
        resolved.directory,
        resolved.attribution,
        json::object {
            { "store_kind", std::string(store_kind) },
            { "capture_phase", std::string(capture_phase) },
            { "refresh_index", refresh_index },
            { "capture_kind", captured_image == nullptr ? "stable_store_page" : "stitched_store_goods" },
            { "capture_stability",
              json::object {
                  { "sampled_frames", sampled_frames },
                  { "mean_difference", mean_difference },
              } },
        },
        error);
}

bool BlackFlowTaskPort::record_store_purchase(
    std::string_view store_kind,
    std::string_view item_name,
    std::optional<int> ingots_before,
    std::optional<int> ingots_after,
    bool collectible,
    std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection") {
        return true;
    }
    const bool wallet_verified = ingots_before.has_value() && ingots_after.has_value();
    const bool succeeded = wallet_verified && *ingots_after < *ingots_before;
    json::object details {
        { "store_kind", std::string(store_kind) },
        { "item_name", std::string(item_name) },
        { "item_category", collectible ? "collectible" : "store_good" },
        { "purchase_succeeded", succeeded },
        { "wallet_changed", wallet_verified && *ingots_after != *ingots_before },
        { "verification", wallet_verified ? "ingot_delta" : "ingot_ocr_unavailable" },
    };
    if (ingots_before.has_value()) {
        details["ingots_before"] = *ingots_before;
    }
    if (ingots_after.has_value()) {
        details["ingots_after"] = *ingots_after;
    }

    const auto destination = resolve_collection_popup_destination(*session, {}, true);
    const CollectionPopupDestination resolved = destination.value_or(
        CollectionPopupDestination {
            collection_popup_other_directory(),
            json::object {
                { "kind", "other" },
                { "reason", "store_purchase_has_no_resolved_node" },
                { "floor", collection_popup_floor(*session) },
            },
        });
    return persist_node_evidence_capture(
        NodeStorePurchaseRunLogAction,
        "BlackFlowAutomationStore",
        "purchase-verification",
        cv::Mat {},
        resolved.directory,
        resolved.attribution,
        std::move(details),
        error);
}

bool BlackFlowTaskPort::resolve_pending_collection_popups(std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection" ||
        m_collection_popup_state == nullptr ||
        (m_collection_popup_state->pending.empty() &&
         m_collection_popup_state->pending_node_evidence.empty())) {
        return true;
    }

    const bool allow_current_landing = session->transaction() == nullptr;
    std::vector<CollectionPopupCaptureState::Pending> unresolved;
    unresolved.reserve(m_collection_popup_state->pending.size());
    for (auto& pending : m_collection_popup_state->pending) {
        const auto destination = resolve_collection_popup_destination(
            *session,
            pending.task,
            allow_current_landing);
        if (!destination.has_value()) {
            unresolved.emplace_back(std::move(pending));
            continue;
        }
        if (!persist_collection_popup_capture(
                pending.task,
                pending.button,
                pending.image,
                pending.sampled_frames,
                pending.mean_difference,
                destination->directory,
                destination->attribution,
                true,
                error)) {
            unresolved.emplace_back(std::move(pending));
            m_collection_popup_state->pending = std::move(unresolved);
            return false;
        }
    }
    m_collection_popup_state->pending = std::move(unresolved);

    std::vector<CollectionPopupCaptureState::PendingNodeEvidence> unresolved_evidence;
    unresolved_evidence.reserve(m_collection_popup_state->pending_node_evidence.size());
    for (auto& pending : m_collection_popup_state->pending_node_evidence) {
        const auto destination = resolve_collection_popup_destination(
            *session,
            pending.task,
            allow_current_landing);
        if (!destination.has_value()) {
            unresolved_evidence.emplace_back(std::move(pending));
            continue;
        }
        if (!persist_node_evidence_capture(
                pending.action,
                pending.task,
                pending.phase,
                pending.image,
                destination->directory,
                destination->attribution,
                pending.details,
                error)) {
            unresolved_evidence.emplace_back(std::move(pending));
            m_collection_popup_state->pending_node_evidence = std::move(unresolved_evidence);
            return false;
        }
    }
    m_collection_popup_state->pending_node_evidence = std::move(unresolved_evidence);
    return true;
}

bool BlackFlowTaskPort::flush_pending_collection_popups(std::string* error)
{
    const auto session = m_collection_popup_session.lock();
    if (session == nullptr || session->profile() != "automation_collection" ||
        m_collection_popup_state == nullptr ||
        (m_collection_popup_state->pending.empty() &&
         m_collection_popup_state->pending_node_evidence.empty())) {
        return true;
    }

    std::vector<CollectionPopupCaptureState::Pending> unwritten;
    unwritten.reserve(m_collection_popup_state->pending.size());
    for (auto& pending : m_collection_popup_state->pending) {
        json::object attribution {
            { "kind", "other" },
            { "reason", "landing_remained_unresolved" },
            { "floor", collection_popup_floor(*session) },
        };
        if (!persist_collection_popup_capture(
                pending.task,
                pending.button,
                pending.image,
                pending.sampled_frames,
                pending.mean_difference,
                collection_popup_other_directory(),
                std::move(attribution),
                true,
                error)) {
            unwritten.emplace_back(std::move(pending));
            m_collection_popup_state->pending = std::move(unwritten);
            return false;
        }
    }
    m_collection_popup_state->pending.clear();

    std::vector<CollectionPopupCaptureState::PendingNodeEvidence> unwritten_evidence;
    unwritten_evidence.reserve(m_collection_popup_state->pending_node_evidence.size());
    for (auto& pending : m_collection_popup_state->pending_node_evidence) {
        json::object attribution {
            { "kind", "other" },
            { "reason", "landing_remained_unresolved" },
            { "floor", collection_popup_floor(*session) },
        };
        if (!persist_node_evidence_capture(
                pending.action,
                pending.task,
                pending.phase,
                pending.image,
                collection_popup_other_directory(),
                std::move(attribution),
                pending.details,
                error)) {
            unwritten_evidence.emplace_back(std::move(pending));
            m_collection_popup_state->pending_node_evidence = std::move(unwritten_evidence);
            return false;
        }
    }
    m_collection_popup_state->pending_node_evidence.clear();
    return true;
}

bool BlackFlowTaskPort::persist_diagnostics(const DiagnosticArtifactRequest& request, std::string* error)
{
    if (m_map_source == nullptr) {
        set_error(error, "BlackFlow diagnostic persistence has no map observation source");
        return false;
    }
    return m_map_source->persist_diagnostics(request, error);
}

bool BlackFlowTaskPort::record_run_event(
    std::uint64_t run_revision,
    const RunLogEvent& event,
    std::shared_ptr<cv::Mat> image,
    bool capture_image,
    std::string* error)
{
    if (m_map_source == nullptr) {
        set_error(error, "BlackFlow run event persistence has no map observation source");
        return false;
    }
    RunLogEvent stored_event = event;
    cv::Mat captured;
    const bool has_provided_image = image != nullptr && !image->empty();
    const RunLogImageCaptureMode mode =
        run_log_image_capture_mode(event.action, event.phase, has_provided_image, capture_image);
    const cv::Mat* evidence = nullptr;
    switch (mode) {
    case RunLogImageCaptureMode::Omit:
        break;
    case RunLogImageCaptureMode::ProvidedObservation:
        evidence = image.get();
        stored_event.details["image_capture"] = json::object {
            { "mode", "provided_observation" },
            { "status", "captured" },
        };
        break;
    case RunLogImageCaptureMode::ImmediateSnapshot:
        if (m_task_context != nullptr) {
            captured = m_task_context->capture();
            evidence = captured.empty() ? nullptr : &captured;
        }
        stored_event.details["image_capture"] = json::object {
            { "mode", "immediate" },
            { "status", evidence != nullptr ? "captured" : "omitted_empty" },
        };
        break;
    case RunLogImageCaptureMode::StableSnapshot: {
        int sampled_frames = 0;
        double mean_difference = -1.0;
        std::string capture_error;
        if (m_task_context != nullptr &&
            m_task_context->capture_stable_frame(captured, sampled_frames, mean_difference, &capture_error)) {
            evidence = &captured;
        }
        stored_event.details["image_capture"] = json::object {
            { "mode", "stable" },
            { "status", evidence != nullptr ? "captured" : "omitted_unstable" },
            { "sampled_frames", sampled_frames },
            { "mean_difference", mean_difference },
        };
        if (!capture_error.empty()) {
            stored_event.details["image_capture"]["error"] = std::move(capture_error);
        }
        break;
    }
    }
    return m_map_source->record_run_event(run_revision, stored_event, evidence, error);
}

bool BlackFlowTaskPort::record_node_attribution(
    std::uint64_t run_revision,
    int floor,
    NodeId node,
    std::string_view virtual_node_name,
    std::string_view attribution,
    std::string* error)
{
    if (m_map_source == nullptr) {
        set_error(error, "BlackFlow node attribution persistence has no map observation source");
        return false;
    }
    if (floor <= 0 || (node == InvalidNodeId && virtual_node_name.empty())) {
        set_error(error, "BlackFlow node attribution has no valid node target");
        return false;
    }
    const std::filesystem::path directory = node != InvalidNodeId
        ? collection_popup_regular_node_directory(floor, node)
        : collection_popup_virtual_node_directory(floor, virtual_node_name, 0);
    return m_map_source->record_node_attribution(run_revision, directory, attribution, error);
}

std::optional<int> BlackFlowTaskPort::recognize_action_points(const cv::Mat& image) const
{
    const auto value = recognize_integer(image, CurrentActionPointsTask);
    if (!value.has_value() || *value < 0 || *value > 64) {
        return std::nullopt;
    }
    return value;
}

UtopiaPanelObservation BlackFlowTaskPort::recognize_utopia_panel(const cv::Mat& image) const
{
    UtopiaPanelObservation result;
    if (const auto policy = recognize_text(image, UtopiaPanelPolicyTask); policy.has_value()) {
        static const std::array<std::pair<std::string_view, std::string_view>, 4> Policies {
            std::pair { "\u6539\u826f", "improvement" },
            std::pair { "\u4fee\u6b63", "correction" },
            std::pair { "\u6fc0\u8fdb", "radical" },
            std::pair { "\u589e\u76ca", "benefit" },
        };
        const auto found = std::ranges::find_if(Policies, [&](const auto& entry) { return entry.first == *policy; });
        if (found != Policies.end()) {
            result.policy = found->second;
        }
    }
    if (const auto ideology = recognize_text(image, UtopiaPanelIdeologyTask); ideology.has_value()) {
        static const std::array<std::pair<std::string_view, std::string_view>, 10> Ideologies {
            std::pair { "\u9ed1\u6d41\u5730\u8109", "blackstream-veins" },
            std::pair { "\u503e\u659c\u6c99\u4e18", "tilted-dune" },
            std::pair { "\u53bb\u6e29\u680f", "dewarming-rail" },
            std::pair { "\u5fae\u578b\u80f6\u56ca", "micro-capsule" },
            std::pair { "\u50a8\u85cf\u5ba4", "storage-room" },
            std::pair { "\u6613\u788e\u540c\u76df", "fragile-alliance" },
            std::pair { "\u505c\u6b62\u70b9", "stopping-point" },
            std::pair { "\u5e0c\u671b\u7684\u6c83\u571f", "hopeful-soil" },
            std::pair { "\u5f25\u6563\u865a\u96fe", "diffused-mist" },
            std::pair { "\u7f8e\u4e3d\u65b0\u5927\u5730", "brave-new-land" },
        };
        const auto found =
            std::ranges::find_if(Ideologies, [&](const auto& entry) { return entry.first == *ideology; });
        if (found != Ideologies.end()) {
            result.ideology = found->second;
        }
    }
    return result;
}

bool BlackFlowTaskPort::inspect_utopia_for_generation(
    std::uint64_t map_generation,
    UtopiaPanelObservation& observation,
    cv::Mat& stable_map_image,
    bool& map_refreshed,
    std::string* error)
{
    map_refreshed = false;
    if (m_utopia_generation == map_generation) {
        observation = m_utopia_observation;
        return true;
    }
    if (m_task_context == nullptr) {
        set_error(error, "utopia inspection has no ProcessTask context");
        return false;
    }

    // 新地图的实托邦标记固定在楼层标题左侧。主动打开标题弹层读取方针与理念，
    // 再点同一个位置关闭；不从容易混淆的 HUD 图标反推身份。
    if (!m_task_context->execute({ std::string(UtopiaPanelToggleTask) }, error)) {
        set_error(error, "utopia marker could not be opened");
        return false;
    }
    const cv::Mat panel_image = m_task_context->capture();
    const UtopiaPanelObservation recognized = recognize_utopia_panel(panel_image);
    UtopiaPanelInspectionDisposition disposition = classify_utopia_panel_inspection(recognized);

    // 实托邦并非每层必定存在。固定位置没有标记时，第一次点击不会打开任何弹层，
    // 方针和理念自然都会拒识。确认画面仍是稳定地图后，将“本层无实托邦”作为
    // 正常的空观测缓存；不要再点一次不存在的弹层，也不要阻断地图重建。
    if (disposition == UtopiaPanelInspectionDisposition::Absent) {
        std::string stable_error;
        if (m_task_context->capture_stable_map(stable_map_image, &stable_error)) {
            m_utopia_generation = map_generation;
            m_utopia_observation = {};
            observation = {};
            Log.info("BlackFlow utopia marker is absent on this map", "generation", map_generation);
            return true;
        }

        // 两项标题都漏识别也会呈现为空；此时地图 HUD 被弹层遮挡，稳定地图检查会
        // 失败。把它保留为真正的 OCR 异常，并先关闭弹层恢复地图。
        disposition = UtopiaPanelInspectionDisposition::Incomplete;
    }

    std::string close_error;
    if (!m_task_context->execute({ std::string(UtopiaPanelToggleTask) }, &close_error)) {
        set_error(error, "utopia panel could not be closed: " + close_error);
        return false;
    }
    if (disposition == UtopiaPanelInspectionDisposition::Incomplete) {
        if (!m_task_context->capture_stable_map(stable_map_image, error)) {
            return false;
        }
        set_error(error, "utopia panel title OCR did not identify both policy and ideology");
        return false;
    }

    // 游戏偶尔只绘制部分理想域而漏掉理想源特效。实托邦标题已经完整确认后，
    // 确定地回到主题主菜单再继续探索，让游戏重新生成完整的地图表现。该流程与
    // 页面异常时的 RecoverMap 兜底严格分开，避免把正常刷新误记成故障恢复。
    std::string refresh_error;
    if (!m_task_context->execute({ std::string(UtopiaMapRefreshTask) }, &refresh_error) ||
        !m_task_context->last_task().ends_with(UtopiaMapRefreshCompletedTask)) {
        set_error(
            error,
            "utopia map refresh did not return to the map: " +
                (refresh_error.empty() ? m_task_context->last_task() : refresh_error));
        return false;
    }
    map_refreshed = true;

    // 回到主菜单再继续探索是有外部副作用的操作：一旦刷新链确认已经回到缩小后的
    // 地图，本代实托邦就已经处理完毕。必须在后续稳定截图之前提交缓存；否则一次
    // 瞬时遮挡或截图不稳定会让地图重建重试再次执行整段主菜单刷新，形成死循环。
    m_utopia_generation = map_generation;
    m_utopia_observation = recognized;
    observation = recognized;

    if (!m_task_context->capture_stable_map(stable_map_image, error)) {
        return false;
    }

    Log.info(
        "BlackFlow utopia title recognized and map refreshed through the main menu",
        "generation",
        map_generation,
        "policy",
        recognized.policy,
        "ideology",
        recognized.ideology);
    return true;
}

bool BlackFlowTaskPort::classify_entered_page(
    const cv::Mat& image,
    EnteredPageObservation& observation,
    std::string* error) const
{
    const auto task = Task.get<OcrTaskInfo>(EnteredPageClassificationTask);
    if (task == nullptr) {
        set_error(error, "entered-page OCR task is missing");
        return false;
    }

    enum class EnteredPageProbe
    {
        Unknown,
        Classified,
        CombatOperatorSelection,
        Failed,
    };

    const auto classify_known_page = [&](const cv::Mat& candidate) {
        if (matches_template(candidate, EnteredPageClassificationCombatTask)) {
            observation = {};
            observation.matched_texts.emplace_back("快捷编队主界面");
            observation.classified_type = NodeType::BattleNormal;
            return EnteredPageProbe::Classified;
        }
        const bool operator_confirm_visible =
            matches_template(candidate, EnteredPageClassificationCombatOperatorConfirmTask);
        OCRer analyzer(candidate);
        analyzer.set_task_info(task);
        const auto results = analyzer.analyze();
        std::vector<std::string> matched_texts;
        if (results.has_value()) {
            matched_texts.reserve(results->size());
            for (const auto& result : *results) {
                matched_texts.emplace_back(result.text);
            }
        }
        observation = classify_entered_page_texts(std::move(matched_texts));
        if (operator_confirm_visible) {
            observation.combat_operator_selection_open = true;
            observation.matched_texts.emplace_back("干员选择确认按钮");
        }
        if (observation.classified_type.has_value() || observation.classification_conflict ||
            observation.inventory_overloaded) {
            return EnteredPageProbe::Classified;
        }
        if (observation.combat_operator_selection_open) {
            return EnteredPageProbe::CombatOperatorSelection;
        }
        return EnteredPageProbe::Unknown;
    };

    const auto classify_and_close_combat_operator_selection = [&](cv::Mat candidate) {
        EnteredPageProbe probe = classify_known_page(candidate);
        for (int back = 0;
             probe == EnteredPageProbe::CombatOperatorSelection && back < EnteredPageCombatOperatorMaximumBacks;
             ++back) {
            Log.warn(
                "BlackFlow entered combat with the operator picker open; returning to quick formation",
                "back_attempt",
                back + 1);
            if (!m_task_context->execute(
                    { std::string(EnteredPageClassificationCombatOperatorBackTask) },
                    error)) {
                set_error(error, "combat operator picker could not be closed safely");
                return EnteredPageProbe::Failed;
            }
            candidate = m_task_context->capture();
            probe = classify_known_page(candidate);
        }
        if (probe == EnteredPageProbe::CombatOperatorSelection) {
            set_error(error, "combat operator picker remained open after two verified return clicks");
            return EnteredPageProbe::Failed;
        }
        return probe;
    };

    const auto initial_probe = classify_and_close_combat_operator_selection(image);
    if (initial_probe == EnteredPageProbe::Classified) {
        return true;
    }
    if (initial_probe == EnteredPageProbe::Failed) {
        return false;
    }

    // 确认移动后的首帧经常还在转场。原先只在该帧检查诡意行商、秘境行商等固定页面，随后即使等待了
    // 事件页动画，也只识别事件标题，导致晚出现的秘境行商仍以 hide_invisible 分发到
    // Page-Default。先被动等待并用新截图重试，避免点击尚未稳定的行商页面。
    for (int retry = 0; retry < EnteredPageClassificationRetryTimes; ++retry) {
        if (!m_task_context->execute({ std::string(EnteredPageClassificationRetryWaitTask) }, error)) {
            return false;
        }
        const auto retry_probe = classify_and_close_combat_operator_selection(m_task_context->capture());
        if (retry_probe == EnteredPageProbe::Classified) {
            return true;
        }
        if (retry_probe == EnteredPageProbe::Failed) {
            return false;
        }
    }

    // 到达节点时可能先获得自然物、概念体或加工品，奖励详情会盖住真正的节点页和地图 HUD。
    // 这类弹窗是正常流程；先按现有奖励模板逐层关闭，再从新截图识别实际落点。旧逻辑把它当成
    // “页面无法分类”并直接重开本局，而左上角退出按钮又被弹窗拦截，最终形成无限退出循环。
    for (int retry = 0; retry < EnteredPageRewardPrepareTimes; ++retry) {
        if (!m_task_context->execute({ std::string(EnteredPageClassificationRewardPrepareTask) }, error)) {
            return false;
        }
        const cv::Mat reward_prepared_image = m_task_context->capture();
        const auto reward_probe = classify_and_close_combat_operator_selection(reward_prepared_image);
        if (reward_probe == EnteredPageProbe::Classified) {
            return true;
        }
        if (reward_probe == EnteredPageProbe::Failed) {
            return false;
        }
        const auto encounter_title = recognize_text(reward_prepared_image, StageEncounterOcrTask);
        if (encounter_title.has_value()) {
            observation = classify_entered_event_name(*encounter_title);
            return true;
        }
        if (recognize_action_points(reward_prepared_image).has_value()) {
            observation.map_visible = true;
            return true;
        }
    }

    // 只有确认当前不是快捷编队的干员列表/详情页后，才允许执行事件动画的中央推进点击。
    // 该兜底会落在干员卡片区域，因此绝不能拿来“试探”未知页面。
    if (!m_task_context->execute({ std::string(EnteredPageClassificationEncounterPrepareTask) }, error)) {
        return false;
    }
    const cv::Mat prepared_image = m_task_context->capture();
    const auto prepared_probe = classify_and_close_combat_operator_selection(prepared_image);
    if (prepared_probe == EnteredPageProbe::Classified) {
        return true;
    }
    if (prepared_probe == EnteredPageProbe::Failed) {
        return false;
    }
    const auto encounter_title = recognize_text(prepared_image, StageEncounterOcrTask);
    if (encounter_title.has_value()) {
        observation = classify_entered_event_name(*encounter_title);
        return true;
    }
    if (recognize_action_points(prepared_image).has_value()) {
        observation.map_visible = true;
        return true;
    }
    set_error(error, "entered page could not be classified and the map HUD is not visible");
    return false;
}
} // namespace asst::blackflow
