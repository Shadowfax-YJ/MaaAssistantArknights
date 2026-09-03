#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "BlackFlowTaskPluginBase.h"
#include "Common/AsstTypes.h"

namespace asst::blackflow
{
class BlackFlowMovementTaskPlugin final : public BlackFlowTaskPluginBase
{
public:
    using BlackFlowTaskPluginBase::BlackFlowTaskPluginBase;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual void reset_in_run_variables() override;

protected:
    virtual bool _run() override;

private:
    enum class PendingWork
    {
        None,
        SelectMovement,
        ObserveInventory,
        CleanupDirectDepartOverload,
    };

    enum class SelectionOutcome
    {
        Selected,
        Unavailable,
        Failed,
    };

    enum class InventoryAnalysisOutcome
    {
        Recognized,
        NoCandidate,
        Failed,
    };

    struct PanelItem
    {
        MovementKind movement = MovementKind::Walk;
        Rect name_rect;
        double name_score = 0.0;
        std::optional<int> remaining_uses;
        bool loaded = false;
        int name_match_count = 1;
        int remaining_match_count = 0;
    };

    struct PanelNameHit
    {
        MovementKind movement = MovementKind::Walk;
        Rect name_rect;
        double name_score = 0.0;
    };

    struct PanelFrame
    {
        std::vector<PanelItem> items;
        std::vector<PanelNameHit> name_hits;
        std::optional<MovementKind> loaded_movement;
        std::shared_ptr<cv::Mat> image;
    };

    struct InventoryItem
    {
        MovementKind movement = MovementKind::Walk;
        Rect name_rect;
        double name_score = 0.0;
        int remaining_uses = 0;
        std::vector<MovementInventoryStarSlot> star_slots;
        bool loaded = false;
        int scan_page = 0;
        int inventory_index = 0;
    };

    struct InventoryBoundaryItem
    {
        std::string name;
        std::string boundary_label;
        Rect name_rect;
        double name_score = 0.0;
        int scan_page = 0;
    };

    struct InventoryFrame
    {
        std::vector<RunResources::MovementInstance> movement_instances;
        std::optional<MovementKind> loaded_movement;
        std::vector<InventoryItem> items;
        std::vector<InventoryBoundaryItem> type_boundary_items;
        std::vector<std::pair<int, std::shared_ptr<cv::Mat>>> images;
        bool scan_complete = false;
        bool stopped_at_ordered_boundary = false;
    };

    bool observe_inventory();
    bool cleanup_direct_depart_overload(std::string_view source_task);
    bool scan_inventory_frame(InventoryFrame& frame, std::string* error);
    InventoryAnalysisOutcome
        analyze_inventory_frame(const cv::Mat& image, InventoryFrame& frame, int minimum_name_x, std::string* error) const;

    SelectionOutcome select_movement(MovementKind target, std::string* error);
    bool ensure_panel_open(std::string* error);
    bool close_panel(std::string* error);
    bool title_visible(const cv::Mat& image) const;
    bool scan_current_frame(PanelFrame& frame, std::string* error) const;
    bool analyze_frame(const cv::Mat& image, PanelFrame& frame, std::string* error) const;
    bool run_fixed_task(std::string_view task);
    void record_inventory_evidence(const InventoryFrame& frame, std::string_view outcome, std::string_view error) const;
    void record_panel_evidence(
        MovementKind target,
        const std::vector<std::pair<int, PanelFrame>>& frames,
        int reset_swipes,
        int forward_swipes,
        std::string_view outcome,
        std::string_view error,
        bool scan_complete = false) const;
    bool report_target_observation(
        MovementKind target,
        const PanelItem& item,
        int completed_swipes,
        std::optional<MovementKind> active_movement,
        std::string* error);

    static const MovementSpec* movement_from_name(std::string_view name) noexcept;
    static std::optional<int> remaining_uses_from_text(std::string_view text) noexcept;
    static std::vector<std::string> ocr_candidates();
    static std::vector<std::string> inventory_ocr_candidates();

    mutable PendingWork m_pending = PendingWork::None;
    mutable std::string m_direct_depart_source;
};
} // namespace asst::blackflow
