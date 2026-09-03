#include "BlackFlowMovementTaskPlugin.h"

#include "BlackFlowMovementRecognition.h"
#include "BlackFlowInventoryRules.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

#include <opencv2/core.hpp>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/OCRer.h"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view SelectMovementTrigger = "BlackFlow@Roguelike@SelectMovement";
constexpr std::string_view SelectionAction = "BlackFlow@Roguelike@SelectMovementAction";
constexpr std::string_view InventoryObservationTrigger = "BlackFlow@Roguelike@MovementInventoryObserve";
constexpr std::string_view InventoryObservationAction = "BlackFlow@Roguelike@MovementInventoryObservationAction";
constexpr std::string_view DirectDepartOverloadTrigger =
    "BlackFlow@Roguelike@DirectDepartInventoryOverloadPrompt";
constexpr std::string_view DirectDepartOverloadAction =
    "BlackFlow@Roguelike@DirectDepartInventoryOverloadAction";
constexpr std::string_view HuntedDepartTask = "BlackFlow@Roguelike@HuntedDepart";
constexpr std::string_view StageEncounterBattleDepartTask = "BlackFlow@Roguelike@StageEncounterBattleDepart";
constexpr std::string_view StageEnterBattleAgainTask = "BlackFlow@Roguelike@StageEnterBattleAgain";
constexpr std::string_view HuntedResumeTask = "BlackFlow@Roguelike@DirectDepartHuntedResume";
constexpr std::string_view EncounterBattleResumeTask = "BlackFlow@Roguelike@DirectDepartEncounterResume";
constexpr std::string_view BattleReenterResumeTask = "BlackFlow@Roguelike@DirectDepartBattleReenterResume";
constexpr std::string_view RecoveryFailedTask = "BlackFlow@Roguelike@RecoveryFailed";
constexpr std::string_view InventoryCheckTask = "BlackFlow@Roguelike@MovementInventoryCheck";
constexpr std::string_view InventoryItemsTask = "BlackFlow@Roguelike@MovementInventoryItems";
constexpr std::string_view InventoryNaturalPriorityTask = "BlackFlow@Roguelike@InventoryNaturalPriority";
constexpr std::string_view InventoryConceptPriorityTask = "BlackFlow@Roguelike@InventoryConceptPriority";
constexpr std::string_view InventoryEmptySlotText = "待收集零件";
constexpr std::string_view InventorySwipeTask = "BlackFlow@Roguelike@MovementInventorySwipe";
// 经 -Enter 转发，避免占位任务从 MovementInventoryClose 继承 maxTimes。
constexpr std::string_view InventoryCloseTask = "BlackFlow@Roguelike@MovementInventoryClose-Enter";
constexpr std::string_view OpenPanelTask = "BlackFlow@Roguelike@MovementPanelOpenClick";
constexpr std::string_view PanelTitleTask = "BlackFlow@Roguelike@MovementPanelTitle";
constexpr std::string_view PanelItemsTask = "BlackFlow@Roguelike@MovementPanelItems";
constexpr std::string_view SwipePanelTask = "BlackFlow@Roguelike@MovementPanelSwipe";
constexpr std::string_view SwipePanelToStartTask = "BlackFlow@Roguelike@MovementPanelSwipeToStart";
constexpr std::string_view ClosePanelTask = "BlackFlow@Roguelike@MovementPanelBackClick";
constexpr std::string_view PanelTitle = "选择移动方式";
constexpr std::string_view LoadedText = "装载中";

constexpr int MaxOpenAttempts = 3;
constexpr int MaxFrameRecognitionAttempts = 2;
constexpr int MaxSelectionVerificationAttempts = 3;
constexpr int MaxSelectionStabilityAttempts = 6;
constexpr int LoadedMarkerMaximumDistance = 60;
// 「剩余N次」在卡片左上、名字在卡片右下，实测两者相距约 69px，下一张卡片的名字则在 200px 开外。
constexpr int RemainingMarkerMaximumGap = 120;
constexpr int SelectionSettleDelay = 500;
constexpr int SelectionStabilityProbeDelay = 150;
constexpr int RecognitionRetryDelay = 200;
constexpr int InventoryLoadedMarkerMaximumVerticalGap = 32;
constexpr int InventoryNewRightColumnMinimumX = 760;
constexpr double InventoryUnchangedFrameMaximumDifference = 3.0;

void set_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

int vertical_center(const Rect& rect) noexcept
{
    return rect.y + rect.height / 2;
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

bool task_contains_inventory_name(std::string_view task_name, std::string_view name)
{
    const auto task = Task.get<OcrTaskInfo>(std::string(task_name));
    return task != nullptr && std::ranges::find(task->text, name) != task->text.end();
}

std::optional<std::string_view> inventory_boundary_label(std::string_view name)
{
    if (name == InventoryEmptySlotText) {
        return "空格";
    }
    if (task_contains_inventory_name(InventoryNaturalPriorityTask, name)) {
        return "自然物";
    }
    if (task_contains_inventory_name(InventoryConceptPriorityTask, name)) {
        return "概念体";
    }
    return std::nullopt;
}

} // namespace

bool BlackFlowMovementTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    const std::string task = details.get("details", "task", "");
    if (task == SelectMovementTrigger) {
        m_pending = PendingWork::SelectMovement;
        return true;
    }
    if (task == InventoryObservationTrigger) {
        m_pending = PendingWork::ObserveInventory;
        return true;
    }
    if (task == DirectDepartOverloadTrigger) {
        m_pending = PendingWork::CleanupDirectDepartOverload;
        m_direct_depart_source = details.get("pre_task", std::string {});
        return true;
    }
    return false;
}

void BlackFlowMovementTaskPlugin::reset_in_run_variables()
{
    m_pending = PendingWork::None;
    m_direct_depart_source.clear();
}

bool BlackFlowMovementTaskPlugin::_run()
{
    LogTraceFunction;
    const PendingWork work = m_pending;
    m_pending = PendingWork::None;
    if (work == PendingWork::None) {
        return true;
    }
    if (work == PendingWork::CleanupDirectDepartOverload) {
        const std::string source = std::move(m_direct_depart_source);
        m_direct_depart_source.clear();
        return cleanup_direct_depart_overload(source);
    }
    if (work == PendingWork::ObserveInventory) {
        return observe_inventory();
    }

    Task.set_task_base(std::string(SelectionAction), "BlackFlow@Roguelike@RecoveryFailed");

    if (m_session == nullptr || !m_session->pending_candidate().has_value()) {
        Log.error("BlackFlow movement selection has no pending route candidate");
        return true;
    }

    const MovementKind target = m_session->pending_candidate()->candidate.movement;
    const MovementSpec* target_spec = find_movement_spec(target);
    std::string error;
    const SelectionOutcome outcome = select_movement(target, &error);
    if (outcome == SelectionOutcome::Selected) {
        Task.set_task_base(std::string(SelectionAction), "BlackFlow@Roguelike@RoutingResume-Enter");
        Log.info("BlackFlow movement selected", target_spec == nullptr ? std::string_view("unknown") : target_spec->id);
    }
    else if (outcome == SelectionOutcome::Unavailable) {
        Task.set_task_base(std::string(SelectionAction), "BlackFlow@Roguelike@MapPrepare");
        Log.info(
            "BlackFlow movement unavailable; route will be replanned",
            target_spec == nullptr ? std::string_view("unknown") : target_spec->id);
    }
    else {
        // 不写结果的话，外层只会报「终止时没有策略结果」，真实原因就丢了。
        m_session->fail(
            "movement_selection_failed",
            error.empty() ? "movement selection failed" : error,
            FailureDisposition::RestartRun);
        Log.error("BlackFlow movement selection failed", error);
    }
    report_outputs();
    return true;
}

bool BlackFlowMovementTaskPlugin::cleanup_direct_depart_overload(std::string_view source_task)
{
    Task.set_task_base(std::string(DirectDepartOverloadAction), std::string(RecoveryFailedTask));
    if (m_session == nullptr || m_port == nullptr) {
        Log.error("BlackFlow direct-depart inventory cleanup has no active session or task port");
        return true;
    }

    const bool hunted = source_task == HuntedDepartTask;
    const bool encounter_battle = source_task == StageEncounterBattleDepartTask;
    const bool battle_reenter = source_task == StageEnterBattleAgainTask;
    if (!hunted && !encounter_battle && !battle_reenter) {
        m_session->fail(
            "direct_depart_inventory_cleanup_failed",
            "inventory overload was reached from an unknown direct-depart task: " + std::string(source_task),
            FailureDisposition::StopTask);
        Log.error("BlackFlow direct-depart inventory overload has unknown source", source_task);
        report_outputs();
        return true;
    }

    record_run_event(
        RunLogLevel::Info,
        "inventory.depart-overload",
        "started",
        "pending",
        json::object { { "source_task", std::string(source_task) } },
        "BlackFlowDirectDepartInventory");
    std::string error;
    if (!m_port->cleanup_depart_inventory_overload(&error)) {
        record_run_event(
            RunLogLevel::Error,
            "inventory.depart-overload",
            "failed",
            "error",
            json::object { { "source_task", std::string(source_task) }, { "error", error } },
            "BlackFlowDirectDepartInventory",
            nullptr,
            true);
        m_session->fail(
            "direct_depart_inventory_cleanup_failed",
            error.empty() ? "direct-depart inventory overload cleanup failed" : error,
            FailureDisposition::StopTask);
        Log.error("BlackFlow direct-depart inventory overload cleanup failed", source_task, error);
        report_outputs();
        return true;
    }

    // 过载拦截发生在实际进入节点之前，且整理可能丢掉加工品；废弃旧背包快照。
    // 关闭零件箱后可能回到原预览，也可能回到地图，因此先走来源专用的现场路由，
    // 不能直接把追猎送进 Stages，否则地图先于追猎确认出现时会误走 NextLevel。
    m_session->invalidate_movement_inventory();
    const std::string_view resume_task = hunted          ? HuntedResumeTask
                                         : encounter_battle ? EncounterBattleResumeTask
                                                            : BattleReenterResumeTask;
    Task.set_task_base(std::string(DirectDepartOverloadAction), std::string(resume_task));
    record_run_event(
        RunLogLevel::Info,
        "inventory.depart-overload",
        "completed",
        "success",
        json::object {
            { "source_task", std::string(source_task) },
            { "resume_task", std::string(resume_task) },
        },
        "BlackFlowDirectDepartInventory",
        nullptr,
        true);
    Log.info("BlackFlow direct-depart inventory overload cleaned", source_task, "resume", resume_task);
    report_outputs();
    return true;
}

bool BlackFlowMovementTaskPlugin::observe_inventory()
{
    Task.set_task_base(std::string(InventoryObservationAction), std::string(InventoryCloseTask));
    if (m_session == nullptr || m_port == nullptr) {
        Log.error("BlackFlow movement inventory observation has no active session or task port");
        return true;
    }

    InventoryFrame frame;
    std::string error;
    bool cleanup_performed = false;
    record_run_event(
        RunLogLevel::Info,
        "inventory.overload-check",
        "started",
        "pending",
        {},
        "BlackFlowMovementInventory",
        nullptr,
        true);
    if (!m_port->cleanup_open_inventory_if_overloaded(cleanup_performed, &error)) {
        record_run_event(
            RunLogLevel::Error,
            "inventory.overload-check",
            "failed",
            "error",
            json::object { { "error", error } },
            "BlackFlowMovementInventory",
            nullptr,
            true);
        record_inventory_evidence(frame, "过载清理失败", error);
        m_session->fail(
            "movement_inventory_cleanup_failed",
            error.empty() ? "movement inventory overload cleanup failed" : error,
            FailureDisposition::StopTask);
        Log.error("BlackFlow movement inventory overload cleanup failed", error);
        report_outputs();
        return true;
    }
    record_run_event(
        RunLogLevel::Info,
        "inventory.overload-check",
        "completed",
        "success",
        json::object { { "cleanup_performed", cleanup_performed } },
        "BlackFlowMovementInventory",
        nullptr,
        true);
    if (cleanup_performed) {
        // 清理流程已经从零件箱返回地图。重新进入零件箱，以清理后的内容刷新加工品状态，
        // 避免沿用清理前的路线规划资源快照。
        Task.set_task_base(std::string(InventoryObservationAction), std::string(InventoryCheckTask));
        Log.info("BlackFlow movement inventory overload cleaned proactively; reopening inventory for observation");
        report_outputs();
        return true;
    }
    if (!scan_inventory_frame(frame, &error) ||
        !m_session->apply_movement_inventory_observation(frame.movement_instances, &error)) {
        record_inventory_evidence(frame, "识别或状态更新失败", error);
        m_session->fail(
            "movement_inventory_observation_failed",
            error.empty() ? "movement inventory OCR failed" : error,
            FailureDisposition::StopTask);
        Log.error("BlackFlow movement inventory observation failed", error);
        report_outputs();
        return true;
    }
    record_inventory_evidence(frame, "识别完成", {});
    m_session->mark_map_preserved_after_inventory();

    const MovementSpec* loaded =
        frame.loaded_movement.has_value() ? find_movement_spec(*frame.loaded_movement) : nullptr;
    Log.info(
        "BlackFlow movement inventory observed",
        "visible instances",
        frame.movement_instances.size(),
        "loaded marker",
        loaded == nullptr ? std::string_view("none") : loaded->id);
    report_outputs();
    return true;
}

bool BlackFlowMovementTaskPlugin::scan_inventory_frame(InventoryFrame& frame, std::string* error)
{
    frame = {};
    std::optional<cv::Mat> previous;
    int scan_page = 0;
    bool inspect_new_right_column = false;
    bool reached_end = false;

    for (const InventoryScanAction action : inventory_full_scan_plan()) {
        if (action == InventoryScanAction::AdvanceTowardEnd) {
            if (!run_fixed_task(InventorySwipeTask)) {
                set_error(error, "movement inventory could not advance to the next column");
                return false;
            }
            ++scan_page;
            inspect_new_right_column = true;
            continue;
        }

        const cv::Mat image = ctrler()->get_image();
        if (previous.has_value() && previous->size() == image.size() && previous->type() == image.type()) {
            const cv::Mat previous_cards = inventory_card_region(*previous);
            const cv::Mat current_cards = inventory_card_region(image);
            const double denominator =
                static_cast<double>(current_cards.total()) * static_cast<double>(current_cards.channels());
            const double difference = cv::norm(previous_cards, current_cards, cv::NORM_L1) / denominator;
            if (inspect_new_right_column && difference <= InventoryUnchangedFrameMaximumDifference) {
                reached_end = true;
                break;
            }
        }

        InventoryFrame page;
        std::string latest_error;
        InventoryAnalysisOutcome outcome = InventoryAnalysisOutcome::Failed;
        for (int attempt = 0; attempt < MaxFrameRecognitionAttempts; ++attempt) {
            outcome = analyze_inventory_frame(
                image,
                page,
                inspect_new_right_column ? InventoryNewRightColumnMinimumX : 0,
                &latest_error);
            if (outcome != InventoryAnalysisOutcome::Failed) {
                break;
            }
            if (attempt + 1 < MaxFrameRecognitionAttempts) {
                sleep(RecognitionRetryDelay);
            }
        }
        if (outcome == InventoryAnalysisOutcome::Failed) {
            set_error(error, latest_error.empty() ? "movement inventory OCR failed" : latest_error);
            return false;
        }
        if (page.loaded_movement.has_value()) {
            frame.loaded_movement = page.loaded_movement;
        }
        for (InventoryItem& item : page.items) {
            item.scan_page = scan_page;
            item.inventory_index = static_cast<int>(frame.items.size());
            frame.movement_instances.emplace_back(
                RunResources::MovementInstance {
                    item.movement,
                    item.remaining_uses,
                    item.inventory_index,
                    item.loaded,
                });
            frame.items.emplace_back(std::move(item));
        }
        for (InventoryBoundaryItem& item : page.type_boundary_items) {
            item.scan_page = scan_page;
            frame.type_boundary_items.emplace_back(std::move(item));
        }
        frame.images.emplace_back(scan_page, std::make_shared<cv::Mat>(image.clone()));
        previous = image.clone();
        inspect_new_right_column = false;
        if (!page.type_boundary_items.empty()) {
            frame.stopped_at_ordered_boundary = true;
            break;
        }
    }
    frame.scan_complete =
        frame.stopped_at_ordered_boundary || reached_end || scan_page == InventoryMaximumSwipes;
    return true;
}

BlackFlowMovementTaskPlugin::InventoryAnalysisOutcome BlackFlowMovementTaskPlugin::analyze_inventory_frame(
    const cv::Mat& image,
    InventoryFrame& frame,
    int minimum_name_x,
    std::string* error) const
{
    if (image.empty()) {
        set_error(error, "movement inventory screenshot is empty");
        return InventoryAnalysisOutcome::Failed;
    }

    const auto task = Task.get<OcrTaskInfo>(std::string(InventoryItemsTask));
    if (task == nullptr) {
        set_error(error, "movement inventory OCR task is missing");
        return InventoryAnalysisOutcome::Failed;
    }

    OCRer analyzer(image);
    analyzer.set_task_info(task);
    analyzer.set_required(inventory_ocr_candidates());
    const auto results = analyzer.analyze();
    if (!results.has_value()) {
        return InventoryAnalysisOutcome::NoCandidate;
    }

    std::vector<InventoryItem> items;
    std::vector<Rect> loaded_markers;
    for (const auto& result : *results) {
        if (const MovementSpec* movement = movement_from_name(result.text);
            movement != nullptr && movement->kind != MovementKind::Walk) {
            if (result.rect.x + result.rect.width / 2 < minimum_name_x) {
                continue;
            }
            const auto charge_recognition =
                recognize_movement_inventory_remaining_uses(image, result.rect, movement->initial_charges);
            if (!charge_recognition.has_value()) {
                set_error(error, "movement inventory star slots extend outside the screenshot");
                return InventoryAnalysisOutcome::Failed;
            }
            items.emplace_back(
                InventoryItem {
                    movement->kind,
                    result.rect,
                    result.score,
                    charge_recognition->first,
                    std::move(charge_recognition->second),
                    false,
                });
        }
        else if (result.text == LoadedText) {
            loaded_markers.emplace_back(result.rect);
        }
        else if (const auto boundary_label = inventory_boundary_label(result.text); boundary_label.has_value()) {
            if (result.rect.x + result.rect.width / 2 < minimum_name_x) {
                continue;
            }
            frame.type_boundary_items.emplace_back(
                InventoryBoundaryItem { result.text, std::string(*boundary_label), result.rect, result.score, 0 });
        }
    }

    InventoryItem* loaded_item = nullptr;
    int best_horizontal_gap = std::numeric_limits<int>::max();
    for (const Rect& marker : loaded_markers) {
        const int marker_center = vertical_center(marker);
        for (InventoryItem& item : items) {
            if (std::abs(vertical_center(item.name_rect) - marker_center) > InventoryLoadedMarkerMaximumVerticalGap) {
                continue;
            }
            const int horizontal_gap = marker.x - (item.name_rect.x + item.name_rect.width);
            if (horizontal_gap >= 0 && horizontal_gap < best_horizontal_gap) {
                frame.loaded_movement = item.movement;
                loaded_item = &item;
                best_horizontal_gap = horizontal_gap;
            }
        }
    }
    if (!loaded_markers.empty() && !frame.loaded_movement.has_value()) {
        Log.warn("BlackFlow movement inventory loaded marker has no recognized item on its left");
    }
    if (loaded_item != nullptr) {
        loaded_item->loaded = true;
    }
    frame.items = std::move(items);
    return InventoryAnalysisOutcome::Recognized;
}

BlackFlowMovementTaskPlugin::SelectionOutcome
    BlackFlowMovementTaskPlugin::select_movement(MovementKind target, std::string* error)
{
    if (find_movement_spec(target) == nullptr) {
        set_error(error, "movement selection requested an unknown movement kind");
        return SelectionOutcome::Failed;
    }

    const cv::Mat loaded_icon_image = ctrler()->get_image();
    const auto loaded_icon = recognize_loaded_movement_with_evidence(loaded_icon_image);
    {
        const MovementSpec* target_spec = find_movement_spec(target);
        std::vector<json::value> loaded_items;
        if (loaded_icon.has_value()) {
            const MovementSpec* loaded_spec = find_movement_spec(loaded_icon->movement);
            loaded_items.emplace_back(
                json::object {
                    { "movement", std::string(to_string(loaded_icon->movement)) },
                    { "name", loaded_spec == nullptr ? std::string {} : std::string(loaded_spec->name) },
                    { "name_score", loaded_icon->score },
                    { "loaded", true },
                    { "evidence_image_role", "loaded-icon" },
                    { "name_rect",
                      json::object {
                          { "x", loaded_icon->rect.x },
                          { "y", loaded_icon->rect.y },
                          { "width", loaded_icon->rect.width },
                          { "height", loaded_icon->rect.height },
                      } },
                });
        }
        std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images;
        evidence_images.emplace_back(
            DiagnosticArtifactRequest::EvidenceImage {
                "loaded-icon",
                std::make_shared<cv::Mat>(loaded_icon_image.clone()),
            });
        m_session->record_processing_item_evidence(
            json::object {
                { "evidence_type", "loaded_icon" },
                { "target", std::string(to_string(target)) },
                { "target_name", target_spec == nullptr ? std::string {} : std::string(target_spec->name) },
                { "outcome", loaded_icon.has_value() ? "已识别地图装载图标" : "地图装载图标未识别" },
                { "scan_complete", false },
                { "items", json::array(std::move(loaded_items)) },
            },
            std::move(evidence_images));
    }
    if (loaded_icon.has_value()) {
        const MovementKind loaded = loaded_icon->movement;
        MovementPanelObservation current;
        current.target = loaded;
        current.target_found = true;
        if (!m_session->apply_movement_panel_observation(std::move(current), loaded, error)) {
            return SelectionOutcome::Failed;
        }
        const auto same_kind_instances = std::ranges::count_if(
            m_session->run().resources.movement_instances,
            [target](const RunResources::MovementInstance& instance) { return instance.movement == target; });
        if (loaded == target && same_kind_instances <= 1) {
            return SelectionOutcome::Selected;
        }
    }
    if (!ensure_panel_open(error)) {
        return SelectionOutcome::Failed;
    }

    std::vector<std::pair<int, PanelFrame>> observed_frames;
    int reset_swipes = 0;
    int forward_swipes = 0;
    bool target_seen = false;
    bool complete_scan_seen = false;
    std::optional<int> preferred_remaining = m_session->minimum_movement_instance_charges(target);
    for (int selection_attempt = 0; selection_attempt < MaxSelectionVerificationAttempts; ++selection_attempt) {
        // 每次重试都重新做一次完整定位。移动列表会在滑动结束后继续回弹，装载卡片也会自动居中；
        // 因此任何一次失败后的旧页码和旧坐标都不再可信。
        int scan_page = 0;
        int attempt_forward_swipes = 0;
        bool top_anchor_seen = false;
        bool continuous_scan = true;
        bool end_anchor_seen = false;
        bool skip_next_inspect = false;
        int last_inspected_forward_swipes = -1;
        std::optional<std::vector<MovementPanelLayoutEntry>> previous_layout;
        std::optional<PanelItem> located_target;
        std::optional<std::vector<MovementPanelLayoutEntry>> located_layout;
        std::optional<int> best_observed_remaining;
        bool target_without_count_seen = false;
        for (const MovementPanelScanAction action : movement_panel_full_scan_plan()) {
            if (action == MovementPanelScanAction::ResetTowardStart) {
                if (top_anchor_seen) {
                    skip_next_inspect = true;
                    continue;
                }
                if (!run_fixed_task(SwipePanelToStartTask)) {
                    set_error(error, "movement panel could not reset to the first group of items");
                    record_panel_evidence(
                        target,
                        observed_frames,
                        reset_swipes,
                        forward_swipes,
                        "回到列表顶部失败",
                        error == nullptr ? std::string_view {} : std::string_view(*error));
                    close_panel(nullptr);
                    return SelectionOutcome::Failed;
                }
                ++reset_swipes;
                continue;
            }
            if (action == MovementPanelScanAction::AdvanceTowardEnd) {
                if (!run_fixed_task(SwipePanelTask)) {
                    set_error(error, "movement panel could not advance to the next group of items");
                    record_panel_evidence(
                        target,
                        observed_frames,
                        reset_swipes,
                        forward_swipes,
                        "向列表尾部翻页失败",
                        error == nullptr ? std::string_view {} : std::string_view(*error));
                    close_panel(nullptr);
                    return SelectionOutcome::Failed;
                }
                ++forward_swipes;
                ++attempt_forward_swipes;
                ++scan_page;
                continue;
            }

            if (skip_next_inspect) {
                skip_next_inspect = false;
                continue;
            }

            PanelFrame frame;
            if (!scan_current_frame(frame, error)) {
                record_panel_evidence(
                    target,
                    observed_frames,
                    reset_swipes,
                    forward_swipes,
                    "当前页 OCR 失败",
                    error == nullptr ? std::string_view {} : std::string_view(*error));
                close_panel(nullptr);
                return SelectionOutcome::Failed;
            }
            observed_frames.emplace_back(scan_page, frame);
            top_anchor_seen = top_anchor_seen || std::ranges::any_of(frame.items, [](const PanelItem& item) {
                                  return item.movement == MovementKind::Walk;
                              });

            std::vector<MovementPanelCandidate> panel_candidates;
            panel_candidates.reserve(frame.items.size());
            for (const PanelItem& item : frame.items) {
                panel_candidates.emplace_back(MovementPanelCandidate { item.movement, item.remaining_uses });
            }
            const auto target_index = choose_movement_panel_candidate(panel_candidates, target);
            if (target_index.has_value()) {
                target_seen = true;
                const PanelItem& visible_target = frame.items[*target_index];
                if (visible_target.remaining_uses.has_value()) {
                    if (!best_observed_remaining.has_value() ||
                        *visible_target.remaining_uses < *best_observed_remaining) {
                        best_observed_remaining = visible_target.remaining_uses;
                    }
                }
                else {
                    target_without_count_seen = true;
                }
                if (!preferred_remaining.has_value() || visible_target.remaining_uses == preferred_remaining) {
                    located_target = visible_target;
                    located_layout.emplace();
                    located_layout->reserve(frame.name_hits.size());
                    for (const PanelNameHit& item : frame.name_hits) {
                        located_layout->emplace_back(
                            MovementPanelLayoutEntry { item.movement, vertical_center(item.name_rect) });
                    }
                    break;
                }
            }

            std::vector<MovementPanelLayoutEntry> layout;
            layout.reserve(frame.name_hits.size());
            for (const PanelNameHit& item : frame.name_hits) {
                layout.emplace_back(MovementPanelLayoutEntry { item.movement, vertical_center(item.name_rect) });
            }
            if (top_anchor_seen && attempt_forward_swipes != last_inspected_forward_swipes) {
                if (!previous_layout.has_value()) {
                    previous_layout = layout;
                }
                else {
                    const bool unchanged = movement_panel_layout_unchanged(*previous_layout, layout);
                    const bool overlaps = movement_panel_layout_overlaps(*previous_layout, layout);
                    end_anchor_seen = unchanged;
                    continuous_scan = continuous_scan && (unchanged || overlaps);
                    previous_layout = layout;
                }
                last_inspected_forward_swipes = attempt_forward_swipes;
            }
            if ((end_anchor_seen || !continuous_scan) &&
                !movement_panel_should_continue_target_search(false, attempt_forward_swipes)) {
                break;
            }
        }

        if (!located_target.has_value()) {
            const bool attempt_scan_complete =
                movement_panel_scan_is_complete(top_anchor_seen, end_anchor_seen, continuous_scan);
            complete_scan_seen = complete_scan_seen || attempt_scan_complete;
            if (target_seen && selection_attempt + 1 < MaxSelectionVerificationAttempts) {
                if (best_observed_remaining.has_value() && best_observed_remaining != preferred_remaining) {
                    preferred_remaining = best_observed_remaining;
                    continue;
                }
                if (target_without_count_seen && preferred_remaining.has_value()) {
                    preferred_remaining.reset();
                    continue;
                }
            }
            if (selection_attempt + 1 < MaxSelectionVerificationAttempts) {
                Log.warn(
                    "BlackFlow authoritative movement target was not located after a full downward search; retrying",
                    "target",
                    find_movement_spec(target)->id,
                    "attempt",
                    selection_attempt + 1,
                    "continuous",
                    continuous_scan,
                    "end_anchor",
                    end_anchor_seen);
                continue;
            }
            break;
        }

        if (located_target->loaded) {
            if (!report_target_observation(target, *located_target, attempt_forward_swipes, target, error)) {
                record_panel_evidence(
                    target,
                    observed_frames,
                    reset_swipes,
                    forward_swipes,
                    "目标观测无法写入状态",
                    error == nullptr ? std::string_view {} : std::string_view(*error));
                close_panel(nullptr);
                return SelectionOutcome::Failed;
            }
            record_panel_evidence(
                target,
                observed_frames,
                reset_swipes,
                forward_swipes,
                "目标已在当前页装载",
                {});
            return close_panel(error) ? SelectionOutcome::Selected : SelectionOutcome::Failed;
        }

        // 固定等待无法覆盖不同设备上的惯性和顶部回弹。连续两帧布局一致后才使用
        // 当前坐标；稳定得快就立即识别，仍在移动则继续观察，绝不复用旧坐标。
        std::vector<MovementPanelLayoutEntry> stability_layout = std::move(*located_layout);
        std::optional<PanelFrame> settled_frame;
        for (int stability_attempt = 0; stability_attempt < MaxSelectionStabilityAttempts; ++stability_attempt) {
            sleep(SelectionStabilityProbeDelay);
            PanelFrame candidate;
            if (!scan_current_frame(candidate, error)) {
                record_panel_evidence(
                    target,
                    observed_frames,
                    reset_swipes,
                    forward_swipes,
                    "点击前稳定帧 OCR 失败",
                    error == nullptr ? std::string_view {} : std::string_view(*error));
                close_panel(nullptr);
                return SelectionOutcome::Failed;
            }
            observed_frames.emplace_back(scan_page, candidate);
            std::vector<MovementPanelLayoutEntry> candidate_layout;
            candidate_layout.reserve(candidate.name_hits.size());
            for (const PanelNameHit& item : candidate.name_hits) {
                candidate_layout.emplace_back(
                    MovementPanelLayoutEntry { item.movement, vertical_center(item.name_rect) });
            }
            if (movement_panel_layout_unchanged(stability_layout, candidate_layout)) {
                settled_frame = std::move(candidate);
                break;
            }
            stability_layout = std::move(candidate_layout);
        }
        if (!settled_frame.has_value()) {
            Log.warn(
                "BlackFlow movement panel was still moving before selection; reacquiring target",
                "target",
                find_movement_spec(target)->id,
                "attempt",
                selection_attempt + 1);
            continue;
        }
        PanelFrame stable_frame = std::move(*settled_frame);
        std::vector<MovementPanelCandidate> stable_candidates;
        stable_candidates.reserve(stable_frame.items.size());
        for (const PanelItem& item : stable_frame.items) {
            stable_candidates.emplace_back(MovementPanelCandidate { item.movement, item.remaining_uses });
        }
        const auto stable_index = choose_movement_panel_candidate(stable_candidates, target);
        PanelItem* stable_item =
            stable_index.has_value() ? &stable_frame.items[*stable_index] : nullptr;
        if (stable_item != nullptr && located_target->remaining_uses.has_value() &&
            stable_item->remaining_uses != located_target->remaining_uses) {
            stable_item = nullptr;
        }
        std::optional<MovementKind> loaded_for_decision = stable_frame.loaded_movement;
        if (loaded_for_decision == target && (stable_item == nullptr || !stable_item->loaded)) {
            // 同类另一件已经装载不等于目标实例已经装载；继续点击剩余次数最少的那张卡。
            loaded_for_decision.reset();
        }
        const MovementSelectionClickDecision click_decision = movement_selection_click_decision(
            target,
            stable_item != nullptr,
            loaded_for_decision);
        if (click_decision == MovementSelectionClickDecision::AcceptAlreadyLoaded && stable_item == nullptr) {
            Log.warn(
                "BlackFlow loaded movement has no matching instance card; reacquiring target",
                "target",
                find_movement_spec(target)->id);
            continue;
        }
        if (click_decision == MovementSelectionClickDecision::AcceptAlreadyLoaded) {
            if (!report_target_observation(target, *stable_item, attempt_forward_swipes, target, error)) {
                record_panel_evidence(
                    target,
                    observed_frames,
                    reset_swipes,
                    forward_swipes,
                    "目标观测无法写入状态",
                    error == nullptr ? std::string_view {} : std::string_view(*error));
                close_panel(nullptr);
                return SelectionOutcome::Failed;
            }
            record_panel_evidence(
                target,
                observed_frames,
                reset_swipes,
                forward_swipes,
                "稳定等待期间目标已经装载",
                {});
            return close_panel(error) ? SelectionOutcome::Selected : SelectionOutcome::Failed;
        }
        if (click_decision == MovementSelectionClickDecision::ReacquireTarget) {
            Log.warn(
                "BlackFlow movement target left the stable frame; refusing the stale click coordinate",
                "target",
                find_movement_spec(target)->id,
                "attempt",
                selection_attempt + 1);
            continue;
        }

        const Rect selection_rect = movement_panel_selection_rect(stable_item->name_rect);
        record_run_event(
            RunLogLevel::Info,
            "movement.processing-item.select",
            "started",
            "pending",
            json::object {
                { "movement", std::string(find_movement_spec(target)->id) },
                { "remaining_charges", stable_item->remaining_uses.value_or(-1) },
                { "rect", json::array { selection_rect.x, selection_rect.y,
                                         selection_rect.width, selection_rect.height } },
            },
            "BlackFlowMovement",
            nullptr,
            true);
        ctrler()->click(selection_rect);
        sleep(SelectionSettleDelay);

        PanelFrame verification;
        if (!scan_current_frame(verification, error)) {
            record_panel_evidence(
                target,
                observed_frames,
                reset_swipes,
                forward_swipes,
                "点击后复核 OCR 失败",
                error == nullptr ? std::string_view {} : std::string_view(*error));
            close_panel(nullptr);
            return SelectionOutcome::Failed;
        }
        observed_frames.emplace_back(scan_page, verification);
        const auto verified_item = std::find_if(
            verification.items.begin(),
            verification.items.end(),
            [target](const PanelItem& item) { return item.movement == target && item.loaded; });
        if (verified_item != verification.items.end() && verified_item->loaded &&
            verification.loaded_movement == target) {
            if (!report_target_observation(target, *verified_item, attempt_forward_swipes, target, error)) {
                record_panel_evidence(
                    target,
                    observed_frames,
                    reset_swipes,
                    forward_swipes,
                    "目标观测无法写入状态",
                    error == nullptr ? std::string_view {} : std::string_view(*error));
                close_panel(nullptr);
                return SelectionOutcome::Failed;
            }
            record_panel_evidence(
                target,
                observed_frames,
                reset_swipes,
                forward_swipes,
                selection_attempt == 0 ? "选择并复核成功" : "重新完整定位后选择并复核成功",
                {});
            return close_panel(error) ? SelectionOutcome::Selected : SelectionOutcome::Failed;
        }

        const MovementSpec* loaded_spec = verification.loaded_movement.has_value()
                                              ? find_movement_spec(*verification.loaded_movement)
                                              : nullptr;
        Log.warn(
            "BlackFlow movement selection did not load the target; next attempt will rescan the complete panel",
            "target",
            find_movement_spec(target)->id,
            "loaded",
            loaded_spec == nullptr ? std::string_view("unknown") : loaded_spec->id,
            "attempt",
            selection_attempt + 1);
    }

    if (target_seen) {
        set_error(error, "authoritative movement target was seen but could not be stably selected after full rescans");
        record_panel_evidence(
            target,
            observed_frames,
            reset_swipes,
            forward_swipes,
            "零件箱确认目标存在，但多次完整向下扫描后仍无法稳定装载",
            error == nullptr ? std::string_view {} : std::string_view(*error));
        close_panel(nullptr);
        return SelectionOutcome::Failed;
    }

    if (target == MovementKind::Walk) {
        set_error(error, "movement panel full scan did not recognize walking");
        record_panel_evidence(
            target,
            observed_frames,
            reset_swipes,
            forward_swipes,
            "完整扫描后没有识别到徒步",
            error == nullptr ? std::string_view {} : std::string_view(*error));
        close_panel(nullptr);
        return SelectionOutcome::Failed;
    }

    set_error(error, "authoritative movement target was not recognized after repeated full downward searches");
    record_panel_evidence(
        target,
        observed_frames,
        reset_swipes,
        forward_swipes,
        "零件箱确认目标存在；选择面板已反复扫满安全上限但仍未识别到目标",
        error == nullptr ? std::string_view {} : std::string_view(*error),
        complete_scan_seen);
    close_panel(nullptr);
    return SelectionOutcome::Failed;
}

bool BlackFlowMovementTaskPlugin::ensure_panel_open(std::string* error)
{
    if (title_visible(ctrler()->get_image())) {
        return true;
    }
    for (int attempt = 0; attempt < MaxOpenAttempts; ++attempt) {
        if (!run_fixed_task(OpenPanelTask)) {
            continue;
        }
        if (title_visible(ctrler()->get_image())) {
            return true;
        }
    }
    set_error(error, "movement panel title did not appear after the open action");
    return false;
}

bool BlackFlowMovementTaskPlugin::close_panel(std::string* error)
{
    if (!title_visible(ctrler()->get_image())) {
        return true;
    }
    for (int attempt = 0; attempt < MaxOpenAttempts; ++attempt) {
        if (!run_fixed_task(ClosePanelTask)) {
            continue;
        }
        if (!title_visible(ctrler()->get_image())) {
            return true;
        }
    }
    set_error(error, "movement panel title remained visible after the close action");
    return false;
}

bool BlackFlowMovementTaskPlugin::title_visible(const cv::Mat& image) const
{
    const auto task = Task.get<OcrTaskInfo>(std::string(PanelTitleTask));
    if (task == nullptr) {
        Log.error("BlackFlow movement panel title OCR task is missing", PanelTitleTask);
        return false;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task);
    analyzer.set_required({ std::string(PanelTitle) });
    const auto result = analyzer.analyze();
    return result.has_value() && !result->empty();
}

bool BlackFlowMovementTaskPlugin::scan_current_frame(PanelFrame& frame, std::string* error) const
{
    std::string latest_error;
    for (int attempt = 0; attempt < MaxFrameRecognitionAttempts; ++attempt) {
        PanelFrame candidate;
        const cv::Mat image = ctrler()->get_image();
        if (analyze_frame(image, candidate, &latest_error)) {
            candidate.image = std::make_shared<cv::Mat>(image.clone());
            frame = std::move(candidate);
            return true;
        }
        if (attempt + 1 < MaxFrameRecognitionAttempts) {
            sleep(RecognitionRetryDelay);
        }
    }
    set_error(
        error,
        latest_error.empty() ? "movement panel item recognition returned no movement names" : latest_error);
    return false;
}

bool BlackFlowMovementTaskPlugin::analyze_frame(const cv::Mat& image, PanelFrame& frame, std::string* error) const
{
    const auto task = Task.get<OcrTaskInfo>(std::string(PanelItemsTask));
    if (task == nullptr) {
        set_error(error, "movement panel item OCR task is missing");
        return false;
    }

    OCRer analyzer(image);
    analyzer.set_task_info(task);
    analyzer.set_required(ocr_candidates());
    const auto results = analyzer.analyze();
    if (!results.has_value()) {
        set_error(error, "movement panel item OCR failed");
        return false;
    }

    std::vector<Rect> loaded_markers;
    std::vector<std::pair<Rect, int>> remaining_markers;
    for (const auto& result : *results) {
        if (const MovementSpec* movement = movement_from_name(result.text); movement != nullptr) {
            frame.name_hits.emplace_back(PanelNameHit { movement->kind, result.rect, result.score });
            frame.items.emplace_back(
                PanelItem {
                    movement->kind,
                    result.rect,
                    result.score,
                    std::nullopt,
                    false,
                });
            continue;
        }
        if (result.text == LoadedText) {
            loaded_markers.emplace_back(result.rect);
            continue;
        }
        if (const auto remaining = remaining_uses_from_text(result.text); remaining.has_value()) {
            remaining_markers.emplace_back(result.rect, *remaining);
        }
    }

    if (frame.items.empty()) {
        set_error(error, "movement panel item OCR found no recognized movement names");
        return false;
    }

    std::sort(frame.items.begin(), frame.items.end(), [](const PanelItem& left, const PanelItem& right) {
        const int left_center = vertical_center(left.name_rect);
        const int right_center = vertical_center(right.name_rect);
        return left_center == right_center ? left.name_rect.x < right.name_rect.x : left_center < right_center;
    });
    std::sort(frame.name_hits.begin(), frame.name_hits.end(), [](const PanelNameHit& left, const PanelNameHit& right) {
        const int left_center = vertical_center(left.name_rect);
        const int right_center = vertical_center(right.name_rect);
        return left_center == right_center ? left.name_rect.x < right.name_rect.x : left_center < right_center;
    });

    // 「装载中」就贴在它那张实例卡片的名字上方，取垂直距离最近的名字即可；同类卡片
    // 不能再按 MovementKind 合并，否则装载标记和剩余次数都会被错误挂到第一件上。
    PanelItem* loaded_item = nullptr;
    int loaded_distance = std::numeric_limits<int>::max();
    for (const Rect& marker : loaded_markers) {
        const int marker_center = vertical_center(marker);
        for (PanelItem& item : frame.items) {
            const int distance = std::abs(vertical_center(item.name_rect) - marker_center);
            if (distance > LoadedMarkerMaximumDistance || distance >= loaded_distance) {
                continue;
            }
            loaded_item = &item;
            loaded_distance = distance;
        }
    }
    if (loaded_item != nullptr) {
        loaded_item->loaded = true;
        frame.loaded_movement = loaded_item->movement;
    }

    // 一张卡片里「剩余N次」在上、名字在下，所以标记归属它下方最近的那个名字；
    // 卡片只露出上半截时下方没有名字，丢掉，不能算到别人头上。
    for (const auto& [marker, remaining] : remaining_markers) {
        const int marker_bottom = marker.y + marker.height;
        PanelItem* owner = nullptr;
        int best_gap = std::numeric_limits<int>::max();
        for (PanelItem& item : frame.items) {
            const int gap = item.name_rect.y - marker_bottom;
            if (gap >= 0 && gap <= RemainingMarkerMaximumGap && gap < best_gap) {
                owner = &item;
                best_gap = gap;
            }
        }
        if (owner == nullptr) {
            continue;
        }
        ++owner->remaining_match_count;
        owner->remaining_uses = remaining;
    }
    return true;
}

bool BlackFlowMovementTaskPlugin::run_fixed_task(std::string_view task)
{
    return ProcessTask(*this, { std::string(task) }).set_retry_times(0).run();
}

void BlackFlowMovementTaskPlugin::record_inventory_evidence(
    const InventoryFrame& frame,
    std::string_view outcome,
    std::string_view error) const
{
    if (m_session == nullptr) {
        return;
    }
    std::vector<json::value> items;
    items.reserve(frame.items.size());
    for (const InventoryItem& item : frame.items) {
        const MovementSpec* spec = find_movement_spec(item.movement);
        std::vector<json::value> star_slots;
        star_slots.reserve(item.star_slots.size());
        for (const MovementInventoryStarSlot& slot : item.star_slots) {
            star_slots.emplace_back(
                json::object {
                    { "lit", slot.lit },
                    { "bright_pixels", slot.bright_pixels },
                    { "rect",
                      json::object {
                          { "x", slot.rect.x },
                          { "y", slot.rect.y },
                          { "width", slot.rect.width },
                          { "height", slot.rect.height },
                      } },
                });
        }
        items.emplace_back(
            json::object {
                { "movement", std::string(to_string(item.movement)) },
                { "name", spec == nullptr ? std::string {} : std::string(spec->name) },
                { "scan_page", item.scan_page },
                { "instance_index", item.inventory_index },
                { "evidence_image_role", "inventory-page-" + std::to_string(item.scan_page + 1) },
                { "name_score", item.name_score },
                { "remaining_charges", item.remaining_uses },
                { "star_slots", json::array(std::move(star_slots)) },
                { "loaded", item.loaded },
                { "name_rect",
                  json::object {
                      { "x", item.name_rect.x },
                      { "y", item.name_rect.y },
                      { "width", item.name_rect.width },
                      { "height", item.name_rect.height },
                  } },
            });
    }
    std::vector<json::value> type_boundary_items;
    type_boundary_items.reserve(frame.type_boundary_items.size());
    for (const InventoryBoundaryItem& item : frame.type_boundary_items) {
        type_boundary_items.emplace_back(
            json::object {
                { "name", item.name },
                { "boundary_label", item.boundary_label },
                { "scan_page", item.scan_page },
                { "evidence_image_role", "inventory-page-" + std::to_string(item.scan_page + 1) },
                { "name_score", item.name_score },
                { "type_boundary", true },
                { "name_rect",
                  json::object {
                      { "x", item.name_rect.x },
                      { "y", item.name_rect.y },
                      { "width", item.name_rect.width },
                      { "height", item.name_rect.height },
                  } },
            });
    }
    const std::string scan_stop_reason = frame.type_boundary_items.empty()
                                             ? "已扫描到零件箱末尾"
                                             : "看到" + frame.type_boundary_items.front().boundary_label +
                                                   "，已到达加工品列表末尾";
    json::object evidence {
        { "evidence_type", "inventory_ocr" },
        { "outcome", std::string(outcome) },
        { "scan_complete", frame.scan_complete },
        { "stopped_at_ordered_boundary", frame.stopped_at_ordered_boundary },
        { "scan_stop_reason", scan_stop_reason },
        { "items", json::array(std::move(items)) },
        { "type_boundary_items", json::array(std::move(type_boundary_items)) },
    };
    if (!error.empty()) {
        evidence["error"] = std::string(error);
    }
    std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images;
    for (const auto& [scan_page, image] : frame.images) {
        if (image != nullptr && !image->empty()) {
            evidence_images.emplace_back(
                DiagnosticArtifactRequest::EvidenceImage {
                    "inventory-page-" + std::to_string(scan_page + 1),
                    image,
                });
        }
    }
    m_session->record_processing_item_evidence(std::move(evidence), std::move(evidence_images));
}

void BlackFlowMovementTaskPlugin::record_panel_evidence(
    MovementKind target,
    const std::vector<std::pair<int, PanelFrame>>& frames,
    int reset_swipes,
    int forward_swipes,
    std::string_view outcome,
    std::string_view error,
    bool scan_complete) const
{
    if (m_session == nullptr) {
        return;
    }
    std::vector<json::value> items;
    std::vector<json::value> ocr_name_hits;
    std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images;
    int frame_index = 0;
    for (const auto& [scan_page, frame] : frames) {
        const std::string image_role =
            "panel-page-" + std::to_string(scan_page + 1) + "-frame-" + std::to_string(++frame_index);
        if (frame.image != nullptr && !frame.image->empty()) {
            evidence_images.emplace_back(
                DiagnosticArtifactRequest::EvidenceImage {
                    image_role,
                    frame.image,
                });
        }
        for (const PanelItem& item : frame.items) {
            const MovementSpec* spec = find_movement_spec(item.movement);
            json::object serialized {
                { "scan_page", scan_page + 1 },
                { "evidence_image_role", image_role },
                { "movement", std::string(to_string(item.movement)) },
                { "name", spec == nullptr ? std::string {} : std::string(spec->name) },
                { "name_score", item.name_score },
                { "loaded", item.loaded },
                { "name_match_count", item.name_match_count },
                { "remaining_match_count", item.remaining_match_count },
                { "name_rect",
                  json::object {
                      { "x", item.name_rect.x },
                      { "y", item.name_rect.y },
                      { "width", item.name_rect.width },
                      { "height", item.name_rect.height },
                  } },
            };
            if (item.remaining_uses.has_value()) {
                serialized["remaining_charges"] = *item.remaining_uses;
            }
            items.emplace_back(std::move(serialized));
        }
        for (const PanelNameHit& hit : frame.name_hits) {
            const MovementSpec* spec = find_movement_spec(hit.movement);
            ocr_name_hits.emplace_back(
                json::object {
                    { "scan_page", scan_page + 1 },
                    { "evidence_image_role", image_role },
                    { "movement", std::string(to_string(hit.movement)) },
                    { "name", spec == nullptr ? std::string {} : std::string(spec->name) },
                    { "name_score", hit.name_score },
                    { "name_rect",
                      json::object {
                          { "x", hit.name_rect.x },
                          { "y", hit.name_rect.y },
                          { "width", hit.name_rect.width },
                          { "height", hit.name_rect.height },
                      } },
                });
        }
    }
    const MovementSpec* target_spec = find_movement_spec(target);
    json::object evidence {
        { "evidence_type", "movement_panel_ocr" },
        { "target", std::string(to_string(target)) },
        { "target_name", target_spec == nullptr ? std::string {} : std::string(target_spec->name) },
        { "outcome", std::string(outcome) },
        { "reset_swipes", reset_swipes },
        { "forward_swipes", forward_swipes },
        { "scan_complete", scan_complete },
        { "items", json::array(std::move(items)) },
        { "ocr_name_hits", json::array(std::move(ocr_name_hits)) },
    };
    if (!error.empty()) {
        evidence["error"] = std::string(error);
    }
    m_session->record_processing_item_evidence(std::move(evidence), std::move(evidence_images));
}

bool BlackFlowMovementTaskPlugin::report_target_observation(
    MovementKind target,
    const PanelItem& item,
    int completed_swipes,
    std::optional<MovementKind> active_movement,
    std::string* error)
{
    if (m_session == nullptr) {
        set_error(error, "movement panel observation has no active session");
        return false;
    }
    MovementPanelObservation panel;
    panel.target = target;
    panel.completed_swipes = completed_swipes;
    panel.target_found = true;
    // 徒步跋涉没有剩余次数，识别到的一律不采信，免得错位的标记把观测判成不一致。
    const bool countable = target != MovementKind::Walk;
    panel.unique_record = countable && item.name_match_count == 1 && item.remaining_match_count == 1;
    panel.remaining_charges = countable ? item.remaining_uses : std::nullopt;
    return m_session->apply_movement_panel_observation(std::move(panel), active_movement, error);
}

const MovementSpec* BlackFlowMovementTaskPlugin::movement_from_name(std::string_view name) noexcept
{
    const auto movement =
        std::find_if(movement_specs().begin(), movement_specs().end(), [name](const MovementSpec& spec) {
            return spec.name == name;
        });
    return movement == movement_specs().end() ? nullptr : &*movement;
}

std::optional<int> BlackFlowMovementTaskPlugin::remaining_uses_from_text(std::string_view text) noexcept
{
    if (text == "剩余1次") {
        return 1;
    }
    if (text == "剩余2次") {
        return 2;
    }
    if (text == "剩余3次") {
        return 3;
    }
    return std::nullopt;
}

std::vector<std::string> BlackFlowMovementTaskPlugin::ocr_candidates()
{
    std::vector<std::string> candidates;
    candidates.reserve(movement_specs().size() + 4);
    for (const MovementSpec& movement : movement_specs()) {
        candidates.emplace_back(movement.name);
    }
    candidates.emplace_back(LoadedText);
    candidates.emplace_back("剩余1次");
    candidates.emplace_back("剩余2次");
    candidates.emplace_back("剩余3次");
    return candidates;
}

std::vector<std::string> BlackFlowMovementTaskPlugin::inventory_ocr_candidates()
{
    std::vector<std::string> candidates;
    candidates.reserve(movement_specs().size() + 16);
    for (const MovementSpec& movement : movement_specs()) {
        if (movement.kind != MovementKind::Walk) {
            candidates.emplace_back(movement.name);
        }
    }
    for (const std::string_view priority_task : { InventoryNaturalPriorityTask, InventoryConceptPriorityTask }) {
        if (const auto task = Task.get<OcrTaskInfo>(std::string(priority_task)); task != nullptr) {
            candidates.insert(candidates.end(), task->text.begin(), task->text.end());
        }
    }
    candidates.emplace_back(InventoryEmptySlotText);
    candidates.emplace_back(LoadedText);
    return candidates;
}
} // namespace asst::blackflow
