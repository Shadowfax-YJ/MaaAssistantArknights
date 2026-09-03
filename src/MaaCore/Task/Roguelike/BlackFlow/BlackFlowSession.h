#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "BlackFlowNodeExecutionTypes.h"
#include "BlackFlowDeterministicPrediction.h"
#include "BlackFlowFailureRules.h"
#include "BlackFlowInventoryRefresh.h"
#include "Config/Roguelike/BlackFlow/BlackFlowStrategyConfig.h"

#include "BlackFlowTaskPort.h"

namespace asst::blackflow
{
enum class CultivatedAnimalType
{
    Cat,
    FeatheredSerpent,
    Dog,
    Cerberus,
};

[[nodiscard]] std::string_view to_string(CultivatedAnimalType type) noexcept;
[[nodiscard]] std::optional<CultivatedAnimalType> parse_cultivated_animal_type(std::string_view value) noexcept;
[[nodiscard]] std::optional<CultivatedAnimalType> cultivated_animal_type_from_name(std::string_view name) noexcept;

struct BlackFlowStrategyResult
{
    std::string profile;
    std::string outcome;
    std::string termination_reason;
    int cultivated_animals = 0;
    bool succeeded = false;
    std::string next_action = "stop_run";
    std::string cultivation_target;
    std::vector<std::string> cultivated_animal_types;

    [[nodiscard]] json::object to_json() const;
};

enum class PageExecutionStage
{
    None,
    PendingDispatch,
    Running,
    Resolved,
};

struct VerifiedMoveArc
{
    std::string action_id;
    NodeId source = InvalidNodeId;
    NodeId target = InvalidNodeId;
    NodeId landing = InvalidNodeId;
    MovementKind movement = MovementKind::Walk;
    int exact_action_point_cost = 0;
    int required_action_points_after = UnreachableActionPointRequirement;
    std::optional<std::size_t> proof_depth;
    std::uint64_t map_revision = 0;
    std::uint64_t cost_revision = 0;
    std::uint64_t resources_revision = 0;
    std::uint64_t viewport_revision = 0;
};

struct PendingMoveCandidate
{
    MoveCandidate candidate;
    std::uint64_t run_revision = 0;
    std::uint64_t map_revision = 0;
    std::uint64_t cost_revision = 0;
    std::uint64_t resources_revision = 0;
    std::uint64_t viewport_revision = 0;
};

struct PageExecutionContext
{
    std::uint64_t run_revision = 0;
    std::uint64_t page_revision = 0;
    std::string decision_id;
    std::string transaction_id;
    int floor = 0;
    NodeId node = InvalidNodeId;
    NodeType node_type = NodeType::Unknown;
    std::string node_name;
    std::string page_intent = "default";
    std::vector<std::string> entry_markers;
    std::vector<std::string> observed_contents;
    PageExecutionStage stage = PageExecutionStage::None;
    std::optional<NodeStateUpdate> result;
    std::optional<NodeBattleRecord> battle;
    bool changes_floor = false;
    bool resolution_reported = false;
    // 事件标题是实际节点身份的权威来源；小八界要等回图确定 node 后再回写。
    bool identity_from_event_name = false;
    // 安眠一隅没有地图落点，只记录事件和跨层效果，不写探索笔记或当前地图节点。
    bool has_landing = true;
    // 和平守卫者-2 / 独活-2 选择第一项后，事件结算与免费传送属于同一页面生命周期。
    // 这里保存规划时允许的关联目标；回图必须命中其中之一，不能把任意落点当成合法传送。
    bool linked_transfer_selected = false;
    std::optional<NodeType> linked_transfer_type;
    std::vector<NodeId> linked_transfer_targets;
};

struct NodeAttributionRecord
{
    int floor = 0;
    NodeId node = InvalidNodeId;
    // 无地图落点的追猎等抽象节点使用现有虚拟节点目录。
    std::string virtual_node_name;
    std::string attribution;
};

class BlackFlowSession
{
public:
    bool initialize(std::string profile, std::string* error = nullptr);
    void reset_run();
    bool configure_diagnostics(DiagnosticSettings settings, std::string* error = nullptr);

    bool update(const BlackFlowPerceptionSnapshot& snapshot, std::string* error = nullptr);
    [[nodiscard]] bool
        should_retry_initial_reveal_observation(const BlackFlowPerceptionSnapshot& snapshot) const;
    [[nodiscard]] bool
        should_retry_post_move_reveal_observation(const BlackFlowPerceptionSnapshot& snapshot) const;
    bool apply_movement_panel_observation(
        MovementPanelObservation panel,
        std::optional<MovementKind> active_movement,
        std::string* error = nullptr);
    bool apply_movement_inventory_observation(
        const std::vector<RunResources::MovementInstance>& visible_instances,
        std::string* error = nullptr);
    [[nodiscard]] std::optional<int> minimum_movement_instance_charges(MovementKind movement) const noexcept;
    void record_processing_item_evidence(
        json::object evidence,
        std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images = {});
    bool report_movement_unavailable(MovementKind target, std::string* error = nullptr);
    [[nodiscard]] BlackFlowPlan plan(std::string* error = nullptr);
    bool record_direct_exhaustion_decision(std::string* error = nullptr);
    bool save_pending_candidate(const MoveCandidate& candidate, std::string* error = nullptr);
    [[nodiscard]] bool validate_pending_candidate(std::string* error = nullptr) const;
    bool begin_pending_transaction(std::string* error = nullptr);

    void clear_pending_candidate() noexcept { m_pending_candidate.reset(); }

    bool begin_transaction(const MoveCandidate& candidate, std::string* error = nullptr);
    PreviewDisposition accept_preview(MovePreview preview, std::string* error = nullptr);
    [[nodiscard]] bool validate_commit(std::string* error = nullptr) const;
    bool commit(EnteredPageObservation entered_page = {}, std::string* error = nullptr);
    void cancel_transaction();
    bool set_current_floor(int floor, std::string* error = nullptr);

    void clear_current_floor() noexcept;

    [[nodiscard]] std::optional<int> current_floor() const noexcept { return m_current_floor; }
    void set_difficulty(int difficulty) noexcept { m_difficulty = difficulty; }
    [[nodiscard]] int difficulty() const noexcept { return m_difficulty; }
    [[nodiscard]] std::uint64_t map_generation() const noexcept { return m_map_generation; }
    [[nodiscard]] std::uint64_t run_revision() const noexcept { return m_run_revision; }
    [[nodiscard]] json::object run_log_state() const;

    [[nodiscard]] bool completed_page_changes_floor() const noexcept;

    [[nodiscard]] bool movement_inventory_refresh_required() const noexcept
    {
        return m_movement_inventory_refresh_required;
    }

    // 过载整理会改变加工品集合，而被拦截的那次移动并未发生。下一轮规划前必须重新读零件箱。
    void invalidate_movement_inventory() noexcept { m_movement_inventory_refresh_required = true; }

    // 零件箱从已经重建完成的地图上打开并关闭后，地图及横向视口都不变。记录当时的
    // 地图代次和 revision；紧随其后的规划轮次只有在两者仍一致时才可跳过重复建图。
    void mark_map_preserved_after_inventory() noexcept
    {
        const MapSnapshot& map = m_map.snapshot();
        if (m_run.floor > 0 && m_map.floor() == m_run.floor && map.revision > 0) {
            m_map_preserved_after_inventory = PreservedMapStamp { m_map_generation, map.revision };
        }
        else {
            m_map_preserved_after_inventory.reset();
        }
    }
    [[nodiscard]] bool consume_map_preserved_after_inventory() noexcept
    {
        const MapSnapshot& map = m_map.snapshot();
        const bool preserved = m_map_preserved_after_inventory.has_value() &&
                               m_map_preserved_after_inventory->map_generation == m_map_generation &&
                               m_map_preserved_after_inventory->map_revision == map.revision &&
                               m_run.floor > 0 && m_map.floor() == m_run.floor;
        m_map_preserved_after_inventory.reset();
        return preserved;
    }

    // 开局干员、分队、职业组整局不变，策略条件靠它们判断这一局能不能拿到结构性原理。
    // 必须在 initialize() 之前设置：事实是在 initialize() 末尾写入的，reset_run() 也会再写一次。
    void set_start_loadout(std::string core_char, std::string squad, std::string roles);
    // “选择支援”可能允许连续领取多项奖励；逐次回调必须累积，不能让后选资源覆盖
    // 先选的襁褓骏鹰，否则一、二层全图点亮规则会静默失效。
    void set_start_reward(std::string reward);
    [[nodiscard]] bool has_start_reward(std::string_view reward) const noexcept;

    // 事件选项在页面识别完成后询问当前会话：关联节点免费传送由两份假设规划比较，
    // 险路尽头则直接服从本次已选路线的终局/路过语义。
    [[nodiscard]] std::optional<std::size_t> preferred_encounter_choice(std::string_view event_name);

    // 需要按配置名称而非画面位置动态排序的事件走这个接口。“愈创之心”会先保留固定
    // 第一项，再按物理终点安全性与正常路线收益排列两个代价选项，并省略不安全的项。
    [[nodiscard]] std::optional<std::vector<std::string>>
        preferred_encounter_choice_order(std::string_view event_name);

    bool set_automation_collection_core_operator_elite_two(bool elite_two, std::string* error = nullptr);

    void set_cultivation_target(CultivatedAnimalType target) noexcept { m_cultivation_target = target; }

    void set_cultivated_animal_types(std::vector<CultivatedAnimalType> types);

    bool mark_page_running(std::string* error = nullptr);
    bool apply_node_task_result(
        const NodeTaskResult& result,
        const json::value& callback_details,
        std::string* error = nullptr);
    bool observe_page_content(std::string content, std::string source, std::string* error = nullptr);
    // 三层行动力耗尽触发的追猎没有地图节点事务，先留下这一拍的归属，待战斗插件
    // 回调已正则化的关卡名时仍可准确写回探索笔记中的险路恶敌。
    void mark_floor_three_pursuit_battle_pending() noexcept
    {
        m_floor_three_pursuit_battle_pending = true;
        m_collection_popup_pursuit_floor = m_current_floor.value_or(m_run.floor);
        m_collection_popup_pursuit_stage_name.clear();
        m_collection_popup_pursuit_total_kills.reset();
        if (*m_collection_popup_pursuit_floor <= 0) {
            m_collection_popup_pursuit_floor = 3;
        }
    }
    [[nodiscard]] std::optional<int> collection_popup_pursuit_floor() const noexcept
    {
        return m_collection_popup_pursuit_floor;
    }
    [[nodiscard]] const std::string& collection_popup_pursuit_stage_name() const noexcept
    {
        return m_collection_popup_pursuit_stage_name;
    }
    [[nodiscard]] std::optional<int> collection_popup_pursuit_total_kills() const noexcept
    {
        return m_collection_popup_pursuit_total_kills;
    }
    bool observe_battle_stage_name(std::string stage_name, std::string* error = nullptr);
    bool observe_battle_total_kills(
        std::string stage_name,
        int total_kills,
        std::string* error = nullptr);
    void record_virtual_auto_skill_activation(std::string_view device_name);
    [[nodiscard]] std::optional<NodeId> next_battle_intel_probe() const;
    bool record_battle_intel_probe(
        NodeId node,
        std::optional<std::string> stage_name,
        std::string observation_error = {},
        std::string* error = nullptr);

    void fail(std::string outcome, std::string reason, FailureDisposition disposition = FailureDisposition::StopTask);

    [[nodiscard]] const std::optional<BlackFlowStrategyResult>& result() const noexcept { return m_result; }

    [[nodiscard]] bool terminated() const noexcept { return m_result.has_value(); }

    bool claim_result_report() noexcept;

    [[nodiscard]] const ViewportObservation& viewport() const noexcept { return m_viewport; }

    [[nodiscard]] const RunState& run() const noexcept { return m_run; }

    // 当前观测地图是路线规划的唯一地图输入；节点身份、流窜“居民”标记等以最近一次截图为准。
    [[nodiscard]] const NormalizedMap& map() const noexcept { return m_map; }

    // 探索笔记按层累计已揭示的身份和具体内容，不参与路线规划。
    [[nodiscard]] const NormalizedMap& exploration_notebook() const noexcept { return m_exploration_notebook; }

    [[nodiscard]] FactStore facts() const { return m_facts.merged(); }

    [[nodiscard]] const std::string& profile() const noexcept { return m_profile; }

    [[nodiscard]] const std::optional<PageExecutionContext>& page_context() const noexcept { return m_page_context; }

    [[nodiscard]] bool page_dispatch_required() const noexcept { return m_page_context.has_value(); }

    [[nodiscard]] const std::optional<PendingMoveCandidate>& pending_candidate() const noexcept
    {
        return m_pending_candidate;
    }

    [[nodiscard]] std::vector<BlackFlowTelemetryEvent> take_telemetry_events();
    [[nodiscard]] std::vector<DiagnosticArtifactRequest> take_diagnostic_requests();
    [[nodiscard]] std::vector<NodeAttributionRecord> take_node_attribution_records();

    [[nodiscard]] MoveTransaction* transaction() noexcept
    {
        return m_transaction.has_value() ? &*m_transaction : nullptr;
    }

    [[nodiscard]] const MoveTransaction* transaction() const noexcept
    {
        return m_transaction.has_value() ? &*m_transaction : nullptr;
    }

private:
    [[nodiscard]] bool no_action_points_is_terminal() const;
    [[nodiscard]] BlackFlowPlan plan_internal(
        bool require_physical_endpoint,
        bool allow_endpoint_fallback,
        std::string* error);
    bool update_in_place(const BlackFlowPerceptionSnapshot& snapshot, std::string* error);
    bool synchronize_resource_facts(std::string* error);
    bool apply_granted_scraps(std::string* error);
    void refresh_mission();
    void publish_milestone_facts();
    void evaluate_milestone_miss_actions();
    [[nodiscard]] std::string resolve_page_intent(const PageIdentityResolution& identity, NodeId node, int floor) const;
    void evaluate_terminal_rules();
    bool apply_run_observation(const RunObservation& observation, std::string* error);
    bool merge_perception(
        const BlackFlowMapObservation& observation,
        const RunObservation& run,
        const FactStore& observed_facts,
        bool reconcile_move,
        std::string* error);
    bool reconcile_committed_move(const BlackFlowPerceptionSnapshot& snapshot, std::string* error);
    void finalize_entered_node(const PageExecutionContext& context, bool page_completed);
    void finalize_linked_encounter_landing(const LinkedEncounterReturnResolution& resolution);
    void append_map_visualization(json::object& details) const;
    void queue_map_summary(const PerceptionSummary& summary);
    void queue_warning(
        std::string code,
        std::string message,
        DiagnosticTrigger trigger,
        json::object evidence = {});
    void queue_decision(
        const MoveCandidate* proposal_without_transaction = nullptr,
        std::string failure_code = {},
        std::string failure_message = {});
    void queue_node_resolution(const PageExecutionContext& context);
    void request_diagnostics(
        DiagnosticTrigger trigger,
        json::object snapshot = {},
        std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images = {});
    bool apply_observed_facts(const FactStore& facts, std::string* error);
    bool set_fact(std::string_view name, FactValue value, std::string* error);
    bool apply_node_signal(const NodeStrategySignal& signal, const json::value& callback_details, std::string* error);

    std::string m_profile;
    std::string m_start_core_char;
    std::string m_start_squad;
    std::string m_start_roles;
    std::vector<std::string> m_start_rewards;
    CultivatedAnimalType m_cultivation_target = CultivatedAnimalType::Cat;
    std::vector<CultivatedAnimalType> m_cultivated_animal_types;
    std::optional<ResolvedPolicy> m_policy;
    FactContext m_facts;
    MissionState m_mission;
    BlackFlowObservationAdapter m_observation_adapter;
    NormalizedMap m_map;
    NormalizedMap m_exploration_notebook;
    ViewportObservation m_viewport;
    RunState m_run;
    std::optional<int> m_current_floor;
    int m_difficulty = 0;
    std::uint64_t m_map_generation = 0;
    std::uint64_t m_initial_prediction_generation = 0;
    std::optional<std::uint64_t> m_initial_reveal_checked_generation;
    bool m_current_map_is_floor_four_remembrance = false;
    ResidentSettlementPrediction m_resident_settlement_prediction;
    // clear_current_floor -> NextLevel -> set_current_floor 的边沿证据；成功合并新地图后立即消费。
    bool m_floor_recognition_pending = false;
    bool m_next_level_transition_confirmed = false;
    ResourceRegistry m_resources;
    std::unordered_set<std::string> m_unreachable_actions;
    // 移动面板只描述“此刻可选”，不能覆盖零件箱的持有事实。
    std::unordered_set<MovementKind> m_temporarily_unavailable_movements;
    std::unordered_set<NodeId> m_battle_intel_probed;
    bool m_floor_three_pursuit_battle_pending = false;
    // 关卡名识别后 pending 会被消费，但追猎战的掉落弹窗仍需一直归属于“追猎”抽象节点，
    // 直到 NextLevel 明确进入下一层。
    std::optional<int> m_collection_popup_pursuit_floor;
    std::string m_collection_popup_pursuit_stage_name;
    std::optional<int> m_collection_popup_pursuit_total_kills;
    std::optional<NodeId> m_pending_probe_target;
    std::optional<VerifiedMoveArc> m_verified_move_arc;
    std::optional<PendingMoveCandidate> m_pending_candidate;
    std::optional<MoveTransaction> m_transaction;
    std::optional<BlackFlowPlan> m_last_plan;
    std::optional<json::object> m_last_reveal_consistency;
    std::optional<PageExecutionContext> m_page_context;
    std::optional<BlackFlowStrategyResult> m_result;
    bool m_result_reported = false;
    bool m_movement_inventory_refresh_required = true;
    struct PreservedMapStamp
    {
        std::uint64_t map_generation = 0;
        std::uint64_t map_revision = 0;
    };
    std::optional<PreservedMapStamp> m_map_preserved_after_inventory;
    DiagnosticSettings m_diagnostics;
    std::size_t m_persisted_image_packages = 0;
    std::uint64_t m_run_revision = 0;
    std::uint64_t m_page_revision = 0;
    std::uint64_t m_decision_sequence = 0;
    std::uint64_t m_transaction_sequence = 0;
    std::uint64_t m_artifact_sequence = 0;
    NodeId m_observed_current_node = InvalidNodeId;
    std::string m_observation_id;
    std::string m_decision_id;
    std::string m_transaction_id;
    std::string m_topology_template_id;
    std::string m_topology_source_digest;
    int m_topology_base_edge_count = 0;
    int m_topology_extra_edge_count = 0;
    int m_topology_match_score = 0;
    std::string m_utopia_status;
    std::string m_utopia_reason;
    std::string m_utopia_ideology;
    std::string m_utopia_policy;
    std::optional<GridPosition> m_ideal_source;
    std::optional<std::uint64_t> m_ideal_source_generation;
    std::vector<GridPosition> m_ideal_domain;
    std::vector<GridPosition> m_observed_ideal_domain;
    bool m_utopia_effect_expired = false;
    double m_ideal_source_score_margin = 0.0;
    bool m_ideal_source_heads_agree = false;
    std::vector<BlackFlowTelemetryEvent> m_telemetry_events;
    std::vector<DiagnosticArtifactRequest> m_diagnostic_requests;
    std::vector<NodeAttributionRecord> m_node_attribution_records;
    std::vector<std::string> m_pending_move_node_attributions;
};
} // namespace asst::blackflow
