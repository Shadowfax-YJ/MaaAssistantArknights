#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/AsstTypes.h"

namespace asst::blackflow
{
using NodeId = std::uint64_t;

inline constexpr NodeId InvalidNodeId = std::numeric_limits<NodeId>::max();

struct GridPosition
{
    int row = 0;
    int column = 0;

    bool operator==(const GridPosition&) const noexcept = default;
    auto operator<=>(const GridPosition&) const noexcept = default;
};

struct GridPositionHash
{
    std::size_t operator()(const GridPosition& position) const noexcept;
};

[[nodiscard]] std::optional<NodeId> make_stable_node_id(int floor, GridPosition position) noexcept;

enum class NodeType
{
    Unknown,
    BattleElite,
    BattleNormal,
    BattleSavage,
    Duel,
    Door,
    Employ,
    Expedition,
    HideBattle,
    HideInvisible,
    Incident,
    Light,
    Portal,
    Rest,
    Sacrifice,
    ScrapShop,
    Shop,
    Wish,
    Empty,
    Evacuate,
    Final,
    BattleBoss,
};

struct PageContentEffect
{
    std::optional<NodeType> resolved_type;
    bool changes_floor = false;
    // 安眠一隅是没有地图落点的隐藏事件；它只负责推进楼层，不能把事件身份写到激活位置。
    bool has_landing = true;
};

// 隐藏节点只有进入页面后才能从具体事件名恢复真实语义。
// “三重身”对应险路小径；“安眠一隅”没有地图落点，但结算后同样会直接进入下一层。
[[nodiscard]] PageContentEffect
    classify_page_content_effect(std::string_view source, std::string_view content) noexcept;

[[nodiscard]] constexpr bool is_route_battle_node_type(NodeType type) noexcept
{
    // 狭路相逢使用事件页执行，计两个有效节点，但不属于路线避战或页面执行语义中的作战节点。
    return type == NodeType::BattleNormal || type == NodeType::BattleElite || type == NodeType::BattleSavage ||
           type == NodeType::HideBattle || type == NodeType::BattleBoss;
}

enum class NodeProgress
{
    Active,
    Completed,
    Removed,
};

enum class NodeIdentityState
{
    Classified,
    Hidden,
    Unclassified,
};

enum class EdgeKnowledge
{
    Unknown,
    Confirmed,
    Absent,
};

enum class GraphLayer
{
    Confirmed,
    Relaxed,
};

struct NodeTraversal
{
    bool blocks_walk = true;
    bool blocks_vision = true;
    bool repeatable = false;
    bool enterable = true;

    bool operator==(const NodeTraversal&) const noexcept = default;
};

struct NodeBattleRecord
{
    std::string stage_name;
    std::optional<int> total_kills;

    bool operator==(const NodeBattleRecord&) const noexcept = default;
};

struct Node
{
    NodeId id = InvalidNodeId;
    int floor = 0;
    GridPosition position;
    NodeType type = NodeType::Unknown;
    std::string name;
    // 同一节点页面最多发生一次战斗。事件内嵌战斗只附加到事件节点，
    // 不覆盖事件节点本身的类型和名称。
    std::optional<NodeBattleRecord> battle;
    // 地图节点原始类别是“命运所指”。具体事件名会被事件插件回调覆盖到 name，
    // 因而必须独立保留该类别，不能靠事件名白名单反推。
    bool fate_event = false;
    NodeProgress progress = NodeProgress::Active;
    NodeTraversal traversal;
    NodeIdentityState identity_state = NodeIdentityState::Unclassified;
    bool identity_revealed = false;
    bool visually_hidden = false;
    bool identity_from_topology = false;
    // 由实托邦中心或初始流窜“居民”等确定性规则直接给出的原始身份。
    // 它与模板身份一样不计入探明收益，但必须允许后续现场观测覆盖并校验冲突。
    bool identity_from_prediction = false;
    std::string prediction_rule;
    // 该节点位于实托邦·理念「弥散虚雾」的理想域内，不能作为连线视野中的
    // 待揭示节点；直接进入仍会揭示节点本身。理想域外的连线节点不受影响，
    // 已揭示的透明域内节点仍可继续传递视野。
    bool natural_reveal_suppressed = false;
    std::string existence_source;
    std::string identity_source;
    bool detected_by_vision = false;
    bool confirmed_by_topology = false;
    std::string marker_type;
    std::string marker_display_name;
    double marker_score = 0.0;
    // 视觉上仍保留藏果地/线人等真实标记；当可见流窜“居民”不足三个时，
    // 该标记也可能与一个居民标记重合。路线避让按可能居民处理，确定性据点
    // 推断则必须同时枚举“重合/不重合”两种解释。
    bool marker_resident_overlap_possible = false;
    bool badged = false;
    std::optional<NodeId> transfer_target;

    bool operator==(const Node&) const noexcept = default;
};

// “savage”是流窜“居民”标记模板的内部名。“居民”据点是 BattleSavage 节点类型，
// 由小八界的非战斗类型白名单单独排除，不能混入标记判定。小八界的随机落点池只
// 额外排除已经明确识别出的 savage 标记；可能重叠但未确认的藏果地/线人节点仍保留。
[[nodiscard]] inline bool node_has_explicit_roaming_resident_marker(const Node& node) noexcept
{
    return node.marker_type == "savage";
}

// 常规路线规划采用相反的保守规则：藏果地/线人图标在居民数不足时还可能与居民
// 重叠，因此潜在居民也要避开。
[[nodiscard]] inline bool node_has_roaming_resident_marker(const Node& node) noexcept
{
    return node_has_explicit_roaming_resident_marker(node) || node.marker_resident_overlap_possible;
}

// 普通/紧急作战与险路恶敌在地图上只能看到泛型节点名；预览页或战斗插件识别出的
// 具体关卡名由该字段语义导出，供探索笔记与诊断输出使用。泛型节点名不算关卡名。
[[nodiscard]] bool is_generic_battle_name(NodeType type, std::string_view name) noexcept;
[[nodiscard]] std::string_view battle_stage_name(const Node& node) noexcept;

inline constexpr std::string_view EmptyNodeName = "林间空地";

struct EdgeEvidence
{
    double probability = 0.0;
    bool cnn_connected = false;
    bool forced_by_connectivity_constraint = false;
    std::string decision_source;
};

struct Edge
{
    NodeId first = InvalidNodeId;
    NodeId second = InvalidNodeId;
    EdgeKnowledge knowledge = EdgeKnowledge::Unknown;
    EdgeEvidence evidence;
};

class MapSnapshot
{
public:
    bool upsert_node(Node node);
    bool remove_node(NodeId id);
    bool upsert_edge(Edge edge);

    [[nodiscard]] const Node* find_node(NodeId id) const noexcept;
    [[nodiscard]] const Node* find_node(int floor, GridPosition position) const noexcept;
    [[nodiscard]] EdgeKnowledge edge_knowledge(NodeId first, NodeId second) const noexcept;
    [[nodiscard]] const Edge* find_edge(NodeId first, NodeId second) const noexcept;
    [[nodiscard]] std::vector<NodeId> neighbors(NodeId id, GraphLayer layer = GraphLayer::Confirmed) const;
    [[nodiscard]] std::unordered_set<NodeId> reveal_through_transparent_nodes(NodeId origin) const;
    [[nodiscard]] std::unordered_set<NodeId> nodes_within_manhattan(NodeId origin, int distance) const;
    [[nodiscard]] bool has_valid_transfer_pair(NodeId node) const noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;

    [[nodiscard]] const auto& nodes() const noexcept { return m_nodes; }

    [[nodiscard]] const auto& edges() const noexcept { return m_edges; }

    std::uint64_t revision = 0;

private:
    std::unordered_map<NodeId, Node> m_nodes;
    std::vector<Edge> m_edges;
};

struct LinkedEncounterReturnResolution
{
    NodeId event_node = InvalidNodeId;
    NodeId linked_node = InvalidNodeId;
    NodeType linked_type = NodeType::Unknown;
};

// 关联事件的免费传送会同时结算两个位置：原不期而遇变为空地，角色落到据点/应急助力。
// 普通移动直接使用进入页面前已知的 event_node；小八界则只能在回图后从唯一的
// “活动节点 -> 空地”变化反推原事件格。任何歧义都拒绝授权传送事务。
[[nodiscard]] std::optional<LinkedEncounterReturnResolution> resolve_linked_encounter_return(
    const MapSnapshot& before,
    const MapSnapshot& after,
    NodeId known_event_node,
    NodeId current_node,
    NodeType linked_type,
    const std::vector<NodeId>& target_hypotheses) noexcept;

enum class ObservationCoverage
{
    PartialViewport,
    FullMap,
};

struct ObservedNode
{
    GridPosition position;
    std::optional<NodeType> type;
    std::optional<std::string> name;
    std::optional<bool> fate_event;
    std::optional<NodeProgress> progress;
    std::optional<NodeTraversal> traversal;
    std::optional<NodeIdentityState> identity_state;
    std::optional<bool> identity_revealed;
    std::optional<bool> visually_hidden;
    std::optional<bool> identity_from_topology;
    std::optional<bool> identity_from_prediction;
    std::optional<std::string> prediction_rule;
    std::optional<bool> natural_reveal_suppressed;
    std::optional<std::string> existence_source;
    std::optional<std::string> identity_source;
    std::optional<bool> detected_by_vision;
    std::optional<bool> confirmed_by_topology;
    std::optional<std::string> marker_type;
    std::optional<std::string> marker_display_name;
    std::optional<double> marker_score;
    std::optional<bool> marker_resident_overlap_possible;
    std::optional<bool> badged;
    std::optional<std::optional<GridPosition>> transfer_target;
};

struct ObservedEdge
{
    GridPosition first;
    GridPosition second;
    EdgeKnowledge knowledge = EdgeKnowledge::Unknown;
    EdgeEvidence evidence;
};

struct MapObservationBatch
{
    int floor = 0;
    ObservationCoverage coverage = ObservationCoverage::PartialViewport;
    std::vector<GridPosition> covered_positions;
    std::vector<ObservedNode> nodes;
    std::vector<ObservedEdge> edges;
};

enum class MapMergePurpose
{
    // 路线规划使用的当前观测：本次截图看到空地，就必须覆盖上一帧的节点身份。
    CurrentObservation,
    // 本层探索笔记：保留已经揭示过的节点身份和内容，后续空地观测只更新存在性证据。
    ExplorationNotebook,
};

class NormalizedMap
{
public:
    [[nodiscard]] bool merge(const MapObservationBatch& batch, std::string* error = nullptr);
    [[nodiscard]] bool
        merge(const MapObservationBatch& batch, MapMergePurpose purpose, std::string* error = nullptr);
    void reset();

    [[nodiscard]] const MapSnapshot& snapshot() const noexcept { return m_snapshot; }

    [[nodiscard]] MapSnapshot& snapshot() noexcept { return m_snapshot; }

    [[nodiscard]] int floor() const noexcept { return m_floor; }

private:
    int m_floor = 0;
    MapSnapshot m_snapshot;
};

struct NodeObservation
{
    NodeId node = InvalidNodeId;
    Rect icon_rect;
    std::optional<Rect> text_rect;
    double icon_confidence = 0.0;
    double text_confidence = 0.0;
};

class ViewportObservation
{
public:
    void
        replace(std::vector<NodeObservation> observations, std::uint64_t map_revision, std::uint64_t viewport_revision);
    void clear(std::uint64_t map_revision, std::uint64_t viewport_revision);
    [[nodiscard]] const NodeObservation* find(NodeId node) const noexcept;
    [[nodiscard]] std::optional<Rect> clickable_rect(
        NodeId node,
        std::uint64_t expected_map_revision,
        std::uint64_t expected_viewport_revision) const;
    // 页面结算只改变节点语义、不改变这帧截图中的图标位置时，将现有坐标重新绑定到新地图修订。
    [[nodiscard]] bool rebind_map_revision_after_semantic_update(
        std::uint64_t expected_previous_revision,
        std::uint64_t updated_revision) noexcept;

    [[nodiscard]] const auto& nodes() const noexcept { return m_nodes; }

    [[nodiscard]] std::uint64_t map_revision() const noexcept { return m_map_revision; }

    [[nodiscard]] std::uint64_t viewport_revision() const noexcept { return m_viewport_revision; }

private:
    std::unordered_map<NodeId, NodeObservation> m_nodes;
    std::uint64_t m_map_revision = 0;
    std::uint64_t m_viewport_revision = 0;
};

[[nodiscard]] NodeTraversal default_traversal_for(NodeType type) noexcept;
[[nodiscard]] bool completed_node_becomes_empty(
    bool repeatable,
    std::optional<bool> explicit_becomes_empty = std::nullopt) noexcept;
[[nodiscard]] bool is_transfer_node(NodeType type) noexcept;
[[nodiscard]] bool is_combat_node_type(NodeType type) noexcept;
[[nodiscard]] bool is_exit_node_type(NodeType type) noexcept;
[[nodiscard]] std::optional<NodeType> node_type_from_string(std::string_view value) noexcept;

enum class MovementKind
{
    Walk,
    M01,
    M02,
    M03,
    M04,
    M05,
    M06,
    M07,
    M08,
    M09,
    M10,
    M11,
    M12,
};

inline constexpr std::size_t ProcessingMovementSlotCount = static_cast<std::size_t>(MovementKind::M12) + 1;

// 只描述加工品的移动/资源能力，由强到弱；路线收益和总残值打平后才逐项使用这一顺序。
inline constexpr std::array<MovementKind, 12> ProcessingMovementStrengthOrder = {
    MovementKind::M08,
    MovementKind::M11,
    MovementKind::M12,
    MovementKind::M09,
    MovementKind::M07,
    MovementKind::M10,
    MovementKind::M06,
    MovementKind::M04,
    MovementKind::M05,
    MovementKind::M03,
    MovementKind::M02,
    MovementKind::M01,
};

enum class MovementRange
{
    WalkEdges,
    OrthogonalTwo,
    SurroundingEight,
    ManhattanTwo,
    OrthogonalThree,
    FullMap,
};

struct MovementEffect
{
    int action_point_gain = 0;
    int hope_gain = 0;
    int ingot_gain = 0;
};

struct MovementSpec
{
    MovementKind kind = MovementKind::Walk;
    std::string_view id;
    std::string_view name;
    MovementRange range = MovementRange::WalkEdges;
    std::vector<NodeType> target_types;
    int action_point_cost = 1;
    int initial_charges = 0;
    bool random_target = false;
    bool expires_on_floor_end = false;
    MovementEffect effect;
};

[[nodiscard]] bool node_type_allowed(const MovementSpec& movement, NodeType type) noexcept;

// 坎诺特的触须只能点击当前画面已经显形的商店。模板固定身份虽然可供规划认知，
// 但节点仍处于隐藏外观时，游戏不会把它作为“行商节点”高亮。
[[nodiscard]] constexpr bool movement_target_is_currently_selectable(
    MovementKind movement,
    bool visually_hidden) noexcept
{
    return movement != MovementKind::M10 || !visually_hidden;
}

// 规划可以保留由拓扑/确定性规则推断出的真实身份，但加工品的可选目标和随机池必须按
// 游戏当前画面上的身份计算。尚未显形的作战类显示为“未知的凶戾”，其余节点（包括
// 尚未探明的险路尽头）显示为“未知的诡秘”。
[[nodiscard]] constexpr NodeType movement_visible_node_type(NodeType semantic_type, bool visually_hidden) noexcept
{
    if (!visually_hidden) {
        return semantic_type;
    }
    return is_route_battle_node_type(semantic_type) ? NodeType::HideBattle : NodeType::HideInvisible;
}

struct DynamicCostModel
{
    int walk_cost_per_edge = 1;
    std::unordered_map<MovementKind, int> movement_cost_overrides;
    std::unordered_map<std::string, int> action_cost_overrides;
    std::uint64_t revision = 0;

    [[nodiscard]] int movement_cost(const MovementSpec& movement, std::size_t walked_edges = 0) const noexcept;
    [[nodiscard]] int action_cost(std::string_view action_id, int fallback) const noexcept;
    bool clear_action_cost_overrides() noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] int action_points_after(int current, int cost, int gain) noexcept;

[[nodiscard]] const std::vector<MovementSpec>& movement_specs();
[[nodiscard]] const MovementSpec* find_movement_spec(MovementKind kind) noexcept;
[[nodiscard]] bool is_in_geometric_range(
    const MapSnapshot& map,
    NodeId source,
    NodeId target,
    const MovementSpec& movement,
    GraphLayer layer = GraphLayer::Confirmed);
[[nodiscard]] std::vector<NodeId> enumerate_geometric_targets(
    const MapSnapshot& map,
    NodeId source,
    const MovementSpec& movement,
    GraphLayer layer = GraphLayer::Confirmed);

struct RunResources
{
    int action_points = 0;
    int hope = 0;
    int ingots = 0;
    int seeds = 0;
    int sellable_scraps = 0;
    int white_model_birds = 0;
    bool painted_liberi = false;
    // 零件箱观测到的加工品实例。它是持有件数与剩余次数的事实源；下面两张按种类
    // 聚合的表只供规划器使用，不能反过来覆盖实例。
    struct MovementInstance
    {
        MovementKind movement = MovementKind::Walk;
        int remaining_charges = 0;
        int inventory_index = 0;
        bool loaded = false;

        bool operator==(const MovementInstance&) const noexcept = default;
    };
    std::vector<MovementInstance> movement_instances;
    std::unordered_map<MovementKind, int> movement_charges;
    std::unordered_map<MovementKind, int> movement_pieces;

    bool operator==(const RunResources&) const noexcept = default;
};

inline void rebuild_movement_aggregates(RunResources& resources)
{
    resources.movement_charges.clear();
    resources.movement_pieces.clear();
    for (const RunResources::MovementInstance& instance : resources.movement_instances) {
        if (instance.movement == MovementKind::Walk || instance.remaining_charges < 0) {
            continue;
        }
        ++resources.movement_pieces[instance.movement];
        if (instance.remaining_charges > 0) {
            resources.movement_charges[instance.movement] += instance.remaining_charges;
        }
    }
}

struct RunState
{
    int floor = 0;
    NodeId current_node = InvalidNodeId;
    RunResources resources;
    std::optional<MovementKind> active_movement;
    DynamicCostModel costs;
    std::uint64_t resources_revision = 0;
    std::unordered_set<NodeId> visited_nodes;
    std::unordered_set<NodeId> consumed_one_time_nodes;
    std::unordered_set<NodeId> revealed_nodes;
    std::unordered_set<MovementKind> cross_floor_expired;
    std::unordered_map<NodeId, NodeProgress> node_progress;
    bool strategy_terminal = false;
};

struct MoveCandidate
{
    std::string action_id;
    MovementKind movement = MovementKind::Walk;
    NodeId source = InvalidNodeId;
    NodeId target = InvalidNodeId;
    NodeId landing = InvalidNodeId;
    std::vector<NodeId> path;
    std::vector<NodeId> possible_landings;
    // 不可控移动必须逐落点保留真实节点语义。target 只是用来打开移动预览的激活格，
    // 不能代替实际落点；尤其小八界随机落到已探明或未探明的险路尽头时，页面处理和
    // 搜索都需要知道该落点是 final。
    std::unordered_map<NodeId, NodeType> landing_node_types;
    std::unordered_map<NodeId, int> landing_action_point_gains;
    int predicted_action_point_cost = 0;
    int predicted_action_point_gain = 0;
    int action_point_requirement = std::numeric_limits<int>::max() / 4;
    bool controllable = true;
    // 不选择地图节点，直接在移动方式面板耗尽剩余行动力并进入追猎。
    bool direct_exhaustion = false;
    bool terminal_on_completion = false;
    // 险路尽头的第三项只把本次移动作为路过：返回同层地图、节点仍可再次进入，
    // 且不满足本层终点。它与真正进入下一层的物理移动目标相同，只是页面决策不同。
    bool bypass_final_on_completion = false;
    bool requires_preview_verification = false;
    GraphLayer graph_layer = GraphLayer::Confirmed;
    bool uses_unconfirmed_edge = false;
    bool uses_inferred_edge = false;
    std::optional<NodeId> first_unclassified;
};

[[nodiscard]] NodeType move_landing_type(const MoveCandidate& candidate, NodeId landing) noexcept;
[[nodiscard]] bool move_landing_is_terminal(const MoveCandidate& candidate, NodeId landing) noexcept;

[[nodiscard]] bool move_preview_updates_target_identity(const MoveCandidate& candidate) noexcept;

struct MoveAction
{
    MoveCandidate candidate;
    std::vector<NodeId> possible_landings;
};

[[nodiscard]] NodeId resolve_landing(const MapSnapshot& map, NodeId target) noexcept;
[[nodiscard]] std::vector<MoveAction>
    enumerate_move_actions(const MapSnapshot& map, const RunState& state, GraphLayer layer = GraphLayer::Confirmed);

enum class PreviewReachability
{
    Unknown,
    Reachable,
    Blocked,
    InsufficientActionPoints,
    TargetStateChanged,
};

struct MovePreview
{
    PreviewReachability reachability = PreviewReachability::Unknown;
    int exact_action_point_cost = 0;
    NodeType displayed_type = NodeType::Unknown;
    std::string displayed_name;
    bool identity_revealed = false;
};

// 移动预览展示的是当前节点的实际标题，可靠性高于模板附带的初始身份。
// 只有预览已真正揭示身份时才允许覆盖，未知的诡秘/凶戾仍由原有隐藏身份规则处理。
[[nodiscard]] bool should_apply_revealed_preview_identity(const Node& current, const MovePreview& preview) noexcept;

// 已揭示的非作战节点若在移动预览中显示为普通作战，表示流窜“居民”当前占据了该节点；
// 预览揭示的是临时战斗效果，不能覆盖节点本身的不期而遇、空地等身份。
[[nodiscard]] bool preview_confirms_roaming_resident(const Node& current, const MovePreview& preview) noexcept;

enum class MoveTransactionStage
{
    Proposed,
    Previewed,
    Committed,
    PageResolved,
    Observed,
    Applied,
    Cancelled,
    Invalidated,
};

struct MoveObservation
{
    NodeId current_node = InvalidNodeId;
    int floor = 0;
    int action_points = 0;
    NodeProgress target_progress = NodeProgress::Active;
    NodeType landed_type = NodeType::Unknown;
    std::uint64_t map_revision = 0;
    std::uint64_t viewport_revision = 0;
    // 第三层行动力耗尽后会先结算当前节点、再进入已适配的追猎；胜利后的首张地图已经是第四层。
    // 该信号由会话结合策略、页面结算阶段和 NextLevel 楼层识别共同确认，不能仅凭地图 OCR 设置。
    bool advanced_via_adapted_pursuit = false;
    // 未揭示的诡秘可能在页面结算后直接把探索推进到下一层；事务创建时尚不知道它是跨层事件。
    // 只有页面已经结算且 NextLevel 明确识别到下一层时，会话才允许设置该信号。
    bool advanced_via_resolved_page = false;
    // 四层追忆会在险路尽头/小径结算后保持楼层编号为 4，但生成一张全新的地图。
    // 该信号必须由页面已结算、NextLevel 再次确认四层及新地图入口观测共同确认。
    bool renewed_same_floor_after_terminal = false;
    // 关联事件第一项会先结算原事件格，再免费传送到据点/应急助力。该字段记录传送前的
    // 真实移动落点，必须由会话用候选目标和回图差分校验后设置，不能由地图 OCR 单独设置。
    NodeId linked_encounter_origin_node = InvalidNodeId;
};

class MoveTransaction
{
public:
    static std::optional<MoveTransaction> propose(
        MoveCandidate proposal,
        const MapSnapshot& map,
        const ViewportObservation& viewport,
        std::string* error = nullptr);

    [[nodiscard]] bool record_preview(MovePreview preview, std::string* error = nullptr);
    [[nodiscard]] bool commit(
        std::uint64_t current_map_revision,
        std::uint64_t current_viewport_revision,
        std::string* error = nullptr);
    [[nodiscard]] bool mark_page_resolved(std::string* error = nullptr);
    [[nodiscard]] bool observe(MoveObservation observation, std::string* error = nullptr);
    [[nodiscard]] bool apply(RunState& state, std::string* error = nullptr);
    void cancel() noexcept;
    void invalidate() noexcept;

    [[nodiscard]] const MoveCandidate& proposal() const noexcept { return m_proposal; }

    [[nodiscard]] const std::optional<MovePreview>& preview() const noexcept { return m_preview; }

    [[nodiscard]] MoveTransactionStage stage() const noexcept { return m_stage; }

    [[nodiscard]] int authoritative_cost() const noexcept;

    [[nodiscard]] std::uint64_t map_revision() const noexcept { return m_map_revision; }

    [[nodiscard]] std::uint64_t viewport_revision() const noexcept { return m_viewport_revision; }

private:
    MoveCandidate m_proposal;
    std::optional<MovePreview> m_preview;
    std::optional<MoveObservation> m_observation;
    MoveTransactionStage m_stage = MoveTransactionStage::Proposed;
    int m_source_floor = 0;
    NodeType m_target_type = NodeType::Unknown;
    std::uint64_t m_map_revision = 0;
    std::uint64_t m_viewport_revision = 0;
};

[[nodiscard]] std::string_view to_string(NodeType type) noexcept;
[[nodiscard]] std::string_view to_string(MovementKind kind) noexcept;
} // namespace asst::blackflow
