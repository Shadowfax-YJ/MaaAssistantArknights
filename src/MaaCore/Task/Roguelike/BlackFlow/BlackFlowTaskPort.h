#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BlackFlowObservation.h"
#include "BlackFlowPlanner.h"
#include "BlackFlowMovementRecognition.h"
#include "BlackFlowRunLog.h"
#include "BlackFlowTelemetry.h"

#include "Common/AsstMsg.h"

namespace cv
{
class Mat;
}

namespace asst
{
class Assistant;
}

namespace asst::blackflow
{
class BlackFlowSession;

struct MovementPanelObservation
{
    MovementKind target = MovementKind::Walk;
    int completed_swipes = 0;
    bool complete = false;
    bool target_found = false;
    bool unique_record = false;
    std::optional<int> remaining_charges;
};

[[nodiscard]] inline bool
    movement_panel_observation_is_structurally_valid(const MovementPanelObservation& observation) noexcept
{
    return observation.completed_swipes >= 0 && observation.completed_swipes <= MovementPanelMaximumSwipes &&
           (!observation.complete || observation.completed_swipes == MovementPanelMaximumSwipes) &&
           (!observation.remaining_charges.has_value() || *observation.remaining_charges >= 0) &&
           (observation.target_found || !observation.remaining_charges.has_value()) &&
           (observation.target != MovementKind::Walk || !observation.remaining_charges.has_value()) &&
           (!observation.unique_record || observation.remaining_charges.has_value());
}

[[nodiscard]] inline bool movement_panel_has_reliable_count(const MovementPanelObservation& observation) noexcept
{
    return observation.target != MovementKind::Walk && observation.target_found && observation.unique_record &&
           observation.remaining_charges.has_value();
}

[[nodiscard]] inline bool movement_panel_confirms_absent(const MovementPanelObservation& observation) noexcept
{
    return observation.complete && observation.completed_swipes == MovementPanelMaximumSwipes &&
           !observation.target_found;
}

struct RunObservation
{
    std::optional<int> action_points;
    std::optional<int> hope;
    std::optional<int> ingots;
    std::optional<int> seeds;
    std::optional<int> sellable_scraps;
    std::optional<int> white_model_birds;
    std::optional<bool> painted_liberi;
    std::optional<MovementKind> active_movement;
    std::optional<std::unordered_map<MovementKind, int>> movement_charges;
    std::optional<MovementPanelObservation> movement_panel;
    std::optional<std::unordered_set<MovementKind>> cross_floor_expired;
    std::optional<DynamicCostModel> costs;
};

// Moving through a winding passage is committed as a transfer-node move, so it never enters the
// ordinary node-page dispatcher.  Keep the decision at the move-confirmation seam where the
// transition animation is actually visible.
[[nodiscard]] inline bool
    move_confirmation_requires_door_animation_wait(const MoveTransaction& transaction) noexcept
{
    const MoveCandidate& move = transaction.proposal();
    if (!move.controllable) {
        return false;
    }
    if (move_landing_type(move, move.landing) == NodeType::Door) {
        return true;
    }
    return transaction.preview().has_value() && transaction.preview()->displayed_type == NodeType::Door;
}

[[nodiscard]] inline RunObservation make_map_hud_run_observation(
    std::optional<int> action_points,
    std::optional<int> ingots,
    std::optional<MovementKind> active_movement)
{
    RunObservation observation;
    observation.action_points = action_points;
    observation.ingots = ingots;
    observation.active_movement = active_movement;
    return observation;
}

struct BlackFlowPerceptionSnapshot
{
    BlackFlowMapObservation observation;
    RunObservation run;
    FactStore observed_facts;
};

struct UtopiaPanelObservation
{
    std::string ideology;
    std::string policy;

    [[nodiscard]] bool complete() const noexcept { return !ideology.empty() && !policy.empty(); }
};

enum class UtopiaPanelInspectionDisposition
{
    Present,
    Absent,
    Incomplete,
};

[[nodiscard]] inline UtopiaPanelInspectionDisposition
    classify_utopia_panel_inspection(const UtopiaPanelObservation& observation) noexcept
{
    if (observation.complete()) {
        return UtopiaPanelInspectionDisposition::Present;
    }
    if (observation.ideology.empty() && observation.policy.empty()) {
        return UtopiaPanelInspectionDisposition::Absent;
    }
    return UtopiaPanelInspectionDisposition::Incomplete;
}

struct BlackFlowObservationRequest
{
    int floor = 0;
    int difficulty = 0;
    std::uint64_t map_generation = 0;
    bool inspect_utopia = false;
    // 五层打开再关闭零件箱不会改变地图横向视口；紧随其后的地图重建可复用
    // 已完成的左滑定位。若本次检查实托邦时回到主菜单刷新了地图，则仍须重新定位。
    bool viewport_already_normalized = false;
    std::string utopia_ideology;
    std::string utopia_policy;
    int attempt_count = 1;
    std::int64_t capture_us = 0;
};

// 地图拓扑缓存只在同一张地图的连续观测间有效。追忆四层会在 NextLevel 后仍然
// OCR 为四层，因此不能用楼层号判断是否换图；Session 提供的 map_generation 才是
// 唯一可靠的换图边沿。
[[nodiscard]] constexpr bool topology_cache_requires_reset(
    const std::optional<std::uint64_t>& cached_generation,
    std::uint64_t requested_generation) noexcept
{
    return cached_generation.has_value() && *cached_generation != requested_generation;
}

[[nodiscard]] constexpr bool should_normalize_map_viewport(
    bool before_every_capture,
    bool viewport_already_normalized,
    bool map_refreshed) noexcept
{
    return before_every_capture && (!viewport_already_normalized || map_refreshed);
}

[[nodiscard]] inline bool move_confirmation_left_preview(std::string_view last_task) noexcept
{
    return !last_task.empty() && !last_task.ends_with("@MovePreviewConfirm");
}

[[nodiscard]] inline bool move_confirmation_failure_is_recoverable(std::string_view error) noexcept
{
    return error.starts_with("move confirmation did not leave the preview after its retry limit:");
}

// 预览页行动力消耗只有 0..-9。小 ROI 的负号和数字 1 容易粘连成
// 1-1、--1、I-1，甚至只剩一个 1；在这个封闭数值域内按末位数字恢复即可。
[[nodiscard]] inline std::optional<int> parse_move_preview_action_point_cost(std::string_view text) noexcept
{
    if (text == "0" || text == "-0") {
        return 0;
    }
    if (text.empty() || text.back() < '1' || text.back() > '9') {
        return std::nullopt;
    }
    for (const char ch : text.substr(0, text.size() - 1)) {
        if (ch != '-' && ch != 'I' && ch != 'l' && ch != '|' && ch != '1') {
            return std::nullopt;
        }
    }
    return -(text.back() - '0');
}

struct EnteredPageObservation
{
    std::vector<std::string> matched_texts;
    std::optional<NodeType> classified_type;
    // 只有事件标题 OCR 会设置；它既是节点具体名称，也是事件语义回写的可信来源。
    std::optional<std::string> event_name;
    bool classification_conflict = false;
    // 移动尚未发生：零件箱过载弹窗拦截了确认，需要先整理零件箱并重新规划。
    bool inventory_overloaded = false;
    bool inventory_cleanup_performed = false;
    // 快捷编队的干员列表/详情页不是节点身份；需要先退出到快捷编队主界面，
    // 再以主界面模板确认确实进入了战斗。
    bool combat_operator_selection_open = false;
    // 随机移动可能直接落到林间空地并回到地图；这与“节点页面 OCR 失败”必须区分。
    bool map_visible = false;
};

[[nodiscard]] EnteredPageObservation classify_entered_event_name(std::string event_name);

struct PageIdentityResolution
{
    NodeType type = NodeType::Unknown;
    std::string name;
};

[[nodiscard]] PageIdentityResolution resolve_page_identity(
    NodeType map_type,
    std::string map_name,
    const MovePreview* preview,
    const EnteredPageObservation& entered_page);

// 战斗情报探查只打开节点预览并读取关卡名，不创建移动事务，也不会确认移动。
struct BattleIntelPreview
{
    bool panel_open = false;
    bool target_verified = false;
    std::optional<NodeType> displayed_type;
    std::string displayed_name;
    std::optional<std::string> stage_name;
};

enum class BattlePreviewMapDisposition
{
    NotPending,
    Unchanged,
    Changed,
};

struct BattlePreviewMapCheck
{
    BattlePreviewMapDisposition disposition = BattlePreviewMapDisposition::NotPending;
    double mean_difference = -1.0;
};

enum class MovePreviewFrameState
{
    Missing,
    Reachable,
    Blocked,
};

inline constexpr int MovePreviewStabilityAttempts = 24;
inline constexpr int MovePreviewStabilityInterval = 150;
inline constexpr double MovePreviewMaximumMeanDifference = 3.0;
// 用 2026-09-02 两次完整 run 中 44 组战斗预览前后截图标定：地图区域同视口最大 3.192，
// 发生纵向平移最小 6.904。取两者间的 4.0，保留足够分类余量。
inline constexpr double BattlePreviewMapMaximumMeanDifference = 4.0;

[[nodiscard]] constexpr bool battle_preview_map_frame_is_unchanged(double mean_difference) noexcept
{
    return mean_difference >= 0.0 && mean_difference <= BattlePreviewMapMaximumMeanDifference;
}

[[nodiscard]] constexpr bool move_preview_frame_is_stable(
    MovePreviewFrameState previous,
    MovePreviewFrameState current,
    double mean_difference) noexcept
{
    return current != MovePreviewFrameState::Missing && current == previous && mean_difference >= 0.0 &&
           mean_difference <= MovePreviewMaximumMeanDifference;
}

class MovePreviewSemanticStability
{
public:
    void reset() noexcept { m_previous.reset(); }

    [[nodiscard]] bool observe(std::optional<std::string_view> signature)
    {
        if (!signature.has_value() || signature->empty()) {
            reset();
            return false;
        }
        const bool stable = m_previous.has_value() && *m_previous == *signature;
        m_previous = std::string(*signature);
        return stable;
    }

private:
    std::optional<std::string> m_previous;
};

[[nodiscard]] constexpr bool is_battle_intel_preview_type(NodeType type) noexcept
{
    return type == NodeType::BattleNormal || type == NodeType::BattleElite;
}

// 流窜“居民”会在节点结算后移动。若它正压在刚点亮的作战节点上，此刻打开预览
// 会先进入居民战斗而不是安全读取关卡名。候选每次地图重建后重新计算：标记仍在
// 就继续延后，某次重建确认它移走后自然恢复探查。
[[nodiscard]] inline bool battle_intel_probe_blocked_by_resident(const Node& node) noexcept
{
    return node_has_roaming_resident_marker(node);
}

[[nodiscard]] EnteredPageObservation classify_entered_page_texts(std::vector<std::string> matched_texts);

class IBlackFlowMapObservationSource
{
public:
    virtual ~IBlackFlowMapObservationSource() = default;

    virtual bool recognize(
        const cv::Mat& image,
        const BlackFlowObservationRequest& request,
        BlackFlowMapObservation& observation,
        FactStore& observed_facts,
        std::string* error) = 0;

    virtual void reset_run() {}

    virtual void configure_diagnostics(const DiagnosticSettings&) {}

    virtual bool persist_diagnostics(const DiagnosticArtifactRequest& request, std::string* error) = 0;
    virtual bool record_run_event(
        std::uint64_t run_revision,
        const RunLogEvent& event,
        const cv::Mat* image,
        std::string* error) = 0;
    virtual bool record_node_attribution(
        std::uint64_t run_revision,
        const std::filesystem::path& relative_directory,
        std::string_view attribution,
        std::string* error) = 0;
};

class IBlackFlowTaskPort
{
public:
    virtual ~IBlackFlowTaskPort() = default;

    virtual bool refresh(
        const BlackFlowObservationRequest& request,
        BlackFlowPerceptionSnapshot& snapshot,
        std::string* error) = 0;
    virtual bool preview(
        const MoveCandidate& candidate,
        const ViewportObservation& viewport,
        MovePreview& preview,
        bool& panel_open,
        std::string* error) = 0;
    virtual bool inspect_battle(
        NodeId node,
        const ViewportObservation& viewport,
        BattleIntelPreview& intel,
        std::string* error) = 0;
    // 战斗预览关闭后只比较地图区域。视口没动时沿用 Session 中的拓扑和点击坐标；
    // 发生平移时把已稳定的新帧交给 refresh()，避免为判脏再额外截一次。
    virtual bool check_battle_preview_map(BattlePreviewMapCheck& check, std::string*)
    {
        check = {};
        return true;
    }
    virtual bool
        confirm(const MoveTransaction& transaction, EnteredPageObservation& entered_page, std::string* error) = 0;
    virtual bool cleanup_open_inventory_if_overloaded(bool& cleanup_performed, std::string* error) = 0;

    virtual void reset_run() {}

    virtual void configure_diagnostics(const DiagnosticSettings&) {}

    virtual void set_collection_popup_session(std::weak_ptr<BlackFlowSession>) {}
    virtual bool capture_collection_popup(std::string_view, std::string* = nullptr) { return true; }
    virtual bool capture_event_page(std::string_view, const cv::Mat&, std::string* = nullptr) { return true; }
    virtual bool capture_get_drop(std::string_view, std::optional<Rect> = std::nullopt, std::string* = nullptr)
    {
        return true;
    }
    virtual bool capture_store_page(std::string_view, std::string_view, int, std::string* = nullptr) { return true; }
    virtual bool record_store_purchase(
        std::string_view,
        std::string_view,
        std::optional<int>,
        std::optional<int>,
        bool,
        std::string* = nullptr)
    {
        return true;
    }
    virtual bool resolve_pending_collection_popups(std::string* = nullptr) { return true; }
    virtual bool flush_pending_collection_popups(std::string* = nullptr) { return true; }

    virtual bool persist_diagnostics(const DiagnosticArtifactRequest&, std::string*) { return false; }
    virtual bool record_run_event(
        std::uint64_t,
        const RunLogEvent&,
        std::shared_ptr<cv::Mat> = nullptr,
        bool = false,
        std::string* = nullptr)
    {
        return false;
    }
    virtual bool record_node_attribution(
        std::uint64_t,
        int,
        NodeId,
        std::string_view,
        std::string_view,
        std::string* = nullptr)
    {
        return false;
    }
};

class BlackFlowTaskPort final : public IBlackFlowTaskPort
{
public:
    BlackFlowTaskPort(
        const AsstCallback& callback,
        Assistant* inst,
        std::string_view task_chain,
        std::shared_ptr<IBlackFlowMapObservationSource> map_source);
    ~BlackFlowTaskPort() override;

    bool refresh(const BlackFlowObservationRequest& request, BlackFlowPerceptionSnapshot& snapshot, std::string* error)
        override;
    bool preview(
        const MoveCandidate& candidate,
        const ViewportObservation& viewport,
        MovePreview& preview,
        bool& panel_open,
        std::string* error) override;
    bool inspect_battle(
        NodeId node,
        const ViewportObservation& viewport,
        BattleIntelPreview& intel,
        std::string* error) override;
    bool check_battle_preview_map(BattlePreviewMapCheck& check, std::string* error) override;
    bool confirm(const MoveTransaction& transaction, EnteredPageObservation& entered_page, std::string* error) override;
    bool cleanup_open_inventory_if_overloaded(bool& cleanup_performed, std::string* error) override;

    void reset_run() override;
    void configure_diagnostics(const DiagnosticSettings& settings) override;
    void set_collection_popup_session(std::weak_ptr<BlackFlowSession> session) override;
    bool capture_collection_popup(std::string_view task, std::string* error = nullptr) override;
    bool capture_event_page(
        std::string_view event_name,
        const cv::Mat& stitched_image,
        std::string* error = nullptr) override;
    bool capture_get_drop(
        std::string_view task,
        std::optional<Rect> selected_button = std::nullopt,
        std::string* error = nullptr) override;
    bool capture_store_page(
        std::string_view store_kind,
        std::string_view capture_phase,
        int refresh_index,
        std::string* error = nullptr) override;
    bool record_store_purchase(
        std::string_view store_kind,
        std::string_view item_name,
        std::optional<int> ingots_before,
        std::optional<int> ingots_after,
        bool collectible,
        std::string* error = nullptr) override;
    bool resolve_pending_collection_popups(std::string* error = nullptr) override;
    bool flush_pending_collection_popups(std::string* error = nullptr) override;
    bool persist_diagnostics(const DiagnosticArtifactRequest& request, std::string* error) override;
    bool record_run_event(
        std::uint64_t run_revision,
        const RunLogEvent& event,
        std::shared_ptr<cv::Mat> image = nullptr,
        bool capture_image = false,
        std::string* error = nullptr) override;
    bool record_node_attribution(
        std::uint64_t run_revision,
        int floor,
        NodeId node,
        std::string_view virtual_node_name,
        std::string_view attribution,
        std::string* error = nullptr) override;

private:
    class ProcessTaskContext;
    class CollectionPopupCaptureState;

    [[nodiscard]] std::optional<int> recognize_action_points(const cv::Mat& image) const;
    [[nodiscard]] UtopiaPanelObservation recognize_utopia_panel(const cv::Mat& image) const;
    bool inspect_utopia_for_generation(
        std::uint64_t map_generation,
        UtopiaPanelObservation& observation,
        cv::Mat& stable_map_image,
        bool& map_refreshed,
        std::string* error);
    bool cleanup_overloaded_inventory(bool inventory_already_open, std::string* error);
    bool classify_entered_page(const cv::Mat& image, EnteredPageObservation& observation, std::string* error) const;
    bool persist_collection_popup_capture(
        const std::string& task,
        std::string_view button,
        const cv::Mat& image,
        int sampled_frames,
        double mean_difference,
        const std::filesystem::path& relative_directory,
        json::object attribution,
        bool deferred,
        std::string* error);
    bool persist_node_evidence_capture(
        std::string_view action,
        std::string task,
        std::string phase,
        const cv::Mat& image,
        const std::filesystem::path& relative_directory,
        json::object attribution,
        json::object details,
        std::string* error);

    std::unique_ptr<ProcessTaskContext> m_task_context;
    std::unique_ptr<CollectionPopupCaptureState> m_collection_popup_state;
    std::shared_ptr<IBlackFlowMapObservationSource> m_map_source;
    std::weak_ptr<BlackFlowSession> m_collection_popup_session;
    std::optional<std::uint64_t> m_utopia_generation;
    UtopiaPanelObservation m_utopia_observation;
    std::unique_ptr<cv::Mat> m_last_stable_map_image;
    std::unique_ptr<cv::Mat> m_battle_preview_map_reference;
    std::unique_ptr<cv::Mat> m_pending_stable_map_image;
};

enum class PreviewDisposition
{
    ReadyToCommit,
    ReplanAfterDismiss,
    Failed,
};
} // namespace asst::blackflow
