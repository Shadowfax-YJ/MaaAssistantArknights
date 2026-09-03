#include "BlackFlowSession.h"

#include "BlackFlowDiagnosticTimeline.h"
#include "BlackFlowEncounterRules.h"
#include "BlackFlowInventoryRules.h"
#include "BlackFlowPlannerRules.h"
#include "BlackFlowMovementRecognition.h"
#include "BlackFlowRevealSemantics.h"
#include "BlackFlowRoutingLoop.h"

#include <algorithm>
#include <limits>
#include <regex>
#include <tuple>
#include <unordered_map>

#include "Config/TaskData.h"
#include "Utils/Logger.hpp"

namespace asst::blackflow
{
std::string_view to_string(CultivatedAnimalType type) noexcept
{
    switch (type) {
    case CultivatedAnimalType::Cat:
        return "swaddled_cat";
    case CultivatedAnimalType::FeatheredSerpent:
        return "swaddled_feathered_serpent";
    case CultivatedAnimalType::Dog:
        return "swaddled_dog";
    case CultivatedAnimalType::Cerberus:
        return "swaddled_cerberus";
    }
    return "swaddled_cat";
}

std::optional<CultivatedAnimalType> parse_cultivated_animal_type(std::string_view value) noexcept
{
    if (value == "swaddled_cat") {
        return CultivatedAnimalType::Cat;
    }
    if (value == "swaddled_feathered_serpent") {
        return CultivatedAnimalType::FeatheredSerpent;
    }
    if (value == "swaddled_dog") {
        return CultivatedAnimalType::Dog;
    }
    if (value == "swaddled_cerberus") {
        return CultivatedAnimalType::Cerberus;
    }
    return std::nullopt;
}

std::optional<CultivatedAnimalType> cultivated_animal_type_from_name(std::string_view name) noexcept
{
    if (name == "襁褓中的猫") {
        return CultivatedAnimalType::Cat;
    }
    if (name == "襁褓羽蛇") {
        return CultivatedAnimalType::FeatheredSerpent;
    }
    if (name == "襁褓中的狗") {
        return CultivatedAnimalType::Dog;
    }
    if (name == "襁褓三头犬") {
        return CultivatedAnimalType::Cerberus;
    }
    return std::nullopt;
}

namespace
{
FactValue default_fact_value(FactType type)
{
    switch (type) {
    case FactType::Boolean:
        return false;
    case FactType::Integer:
        return std::int64_t { 0 };
    case FactType::String:
        return std::string {};
    case FactType::StringList:
        return std::vector<std::string> {};
    }
    return false;
}

std::int64_t integer_fact(const FactStore& facts, std::string_view name)
{
    const FactValue* value = facts.find(name);
    return value != nullptr && std::holds_alternative<std::int64_t>(*value) ? std::get<std::int64_t>(*value) : 0;
}

std::int64_t saturated_add(std::int64_t value, std::int64_t delta) noexcept
{
    if (delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return value + delta;
}

bool has_node_type(const MapSnapshot& map, NodeType type)
{
    return std::ranges::any_of(map.nodes(), [&](const auto& pair) {
        return pair.second.type == type && pair.second.progress != NodeProgress::Removed;
    });
}

std::optional<Node> floor_boss_node(const MapSnapshot& primary, const MapSnapshot& fallback, int floor)
{
    const auto candidates = [floor](const MapSnapshot& map) {
        std::vector<Node> result;
        for (const auto& [id, node] : map.nodes()) {
            (void)id;
            if (node.floor != floor || node.type != NodeType::BattleBoss || node.progress == NodeProgress::Removed) {
                continue;
            }
            result.emplace_back(node);
        }
        return result;
    };
    auto result = candidates(primary);
    if (result.size() == 1) {
        return std::move(result.front());
    }
    if (result.size() > 1) {
        return std::nullopt;
    }
    result = candidates(fallback);
    return result.size() == 1 ? std::optional<Node>(std::move(result.front())) : std::nullopt;
}

bool event_node_type_is_known(NodeType type) noexcept
{
    return type != NodeType::Unknown && type != NodeType::HideInvisible && type != NodeType::HideBattle;
}

std::string_view progress_name(NodeProgress progress) noexcept
{
    switch (progress) {
    case NodeProgress::Active:
        return "active";
    case NodeProgress::Completed:
        return "completed";
    case NodeProgress::Removed:
        return "removed";
    }
    return "active";
}

std::string_view identity_state_name(NodeIdentityState state) noexcept
{
    switch (state) {
    case NodeIdentityState::Classified:
        return "classified";
    case NodeIdentityState::Hidden:
        return "hidden";
    case NodeIdentityState::Unclassified:
        return "unclassified";
    }
    return "unclassified";
}

std::string_view edge_knowledge_name(EdgeKnowledge knowledge) noexcept
{
    switch (knowledge) {
    case EdgeKnowledge::Confirmed:
        return "confirmed";
    case EdgeKnowledge::Absent:
        return "absent";
    case EdgeKnowledge::Unknown:
        return "unknown";
    }
    return "unknown";
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

json::array serialize_map_nodes(const MapSnapshot& map, const RunState* run = nullptr)
{
    std::vector<json::value> nodes;
    nodes.reserve(map.nodes().size());
    for (const auto& [id, node] : map.nodes()) {
        json::object serialized {
                { "id", id },
                { "row", node.position.row },
                { "column", node.position.column },
                { "type", std::string(to_string(node.type)) },
                { "node_type", std::string(to_string(node.type)) },
                { "name", node.name },
                { "node_name", node.name },
                { "fate_event", node.fate_event },
                { "stage_name",
                  node.battle.has_value() ? node.battle->stage_name : std::string(battle_stage_name(node)) },
                { "marker_type", node.marker_type },
                { "marker_display_name", node.marker_display_name },
                { "marker_score", node.marker_score },
                { "marker_resident_overlap_possible", node.marker_resident_overlap_possible },
                { "identity_revealed", node.identity_revealed },
                { "visually_hidden", node.visually_hidden },
                { "identity_from_topology", node.identity_from_topology },
                { "identity_from_prediction", node.identity_from_prediction },
                { "prediction_rule", node.prediction_rule },
                { "natural_reveal_suppressed", node.natural_reveal_suppressed },
                { "existence_source", node.existence_source },
                { "identity_source", node.identity_source },
                { "detected_by_vision", node.detected_by_vision },
                { "confirmed_by_topology", node.confirmed_by_topology },
                { "identity_state", std::string(identity_state_name(node.identity_state)) },
                { "progress", std::string(progress_name(node.progress)) },
                { "semantically_known", node.identity_revealed },
                { "revealed_now", run != nullptr && run->revealed_nodes.contains(id) },
                { "blocks_vision", node.traversal.blocks_vision },
                { "is_exit", is_exit_node_type(node.type) },
            };
        if (node.battle.has_value()) {
            serialized["battle"] = serialize_node_battle(*node.battle);
            if (node.battle->total_kills.has_value()) {
                serialized["battle_total_kills"] = *node.battle->total_kills;
            }
        }
        nodes.emplace_back(std::move(serialized));
    }
    return json::array(std::move(nodes));
}

json::array serialize_map_edges(const MapSnapshot& map)
{
    std::vector<json::value> edges;
    edges.reserve(map.edges().size());
    for (const Edge& edge : map.edges()) {
        edges.emplace_back(
            json::object {
                { "first", edge.first },
                { "second", edge.second },
                { "knowledge", std::string(edge_knowledge_name(edge.knowledge)) },
                { "forced", edge.evidence.forced_by_connectivity_constraint },
                { "probability", edge.evidence.probability },
                { "cnn_connected", edge.evidence.cnn_connected },
                { "forced_by_connectivity_constraint", edge.evidence.forced_by_connectivity_constraint },
                { "decision_source", edge.evidence.decision_source },
            });
    }
    return json::array(std::move(edges));
}

json::object serialize_map_snapshot(const NormalizedMap& map, const RunState* run = nullptr)
{
    return {
        { "floor", map.floor() },
        { "revision", map.snapshot().revision },
        { "nodes", serialize_map_nodes(map.snapshot(), run) },
        { "edges", serialize_map_edges(map.snapshot()) },
    };
}

struct StrategyGoals
{
    // 声明「达成即收工」的目标节点。它与物理出口取并集，所以端点集合恒非空。
    std::unordered_set<NodeId> terminal_nodes;
    // 待锁定的强制目标，按优先级从高到低。前 undemotable_count 条是无条件必达的，阶梯不会降级它们。
    std::vector<std::string> binding_candidates;
    std::size_t undemotable_count = 0;
};

StrategyGoals strategy_goals_for(
    const ResolvedPolicy& policy,
    const MissionState& mission,
    const FactStore& facts,
    const MapSnapshot& map,
    int floor)
{
    StrategyGoals result;
    std::vector<const Milestone*> candidates;
    for (const Milestone& milestone : policy.milestones) {
        if (!milestone_is_active(milestone, floor, facts, mission)) {
            continue;
        }
        bool has_matching_node = false;
        for (const auto& [id, node] : map.nodes()) {
            if (node.progress == NodeProgress::Removed || !milestone_matches_node(milestone, node)) {
                continue;
            }
            has_matching_node = true;
            if (milestone.terminality == MilestoneTerminality::IsTerminal) {
                result.terminal_nodes.emplace(id);
            }
        }
        if (!milestone.binding_candidate() || mission.progress(milestone.id) >= milestone.required_count) {
            continue;
        }
        // 目标在本层地图上没有任何匹配节点时，可行性求解一定得出无解。可行则必达的目标直接跳过，
        // 省掉一次白跑的安全求解；无条件必达的目标仍然送进去，让它按声明把本层判成无解。
        if (milestone.enforcement == MilestoneEnforcement::FeasibleHard && !has_matching_node) {
            continue;
        }
        candidates.emplace_back(&milestone);
    }
    std::ranges::sort(candidates, [](const Milestone* lhs, const Milestone* rhs) {
        if (lhs->enforcement != rhs->enforcement) {
            // Hard 排在 FeasibleHard 之前，阶梯从末尾开始降级，因此永远不会降到 Hard。
            return lhs->enforcement > rhs->enforcement;
        }
        return std::tie(lhs->rank, lhs->id) < std::tie(rhs->rank, rhs->id);
    });
    for (const Milestone* milestone : candidates) {
        result.binding_candidates.emplace_back(milestone->id);
        result.undemotable_count += milestone->enforcement == MilestoneEnforcement::Hard ? 1 : 0;
    }
    return result;
}

LinkedEncounterRouteValue encounter_route_value(const BlackFlowPlan& plan)
{
    LinkedEncounterRouteValue value;
    value.viable = static_cast<bool>(plan) && !plan.endpoint_fallback_active &&
                   plan.safety.required_action_points < UnreachableActionPointRequirement &&
                   !plan.decision.candidate_summaries.empty();
    value.required_action_points = plan.safety.required_action_points;
    if (!plan.decision.candidate_summaries.empty()) {
        value.lexicographic_score = plan.decision.candidate_summaries.front().lexicographic_score;
        value.lexicographic_score_labels =
            plan.decision.candidate_summaries.front().lexicographic_score_labels;
    }
    return value;
}

bool erase_processing_item_instances(RunState& run, int first_inventory_index, int second_inventory_index)
{
    if (first_inventory_index == second_inventory_index) {
        return false;
    }
    bool removed_loaded_instance = false;
    std::size_t removed = 0;
    std::erase_if(run.resources.movement_instances, [&](const RunResources::MovementInstance& instance) {
        if (instance.inventory_index != first_inventory_index && instance.inventory_index != second_inventory_index) {
            return false;
        }
        removed_loaded_instance = removed_loaded_instance || instance.loaded;
        ++removed;
        return true;
    });
    if (removed != 2) {
        return false;
    }
    if (removed_loaded_instance) {
        run.active_movement.reset();
    }
    rebuild_movement_aggregates(run.resources);
    return true;
}

} // namespace

json::object BlackFlowStrategyResult::to_json() const
{
    return {
        { "profile", profile },
        { "outcome", outcome },
        { "termination_reason", termination_reason },
        { "cultivated_animals", cultivated_animals },
        { "succeeded", succeeded },
        { "next_action", next_action },
        { "cultivation_target", cultivation_target },
        { "cultivated_animal_types", json::array(cultivated_animal_types) },
    };
}

bool BlackFlowSession::initialize(std::string profile, std::string* error)
{
    const PolicyProfile* definition = BlackFlowStrategy.get_profile(profile);
    if (definition == nullptr) {
        if (error != nullptr) {
            *error = "unknown BlackFlow profile: " + profile;
        }
        return false;
    }
    auto resolved = BlackFlowStrategy.resolve_profile(profile, error);
    if (!resolved.has_value()) {
        return false;
    }

    m_profile = std::move(profile);
    m_policy = std::move(resolved);
    m_facts = FactContext {};
    for (const auto& [name, fact] : BlackFlowStrategy.facts()) {
        (void)name;
        if (!m_facts.define(fact, error)) {
            return false;
        }
    }
    m_facts.begin_run();
    for (const auto& [name, definition_value] : BlackFlowStrategy.facts()) {
        if (definition_value.scope != FactScope::Candidate &&
            !m_facts.set(definition_value.scope, name, default_fact_value(definition_value.type), error)) {
            return false;
        }
    }
    m_mission = MissionState {};
    m_map.reset();
    m_exploration_notebook.reset();
    m_viewport.clear(0, 0);
    m_run = RunState {};
    m_current_floor.reset();
    m_map_generation = 0;
    m_initial_prediction_generation = 0;
    m_initial_reveal_checked_generation.reset();
    m_current_map_is_floor_four_remembrance = false;
    m_resident_settlement_prediction = {};
    m_floor_recognition_pending = false;
    m_next_level_transition_confirmed = false;
    m_start_rewards.clear();
    m_cultivated_animal_types.clear();
    m_unreachable_actions.clear();
    m_temporarily_unavailable_movements.clear();
    m_battle_intel_probed.clear();
    m_floor_three_pursuit_battle_pending = false;
    m_collection_popup_pursuit_floor.reset();
    m_collection_popup_pursuit_stage_name.clear();
    m_collection_popup_pursuit_total_kills.reset();
    m_pending_probe_target.reset();
    m_verified_move_arc.reset();
    m_pending_candidate.reset();
    m_transaction.reset();
    m_last_plan.reset();
    m_last_reveal_consistency.reset();
    m_page_context.reset();
    m_result.reset();
    m_result_reported = false;
    m_movement_inventory_refresh_required = next_movement_inventory_refresh_state(
        m_movement_inventory_refresh_required,
        MovementInventoryRefreshEvent::RunStarted);
    m_map_preserved_after_inventory.reset();
    m_persisted_image_packages = 0;
    ++m_run_revision;
    m_page_revision = 0;
    m_decision_sequence = 0;
    m_transaction_sequence = 0;
    m_artifact_sequence = 0;
    m_observed_current_node = InvalidNodeId;
    m_observation_id.clear();
    m_topology_template_id.clear();
    m_utopia_status.clear();
    m_utopia_reason.clear();
    m_utopia_ideology.clear();
    m_utopia_policy.clear();
    m_ideal_source.reset();
    m_ideal_source_generation.reset();
    m_ideal_domain.clear();
    m_observed_ideal_domain.clear();
    m_utopia_effect_expired = false;
    m_ideal_source_score_margin = 0.0;
    m_ideal_source_heads_agree = false;
    m_topology_source_digest.clear();
    m_topology_base_edge_count = 0;
    m_topology_extra_edge_count = 0;
    m_topology_match_score = 0;
    m_decision_id.clear();
    m_transaction_id.clear();
    m_telemetry_events.clear();
    m_diagnostic_requests.clear();
    m_node_attribution_records.clear();
    m_pending_move_node_attributions.clear();
    // 开局配置整局不变，但事实刚被重置成默认值，所以每次初始化都要重新写回去；
    // reset_run() 走的也是这里，否则重开一局后依赖开局配置的策略条件会静默失效。
    if (!set_fact("start_core_char", m_start_core_char, error) || !set_fact("start_squad", m_start_squad, error) ||
        !set_fact("start_roles", m_start_roles, error)) {
        return false;
    }
    return apply_granted_scraps(error);
}

bool BlackFlowSession::apply_granted_scraps(std::string* error)
{
    if (!m_policy.has_value() || m_policy->granted_scraps.empty()) {
        return true;
    }
    bool applied = false;
    const FactStore facts = m_facts.merged();
    for (const GrantedScrap& scrap : m_policy->granted_scraps) {
        if (!scrap.when.evaluate(facts)) {
            continue;
        }
        const MovementSpec* spec = find_movement_spec(scrap.movement);
        if (spec == nullptr) {
            continue;
        }
        m_run.resources.movement_instances.emplace_back(
            RunResources::MovementInstance {
                scrap.movement,
                spec->initial_charges,
                static_cast<int>(m_run.resources.movement_instances.size()),
            });
        applied = true;
    }
    if (!applied) {
        return true;
    }
    rebuild_movement_aggregates(m_run.resources);
    ++m_run.resources_revision;
    return synchronize_resource_facts(error);
}

void BlackFlowSession::set_start_loadout(std::string core_char, std::string squad, std::string roles)
{
    m_start_core_char = std::move(core_char);
    m_start_squad = std::move(squad);
    m_start_roles = std::move(roles);
}

void BlackFlowSession::set_start_reward(std::string reward)
{
    if (!has_start_reward(reward)) {
        m_start_rewards.emplace_back(std::move(reward));
    }
}

bool BlackFlowSession::has_start_reward(std::string_view reward) const noexcept
{
    for (const auto& selected : m_start_rewards) {
        if (selected == reward) {
            return true;
        }
    }
    return false;
}

bool BlackFlowSession::set_automation_collection_core_operator_elite_two(bool elite_two, std::string* error)
{
    return set_fact("automation_collection_core_operator_elite_two", elite_two, error);
}

std::optional<std::size_t> BlackFlowSession::preferred_encounter_choice(std::string_view event_name)
{
    if (event_name == FinalEncounterEventName) {
        if (!m_transaction.has_value() || !m_page_context.has_value() ||
            m_page_context->node_type != NodeType::Final) {
            return std::nullopt;
        }
        const bool bypass = m_transaction->proposal().bypass_final_on_completion;
        Log.info("BlackFlow final encounter follows planned page disposition", bypass ? "pass" : "advance");
        return bypass ? 3U : 1U;
    }

    std::optional<NodeType> linked_type;
    if (event_name == PeaceGuardFollowupEventName) {
        linked_type = NodeType::BattleSavage;
    }
    else if (event_name == LoneSurvivorFollowupEventName) {
        linked_type = NodeType::Employ;
    }
    if (!linked_type.has_value() || !m_page_context.has_value() || !m_transaction.has_value()) {
        return std::nullopt;
    }
    m_page_context->linked_transfer_selected = false;
    m_page_context->linked_transfer_type = *linked_type;
    m_page_context->linked_transfer_targets.clear();

    std::vector<NodeId> targets;
    for (const auto& [id, node] : m_map.snapshot().nodes()) {
        if (node.type == *linked_type) {
            targets.emplace_back(id);
        }
    }
    bool completed_linked_node_known = false;
    for (const auto& [id, node] : m_exploration_notebook.snapshot().nodes()) {
        if (node.type != *linked_type) {
            continue;
        }
        const auto progress = m_run.node_progress.find(id);
        if (node.progress != NodeProgress::Active ||
            (progress != m_run.node_progress.end() && progress->second != NodeProgress::Active)) {
            completed_linked_node_known = true;
            break;
        }
    }
    if (completed_linked_node_known) {
        Log.info("BlackFlow linked encounter keeps reveal-only option: associated node already completed", event_name);
        return 2U;
    }
    if (targets.empty() && *linked_type == NodeType::BattleSavage) {
        for (const GridPosition position : m_resident_settlement_prediction.candidates) {
            if (const auto id = make_stable_node_id(m_run.floor, position); id.has_value() &&
                m_map.snapshot().find_node(*id) != nullptr) {
                targets.emplace_back(*id);
            }
        }
    }
    if (targets.empty() && *linked_type == NodeType::Employ) {
        // 应急助力尚未点亮时外观是“未知的诡秘”。没有更强的模板事实时把每个
        // 合法未知诡秘分别作为假设；只有所有假设都严格获益才使用免费传送。
        for (const auto& [id, node] : m_map.snapshot().nodes()) {
            if (node.type == NodeType::HideInvisible && node.progress == NodeProgress::Active) {
                targets.emplace_back(id);
            }
        }
    }
    std::ranges::sort(targets);
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    m_page_context->linked_transfer_targets = targets;
    if (targets.empty()) {
        Log.info("BlackFlow linked encounter keeps reveal-only option: no target hypothesis", event_name);
        return 2U;
    }

    std::vector<NodeId> event_nodes;
    if (m_page_context->node != InvalidNodeId) {
        event_nodes.emplace_back(m_page_context->node);
    }
    else if (!m_transaction->proposal().controllable) {
        event_nodes = m_transaction->proposal().possible_landings;
        std::ranges::sort(event_nodes);
        event_nodes.erase(std::unique(event_nodes.begin(), event_nodes.end()), event_nodes.end());
    }
    if (event_nodes.empty()) {
        Log.info("BlackFlow linked encounter keeps reveal-only option: no original event hypothesis", event_name);
        return 2U;
    }

    const auto prepare_hypothesis = [&](BlackFlowSession& session,
                                        NodeId target,
                                        bool transfer,
                                        const std::unordered_set<NodeId>& immediate_reveals) {
        Node linked = *session.m_map.snapshot().find_node(target);
        linked.type = *linked_type;
        linked.name = *linked_type == NodeType::BattleSavage ? "“居民”据点" : "应急助力";
        linked.identity_revealed = true;
        linked.identity_state = NodeIdentityState::Classified;
        linked.identity_from_prediction = false;
        linked.prediction_rule.clear();
        linked.traversal = default_traversal_for(*linked_type);
        session.m_map.snapshot().upsert_node(std::move(linked));
        session.m_run.revealed_nodes.emplace(target);
        session.m_run.revealed_nodes.insert(immediate_reveals.begin(), immediate_reveals.end());

        const NodeId event_node = session.m_page_context->node;
        if (const Node* stored = session.m_map.snapshot().find_node(event_node); stored != nullptr) {
            Node current = *stored;
            current.type = NodeType::Empty;
            current.name = std::string(EmptyNodeName);
            current.progress = NodeProgress::Completed;
            current.traversal = default_traversal_for(NodeType::Empty);
            session.m_map.snapshot().upsert_node(std::move(current));
        }
        session.m_run.node_progress.insert_or_assign(event_node, NodeProgress::Completed);
        session.m_run.visited_nodes.emplace(event_node);
        session.m_run.current_node = transfer ? target : event_node;
        session.m_transaction.reset();
        session.m_page_context.reset();
        session.m_pending_candidate.reset();
        session.m_last_plan.reset();
    };

    bool hypotheses_valid = true;
    bool assessed_any_hypothesis = false;
    RandomRouteAggregate baseline_aggregate;
    RandomRouteAggregate transferred_aggregate;
    std::vector<json::value> assessments;
    for (const NodeId target : targets) {
        const Node* current = m_map.snapshot().find_node(target);
        const auto recorded_progress = m_run.node_progress.find(target);
        if (current == nullptr || current->progress != NodeProgress::Active ||
            (recorded_progress != m_run.node_progress.end() && recorded_progress->second != NodeProgress::Active)) {
            hypotheses_valid = false;
            break;
        }
        for (const NodeId event_node : event_nodes) {
            // 关联目标与原事件是两个节点；小八界尚不知原落点时，该组合不属于合法世界。
            if (event_node == target || m_map.snapshot().find_node(event_node) == nullptr) {
                continue;
            }
            assessed_any_hypothesis = true;
            BlackFlowSession baseline = *this;
            BlackFlowSession transferred = *this;
            baseline.m_page_context->node = event_node;
            transferred.m_page_context->node = event_node;
            const auto baseline_immediate_reveals = expected_linked_encounter_return_reveals(
                m_map.snapshot(),
                m_run,
                m_transaction->proposal(),
                event_node,
                target,
                false,
                true);
            const auto transferred_immediate_reveals = expected_linked_encounter_return_reveals(
                m_map.snapshot(),
                m_run,
                m_transaction->proposal(),
                event_node,
                target,
                true,
                true);
            prepare_hypothesis(baseline, target, false, baseline_immediate_reveals);
            prepare_hypothesis(transferred, target, true, transferred_immediate_reveals);
            std::string baseline_error;
            std::string transfer_error;
            const BlackFlowPlan baseline_plan = baseline.plan(&baseline_error);
            const BlackFlowPlan transferred_plan = transferred.plan(&transfer_error);
            const LinkedEncounterRouteValue baseline_value = encounter_route_value(baseline_plan);
            const LinkedEncounterRouteValue transferred_value = encounter_route_value(transferred_plan);
            const int immediate_weight =
                effective_node_weight_at_floor(*linked_type, current->floor, current->marker_type);
            const LinkedEncounterRouteValue adjusted_baseline = adjusted_linked_encounter_route_value(
                baseline_value,
                static_cast<int>(baseline_immediate_reveals.size()),
                0);
            const LinkedEncounterRouteValue adjusted_transfer = adjusted_linked_encounter_route_value(
                transferred_value,
                static_cast<int>(transferred_immediate_reveals.size()),
                immediate_weight);
            append_random_route_sample(baseline_aggregate, adjusted_baseline);
            append_random_route_sample(transferred_aggregate, adjusted_transfer);
            const bool sample_better =
                random_route_is_safe_expected_improvement(transferred_aggregate, baseline_aggregate);
            assessments.emplace_back(
                json::object {
                    { "event_node", event_node },
                    { "target", target },
                    { "baseline_viable", baseline_value.viable },
                    { "transfer_viable", transferred_value.viable },
                    { "baseline_required_action_points", baseline_value.required_action_points },
                    { "transfer_required_action_points", transferred_value.required_action_points },
                    { "score_labels", json::array(baseline_value.lexicographic_score_labels) },
                    { "baseline_score", json::array(baseline_value.lexicographic_score) },
                    { "transfer_score", json::array(transferred_value.lexicographic_score) },
                    { "immediate_effective_node_weight", immediate_weight },
                    { "baseline_immediate_revealed_node_count", baseline_immediate_reveals.size() },
                    { "transfer_immediate_revealed_node_count", transferred_immediate_reveals.size() },
                    { "aggregate_positive_so_far", sample_better },
                    { "baseline_error", baseline_error },
                    { "transfer_error", transfer_error },
                });
        }
    }
    const bool stable_benefit = hypotheses_valid && assessed_any_hypothesis &&
                                random_route_is_safe_expected_improvement(
                                    transferred_aggregate,
                                    baseline_aggregate);

    if (stable_benefit) {
        m_page_context->linked_transfer_selected = true;
    }

    json::object details {
        { "event_name", std::string(event_name) },
        { "target_type", std::string(to_string(*linked_type)) },
        { "target_hypotheses", json::array(std::move(assessments)) },
        { "baseline_worst_score", json::array(baseline_aggregate.worst_score) },
        { "transfer_worst_score", json::array(transferred_aggregate.worst_score) },
        { "baseline_score_sum", json::array(baseline_aggregate.lexicographic_score_sum) },
        { "transfer_score_sum", json::array(transferred_aggregate.lexicographic_score_sum) },
        { "sample_count", baseline_aggregate.sample_count },
        { "selected_option", stable_benefit ? 1 : 2 },
    };
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowLinkedEncounterChoice", details });
    Log.info(
        "BlackFlow linked encounter route assessment",
        event_name,
        stable_benefit ? "free transfer" : "reveal only");
    return stable_benefit ? 1U : 2U;
}

std::optional<std::vector<std::string>>
    BlackFlowSession::preferred_encounter_choice_order(std::string_view event_name)
{
    if (event_name != HealingHeartEventName) {
        return std::nullopt;
    }

    HealingHeartRouteAggregate action_point_route;
    HealingHeartRouteAggregate processing_item_route;
    const auto fallback_order = [&]() {
        return make_healing_heart_choice_order(action_point_route, processing_item_route);
    };
    if (!m_transaction.has_value() || !m_page_context.has_value()) {
        Log.warn("BlackFlow Healing Heart route assessment has no active incident transaction; using safe fallbacks");
        return fallback_order();
    }

    std::vector<NodeId> event_nodes;
    if (m_page_context->node != InvalidNodeId) {
        event_nodes.emplace_back(m_page_context->node);
    }
    else if (!m_transaction->proposal().controllable) {
        for (const NodeId landing : m_transaction->proposal().possible_landings) {
            const Node* node = m_map.snapshot().find_node(landing);
            // 随机移动的 possible_landings 是移动方式的完整落点池；事件标题已经证明实际
            // 落点是“不期而遇”，所以排除地图上已可靠分类为其他类型的假设。
            if (node != nullptr &&
                (!event_node_type_is_known(node->type) || node->type == NodeType::Incident)) {
                event_nodes.emplace_back(landing);
            }
        }
    }
    std::erase_if(event_nodes, [&](NodeId node) { return m_map.snapshot().find_node(node) == nullptr; });
    std::ranges::sort(event_nodes);
    event_nodes.erase(std::unique(event_nodes.begin(), event_nodes.end()), event_nodes.end());
    if (event_nodes.empty()) {
        Log.warn("BlackFlow Healing Heart route assessment has no landing hypothesis; using safe fallbacks");
        return fallback_order();
    }

    const MoveCandidate proposal = m_transaction->proposal();
    const int entry_action_point_cost = m_transaction->authoritative_cost();
    std::vector<std::string> assessment_errors;
    const auto prepare_after_event = [&](BlackFlowSession& session, NodeId event_node, std::string* error) {
        if (entry_action_point_cost < 0 || entry_action_point_cost > session.m_run.resources.action_points) {
            if (error != nullptr) {
                *error = "entry movement cost exceeds the pre-event action points";
            }
            return false;
        }
        const MovementSpec* movement = find_movement_spec(proposal.movement);
        if (movement == nullptr) {
            if (error != nullptr) {
                *error = "entry transaction references an unknown movement";
            }
            return false;
        }

        int action_point_gain = proposal.predicted_action_point_gain;
        const NodeId gain_landing = proposal.controllable ? proposal.landing : event_node;
        if (const auto gain = proposal.landing_action_point_gains.find(gain_landing);
            gain != proposal.landing_action_point_gains.end()) {
            action_point_gain = gain->second;
        }
        session.m_run.resources.action_points = action_points_after(
            session.m_run.resources.action_points,
            entry_action_point_cost,
            action_point_gain);
        session.m_run.resources.hope += movement->effect.hope_gain;
        session.m_run.resources.ingots += movement->effect.ingot_gain;
        if (!project_consumed_entry_processing_item(session.m_run, proposal.movement)) {
            if (error != nullptr) {
                *error = "entry movement has no matching processing-item instance";
            }
            return false;
        }
        ++session.m_run.resources_revision;

        if (const Node* stored = session.m_map.snapshot().find_node(event_node); stored != nullptr) {
            Node current = *stored;
            current.type = NodeType::Empty;
            current.name = std::string(EmptyNodeName);
            current.progress = NodeProgress::Completed;
            current.traversal = default_traversal_for(NodeType::Empty);
            session.m_map.snapshot().upsert_node(std::move(current));
        }
        session.m_run.node_progress.insert_or_assign(event_node, NodeProgress::Completed);
        session.m_run.visited_nodes.emplace(event_node);
        session.m_run.current_node = event_node;
        session.m_transaction.reset();
        session.m_page_context.reset();
        session.m_pending_candidate.reset();
        session.m_last_plan.reset();
        session.m_temporarily_unavailable_movements.clear();
        session.m_movement_inventory_refresh_required = false;
        if (!session.synchronize_resource_facts(error)) {
            return false;
        }
        session.refresh_mission();
        return true;
    };

    for (const NodeId event_node : event_nodes) {
        BlackFlowSession after_event = *this;
        std::string preparation_error;
        if (!prepare_after_event(after_event, event_node, &preparation_error)) {
            LinkedEncounterRouteValue unavailable;
            unavailable.required_action_points = UnreachableActionPointRequirement;
            append_healing_heart_route_sample(action_point_route, unavailable);
            append_healing_heart_route_sample(processing_item_route, unavailable);
            assessment_errors.emplace_back(
                "event node " + std::to_string(event_node) + ": " + preparation_error);
            continue;
        }

        {
            BlackFlowSession paid_action_points = after_event;
            LinkedEncounterRouteValue value;
            value.required_action_points = UnreachableActionPointRequirement;
            if (paid_action_points.m_run.resources.action_points >= HealingHeartActionPointCost) {
                paid_action_points.m_run.resources.action_points -= HealingHeartActionPointCost;
                ++paid_action_points.m_run.resources_revision;
                std::string planning_error;
                if (paid_action_points.synchronize_resource_facts(&planning_error)) {
                    paid_action_points.refresh_mission();
                    value = encounter_route_value(
                        paid_action_points.plan_internal(true, false, &planning_error));
                }
                if (!planning_error.empty()) {
                    assessment_errors.emplace_back(
                        "action-point option at " + std::to_string(event_node) + ": " + planning_error);
                }
            }
            else {
                assessment_errors.emplace_back(
                    "action-point option at " + std::to_string(event_node) + ": fewer than 3 action points");
            }
            append_healing_heart_route_sample(action_point_route, value);
        }

        const auto& instances = after_event.m_run.resources.movement_instances;
        if (instances.size() < 2) {
            LinkedEncounterRouteValue unavailable;
            unavailable.required_action_points = UnreachableActionPointRequirement;
            append_healing_heart_route_sample(processing_item_route, unavailable);
            assessment_errors.emplace_back(
                "processing-item option at " + std::to_string(event_node) + ": fewer than two instances");
            continue;
        }

        std::unordered_map<std::string, LinkedEncounterRouteValue> route_cache;
        for (std::size_t first = 0; first + 1 < instances.size(); ++first) {
            for (std::size_t second = first + 1; second < instances.size(); ++second) {
                BlackFlowSession consumed_items = after_event;
                LinkedEncounterRouteValue value;
                value.required_action_points = UnreachableActionPointRequirement;
                if (!erase_processing_item_instances(
                        consumed_items.m_run,
                        instances[first].inventory_index,
                        instances[second].inventory_index)) {
                    append_healing_heart_route_sample(processing_item_route, value);
                    continue;
                }
                ++consumed_items.m_run.resources_revision;

                std::string outcome_key;
                outcome_key.reserve(consumed_items.m_run.resources.movement_instances.size() * 10 + 8);
                outcome_key.append(consumed_items.m_run.active_movement.has_value()
                                       ? std::to_string(static_cast<int>(*consumed_items.m_run.active_movement))
                                       : "-");
                for (const RunResources::MovementInstance& instance :
                     consumed_items.m_run.resources.movement_instances) {
                    outcome_key.push_back('|');
                    outcome_key.append(std::to_string(static_cast<int>(instance.movement)));
                    outcome_key.push_back(':');
                    outcome_key.append(std::to_string(instance.remaining_charges));
                    outcome_key.push_back(':');
                    outcome_key.push_back(instance.loaded ? '1' : '0');
                }

                const auto cached = route_cache.find(outcome_key);
                if (cached != route_cache.end()) {
                    value = cached->second;
                }
                else {
                    std::string planning_error;
                    if (consumed_items.synchronize_resource_facts(&planning_error)) {
                        consumed_items.refresh_mission();
                        value = encounter_route_value(
                            consumed_items.plan_internal(true, false, &planning_error));
                    }
                    route_cache.emplace(std::move(outcome_key), value);
                    if (!planning_error.empty()) {
                        assessment_errors.emplace_back(
                            "processing-item outcome at " + std::to_string(event_node) + ": " + planning_error);
                    }
                }
                append_healing_heart_route_sample(processing_item_route, value);
            }
        }
    }

    const std::vector<std::string> order =
        make_healing_heart_choice_order(action_point_route, processing_item_route);
    json::object details {
        { "event_name", std::string(event_name) },
        { "entry_movement", std::string(to_string(proposal.movement)) },
        { "entry_action_point_cost", entry_action_point_cost },
        { "landing_hypothesis_count", event_nodes.size() },
        { "action_point_route_viable", healing_heart_route_is_viable(action_point_route) },
        { "action_point_route_samples", action_point_route.sample_count },
        { "action_point_route_worst_required_action_points", action_point_route.worst_required_action_points },
        { "action_point_route_score_sum", json::array(action_point_route.lexicographic_score_sum) },
        { "action_point_route_score_labels", json::array(action_point_route.lexicographic_score_labels) },
        { "processing_item_route_viable", healing_heart_route_is_viable(processing_item_route) },
        { "processing_item_route_samples", processing_item_route.sample_count },
        { "processing_item_route_worst_required_action_points",
          processing_item_route.worst_required_action_points },
        { "processing_item_route_score_sum", json::array(processing_item_route.lexicographic_score_sum) },
        { "processing_item_route_score_labels", json::array(processing_item_route.lexicographic_score_labels) },
        { "choice_order", json::array(order) },
        { "assessment_errors", json::array(std::move(assessment_errors)) },
    };
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowHealingHeartChoice", details });
    Log.info(
        "BlackFlow Healing Heart route assessment",
        "action points",
        healing_heart_route_is_viable(action_point_route) ? "safe" : "unsafe",
        "processing items",
        healing_heart_route_is_viable(processing_item_route) ? "safe" : "unsafe",
        "order",
        json::array(order));
    return order;
}

bool BlackFlowSession::no_action_points_is_terminal() const
{
    if (!m_policy.has_value()) {
        return false;
    }
    if (m_policy->no_AP_is_terminal_floors.contains(m_run.floor)) {
        return true;
    }
    if (m_profile != "automation_collection" || m_run.floor != 3) {
        return false;
    }
    const FactStore facts = m_facts.merged();
    const FactValue* value = facts.find("automation_collection_core_operator_elite_two");
    return value != nullptr && std::holds_alternative<bool>(*value) && std::get<bool>(*value);
}

void BlackFlowSession::set_cultivated_animal_types(std::vector<CultivatedAnimalType> types)
{
    m_cultivated_animal_types.clear();
    m_cultivated_animal_types.reserve(types.size());
    for (const CultivatedAnimalType type : types) {
        if (std::find(m_cultivated_animal_types.begin(), m_cultivated_animal_types.end(), type) ==
            m_cultivated_animal_types.end()) {
            m_cultivated_animal_types.emplace_back(type);
        }
    }
}

void BlackFlowSession::reset_run()
{
    const std::string selected_profile = m_profile;
    std::string ignored;
    initialize(selected_profile, &ignored);
}

bool BlackFlowSession::configure_diagnostics(DiagnosticSettings settings, std::string* error)
{
    if (!settings.validate(error)) {
        return false;
    }
    m_diagnostics = settings;
    return true;
}

bool BlackFlowSession::set_fact(std::string_view name, FactValue value, std::string* error)
{
    const FactDefinition* definition = BlackFlowStrategy.get_fact_definition(std::string(name));
    if (definition == nullptr) {
        if (error != nullptr) {
            *error = "attempted to set unknown strategy fact: " + std::string(name);
        }
        return false;
    }
    return m_facts.set(definition->scope, std::string(name), std::move(value), error);
}

bool BlackFlowSession::synchronize_resource_facts(std::string* error)
{
    const std::vector<std::pair<std::string, std::int64_t>> values = {
        { "current_floor", m_run.floor },
        { "action_points", m_run.resources.action_points },
        { "ingots", m_run.resources.ingots },
        { "seeds", m_run.resources.seeds },
        { "sellable_scraps", m_run.resources.sellable_scraps },
        { "white_model_bird_count", m_run.resources.white_model_birds },
        { "painted_liberi_owned", m_run.resources.painted_liberi ? 1 : 0 },
        { "persistent_long_range_count", m_resources.read("persistent_long_range_movement", m_run).value_or(0) },
        { "persistent_full_map_count", m_resources.read("persistent_full_map_movement", m_run).value_or(0) },
        { "automation_collection_full_map_count",
          m_resources.read("automation_collection_full_map_movement", m_run).value_or(0) },
    };
    for (const auto& [name, value] : values) {
        if (name == "painted_liberi_owned") {
            if (!set_fact(name, value != 0, error)) {
                return false;
            }
        }
        else if (!set_fact(name, value, error)) {
            return false;
        }
    }
    return true;
}

void BlackFlowSession::refresh_mission()
{
    const auto previous = m_mission.milestones;
    m_mission.refresh(m_policy->milestones, m_run.floor, m_facts.merged());
    publish_milestone_facts();
    for (const auto& [id, status] : m_mission.milestones) {
        const auto old = previous.find(id);
        if (status == MilestoneStatus::Inactive || (old != previous.end() && old->second == status)) {
            continue;
        }
        json::object details {
            { "run_revision", m_run_revision },
            { "observation_id", m_observation_id },
            { "floor", m_run.floor },
            { "milestone_id", id },
            { "status", std::string(to_string(status)) },
            { "progress", m_mission.progress(id) },
        };
        m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowMilestoneChanged", std::move(details) });
    }
}

// 里程碑状态发布成事实，条件语言和终止规则才能读到它。事实名由配置解析时自动登记，
// 形如 milestone.<id>.status 与 milestone.<id>.progress。
void BlackFlowSession::publish_milestone_facts()
{
    if (!m_policy.has_value()) {
        return;
    }
    std::string ignored;
    for (const Milestone& milestone : m_policy->milestones) {
        (void)set_fact(
            "milestone." + milestone.id + ".status",
            std::string(to_string(m_mission.status(milestone.id))),
            &ignored);
        (void)set_fact(
            "milestone." + milestone.id + ".progress",
            static_cast<std::int64_t>(m_mission.progress(milestone.id)),
            &ignored);
    }
}

// 声明了 on_miss 的里程碑一旦错过或不可能，直接结算本局，省得每条策略再写一遍同样的终止规则。
void BlackFlowSession::evaluate_milestone_miss_actions()
{
    if (m_result.has_value() || !m_policy.has_value()) {
        return;
    }
    for (const Milestone& milestone : m_policy->milestones) {
        if (milestone.on_miss != MilestoneMissAction::Terminate) {
            continue;
        }
        const MilestoneStatus status = m_mission.status(milestone.id);
        if (status != MilestoneStatus::Missed && status != MilestoneStatus::Impossible) {
            continue;
        }
        const FactStore facts = m_facts.merged();
        const int cultivated =
            static_cast<int>(std::clamp<std::int64_t>(integer_fact(facts, "cultivated_animals"), 0, 3));
        BlackFlowStrategyResult result {
            m_profile, milestone.miss_outcome, milestone.miss_reason, cultivated, milestone.miss_succeeded,
        };
        result.next_action = milestone.miss_succeeded ? "stop_run" : m_policy->failure_action;
        m_result = std::move(result);
        return;
    }
}

// 页面意图按进入之后确定的真实身份重新解析：取第一条匹配该节点且带意图的活跃里程碑。
// 排序与策略排序一致，锁定候选优先，因此硬目标的意图压过顺路目标。
std::string BlackFlowSession::resolve_page_intent(const PageIdentityResolution& identity, NodeId node, int floor) const
{
    if (!m_policy.has_value()) {
        return "default";
    }
    Node probe;
    if (const Node* stored = m_map.snapshot().find_node(node); stored != nullptr) {
        probe = *stored;
    }
    probe.id = node;
    probe.floor = floor;
    probe.type = identity.type;
    probe.name = identity.name;
    probe.identity_revealed = true;
    probe.identity_state = NodeIdentityState::Classified;

    const FactStore facts = m_facts.merged();
    std::vector<const Milestone*> active;
    for (const Milestone& milestone : m_policy->milestones) {
        if (milestone.page_intent.empty() || !milestone_is_active(milestone, floor, facts, m_mission)) {
            continue;
        }
        active.emplace_back(&milestone);
    }
    std::ranges::sort(active, [](const Milestone* lhs, const Milestone* rhs) {
        if (lhs->binding_candidate() != rhs->binding_candidate()) {
            return lhs->binding_candidate();
        }
        return std::tie(lhs->kind, lhs->rank, lhs->id) < std::tie(rhs->kind, rhs->rank, rhs->id);
    });
    for (const Milestone* milestone : active) {
        if (milestone_matches_node(*milestone, probe)) {
            return milestone->page_intent;
        }
    }
    if (m_profile == "automation_collection") {
        if (identity.type == NodeType::Shop) {
            return "shop.automation_collection";
        }
        if (identity.type == NodeType::ScrapShop) {
            return "scrap_shop.automation_collection";
        }
    }
    return "default";
}

void BlackFlowSession::evaluate_terminal_rules()
{
    if (m_result.has_value() || !m_policy.has_value()) {
        return;
    }
    const FactStore facts = m_facts.merged();
    for (const StrategyTerminalRule& rule : m_policy->terminal_rules) {
        if (!rule.when.evaluate(facts)) {
            continue;
        }
        const int cultivated =
            static_cast<int>(std::clamp<std::int64_t>(integer_fact(facts, "cultivated_animals"), 0, 3));
        BlackFlowStrategyResult result {
            m_profile, rule.outcome, rule.reason, cultivated, rule.succeeded,
        };
        result.next_action =
            rule.next_action.empty() ? (rule.succeeded ? "stop_run" : m_policy->failure_action) : rule.next_action;
        if (m_profile == "baby_animal" && result.outcome == "baby_cultivation_completed") {
            result.cultivation_target = std::string(to_string(m_cultivation_target));
            result.cultivated_animal_types.reserve(m_cultivated_animal_types.size());
            for (const CultivatedAnimalType type : m_cultivated_animal_types) {
                result.cultivated_animal_types.emplace_back(to_string(type));
            }

            const bool target_obtained =
                std::find(m_cultivated_animal_types.begin(), m_cultivated_animal_types.end(), m_cultivation_target) !=
                m_cultivated_animal_types.end();
            if (target_obtained) {
                result.termination_reason = "cultivation_target_obtained";
                result.succeeded = true;
                result.next_action = "stop_run";
            }
            else {
                result.outcome = "baby_cultivation_target_missed";
                result.termination_reason = "cultivation_target_not_obtained";
                result.succeeded = false;
                result.next_action = m_policy->failure_action;
            }
        }
        m_result = std::move(result);
        return;
    }
}

bool BlackFlowSession::apply_observed_facts(const FactStore& facts, std::string* error)
{
    for (const auto& [name, value] : facts.values()) {
        const FactDefinition* definition = BlackFlowStrategy.get_fact_definition(name);
        if (definition == nullptr || definition->scope == FactScope::Candidate) {
            if (error != nullptr) {
                *error = "observed fact is undeclared or candidate-scoped: " + name;
            }
            return false;
        }
        if (!set_fact(name, value, error)) {
            return false;
        }
    }
    return true;
}

bool BlackFlowSession::apply_run_observation(const RunObservation& observation, std::string* error)
{
    const RunResources resources_before = m_run.resources;
    const auto cross_floor_expired_before = m_run.cross_floor_expired;
    auto assign_nonnegative = [&](const std::optional<int>& value, int& target, std::string_view name) {
        if (!value.has_value()) {
            return true;
        }
        if (*value < 0) {
            if (error != nullptr) {
                *error = std::string(name) + " cannot be negative";
            }
            return false;
        }
        target = *value;
        return true;
    };
    if (observation.action_points.has_value()) {
        if (*observation.action_points < 0 || *observation.action_points > 64) {
            if (error != nullptr) {
                *error = "action points must be between 0 and 64";
            }
            return false;
        }
        m_run.resources.action_points = *observation.action_points;
    }
    if (!assign_nonnegative(observation.hope, m_run.resources.hope, "hope") ||
        !assign_nonnegative(observation.ingots, m_run.resources.ingots, "ingots") ||
        !assign_nonnegative(observation.seeds, m_run.resources.seeds, "seeds") ||
        !assign_nonnegative(observation.sellable_scraps, m_run.resources.sellable_scraps, "sellable scraps") ||
        !assign_nonnegative(observation.white_model_birds, m_run.resources.white_model_birds, "white model birds")) {
        return false;
    }
    if (observation.painted_liberi.has_value()) {
        m_run.resources.painted_liberi = *observation.painted_liberi;
    }
    if (observation.movement_charges.has_value()) {
        std::vector<RunResources::MovementInstance> observed_instances;
        for (const auto& [movement, charges] : *observation.movement_charges) {
            const MovementSpec* spec = find_movement_spec(movement);
            if (movement == MovementKind::Walk || spec == nullptr || charges < 0) {
                if (error != nullptr) {
                    *error = "movement charges contain an invalid movement or negative count";
                }
                return false;
            }
            int remaining = charges;
            const int per_piece = std::max(1, spec->initial_charges);
            while (remaining > 0) {
                observed_instances.emplace_back(
                    RunResources::MovementInstance {
                        movement,
                        std::min(remaining, per_piece),
                        static_cast<int>(observed_instances.size()),
                    });
                remaining -= per_piece;
            }
        }
        m_run.resources.movement_instances = std::move(observed_instances);
        rebuild_movement_aggregates(m_run.resources);
        if (m_run.active_movement.has_value() && *m_run.active_movement != MovementKind::Walk &&
            !m_run.resources.movement_charges.contains(*m_run.active_movement)) {
            m_run.active_movement.reset();
        }
    }
    if (observation.movement_panel.has_value()) {
        const MovementPanelObservation& panel = *observation.movement_panel;
        if (find_movement_spec(panel.target) == nullptr || !movement_panel_observation_is_structurally_valid(panel)) {
            if (error != nullptr) {
                *error = "movement panel observation is inconsistent";
            }
            return false;
        }
    }
    if (observation.active_movement.has_value()) {
        if (find_movement_spec(*observation.active_movement) == nullptr) {
            if (error != nullptr) {
                *error = "active movement observation is invalid";
            }
            return false;
        }
        m_run.active_movement = *observation.active_movement;
    }
    if (observation.cross_floor_expired.has_value()) {
        m_run.cross_floor_expired = *observation.cross_floor_expired;
    }
    if (observation.costs.has_value()) {
        std::string validation_error;
        if (!observation.costs->validate(&validation_error)) {
            if (error != nullptr) {
                *error = validation_error;
            }
            return false;
        }
        m_run.costs = *observation.costs;
    }
    if (m_run.resources != resources_before || m_run.cross_floor_expired != cross_floor_expired_before) {
        ++m_run.resources_revision;
    }
    return true;
}

void BlackFlowSession::queue_map_summary(const PerceptionSummary& summary)
{
    json::object details {
        { "run_revision", m_run_revision },
        { "observation_id", summary.observation_id },
        { "map_revision", m_map.snapshot().revision },
        { "floor", summary.floor },
        { "floor_from_ocr", summary.floor_from_ocr },
        { "current_node", summary.current_node },
        { "node_count", summary.node_count },
        { "confirmed_edge_count", summary.confirmed_edge_count },
        { "forced_edge_count", summary.forced_edge_count },
        { "unclassified_count", summary.unclassified_count },
        { "screenshot_us", summary.screenshot_us },
        { "recognition_us", summary.recognition_us },
        { "attempt_count", summary.attempt_count },
        { "retry_count", summary.retry_count },
        { "topology_template_id", summary.topology_template_id },
        { "topology_source_digest", summary.topology_source_digest },
        { "topology_base_edge_count", summary.topology_base_edge_count },
        { "topology_extra_edge_count", summary.topology_extra_edge_count },
        { "topology_match_score", summary.topology_match_score },
    };
    // 每条地图摘要事件都携带两张图的完整快照：仅保留事件流即可重建本层探索笔记，
    // 同时能够回放当时实际参与路线规划的当前观测地图。
    details["planning_map"] = serialize_map_snapshot(m_map, &m_run);
    details["exploration_notebook"] = serialize_map_snapshot(m_exploration_notebook);
    Log.info(
        "BlackFlow map summary",
        "observation",
        summary.observation_id,
        "floor",
        summary.floor,
        "floor source",
        summary.floor_from_ocr ? "ocr" : "fallback",
        "current",
        summary.current_node,
        "nodes",
        summary.node_count,
        "confirmed edges",
        summary.confirmed_edge_count,
        "inferred edges",
        summary.forced_edge_count,
        "unclassified",
        summary.unclassified_count,
        "topology",
        summary.topology_template_id,
        "base edges",
        summary.topology_base_edge_count,
        "extra edges",
        summary.topology_extra_edge_count,
        "attempts",
        summary.attempt_count,
        "retries",
        summary.retry_count,
        "screenshot us",
        summary.screenshot_us,
        "recognition us",
        summary.recognition_us);
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowMapSummary", details });

    std::vector<json::value> nodes;
    nodes.reserve(m_map.snapshot().nodes().size());
    for (const auto& [id, node] : m_map.snapshot().nodes()) {
        json::object serialized {
            { "id", id },
            { "row", node.position.row },
            { "column", node.position.column },
            { "type", std::string(to_string(node.type)) },
            { "name", node.name },
            { "identity_revealed", node.identity_revealed },
            { "visually_hidden", node.visually_hidden },
            { "identity_from_topology", node.identity_from_topology },
            { "identity_from_prediction", node.identity_from_prediction },
            { "prediction_rule", node.prediction_rule },
            { "natural_reveal_suppressed", node.natural_reveal_suppressed },
            { "existence_source", node.existence_source },
            { "identity_source", node.identity_source },
            { "detected_by_vision", node.detected_by_vision },
            { "confirmed_by_topology", node.confirmed_by_topology },
            { "identity_state",
              node.identity_state == NodeIdentityState::Classified
                  ? "classified"
                  : (node.identity_state == NodeIdentityState::Hidden ? "hidden" : "unclassified") },
            { "progress",
              node.progress == NodeProgress::Active
                  ? "active"
                  : (node.progress == NodeProgress::Completed ? "completed" : "removed") },
        };
        if (node.battle.has_value()) {
            serialized["battle"] = serialize_node_battle(*node.battle);
            if (node.battle->total_kills.has_value()) {
                serialized["battle_total_kills"] = *node.battle->total_kills;
            }
        }
        if (const NodeObservation* visual = m_viewport.find(id); visual != nullptr) {
            serialized["visual_x"] = visual->icon_rect.x + visual->icon_rect.width / 2.0;
            serialized["visual_y"] = visual->icon_rect.y + visual->icon_rect.height / 2.0;
            serialized["visual_width"] = visual->icon_rect.width;
            serialized["visual_height"] = visual->icon_rect.height;
        }
        nodes.emplace_back(std::move(serialized));
    }
    std::vector<json::value> edges;
    edges.reserve(m_map.snapshot().edges().size());
    for (const Edge& edge : m_map.snapshot().edges()) {
        edges.emplace_back(
            json::object {
                { "first", edge.first },
                { "second", edge.second },
                { "knowledge",
                  edge.knowledge == EdgeKnowledge::Confirmed
                      ? "confirmed"
                      : (edge.knowledge == EdgeKnowledge::Absent ? "absent" : "unknown") },
                { "probability", edge.evidence.probability },
                { "cnn_connected", edge.evidence.cnn_connected },
                { "forced_by_connectivity_constraint", edge.evidence.forced_by_connectivity_constraint },
                { "decision_source", edge.evidence.decision_source },
            });
    }
    json::object diagnostic = details;
    diagnostic["nodes"] = json::array(std::move(nodes));
    diagnostic["edges"] = json::array(std::move(edges));
    request_diagnostics(DiagnosticTrigger::RoutineObservation, std::move(diagnostic));
}

void BlackFlowSession::append_map_visualization(json::object& details) const
{
    std::vector<json::value> nodes;
    nodes.reserve(m_map.snapshot().nodes().size());
    for (const auto& [id, node] : m_map.snapshot().nodes()) {
        json::object serialized {
            { "id", id },
            { "row", node.position.row },
            { "column", node.position.column },
            { "node_type", std::string(to_string(node.type)) },
            { "node_name", node.name },
            { "fate_event", node.fate_event },
            { "marker_type", node.marker_type },
            { "marker_display_name", node.marker_display_name },
            { "marker_score", node.marker_score },
            { "marker_resident_overlap_possible", node.marker_resident_overlap_possible },
            { "identity_revealed", node.identity_revealed },
            { "visually_hidden", node.visually_hidden },
            { "identity_from_topology", node.identity_from_topology },
            { "identity_from_prediction", node.identity_from_prediction },
            { "prediction_rule", node.prediction_rule },
            { "natural_reveal_suppressed", node.natural_reveal_suppressed },
            { "existence_source", node.existence_source },
            { "identity_source", node.identity_source },
            { "detected_by_vision", node.detected_by_vision },
            { "confirmed_by_topology", node.confirmed_by_topology },
            { "semantically_known", node.identity_revealed },
            { "identity_state",
              node.identity_state == NodeIdentityState::Classified
                  ? "classified"
                  : (node.identity_state == NodeIdentityState::Hidden ? "hidden" : "unclassified") },
            { "revealed_now", m_run.revealed_nodes.contains(id) },
            { "blocks_vision", node.traversal.blocks_vision },
            { "is_exit", is_exit_node_type(node.type) },
            { "progress",
              node.progress == NodeProgress::Active
                  ? "active"
                  : (node.progress == NodeProgress::Completed ? "completed" : "removed") },
        };
        if (node.battle.has_value()) {
            serialized["battle"] = serialize_node_battle(*node.battle);
            if (node.battle->total_kills.has_value()) {
                serialized["battle_total_kills"] = *node.battle->total_kills;
            }
        }
        if (const NodeObservation* visual = m_viewport.find(id); visual != nullptr) {
            serialized["visual_x"] = visual->icon_rect.x + visual->icon_rect.width / 2.0;
            serialized["visual_y"] = visual->icon_rect.y + visual->icon_rect.height / 2.0;
            serialized["visual_width"] = visual->icon_rect.width;
            serialized["visual_height"] = visual->icon_rect.height;
        }
        nodes.emplace_back(std::move(serialized));
    }
    std::vector<json::value> edges;
    edges.reserve(m_map.snapshot().edges().size());
    for (const Edge& edge : m_map.snapshot().edges()) {
        edges.emplace_back(
            json::object {
                { "first", edge.first },
                { "second", edge.second },
                { "knowledge",
                  edge.knowledge == EdgeKnowledge::Confirmed
                      ? "confirmed"
                      : (edge.knowledge == EdgeKnowledge::Absent ? "absent" : "unknown") },
                { "forced", edge.evidence.forced_by_connectivity_constraint },
                { "probability", edge.evidence.probability },
                { "cnn_connected", edge.evidence.cnn_connected },
                { "decision_source", edge.evidence.decision_source },
            });
    }
    details["map_nodes"] = json::array(std::move(nodes));
    details["map_edges"] = json::array(std::move(edges));
    details["map_kind"] = "current_observation";
    details["exploration_note_nodes"] = serialize_map_nodes(m_exploration_notebook.snapshot());
    details["exploration_note_edges"] = serialize_map_edges(m_exploration_notebook.snapshot());
    details["planning_map_revision"] = m_map.snapshot().revision;
    details["exploration_notebook_revision"] = m_exploration_notebook.snapshot().revision;
    details["topology_template_id"] = m_topology_template_id;
    details["topology_source_digest"] = m_topology_source_digest;
    details["topology_base_edge_count"] = m_topology_base_edge_count;
    details["topology_extra_edge_count"] = m_topology_extra_edge_count;
    details["topology_match_score"] = m_topology_match_score;
    details["utopia_status"] = m_utopia_status;
    details["utopia_reason"] = m_utopia_reason;
    details["utopia_ideology"] = m_utopia_ideology;
    details["utopia_policy"] = m_utopia_policy;
    details["ideal_source_score_margin"] = m_ideal_source_score_margin;
    details["ideal_source_heads_agree"] = m_ideal_source_heads_agree;
    const auto serialize_positions = [](const std::vector<GridPosition>& positions) {
        std::vector<json::value> values;
        values.reserve(positions.size());
        for (const GridPosition position : positions) {
            values.emplace_back(json::object { { "row", position.row }, { "column", position.column } });
        }
        return json::array(std::move(values));
    };
    if (m_ideal_source.has_value()) {
        details["ideal_source"] =
            json::object { { "row", m_ideal_source->row }, { "column", m_ideal_source->column } };
    }
    details["ideal_domain"] = serialize_positions(m_ideal_domain);
    details["observed_ideal_domain"] = serialize_positions(m_observed_ideal_domain);
    details["resident_settlement_candidates"] = serialize_positions(m_resident_settlement_prediction.candidates);
    details["resident_settlement_initial_residents"] =
        serialize_positions(m_resident_settlement_prediction.initial_residents);
    details["resident_settlement_possible_overlap_residents"] =
        serialize_positions(m_resident_settlement_prediction.possible_overlap_residents);
    details["resident_settlement_hypothesis_count"] = m_resident_settlement_prediction.hypothesis_count;
    if (m_resident_settlement_prediction.exact.has_value()) {
        details["resident_settlement_exact"] = json::object {
            { "row", m_resident_settlement_prediction.exact->row },
            { "column", m_resident_settlement_prediction.exact->column },
        };
    }
}

void BlackFlowSession::request_diagnostics(
    DiagnosticTrigger trigger,
    json::object snapshot,
    std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images)
{
    const bool routine = trigger == DiagnosticTrigger::RoutineObservation;
    const bool processing_item_observation = trigger == DiagnosticTrigger::ProcessingItemObservation;
    const bool routing_history_item =
        trigger == DiagnosticTrigger::RoutingDecision || trigger == DiagnosticTrigger::BattleStageObservation ||
        trigger == DiagnosticTrigger::NodeIdentityResolved;
    if (routine && m_diagnostics.level == DiagnosticLevel::Normal) {
        return;
    }
    // 加工品证据来自移动面板/背包自己的 OCR ROI，不能冒充最近一次地图截图证据。
    // 路线 HTML 的每一步都需要底图；图片包上限只限制体积更大的识别叠加图。
    const bool wants_map_images =
        !processing_item_observation && (!routine || m_diagnostics.level == DiagnosticLevel::Full);
    const DiagnosticImageSelection image_selection = diagnostic_image_selection(
        wants_map_images,
        routing_history_item,
        m_persisted_image_packages,
        m_diagnostics.image_package_limit);
    if (!image_selection.captured && !image_selection.overlay && m_diagnostics.level == DiagnosticLevel::Normal) {
        return;
    }
    const std::string artifact_id =
        "BF-A" + std::to_string(m_run_revision) + "-" + std::to_string(++m_artifact_sequence);
    snapshot["trigger"] = std::string(to_string(trigger));
    snapshot["run_revision"] = m_run_revision;
    snapshot["map_revision"] = m_map.snapshot().revision;
    snapshot["map_generation"] = m_map_generation;
    snapshot["floor_four_remembrance"] = m_current_map_is_floor_four_remembrance;
    const int artifact_floor = snapshot.get("floor", m_run.floor);
    snapshot["map_section_key"] = diagnostic_map_section_key(
        artifact_floor,
        m_map_generation,
        m_current_map_is_floor_four_remembrance);
    snapshot["map_section_label"] =
        diagnostic_map_section_label(artifact_floor, m_current_map_is_floor_four_remembrance);
    m_diagnostic_requests.emplace_back(
        DiagnosticArtifactRequest {
            trigger,
            artifact_id,
            m_observation_id,
            m_decision_id,
            m_transaction_id,
            image_selection.captured,
            image_selection.overlay,
            std::move(snapshot),
            std::move(evidence_images),
        });
    if (image_selection.overlay) {
        ++m_persisted_image_packages;
    }
}

void BlackFlowSession::record_processing_item_evidence(
    json::object evidence,
    std::vector<DiagnosticArtifactRequest::EvidenceImage> evidence_images)
{
    evidence["floor"] = diagnostic_processing_item_floor(m_run.floor);
    if (const MovementSpec* active = m_run.active_movement.has_value()
                                         ? find_movement_spec(*m_run.active_movement)
                                         : nullptr;
        active != nullptr) {
        evidence["session_active_movement"] = std::string(to_string(active->kind));
        evidence["session_active_movement_name"] = std::string(active->name);
    }
    request_diagnostics(
        DiagnosticTrigger::ProcessingItemObservation,
        std::move(evidence),
        std::move(evidence_images));
}

void BlackFlowSession::queue_warning(
    std::string code,
    std::string message,
    DiagnosticTrigger trigger,
    json::object evidence)
{
    json::object details {
        { "run_revision", m_run_revision },
        { "observation_id", m_observation_id },
        { "decision_id", m_decision_id },
        { "transaction_id", m_transaction_id },
        { "map_revision", m_map.snapshot().revision },
        { "code", code },
        { "message", message },
    };
    for (auto& [key, value] : evidence) {
        details[key] = std::move(value);
    }
    Log.warn("BlackFlow routing warning", code, message);
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowRoutingWarning", details });
    request_diagnostics(trigger, std::move(details));
}

void BlackFlowSession::queue_decision(
    const MoveCandidate* proposal_without_transaction,
    std::string failure_code,
    std::string failure_message)
{
    if ((!m_transaction.has_value() && proposal_without_transaction == nullptr) || !m_last_plan.has_value()) {
        return;
    }
    const MoveCandidate& move = m_transaction.has_value() ? m_transaction->proposal() : *proposal_without_transaction;
    const Node* target = m_map.snapshot().find_node(move.target);
    const int cost = m_transaction.has_value() ? m_transaction->authoritative_cost() : move.predicted_action_point_cost;
    const int expected_after =
        action_points_after(m_run.resources.action_points, cost, move.predicted_action_point_gain);
    const int requirement = move.action_point_requirement;
    const int margin = requirement >= UnreachableActionPointRequirement ? -UnreachableActionPointRequirement
                                                                        : m_run.resources.action_points - requirement;
    m_decision_id = "BF-D" + std::to_string(m_run_revision) + "-" + std::to_string(++m_decision_sequence);

    std::string reason_detail;
    const PolicyDecision& decision = m_last_plan->decision;
    if (!decision.decisive_rule_id.empty() && m_policy.has_value()) {
        const auto found = std::ranges::find(m_policy->rules, decision.decisive_rule_id, &PolicyRule::id);
        if (found != m_policy->rules.end()) {
            reason_detail = found->description;
        }
    }
    if (decision.decisive_rule_id == "revealed_node_count") {
        reason_detail = "在保留完整安全路线的前提下，优先探明更多未知节点";
    }
    else if (decision.decisive_rule_id == "effective_node_count") {
        reason_detail = "在保留完整安全路线的前提下，优先提高不重复有效落点计分";
    }
    else if (decision.decisive_rule_id == "persistent_processing_move_count") {
        reason_detail = "优先保留可以带到后续层数使用的加工品";
    }
    else if (decision.decisive_rule_id == "processing_move_count") {
        reason_detail = m_profile == "automation_collection" ? "跨层加工品用量相同时，优先使用本层结束后会失效的加工品"
                                                             : "优先减少加工品的总用量";
    }
    else if (decision.decisive_rule_id == "route_length") {
        reason_detail = "探明收益和加工品价值相同时，优先选择更短的路线";
    }
    else if (decision.decisive_rule_id == "movement_action_count") {
        reason_detail = "路线长度相同时，优先用更少的实际移动次数完成";
    }
    else if (decision.decisive_rule_id == "exchangeable_processing_value_order") {
        reason_detail = "整条路线安全结局不变时，优先消耗能力较弱或同能力下剩余次数更少的加工品";
    }
    else if (decision.decisive_rule_id == "direct_exhaustion") {
        reason_detail = "没有剩余探明或有效节点收益，直接耗尽行动力进入追猎";
    }
    else if (decision.decisive_rule_id.starts_with("processing_semantic.")) {
        const std::string movement_id = decision.decisive_rule_id.substr(std::string("processing_semantic.").size());
        const auto movement = std::ranges::find_if(movement_specs(), [&](const MovementSpec& spec) {
            return to_string(spec.kind) == movement_id;
        });
        reason_detail = "加工品类别和总用量相同时，优先使用移动能力较弱、价值较低的加工品";
        if (movement != movement_specs().end()) {
            reason_detail += "；本次产生差异的是“" + std::string(movement->name) + "”";
        }
    }
    if (!decision.decisive_milestone_ids.empty() && m_policy.has_value()) {
        for (const std::string& milestone_id : decision.decisive_milestone_ids) {
            const auto found = std::ranges::find(m_policy->milestones, milestone_id, &Milestone::id);
            if (found == m_policy->milestones.end()) {
                continue;
            }
            if (!reason_detail.empty()) {
                reason_detail += "; ";
            }
            reason_detail += found->description;
        }
    }
    if (reason_detail.empty()) {
        reason_detail = decision.reason == "no eligible safe candidate" ? "没有符合约束且能安全完成路线的候选"
                        : decision.reason == "selected by lexicographic policy order" ? "按照既定优先级逐项比较后选出"
                                                                                      : decision.reason;
    }

    std::vector<json::value> runners_up;
    for (const MoveCandidate& runner : decision.runners_up) {
        const Node* runner_target = m_map.snapshot().find_node(runner.target);
        runners_up.emplace_back(
            json::object {
                { "action_id", runner.action_id },
                { "target", runner.target },
                { "node_type",
                  std::string(runner_target == nullptr ? "unclassified" : to_string(runner_target->type)) },
                { "predicted_cost", runner.predicted_action_point_cost },
            });
    }
    json::object planned_progress;
    for (const auto& [milestone, progress] : decision.planned_milestone_progress) {
        planned_progress[milestone] = progress;
    }
    json::object rejected;
    for (const auto& [category, count] : decision.rejection_counts) {
        rejected[category] = count;
    }
    std::vector<json::value> decisive_milestones;
    decisive_milestones.reserve(decision.decisive_milestone_ids.size());
    for (const std::string& milestone_id : decision.decisive_milestone_ids) {
        decisive_milestones.emplace_back(milestone_id);
    }
    std::vector<json::value> released_reserves;
    released_reserves.reserve(decision.released_reserve_ids.size());
    for (const std::string& reserve_id : decision.released_reserve_ids) {
        released_reserves.emplace_back(reserve_id);
    }
    const auto serialize_landing_semantics = [](const MoveCandidate& candidate) -> json::value {
        std::vector<json::value> landings;
        landings.reserve(candidate.possible_landings.size());
        for (const NodeId landing : candidate.possible_landings) {
            landings.emplace_back(
                json::object {
                    { "node", landing },
                    { "node_type", std::string(to_string(move_landing_type(candidate, landing))) },
                    { "terminal", move_landing_is_terminal(candidate, landing) },
                    { "page_intent",
                      move_landing_is_terminal(candidate, landing)
                          ? (candidate.bypass_final_on_completion ? "final.pass" : "final.advance")
                          : "default" },
                });
        }
        return json::array(std::move(landings));
    };
    const bool can_land_on_final = std::ranges::any_of(
        move.possible_landings,
        [&](NodeId landing) { return move_landing_type(move, landing) == NodeType::Final; });

    json::object details {
        { "run_revision", m_run_revision },
        { "observation_id", m_observation_id },
        { "decision_id", m_decision_id },
        { "profile", m_profile },
        { "transaction_id", m_transaction_id },
        { "map_revision", m_map.snapshot().revision },
        { "floor", m_run.floor },
        { "source", move.source },
        { "target", move.target },
        { "landing", move.landing },
        { "node_name", target == nullptr ? std::string() : target->name },
        { "node_type", std::string(target == nullptr ? "unclassified" : to_string(target->type)) },
        { "movement", std::string(to_string(move.movement)) },
        { "direct_exhaustion", move.direct_exhaustion },
        { "terminal_on_completion", move.terminal_on_completion },
        { "bypass_final_on_completion", move.bypass_final_on_completion },
        { "landing_semantics", serialize_landing_semantics(move) },
        { "final_disposition",
          can_land_on_final ? (move.bypass_final_on_completion ? "pass" : "advance") : "not_applicable" },
        { "path_edge_count", move.path.size() },
        { "predicted_cost", move.predicted_action_point_cost },
        { "exact_cost", cost },
        { "action_points_before", m_run.resources.action_points },
        { "action_points_after", expected_after },
        { "safe_requirement", requirement },
        { "safety_margin", margin },
        { "reason_category", std::string(to_string(decision.reason_category)) },
        { "reason_detail", reason_detail },
        { "decisive_rule_id", decision.decisive_rule_id },
        { "decisive_milestone_id", decision.decisive_milestone_id },
        { "decisive_milestone_ids", json::array(std::move(decisive_milestones)) },
        { "total_candidates", decision.total_candidates },
        { "eligible_candidates", decision.eligible_candidates },
        { "rejection_counts", std::move(rejected) },
        { "released_reserve_ids", json::array(std::move(released_reserves)) },
        { "runners_up", json::array(std::move(runners_up)) },
        { "planned_milestone_progress", std::move(planned_progress) },
        { "uses_inferred_edge", move.uses_inferred_edge },
        { "confirmed_state_count", static_cast<std::int64_t>(m_last_plan->confirmed_state_count) },
        { "relaxed_state_count", static_cast<std::int64_t>(m_last_plan->relaxed_state_count) },
        { "route_search_expansions", static_cast<std::int64_t>(m_last_plan->route_search_expansions) },
        { "planning_elapsed_us", static_cast<std::int64_t>(m_last_plan->planning_elapsed_us) },
        { "graph_initialization_elapsed_us",
          static_cast<std::int64_t>(m_last_plan->graph_initialization_elapsed_us) },
        { "confirmed_safety_elapsed_us",
          static_cast<std::int64_t>(m_last_plan->confirmed_safety_elapsed_us) },
        { "relaxed_safety_elapsed_us", static_cast<std::int64_t>(m_last_plan->relaxed_safety_elapsed_us) },
        { "route_search_elapsed_us", static_cast<std::int64_t>(m_last_plan->route_search_elapsed_us) },
        { "planning_graphs_shared", m_last_plan->planning_graphs_shared },
        { "route_labels_generated", static_cast<std::int64_t>(m_last_plan->route_labels_generated) },
        { "route_labels_dominated", static_cast<std::int64_t>(m_last_plan->route_labels_dominated) },
        { "route_labels_retained_peak", static_cast<std::int64_t>(m_last_plan->route_labels_retained_peak) },
        { "route_trace_nodes", static_cast<std::int64_t>(m_last_plan->route_trace_nodes) },
        { "route_root_action_count", static_cast<std::int64_t>(m_last_plan->route_root_action_count) },
        { "route_search_time_budget_ms", m_last_plan->route_search_time_budget_ms },
        { "route_search_total_expansions",
          static_cast<std::int64_t>(m_last_plan->route_search_total_expansions) },
        { "route_search_expansions_per_root",
          static_cast<std::int64_t>(m_last_plan->route_search_expansions_per_root) },
        { "route_search_time_exhausted", m_last_plan->route_search_time_exhausted },
        { "route_search_expansions_exhausted", m_last_plan->route_search_expansions_exhausted },
        { "route_hint_root_matched", m_last_plan->route_hint_root_matched },
        { "route_hint_replayed_steps", static_cast<std::int64_t>(m_last_plan->route_hint_replayed_steps) },
        { "endpoint_required", !m_last_plan->no_AP_is_terminal },
        { "endpoint_fallback_active", m_last_plan->endpoint_fallback_active },
        { "endpoint_fallback_reason", m_last_plan->endpoint_fallback_reason },
        { "mobile_marker_lookahead_active", m_last_plan->mobile_marker_lookahead_active },
        { "mobile_marker_lookahead_fallback_active",
          m_last_plan->mobile_marker_lookahead_fallback_active },
        { "mobile_marker_lookahead_rejected_candidates",
          static_cast<std::int64_t>(m_last_plan->mobile_marker_lookahead_rejected_candidates) },
        { "mobile_marker_outcomes_checked",
          static_cast<std::int64_t>(m_last_plan->mobile_marker_outcomes_checked) },
        { "automation_collection_full_map_count",
          m_resources.read("automation_collection_full_map_movement", m_run).value_or(0) },
        { "reserved_full_map_charges",
          m_profile == "automation_collection" && m_run.floor >= 1 && m_run.floor <= 3 ? 4 - m_run.floor : 0 },
    };
    if (!failure_code.empty()) {
        details["failure_code"] = std::move(failure_code);
    }
    if (!failure_message.empty()) {
        details["planning_error"] = std::move(failure_message);
    }
    if (m_last_reveal_consistency.has_value()) {
        details["previous_move_reveal_consistency"] = *m_last_reveal_consistency;
    }
    if (includes_full_routing_details(m_diagnostics.level)) {
        std::vector<json::value> forbidden_node_types;
        if (m_profile == "automation_collection") {
            std::vector<std::string> names;
            for (const NodeType type : automation_collection_forbidden_landing_types(m_run.floor)) {
                names.emplace_back(to_string(type));
            }
            std::ranges::sort(names);
            for (std::string& name : names) {
                forbidden_node_types.emplace_back(std::move(name));
            }
        }
        details["forbidden_node_types"] = json::array(std::move(forbidden_node_types));
        std::vector<json::value> future_forbidden_node_types;
        if (m_profile == "automation_collection") {
            std::vector<std::string> names;
            for (const NodeType type :
                 future_forbidden_landing_types(automation_collection_forbidden_landing_types(m_run.floor))) {
                names.emplace_back(to_string(type));
            }
            std::ranges::sort(names);
            for (std::string& name : names) {
                future_forbidden_node_types.emplace_back(std::move(name));
            }
        }
        details["future_forbidden_node_types"] = json::array(std::move(future_forbidden_node_types));
        std::vector<json::value> root_forbidden_marker_types;
        if (m_profile == "automation_collection" && m_run.floor >= 2) {
            root_forbidden_marker_types.emplace_back("savage");
        }
        details["root_forbidden_marker_types"] = json::array(std::move(root_forbidden_marker_types));
        const auto node_details = [&](NodeId id) {
            const Node* node = m_map.snapshot().find_node(id);
            if (node == nullptr) {
                return json::object { { "id", id } };
            }
            return json::object {
                { "id", id },
                { "row", node->position.row },
                { "column", node->position.column },
                { "node_type", std::string(to_string(node->type)) },
                { "node_name", node->name },
                { "marker_type", node->marker_type },
                { "marker_display_name", node->marker_display_name },
                { "marker_resident_overlap_possible", node->marker_resident_overlap_possible },
            };
        };
        const auto serialize_route_steps = [&](const std::vector<PlannedRouteStep>& steps) -> json::value {
            std::vector<json::value> serialized;
            serialized.reserve(steps.size());
            for (const PlannedRouteStep& step : steps) {
                std::vector<json::value> path;
                path.reserve(step.move.path.size());
                for (const NodeId node : step.move.path) {
                    path.emplace_back(node_details(node));
                }
                const MovementSpec* movement = find_movement_spec(step.move.movement);
                serialized.emplace_back(
                    json::object {
                        { "action_id", step.move.action_id },
                        { "movement", std::string(to_string(step.move.movement)) },
                        { "movement_name", movement == nullptr ? std::string {} : std::string(movement->name) },
                        { "direct_exhaustion", step.move.direct_exhaustion },
                        { "source", node_details(step.move.source) },
                        { "target", node_details(step.move.target) },
                        { "landing", node_details(step.move.landing) },
                        { "path", json::array(std::move(path)) },
                        { "action_point_requirement", step.move.action_point_requirement },
                        { "action_points_before", step.action_points_before },
                        { "action_point_cost", step.action_point_cost },
                        { "action_point_gain", step.action_point_gain },
                        { "action_points_after", step.action_points_after },
                        { "uses_processing_item", step.move.movement != MovementKind::Walk },
                        { "uses_inferred_edge", step.move.uses_inferred_edge },
                        { "terminal_on_completion", step.move.terminal_on_completion },
                        { "bypass_final_on_completion", step.move.bypass_final_on_completion },
                        { "landing_semantics", serialize_landing_semantics(step.move) },
                    });
            }
            return json::array(std::move(serialized));
        };
        details["planned_route_steps"] = serialize_route_steps(decision.planned_route_steps);

        std::vector<json::value> processing_item_catalog;
        std::vector<json::value> processing_items;
        for (const MovementSpec& movement : movement_specs()) {
            if (movement.kind == MovementKind::Walk) {
                continue;
            }
            processing_item_catalog.emplace_back(
                json::object {
                    { "movement", std::string(to_string(movement.kind)) },
                    { "name", std::string(movement.name) },
                    { "expires_on_floor_end", movement.expires_on_floor_end },
                });
        }
        for (const RunResources::MovementInstance& instance : m_run.resources.movement_instances) {
            const MovementSpec* movement = find_movement_spec(instance.movement);
            if (movement == nullptr || movement->kind == MovementKind::Walk) {
                continue;
            }
            processing_items.emplace_back(
                json::object {
                    { "movement", std::string(to_string(movement->kind)) },
                    { "name", std::string(movement->name) },
                    { "instance_index", instance.inventory_index },
                    { "charges", instance.remaining_charges },
                    { "pieces", 1 },
                    { "active", instance.loaded },
                    { "available", instance.remaining_charges > 0 },
                    { "expires_on_floor_end", movement->expires_on_floor_end },
                    { "action_point_cost", m_run.costs.movement_cost(*movement) },
                });
        }
        details["processing_item_catalog"] = json::array(std::move(processing_item_catalog));
        details["processing_items"] = json::array(std::move(processing_items));

        std::vector<json::value> candidate_comparison;
        candidate_comparison.reserve(decision.candidate_summaries.size());
        std::size_t candidate_rank = 0;
        for (const PolicyCandidateSummary& candidate : decision.candidate_summaries) {
            const Node* candidate_target = m_map.snapshot().find_node(candidate.move.target);
            std::vector<json::value> score;
            score.reserve(candidate.lexicographic_score.size());
            for (const int dimension : candidate.lexicographic_score) {
                score.emplace_back(dimension);
            }
            std::vector<json::value> expected_score_sum;
            expected_score_sum.reserve(candidate.expected_lexicographic_score_sum.size());
            for (const std::int64_t dimension : candidate.expected_lexicographic_score_sum) {
                expected_score_sum.emplace_back(dimension);
            }
            std::vector<json::value> score_labels;
            score_labels.reserve(candidate.lexicographic_score_labels.size());
            for (const std::string& label : candidate.lexicographic_score_labels) {
                score_labels.emplace_back(label);
            }
            std::vector<json::value> revealed_nodes;
            revealed_nodes.reserve(candidate.revealed_nodes.size());
            for (const NodeId node : candidate.revealed_nodes) {
                revealed_nodes.emplace_back(node_details(node));
            }
            json::object processing_move_counts;
            for (const MovementKind movement : ProcessingMovementStrengthOrder) {
                processing_move_counts[std::string(to_string(movement))] =
                    candidate.processing_move_counts[static_cast<std::size_t>(movement)];
            }
            std::vector<json::value> effective_node_details;
            std::unordered_set<NodeId> effective_nodes;
            for (const PlannedRouteStep& step : candidate.planned_route_steps) {
                const NodeId landing = step.move.landing != InvalidNodeId ? step.move.landing : step.move.target;
                const Node* landing_node = m_map.snapshot().find_node(landing);
                if (landing_node == nullptr) {
                    continue;
                }
                const int weight = record_effective_landing(
                    landing,
                    landing_node->type,
                    landing_node->marker_type,
                    step.move.movement,
                    m_run.current_node,
                    m_run.visited_nodes,
                    effective_nodes,
                    landing_node->floor);
                if (weight > 0) {
                    effective_node_details.emplace_back(
                        json::object {
                            { "node", node_details(landing) },
                            { "weight", weight },
                            { "movement", std::string(to_string(step.move.movement)) },
                            { "knot_tentacle_bonus", step.move.movement == MovementKind::M10 ? 1 : 0 },
                        });
                }
            }
            candidate_comparison.emplace_back(
                json::object {
                    { "rank", ++candidate_rank },
                    { "selected", candidate.move.action_id == move.action_id },
                    { "action_id", candidate.move.action_id },
                    { "target", candidate.move.target },
                    { "node_type",
                      std::string(candidate_target == nullptr ? "unclassified" : to_string(candidate_target->type)) },
                    { "movement", std::string(to_string(candidate.move.movement)) },
                    { "direct_exhaustion", candidate.move.direct_exhaustion },
                    { "terminal_on_completion", candidate.move.terminal_on_completion },
                    { "bypass_final_on_completion", candidate.move.bypass_final_on_completion },
                    { "safe_requirement", candidate.move.action_point_requirement },
                    { "revealed_node_count", candidate.revealed_node_count },
                    { "effective_node_count", candidate.effective_node_count },
                    { "effective_node_details", json::array(std::move(effective_node_details)) },
                    { "battle_count", candidate.battle_count },
                    { "processing_move_count", candidate.processing_move_count },
                    { "persistent_processing_move_count", candidate.persistent_processing_move_count },
                    { "processing_move_counts", std::move(processing_move_counts) },
                    { "route_length", candidate.route_length },
                    { "risk_score", candidate.risk_score },
                    { "lexicographic_score", json::array(std::move(score)) },
                    { "expected_lexicographic_score_sum", json::array(std::move(expected_score_sum)) },
                    { "route_outcome_count", candidate.route_outcome_count },
                    { "lexicographic_score_labels", json::array(std::move(score_labels)) },
                    { "planned_route_steps", serialize_route_steps(candidate.planned_route_steps) },
                    { "revealed_nodes", json::array(std::move(revealed_nodes)) },
                    { "route_deterministic", !candidate.planned_route_steps.empty() },
                });
        }
        details["candidate_comparison"] = json::array(std::move(candidate_comparison));

        std::vector<json::value> rejected_candidates;
        rejected_candidates.reserve(decision.rejected.size());
        for (const std::string& rejected_candidate : decision.rejected) {
            rejected_candidates.emplace_back(rejected_candidate);
        }
        details["rejected_candidates"] = json::array(std::move(rejected_candidates));
        append_map_visualization(details);
        request_diagnostics(DiagnosticTrigger::RoutingDecision, details);
    }
    Log.info(
        "BlackFlow decision",
        m_decision_id,
        "profile",
        m_profile,
        "rule",
        decision.decisive_rule_id,
        "milestone",
        decision.decisive_milestone_id,
        "floor",
        m_run.floor,
        "target",
        move.target,
        "cost",
        cost,
        "margin",
        margin,
        "reason",
        to_string(decision.reason_category),
        "confirmed states",
        m_last_plan->confirmed_state_count,
        "relaxed states",
        m_last_plan->relaxed_state_count,
        "shared graphs",
        m_last_plan->planning_graphs_shared,
        "route expansions",
        m_last_plan->route_search_expansions,
        "route labels",
        m_last_plan->route_labels_generated,
        "dominated labels",
        m_last_plan->route_labels_dominated,
        "time exhausted",
        m_last_plan->route_search_time_exhausted,
        "expansions exhausted",
        m_last_plan->route_search_expansions_exhausted);
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowRoutingDecision", std::move(details) });
    if (move.uses_inferred_edge) {
        queue_warning(
            "inferred_edge_selected",
            "selected action depends on a connectivity-constraint edge",
            DiagnosticTrigger::InferredEdgeSelected);
    }
}

bool BlackFlowSession::merge_perception(
    const BlackFlowMapObservation& observation,
    const RunObservation& run,
    const FactStore& observed_facts,
    bool reconcile_move,
    std::string* error)
{
    auto normalized = m_observation_adapter.normalize(observation, error);
    if (!normalized.has_value()) {
        return false;
    }

    const bool initial_prediction_for_generation = m_initial_prediction_generation != m_map_generation;
    if (initial_prediction_for_generation) {
        m_resident_settlement_prediction = predict_resident_settlement(
            normalized->map,
            normalized->current_node,
            m_current_map_is_floor_four_remembrance);
    }
    // 每次观测都会从视觉结果重新构造 MapObservationBatch；尚未视觉揭示的据点会再次
    // 表现为 hide_battle。把本地图代已经确定的预测重新写入这一拍，避免 merge 后丢失
    // identity_from_prediction，并确保它不再计入待探明节点或自然揭示预期。
    apply_exact_resident_settlement_prediction(normalized->map, m_resident_settlement_prediction);

    std::vector<json::object> prediction_conflicts;
    for (const ObservedNode& observed : normalized->map.nodes) {
        const auto id = make_stable_node_id(normalized->map.floor, observed.position);
        const Node* predicted = id.has_value() ? m_map.snapshot().find_node(*id) : nullptr;
        const bool resolved_current_landing =
            reconcile_move && id.has_value() && *id == normalized->current_node && m_page_context.has_value() &&
            m_page_context->has_landing && m_page_context->stage == PageExecutionStage::Resolved &&
            (m_page_context->node == *id ||
             (m_page_context->linked_transfer_type.has_value() &&
              std::ranges::find(m_page_context->linked_transfer_targets, *id) !=
                  m_page_context->linked_transfer_targets.end()));
        if (predicted == nullptr ||
            !deterministic_prediction_conflicts_with_observation(*predicted, observed, resolved_current_landing)) {
            continue;
        }
        prediction_conflicts.emplace_back(
            json::object {
                { "node", *id },
                { "row", observed.position.row },
                { "column", observed.position.column },
                { "rule", predicted->prediction_rule },
                { "predicted_type", std::string(to_string(predicted->type)) },
                { "observed_type", std::string(to_string(*observed.type)) },
                { "observed_identity_source", observed.identity_source.value_or("") },
                { "observed_detected_by_vision", observed.detected_by_vision.value_or(false) },
            });
        if (predicted->prediction_rule == "initial_roaming_resident_settlement") {
            (void)reject_resident_settlement_prediction(m_resident_settlement_prediction, observed.position);
        }
    }

    const NodeId previous_current_node = m_observed_current_node;
    const bool new_floor = m_run.floor != 0 && m_run.floor != normalized->map.floor;
    if (new_floor) {
        m_movement_inventory_refresh_required = next_movement_inventory_refresh_state(
            m_movement_inventory_refresh_required,
            MovementInventoryRefreshEvent::FloorEntered);
        m_facts.begin_floor();
        m_unreachable_actions.clear();
        m_temporarily_unavailable_movements.clear();
        m_pending_probe_target.reset();
        m_verified_move_arc.reset();
        m_pending_candidate.reset();
        if (!reconcile_move) {
            m_transaction.reset();
            m_page_context.reset();
        }
    }
    else {
        m_facts.begin_page();
    }
    if (m_utopia_effect_expired) {
        // 实托邦中心结算后理想域立即消失。识别器在同一地图代次仍可能从缓存特效
        // 重新给节点打上弥散虚雾标记，合并前必须显式写 false 清掉旧状态。
        for (ObservedNode& node : normalized->map.nodes) {
            node.natural_reveal_suppressed = false;
        }
    }
    // 两张地图消费同一批观测，但合并语义不同：当前地图允许空地覆盖旧身份并供规划使用；
    // 探索笔记保留已经揭示的身份和内容，仅用于诊断、数据收集和事件流重建。
    if (!m_map.merge(normalized->map, MapMergePurpose::CurrentObservation, error) ||
        !m_exploration_notebook.merge(normalized->map, MapMergePurpose::ExplorationNotebook, error)) {
        return false;
    }
    if (m_map.snapshot().find_node(normalized->current_node) == nullptr) {
        if (error != nullptr) {
            *error = "normalized current node is absent from the merged map";
        }
        return false;
    }

    m_observed_current_node = normalized->current_node;
    m_observation_id = normalized->summary.observation_id.empty()
                           ? "BF-O" + std::to_string(m_run_revision) + "-" + std::to_string(observation.sequence)
                           : normalized->summary.observation_id;
    normalized->summary.observation_id = m_observation_id;
    m_topology_template_id = normalized->summary.topology_template_id;
    m_topology_source_digest = normalized->summary.topology_source_digest;
    m_topology_base_edge_count = normalized->summary.topology_base_edge_count;
    m_topology_extra_edge_count = normalized->summary.topology_extra_edge_count;
    m_topology_match_score = normalized->summary.topology_match_score;
    if (!m_utopia_effect_expired) {
        const bool acquire_ideal_source =
            observation.ideal_source.has_value() &&
            (!m_ideal_source.has_value() || m_ideal_source_generation != m_map_generation);
        const bool confirms_stable_ideal_source =
            observation.ideal_source.has_value() && m_ideal_source.has_value() &&
            *observation.ideal_source == *m_ideal_source && m_ideal_source_generation == m_map_generation;
        if (acquire_ideal_source || confirms_stable_ideal_source) {
            m_utopia_status = observation.utopia_status;
            m_utopia_reason = observation.utopia_reason;
            m_utopia_ideology = observation.utopia_ideology;
            m_utopia_policy = observation.utopia_policy;
            if (acquire_ideal_source) {
                m_ideal_source = observation.ideal_source;
                m_ideal_source_generation = m_map_generation;
                m_ideal_domain = observation.ideal_domain;
            }
            m_observed_ideal_domain = observation.observed_ideal_domain;
            m_ideal_source_score_margin = observation.ideal_source_score_margin;
            m_ideal_source_heads_agree = observation.ideal_source_heads_agree;
        }
        else if (!m_ideal_source.has_value()) {
            // 本地图尚未确认理想源时保留拒识原因；一旦确认，后续单帧拒识不得清空
            // 同图状态，否则中心结算和弥散虚雾生命周期都会丢失锚点。
            m_utopia_status = observation.utopia_status;
            m_utopia_reason = observation.utopia_reason;
            m_utopia_ideology = observation.utopia_ideology;
            m_utopia_policy = observation.utopia_policy;
        }
    }
    if (initial_prediction_for_generation) {
        m_initial_prediction_generation = m_map_generation;
    }
    m_viewport.replace(std::move(normalized->viewport), m_map.snapshot().revision, normalized->viewport_revision);

    if (normalized->map.coverage == ObservationCoverage::FullMap &&
        (!m_initial_reveal_checked_generation.has_value() ||
         *m_initial_reveal_checked_generation != m_map_generation)) {
        const bool swaddled_eagle_full_reveal = has_start_reward("襁褓骏鹰") &&
                                                normalized->map.floor >= 1 && normalized->map.floor <= 2;
        const auto expected = expected_initial_floor_reveals(
            m_map.snapshot(),
            normalized->current_node,
            swaddled_eagle_full_reveal);
        const auto observed = observed_initial_floor_reveals(m_map.snapshot(), normalized->current_node);
        const RevealConsistencyResult consistency = compare_move_reveals(expected, observed);
        const auto serialize_ids = [](const auto& ids) {
            std::vector<json::value> result;
            result.reserve(ids.size());
            for (const NodeId id : ids) {
                result.emplace_back(id);
            }
            return json::array(std::move(result));
        };
        json::object consistency_details {
            { "scope", "floor_entry" },
            { "floor", normalized->map.floor },
            { "map_generation", m_map_generation },
            { "floor_four_remembrance", m_current_map_is_floor_four_remembrance },
            { "entrance", normalized->current_node },
            { "start_rewards", json::array(m_start_rewards) },
            { "swaddled_eagle_full_reveal", swaddled_eagle_full_reveal },
            { "initial_light_manhattan_radius", InitialLightRevealRadius },
            { "expected_revealed_nodes", serialize_ids(expected) },
            { "observed_revealed_nodes", serialize_ids(observed) },
            { "missing_revealed_nodes", serialize_ids(consistency.missing) },
            { "unexpected_revealed_nodes", serialize_ids(consistency.unexpected) },
            { "consistency", consistency.consistent() ? "一致" : "不一致" },
            { "correction", "" },
        };
        m_initial_reveal_checked_generation = m_map_generation;
        m_last_reveal_consistency = consistency_details;
        m_telemetry_events.emplace_back(
            BlackFlowTelemetryEvent { "BlackFlowInitialRevealConsistency", consistency_details });
        if (!consistency.consistent()) {
            append_map_visualization(consistency_details);
            queue_warning(
                "reveal_consistency_mismatch",
                "进入新地图时的实际视野与初始揭示规则不一致；已采用当前观测并重新规划",
                DiagnosticTrigger::PostMoveMismatch,
                std::move(consistency_details));
        }
    }

    const bool routing_context_changed =
        !reconcile_move && previous_current_node != InvalidNodeId && previous_current_node != normalized->current_node;
    if (routing_context_changed) {
        m_run.costs.clear_action_cost_overrides();
        m_unreachable_actions.clear();
        m_pending_probe_target.reset();
        m_verified_move_arc.reset();
        m_pending_candidate.reset();
        if (m_transaction.has_value()) {
            m_transaction->invalidate();
            m_transaction.reset();
        }
    }
    if (!reconcile_move) {
        m_run.floor = normalized->map.floor;
        m_current_floor = normalized->map.floor;
        m_run.current_node = normalized->current_node;
        RunObservation effective = run;
        if (!effective.action_points.has_value()) {
            effective.action_points = normalized->hud_action_points;
        }
        if (run.action_points.has_value() && normalized->hud_action_points.has_value() &&
            *run.action_points != *normalized->hud_action_points) {
            if (error != nullptr) {
                *error = "HUD action points conflict with the state observation";
            }
            return false;
        }
        if (!apply_run_observation(effective, error)) {
            return false;
        }
    }

    for (const auto& [node_id, node] : m_map.snapshot().nodes()) {
        if (node.identity_revealed) {
            m_run.revealed_nodes.emplace(node_id);
        }
    }
    if (!apply_observed_facts(observed_facts, error) ||
        !set_fact("map_full_coverage", normalized->map.coverage == ObservationCoverage::FullMap, error) ||
        !set_fact("portal_available", has_node_type(m_map.snapshot(), NodeType::Portal), error) ||
        !set_fact("scrap_shop_available", has_node_type(m_map.snapshot(), NodeType::ScrapShop), error)) {
        return false;
    }
    queue_map_summary(normalized->summary);
    for (json::object& conflict : prediction_conflicts) {
        Log.warn(
            "BlackFlow deterministic map prediction conflicts with observation",
            "evidence",
            conflict);
        queue_warning(
            "deterministic_prediction_conflict",
            "\u786e\u5b9a\u6027\u5730\u56fe\u89c4\u5219\u4e0e\u540e\u7eed\u5b9e\u9645\u89c2\u6d4b\u4e0d\u4e00\u81f4\uff0c\u5df2\u4ee5\u5b9e\u9645\u89c2\u6d4b\u66f4\u6b63",
            DiagnosticTrigger::IdentityConflict,
            std::move(conflict));
    }
    return true;
}

void BlackFlowSession::finalize_linked_encounter_landing(const LinkedEncounterReturnResolution& resolution)
{
    const Node* observed = m_map.snapshot().find_node(resolution.linked_node);
    if (observed == nullptr) {
        return;
    }

    const bool newly_visited = !m_run.visited_nodes.contains(resolution.linked_node);
    const bool visually_completed = observed->type == NodeType::Empty;
    Node linked = *observed;
    linked.type = resolution.linked_type;
    linked.name = resolution.linked_type == NodeType::BattleSavage ? "“居民”据点" : "应急助力";
    linked.traversal = default_traversal_for(resolution.linked_type);
    linked.progress = visually_completed ? NodeProgress::Completed : observed->progress;
    linked.identity_revealed = true;
    linked.identity_state = NodeIdentityState::Classified;
    linked.visually_hidden = false;
    linked.identity_from_topology = false;
    linked.identity_from_prediction = false;
    linked.prediction_rule.clear();
    linked.identity_source = "linked_encounter_transfer";
    linked.detected_by_vision = true;

    m_run.current_node = resolution.linked_node;
    m_run.visited_nodes.emplace(resolution.linked_node);
    m_run.revealed_nodes.emplace(resolution.linked_node);
    m_run.node_progress.insert_or_assign(resolution.linked_node, linked.progress);

    // 当前观测若已经显示为空地，就保留截图事实，只把真实身份写进探索笔记；否则将
    // 尚未分类的当前落点补成关联节点，供紧接着的路线规划使用。
    if (!visually_completed) {
        m_map.snapshot().upsert_node(linked);
    }
    if (linked.floor == m_exploration_notebook.floor()) {
        Node noted = linked;
        noted.marker_type.clear();
        noted.marker_display_name.clear();
        noted.marker_score = 0.0;
        noted.marker_resident_overlap_possible = false;
        m_exploration_notebook.snapshot().upsert_node(std::move(noted));
    }
    if (newly_visited && linked.progress == NodeProgress::Completed && linked.type != NodeType::Empty) {
        m_mission.record_node(m_policy->milestones, m_facts.merged(), linked);
    }

    m_telemetry_events.emplace_back(
        BlackFlowTelemetryEvent {
            "BlackFlowLinkedEncounterLanding",
            json::object {
                { "floor", linked.floor },
                { "event_node", resolution.event_node },
                { "linked_node", resolution.linked_node },
                { "linked_type", std::string(to_string(resolution.linked_type)) },
                { "progress", linked.progress == NodeProgress::Completed ? "completed" : "active" },
            },
        });
}

void BlackFlowSession::finalize_entered_node(const PageExecutionContext& context, bool page_completed)
{
    if (!context.has_landing || context.node == InvalidNodeId) {
        // 安眠一隅没有落点；小八界跨层时也无法再从新层起点反推出旧层落点。
        // 两种情况都只保留页面事件，不得把身份写到激活位置或 InvalidNodeId。
        if (page_completed && !context.resolution_reported) {
            queue_node_resolution(context);
        }
        return;
    }

    Node resolved;
    if (const Node* stored = m_map.snapshot().find_node(context.node);
        stored != nullptr && stored->floor == context.floor) {
        resolved = *stored;
    }
    else {
        resolved.id = context.node;
        resolved.floor = context.floor;
    }

    // 当前观测可能已把完成节点画成空地；页面上下文才是这次实际进入节点的历史身份。
    resolved.type = context.node_type;
    resolved.name = context.node_name;
    if (context.battle.has_value()) {
        resolved.battle = context.battle;
    }
    resolved.traversal = default_traversal_for(resolved.type);
    if (resolved.type != NodeType::Unknown && resolved.type != NodeType::HideBattle &&
        resolved.type != NodeType::HideInvisible) {
        resolved.identity_revealed = true;
        resolved.identity_state = NodeIdentityState::Classified;
    }
    resolved.identity_source = context.identity_from_event_name ? "event_name" : "entered_page";
    if (context.identity_from_event_name) {
        resolved.identity_from_topology = false;
        resolved.identity_from_prediction = false;
        resolved.prediction_rule.clear();
        resolved.detected_by_vision = true;
    }

    bool becomes_empty = false;
    if (context.result.has_value()) {
        const NodeStateUpdate& update = *context.result;
        if (update.actual_type.has_value()) {
            resolved.type = *update.actual_type;
            resolved.traversal = default_traversal_for(resolved.type);
        }
        if (update.actual_name.has_value()) {
            resolved.name = *update.actual_name;
        }
        if (update.identity_revealed.has_value()) {
            resolved.identity_revealed = *update.identity_revealed;
            resolved.identity_state =
                *update.identity_revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
        }
        if (page_completed) {
            if (update.repeatable.has_value()) {
                resolved.traversal.repeatable = *update.repeatable;
            }
            resolved.progress = update.progress.value_or(NodeProgress::Completed);
            becomes_empty = completed_node_becomes_empty(resolved.traversal.repeatable, update.becomes_empty);
        }
    }
    else if (page_completed) {
        resolved.progress = NodeProgress::Completed;
        becomes_empty = completed_node_becomes_empty(resolved.traversal.repeatable);
    }
    if (becomes_empty) {
        resolved.progress = NodeProgress::Completed;
    }

    m_run.visited_nodes.emplace(context.node);
    m_run.node_progress.insert_or_assign(context.node, resolved.progress);
    if (resolved.type == NodeType::Light) {
        m_run.consumed_one_time_nodes.emplace(context.node);
    }
    if (page_completed && resolved.type != NodeType::Empty && context.page_intent != "final.pass") {
        m_mission.record_node(m_policy->milestones, m_facts.merged(), resolved);
    }

    // 探索笔记保留节点结算前的真实身份、事件名/关卡名和完成状态，即使当前地图已显示为空地。
    if (context.floor == m_exploration_notebook.floor()) {
        Node noted = resolved;
        if (const Node* existing = m_exploration_notebook.snapshot().find_node(context.node); existing != nullptr) {
            const std::string existence_source = existing->existence_source;
            const bool detected_by_vision = existing->detected_by_vision;
            const bool confirmed_by_topology = existing->confirmed_by_topology;
            noted.existence_source = existence_source;
            noted.detected_by_vision = detected_by_vision;
            noted.confirmed_by_topology = confirmed_by_topology;
        }
        noted.marker_type.clear();
        noted.marker_display_name.clear();
        noted.marker_score = 0.0;
        noted.marker_resident_overlap_possible = false;
        m_exploration_notebook.snapshot().upsert_node(std::move(noted));
    }

    if (context.floor == m_run.floor && becomes_empty) {
        const Node* observed = m_map.snapshot().find_node(context.node);
        if (observed != nullptr) {
            Node updated = *observed;
            // 当前观测地图只保留截图当下的身份；页面上下文的历史身份只进入探索笔记。
            // 对已完成的非重复节点，仅补上截图中应呈现的林间空地状态。
            updated.type = NodeType::Empty;
            updated.name = EmptyNodeName;
            updated.battle = resolved.battle;
            updated.progress = NodeProgress::Completed;
            updated.traversal = default_traversal_for(NodeType::Empty);
            updated.identity_state = NodeIdentityState::Classified;
            updated.identity_revealed = true;
            updated.visually_hidden = false;
            updated.identity_from_topology = false;
            updated.identity_from_prediction = false;
            updated.prediction_rule.clear();
            updated.natural_reveal_suppressed = false;
            updated.identity_source = "node_resolution_becomes_empty";
            m_map.snapshot().upsert_node(std::move(updated));
        }
    }
    else if (context.floor == m_run.floor && context.identity_from_event_name) {
        const Node* observed = m_map.snapshot().find_node(context.node);
        if (observed != nullptr &&
            !(observed->type == NodeType::Empty && observed->progress == NodeProgress::Completed)) {
            // 页面未完成或节点可重复进入时，当前截图可能仍把它画成未知的诡秘。
            // 事件标题比隐藏图标更具体，因此回写真实类型；非重复已完成节点仍以截图中的空地为准。
            Node updated = *observed;
            updated.type = resolved.type;
            updated.name = resolved.name;
            updated.battle = resolved.battle;
            updated.traversal = resolved.traversal;
            updated.identity_state = NodeIdentityState::Classified;
            updated.identity_revealed = true;
            updated.visually_hidden = false;
            updated.identity_from_topology = false;
            updated.identity_from_prediction = false;
            updated.prediction_rule.clear();
            updated.identity_source = "event_name";
            updated.detected_by_vision = true;
            m_map.snapshot().upsert_node(std::move(updated));
            m_run.revealed_nodes.emplace(context.node);
        }
    }

    if (utopia_effect_expires_after_node_completion(
            page_completed,
            m_utopia_ideology,
            m_ideal_source,
            resolved.position)) {
        m_utopia_effect_expired = true;
        const auto clear_effect = [](NormalizedMap& map) {
            std::vector<Node> updates;
            updates.reserve(map.snapshot().nodes().size());
            for (const auto& [id, node] : map.snapshot().nodes()) {
                (void)id;
                if (node.natural_reveal_suppressed) {
                    Node updated = node;
                    updated.natural_reveal_suppressed = false;
                    updates.emplace_back(std::move(updated));
                }
            }
            for (Node& update : updates) {
                map.snapshot().upsert_node(std::move(update));
            }
        };
        clear_effect(m_map);
        clear_effect(m_exploration_notebook);
        m_utopia_status = "expired";
        m_utopia_reason = "ideal source completed";
        m_utopia_ideology.clear();
        m_utopia_policy.clear();
        m_ideal_source.reset();
        m_ideal_source_generation.reset();
        m_ideal_domain.clear();
        m_observed_ideal_domain.clear();
        m_ideal_source_score_margin = 0.0;
        m_ideal_source_heads_agree = false;
    }

    if (page_completed && !context.resolution_reported) {
        queue_node_resolution(context);
    }
}

bool BlackFlowSession::reconcile_committed_move(const BlackFlowPerceptionSnapshot& snapshot, std::string* error)
{
    if (!m_transaction.has_value()) {
        if (error != nullptr) {
            *error = "map return has no committed BlackFlow movement";
        }
        return false;
    }
    const MoveTransactionStage stage = m_transaction->stage();
    if (stage != MoveTransactionStage::Committed && stage != MoveTransactionStage::PageResolved) {
        if (error != nullptr) {
            *error = "map return cannot reconcile the current movement stage";
        }
        return false;
    }
    if (stage == MoveTransactionStage::Committed && !m_page_context.has_value()) {
        if (error != nullptr) {
            *error = "committed node movement has no BlackFlow page context";
        }
        return false;
    }
    const MapSnapshot map_before_move = m_map.snapshot();
    const RunState run_before_move = m_run;
    const MoveCandidate committed_move = m_transaction->proposal();
    const std::vector<std::string> observed_page_contents =
        m_page_context.has_value() ? m_page_context->observed_contents : std::vector<std::string> {};
    const bool same_floor_recollection =
        stage == MoveTransactionStage::PageResolved && run_before_move.floor == 4 &&
        m_next_level_transition_confirmed && m_current_floor.has_value() && *m_current_floor == run_before_move.floor &&
        m_page_context.has_value() && m_page_context->stage == PageExecutionStage::Resolved &&
        is_exit_node_type(m_page_context->node_type);
    if (same_floor_recollection) {
        // 稳定节点 ID 只编码楼层和网格坐标；追忆新图仍叫四层，若不先换代，旧图同坐标的
        // 身份、完成状态和探索笔记会污染新图。保留整局资源，仅清掉旧地图这一代的节点状态。
        for (const auto& [node_id, node] : map_before_move.nodes()) {
            (void)node;
            m_run.visited_nodes.erase(node_id);
            m_run.consumed_one_time_nodes.erase(node_id);
            m_run.revealed_nodes.erase(node_id);
            m_run.node_progress.erase(node_id);
        }
        m_map.reset();
        m_exploration_notebook.reset();
        m_battle_intel_probed.clear();
        m_unreachable_actions.clear();
        m_pending_probe_target.reset();
        m_verified_move_arc.reset();
        m_pending_candidate.reset();
        m_last_plan.reset();
        m_last_reveal_consistency.reset();
        m_run.costs.clear_action_cost_overrides();
        Log.info(
            "BlackFlow starts a new map generation for floor four recollection",
            "transaction stage",
            static_cast<int>(stage),
            "target type",
            to_string(m_page_context->node_type));
    }
    if (!merge_perception(snapshot.observation, snapshot.run, snapshot.observed_facts, true, error)) {
        return false;
    }

    MoveObservation observation;
    observation.current_node = m_observed_current_node;
    observation.floor = m_map.floor();
    observation.map_revision = m_map.snapshot().revision;
    observation.viewport_revision = m_viewport.viewport_revision();
    std::optional<LinkedEncounterReturnResolution> linked_return;
    const bool current_matches_linked_target =
        m_page_context.has_value() && m_page_context->linked_transfer_type.has_value() &&
        std::ranges::find(m_page_context->linked_transfer_targets, observation.current_node) !=
            m_page_context->linked_transfer_targets.end();
    if (m_page_context.has_value() && m_page_context->linked_transfer_type.has_value() &&
        (m_page_context->linked_transfer_selected || current_matches_linked_target) &&
        m_page_context->stage == PageExecutionStage::Resolved &&
        observation.floor == run_before_move.floor) {
        const NodeId known_event_node = committed_move.controllable ? m_page_context->node : InvalidNodeId;
        linked_return = resolve_linked_encounter_return(
            map_before_move,
            m_map.snapshot(),
            known_event_node,
            observation.current_node,
            *m_page_context->linked_transfer_type,
            m_page_context->linked_transfer_targets);
        if (!linked_return.has_value() && m_page_context->linked_transfer_selected) {
            json::object evidence {
                { "floor", observation.floor },
                { "movement", std::string(to_string(committed_move.movement)) },
                { "known_event_node", known_event_node },
                { "actual_current_node", observation.current_node },
                { "target_hypothesis_count", m_page_context->linked_transfer_targets.size() },
            };
            queue_warning(
                "linked_encounter_return_ambiguous",
                "关联事件传送后的原事件格或关联落点无法唯一确认；拒绝猜测节点回写",
                DiagnosticTrigger::PostMoveMismatch,
                std::move(evidence));
            if (error != nullptr) {
                *error = "linked encounter return cannot uniquely resolve the original event and transferred landing";
            }
            return false;
        }
        if (linked_return.has_value()) {
            if (!m_page_context->linked_transfer_selected) {
                queue_warning(
                    "linked_encounter_unexpected_transfer",
                    "关联事件实际发生了免费传送，与选项预期不一致；已按回图双节点证据纠正",
                    DiagnosticTrigger::PostMoveMismatch,
                    json::object {
                        { "floor", observation.floor },
                        { "event_node", linked_return->event_node },
                        { "linked_node", linked_return->linked_node },
                    });
            }
            observation.linked_encounter_origin_node = linked_return->event_node;
            // 普通移动在进入页面前已经知道事件格；小八界在这一刻才用地图差分补上。
            m_page_context->node = linked_return->event_node;
            Log.info(
                "BlackFlow reconciles linked encounter free transfer",
                "event node",
                linked_return->event_node,
                "linked node",
                linked_return->linked_node,
                "linked type",
                to_string(linked_return->linked_type));
        }
    }
    if (!committed_move.controllable && m_page_context.has_value() &&
        m_page_context->identity_from_event_name && m_page_context->has_landing &&
        observation.floor == run_before_move.floor) {
        // 小八界点选的只是随机移动的激活位置。只有回图后的 current_node 才是真实落点，
        // 所以事件类型的落点校验必须延迟到这里。
        const NodeId event_landing = linked_return.has_value() ? linked_return->event_node : observation.current_node;
        const Node* previous_landing = map_before_move.find_node(event_landing);
        if (previous_landing != nullptr && event_node_type_is_known(previous_landing->type) &&
            previous_landing->type != m_page_context->node_type) {
            json::object evidence {
                { "floor", run_before_move.floor },
                { "movement", std::string(to_string(committed_move.movement)) },
                { "actual_landing", event_landing },
                { "landing_row", previous_landing->position.row },
                { "landing_column", previous_landing->position.column },
                { "map_node_type", std::string(to_string(previous_landing->type)) },
                { "map_node_name", previous_landing->name },
                { "event_node_type", std::string(to_string(m_page_context->node_type)) },
                { "event_name", m_page_context->node_name },
            };
            Log.warn("BlackFlow random landing event type conflicts with map identity", "evidence", evidence);
            queue_warning(
                "event_node_type_conflict",
                "事件标题推导的节点类型与小八界回图后确认的实际落点身份不一致；已采用事件标题",
                DiagnosticTrigger::IdentityConflict,
                std::move(evidence));
        }
    }
    observation.advanced_via_adapted_pursuit =
        m_profile == "automation_collection" && run_before_move.floor == 3 && observation.floor == 4 &&
        m_current_floor.has_value() && *m_current_floor == observation.floor &&
        (stage == MoveTransactionStage::Committed || stage == MoveTransactionStage::PageResolved) &&
        m_page_context.has_value() && !is_exit_node_type(m_page_context->node_type);
    if (observation.advanced_via_adapted_pursuit) {
        Log.info(
            "BlackFlow reconciles floor three move through adapted pursuit",
            "transaction stage",
            static_cast<int>(stage),
            "page stage",
            static_cast<int>(m_page_context->stage),
            "target type",
            to_string(m_page_context->node_type));
    }
    observation.advanced_via_resolved_page =
        stage == MoveTransactionStage::PageResolved && observation.floor == run_before_move.floor + 1 &&
        m_current_floor.has_value() && *m_current_floor == observation.floor && m_page_context.has_value() &&
        m_page_context->stage == PageExecutionStage::Resolved;
    if (observation.advanced_via_resolved_page) {
        Log.info(
            "BlackFlow reconciles resolved hidden page through confirmed floor advance",
            "source floor",
            run_before_move.floor,
            "observed floor",
            observation.floor,
            "target type",
            to_string(m_page_context->node_type));
    }
    observation.renewed_same_floor_after_terminal = same_floor_recollection;
    if (observation.renewed_same_floor_after_terminal) {
        Log.info(
            "BlackFlow reconciles floor four terminal through recollection map renewal",
            "floor",
            observation.floor,
            "old terminal",
            committed_move.target,
            "new entrance",
            observation.current_node);
    }
    if (snapshot.run.action_points.has_value()) {
        observation.action_points = *snapshot.run.action_points;
    }
    else if (snapshot.observation.hud_action_points.has_value()) {
        observation.action_points = *snapshot.observation.hud_action_points;
    }
    else {
        const MoveCandidate& proposal = m_transaction->proposal();
        int gain = proposal.predicted_action_point_gain;
        if (!proposal.controllable) {
            const auto found = proposal.landing_action_point_gains.find(observation.current_node);
            gain = found == proposal.landing_action_point_gains.end() ? 0 : found->second;
        }
        observation.action_points =
            action_points_after(m_run.resources.action_points, m_transaction->authoritative_cost(), gain);
    }

    if (m_page_context.has_value()) {
        if (observation.floor == m_page_context->floor) {
            if (linked_return.has_value()) {
                // 加工品实际落到的是原不期而遇；随后事件效果才免费传送。不能把关联据点
                // 的类型当成加工品落点，否则会错误消耗只针对作战落点的资源。
                observation.landed_type = m_page_context->node_type;
                observation.target_progress = NodeProgress::Completed;
            }
            else if (const Node* landed = m_map.snapshot().find_node(observation.current_node); landed != nullptr) {
                observation.landed_type = landed->type;
                observation.target_progress = landed->progress;
            }
        }
        else {
            observation.landed_type = m_page_context->node_type;
            observation.target_progress =
                m_page_context->result.has_value() && m_page_context->result->progress.has_value()
                    ? *m_page_context->result->progress
                    : NodeProgress::Completed;
        }
    }
    else if (const Node* landed = m_map.snapshot().find_node(observation.current_node); landed != nullptr) {
        observation.landed_type = landed->type;
        observation.target_progress = landed->progress;
    }

    if (observation.floor == run_before_move.floor && !same_floor_recollection) {
        const bool endpoint_available =
            move_endpoint_observation_available(committed_move.terminal_on_completion, observation.action_points);
        NodeType entered_landing_type = NodeType::Unknown;
        if (!linked_return.has_value() && m_page_context.has_value() && m_page_context->has_landing &&
            m_page_context->floor == observation.floor && m_page_context->node == observation.current_node) {
            // Page finalization may already have turned a completed 羽瞰点 into 林间空地 in the
            // current-map snapshot. The page context retains the identity that produced this move's reveal.
            entered_landing_type = m_page_context->node_type;
        }
        auto expected = linked_return.has_value()
                            ? expected_linked_encounter_return_reveals(
                                  map_before_move,
                                  run_before_move,
                                  committed_move,
                                  linked_return->event_node,
                                  observation.current_node,
                                  true,
                                  endpoint_available)
                            : expected_move_reveals(
                                  map_before_move,
                                  run_before_move,
                                  committed_move,
                                  observation.current_node,
                                  endpoint_available,
                                  observed_page_contents,
                                  entered_landing_type);
        add_observed_event_reveal_expectations(
            map_before_move,
            run_before_move,
            m_map.snapshot(),
            observed_page_contents,
            m_page_context.has_value() && m_page_context->node_type == NodeType::Incident,
            expected);
        const auto observed = observed_move_reveals(map_before_move, run_before_move, m_map.snapshot());
        const RevealConsistencyResult consistency = compare_move_reveals(expected, observed);
        const auto serialize_ids = [](const auto& ids) {
            std::vector<json::value> result;
            result.reserve(ids.size());
            for (const NodeId id : ids) {
                result.emplace_back(id);
            }
            return json::array(std::move(result));
        };
        std::vector<json::value> contents;
        contents.reserve(observed_page_contents.size());
        for (const std::string& content : observed_page_contents) {
            contents.emplace_back(content);
        }
        const MovementSpec* movement = find_movement_spec(committed_move.movement);
        json::object consistency_details {
            { "floor", observation.floor },
            { "source", committed_move.source },
            { "selected_target", committed_move.target },
            { "actual_landing", observation.current_node },
            { "entered_event_node",
              linked_return.has_value() ? linked_return->event_node : InvalidNodeId },
            { "movement", std::string(to_string(committed_move.movement)) },
            { "movement_name", movement == nullptr ? std::string {} : std::string(movement->name) },
            { "endpoint_observation_available", endpoint_available },
            { "expected_revealed_nodes", serialize_ids(expected) },
            { "observed_revealed_nodes", serialize_ids(observed) },
            { "missing_revealed_nodes", serialize_ids(consistency.missing) },
            { "unexpected_revealed_nodes", serialize_ids(consistency.unexpected) },
            { "observed_page_contents", json::array(std::move(contents)) },
            { "consistency", consistency.consistent() ? "一致" : "不一致" },
            { "correction", "" },
        };
        m_last_reveal_consistency = consistency_details;
        m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowRevealConsistency", consistency_details });
        if (!consistency.consistent()) {
            append_map_visualization(consistency_details);
            queue_warning(
                "reveal_consistency_mismatch",
                "本步实际揭示的节点与路线规划预期不一致；已采用当前观测并重新规划",
                DiagnosticTrigger::PostMoveMismatch,
                std::move(consistency_details));
        }
    }

    if (!m_transaction->observe(observation, error) || !m_transaction->apply(m_run, error)) {
        queue_warning(
            "post_move_mismatch",
            error == nullptr ? "map return does not match the committed movement" : *error,
            DiagnosticTrigger::PostMoveMismatch);
        return false;
    }
    if (m_last_plan.has_value()) {
        // 到达 Applied 才能证明首步真的执行过。尤其对 source == landing 的合法原地移动，
        // 不能再用当前位置猜测事务是否提交，否则会把已执行首步重新喂给下一轮规划。
        advance_route_hint_after_applied_move(
            m_last_plan->decision.planned_route_steps,
            committed_move.action_id);
    }

    // 随机移动在打开预览时还不知道实际落点。回到地图后用当前标记把页面上下文绑定到
    // 真实节点，再写探索笔记、完成状态和节点内容，避免把 InvalidNodeId 记进地图。
    if (m_page_context.has_value() && !committed_move.controllable &&
        m_page_context->node == InvalidNodeId && observation.floor == m_page_context->floor) {
        m_page_context->node = observation.current_node;
    }

    m_run.floor = observation.floor;
    m_current_floor = observation.floor;
    m_run.current_node = observation.current_node;
    RunObservation effective = snapshot.run;
    if (!effective.action_points.has_value()) {
        effective.action_points = observation.action_points;
    }
    if (snapshot.run.action_points.has_value() && snapshot.observation.hud_action_points.has_value() &&
        *snapshot.run.action_points != *snapshot.observation.hud_action_points) {
        if (error != nullptr) {
            *error = "HUD action points conflict with the state observation";
        }
        return false;
    }
    if (!apply_run_observation(effective, error)) {
        return false;
    }
    if (m_page_context.has_value() && !same_floor_recollection) {
        const bool page_completed = m_transaction->stage() == MoveTransactionStage::Applied &&
                                    m_page_context->stage == PageExecutionStage::Resolved;
        const std::uint64_t map_revision_before_finalization = m_map.snapshot().revision;
        if (linked_return.has_value()) {
            finalize_linked_encounter_landing(*linked_return);
        }
        finalize_entered_node(*m_page_context, page_completed);
        if (m_map.snapshot().revision != map_revision_before_finalization &&
            !m_viewport.rebind_map_revision_after_semantic_update(
                map_revision_before_finalization,
                m_map.snapshot().revision)) {
            if (error != nullptr) {
                *error = "page finalization could not rebind current viewport coordinates";
            }
            return false;
        }
    }

    if (m_profile == "automation_collection" && !same_floor_recollection) {
        NodeId entered_node = InvalidNodeId;
        if (linked_return.has_value()) {
            entered_node = linked_return->event_node;
        }
        else if (m_page_context.has_value() && m_page_context->has_landing &&
                 m_page_context->node != InvalidNodeId) {
            entered_node = m_page_context->node;
        }
        else if (committed_move.controllable) {
            entered_node = committed_move.target;
        }
        else if (observation.floor == run_before_move.floor) {
            entered_node = observation.current_node;
        }

        if (const Node* entered = map_before_move.find_node(entered_node); entered != nullptr) {
            if (entered->marker_type == "fruit_cache") {
                m_node_attribution_records.emplace_back(
                    NodeAttributionRecord { entered->floor, entered->id, {}, "藏果地" });
            }
            for (std::string& attribution : m_pending_move_node_attributions) {
                m_node_attribution_records.emplace_back(
                    NodeAttributionRecord { entered->floor, entered->id, {}, std::move(attribution) });
            }
        }
    }
    m_pending_move_node_attributions.clear();

    m_transaction.reset();
    m_page_context.reset();
    m_transaction_id.clear();
    m_verified_move_arc.reset();
    m_pending_candidate.reset();
    m_pending_probe_target.reset();
    m_run.costs.clear_action_cost_overrides();
    m_unreachable_actions.clear();
    m_next_level_transition_confirmed = false;
    return true;
}

bool BlackFlowSession::apply_movement_panel_observation(
    MovementPanelObservation panel,
    std::optional<MovementKind> active_movement,
    std::string* error)
{
    const MovementKind target = panel.target;
    const bool reliable_count = movement_panel_has_reliable_count(panel);
    const std::optional<int> observed_count = panel.remaining_charges;

    BlackFlowSession staged = *this;
    RunObservation observation;
    observation.active_movement = active_movement;
    observation.movement_panel = std::move(panel);
    if (!staged.apply_run_observation(observation, error)) {
        return false;
    }
    // 移动方式面板只校验零件箱事实，不再写入或删除库存。这样即使同类有多件，面板里
    // 某一张卡的“剩余 N 次”也不会覆盖按种类聚合后的总次数。
    if (reliable_count && observed_count.has_value()) {
        std::vector<int> inventory_counts;
        for (const RunResources::MovementInstance& instance : staged.m_run.resources.movement_instances) {
            if (instance.movement == target && instance.remaining_charges > 0) {
                inventory_counts.emplace_back(instance.remaining_charges);
            }
        }
        if (!inventory_counts.empty()) {
            const int expected_minimum = *std::ranges::min_element(inventory_counts);
            const bool exact_instance_seen =
                std::ranges::find(inventory_counts, *observed_count) != inventory_counts.end();
            std::vector<json::value> expected;
            expected.reserve(inventory_counts.size());
            for (const int count : inventory_counts) {
                expected.emplace_back(count);
            }
            if (!exact_instance_seen) {
                staged.queue_warning(
                    "movement_panel_charge_mismatch",
                    "移动方式显示的剩余次数与零件箱实例不一致；保留零件箱结果",
                    DiagnosticTrigger::ProcessingItemObservation,
                    json::object {
                        { "movement", std::string(to_string(target)) },
                        { "inventory_remaining_charges", json::array(std::move(expected)) },
                        { "panel_remaining_charges", *observed_count },
                    });
            }
            else if (*observed_count != expected_minimum) {
                staged.queue_warning(
                    "movement_panel_nonminimal_instance_selected",
                    "同类加工品未选择剩余次数最少的实例",
                    DiagnosticTrigger::ProcessingItemObservation,
                    json::object {
                        { "movement", std::string(to_string(target)) },
                        { "inventory_remaining_charges", json::array(std::move(expected)) },
                        { "panel_remaining_charges", *observed_count },
                        { "expected_minimum", expected_minimum },
                    });
            }
        }
    }
    if (!staged.synchronize_resource_facts(error)) {
        return false;
    }
    *this = std::move(staged);
    return true;
}

bool BlackFlowSession::apply_movement_inventory_observation(
    const std::vector<RunResources::MovementInstance>& visible_instances,
    std::string* error)
{
    BlackFlowSession staged = *this;
    const RunResources resources_before = staged.m_run.resources;
    std::vector<RunResources::MovementInstance> rebuilt_instances;
    rebuilt_instances.reserve(visible_instances.size());
    for (const RunResources::MovementInstance& observed : visible_instances) {
        const MovementSpec* spec = find_movement_spec(observed.movement);
        if (observed.movement == MovementKind::Walk || spec == nullptr || spec->initial_charges <= 0 ||
            observed.remaining_charges < 0 || observed.remaining_charges > spec->initial_charges) {
            if (error != nullptr) {
                *error = "movement inventory contains an invalid processing item";
            }
            return false;
        }
        RunResources::MovementInstance instance = observed;
        instance.inventory_index = static_cast<int>(rebuilt_instances.size());
        rebuilt_instances.emplace_back(instance);
    }
    staged.m_run.resources.movement_instances = std::move(rebuilt_instances);
    rebuild_movement_aggregates(staged.m_run.resources);
    if (staged.m_run.active_movement.has_value() && *staged.m_run.active_movement != MovementKind::Walk &&
        !staged.m_run.resources.movement_pieces.contains(*staged.m_run.active_movement)) {
        staged.m_run.active_movement.reset();
    }
    if (staged.m_run.resources != resources_before) {
        ++staged.m_run.resources_revision;
    }
    staged.m_movement_inventory_refresh_required = next_movement_inventory_refresh_state(
        staged.m_movement_inventory_refresh_required,
        MovementInventoryRefreshEvent::ObservationApplied);
    // 零件箱是持有数量的主信源；一旦重新完整观测，撤销移动面板造成的临时屏蔽。
    staged.m_temporarily_unavailable_movements.clear();
    if (!staged.synchronize_resource_facts(error)) {
        return false;
    }
    *this = std::move(staged);
    return true;
}

std::optional<int> BlackFlowSession::minimum_movement_instance_charges(MovementKind movement) const noexcept
{
    std::optional<int> result;
    for (const RunResources::MovementInstance& instance : m_run.resources.movement_instances) {
        if (instance.movement != movement || instance.remaining_charges <= 0) {
            continue;
        }
        if (!result.has_value() || instance.remaining_charges < *result) {
            result = instance.remaining_charges;
        }
    }
    return result;
}

bool BlackFlowSession::report_movement_unavailable(MovementKind target, std::string* error)
{
    if (target == MovementKind::Walk || find_movement_spec(target) == nullptr) {
        if (error != nullptr) {
            *error = "temporary movement unavailability requires a known processing item";
        }
        return false;
    }
    // 不删除 movement_charges / movement_pieces；这些是零件箱建立的库存事实。
    // 只在下一次零件箱观测前让规划器忽略该加工品，避免反复选择同一不可装载项。
    m_temporarily_unavailable_movements.emplace(target);
    if (m_pending_candidate.has_value() && m_pending_candidate->candidate.movement == target) {
        m_pending_candidate.reset();
    }
    return true;
}

bool BlackFlowSession::should_retry_initial_reveal_observation(
    const BlackFlowPerceptionSnapshot& snapshot) const
{
    if (m_initial_reveal_checked_generation.has_value() &&
        *m_initial_reveal_checked_generation == m_map_generation) {
        return false;
    }

    std::string normalization_error;
    auto normalized = m_observation_adapter.normalize(snapshot.observation, &normalization_error);
    if (!normalized.has_value() || normalized->map.coverage != ObservationCoverage::FullMap) {
        return false;
    }

    NormalizedMap observed_map;
    if (!observed_map.merge(normalized->map, MapMergePurpose::CurrentObservation, &normalization_error)) {
        return false;
    }
    const bool swaddled_eagle_full_reveal = has_start_reward("襁褓骏鹰") &&
                                            normalized->map.floor >= 1 && normalized->map.floor <= 2;
    return initial_floor_reveal_observation_needs_ocr_retry(
        observed_map.snapshot(),
        normalized->current_node,
        swaddled_eagle_full_reveal);
}

bool BlackFlowSession::should_retry_post_move_reveal_observation(
    const BlackFlowPerceptionSnapshot& snapshot) const
{
    if (!m_transaction.has_value() ||
        (m_transaction->stage() != MoveTransactionStage::Committed &&
         m_transaction->stage() != MoveTransactionStage::PageResolved)) {
        return false;
    }

    std::string normalization_error;
    auto normalized = m_observation_adapter.normalize(snapshot.observation, &normalization_error);
    if (!normalized.has_value() || normalized->map.floor != m_run.floor) {
        return false;
    }

    const bool same_floor_recollection =
        m_transaction->stage() == MoveTransactionStage::PageResolved && m_run.floor == 4 &&
        m_next_level_transition_confirmed && m_current_floor.has_value() && *m_current_floor == m_run.floor &&
        m_page_context.has_value() && m_page_context->stage == PageExecutionStage::Resolved &&
        is_exit_node_type(m_page_context->node_type);
    if (same_floor_recollection) {
        return false;
    }

    MapObservationBatch observed_batch = std::move(normalized->map);
    apply_exact_resident_settlement_prediction(observed_batch, m_resident_settlement_prediction);
    NormalizedMap observed_map = m_map;
    if (!observed_map.merge(observed_batch, MapMergePurpose::CurrentObservation, &normalization_error)) {
        return false;
    }

    const MoveCandidate& committed_move = m_transaction->proposal();
    int action_points = 0;
    if (snapshot.run.action_points.has_value()) {
        action_points = *snapshot.run.action_points;
    }
    else if (snapshot.observation.hud_action_points.has_value()) {
        action_points = *snapshot.observation.hud_action_points;
    }
    else {
        int gain = committed_move.predicted_action_point_gain;
        if (!committed_move.controllable) {
            const auto found = committed_move.landing_action_point_gains.find(normalized->current_node);
            gain = found == committed_move.landing_action_point_gains.end() ? 0 : found->second;
        }
        action_points = action_points_after(m_run.resources.action_points, m_transaction->authoritative_cost(), gain);
    }
    const bool endpoint_available =
        move_endpoint_observation_available(committed_move.terminal_on_completion, action_points);

    const std::vector<std::string> observed_contents =
        m_page_context.has_value() ? m_page_context->observed_contents : std::vector<std::string> {};
    NodeType entered_landing_type = NodeType::Unknown;
    if (m_page_context.has_value() && m_page_context->has_landing && m_page_context->floor == m_run.floor &&
        m_page_context->node == normalized->current_node) {
        entered_landing_type = m_page_context->node_type;
    }
    auto expected = expected_move_reveals(
        m_map.snapshot(),
        m_run,
        committed_move,
        normalized->current_node,
        endpoint_available,
        observed_contents,
        entered_landing_type);
    add_observed_event_reveal_expectations(
        m_map.snapshot(),
        m_run,
        observed_map.snapshot(),
        observed_contents,
        m_page_context.has_value() && m_page_context->node_type == NodeType::Incident,
        expected);
    const auto observed = observed_move_reveals(m_map.snapshot(), m_run, observed_map.snapshot());
    return should_retry_transient_reveal_observation(expected, observed, observed_map.snapshot());
}

bool BlackFlowSession::update(const BlackFlowPerceptionSnapshot& snapshot, std::string* error)
{
    BlackFlowSession staged = *this;
    if (!staged.update_in_place(snapshot, error)) {
        return false;
    }
    *this = std::move(staged);
    return true;
}

bool BlackFlowSession::update_in_place(const BlackFlowPerceptionSnapshot& snapshot, std::string* error)
{
    const bool pending_move =
        m_transaction.has_value() && (m_transaction->stage() == MoveTransactionStage::Committed ||
                                      m_transaction->stage() == MoveTransactionStage::PageResolved);
    if (pending_move) {
        if (!reconcile_committed_move(snapshot, error)) {
            return false;
        }
    }
    else if (!merge_perception(snapshot.observation, snapshot.run, snapshot.observed_facts, false, error)) {
        return false;
    }
    if (!synchronize_resource_facts(error)) {
        return false;
    }

    refresh_mission();
    // 结算只放在观测这一拍。提交事务途中也会刷新里程碑，那时候终止会把页面丢在半路。
    evaluate_milestone_miss_actions();
    evaluate_terminal_rules();
    return true;
}

BlackFlowPlan BlackFlowSession::plan(std::string* error)
{
    return plan_internal(false, true, error);
}

BlackFlowPlan BlackFlowSession::plan_internal(
    bool require_physical_endpoint,
    bool allow_endpoint_fallback,
    std::string* error)
{
    BlackFlowPlan result;
    if (!m_policy.has_value()) {
        result.error = "BlackFlow session has no resolved policy";
    }
    else if (m_result.has_value()) {
        result.error = "BlackFlow session has already terminated";
    }
    else {
        const FactStore merged = m_facts.merged();
        BlackFlowPlanRequest request;
        std::optional<RunState> planning_run;
        if (!m_temporarily_unavailable_movements.empty()) {
            planning_run = m_run;
            for (const MovementKind movement : m_temporarily_unavailable_movements) {
                planning_run->resources.movement_charges.erase(movement);
                planning_run->resources.movement_pieces.erase(movement);
                if (planning_run->active_movement == movement) {
                    planning_run->active_movement.reset();
                }
            }
        }
        request.map = &m_map.snapshot();
        request.run = planning_run.has_value() ? &*planning_run : &m_run;
        request.policy = &*m_policy;
        request.facts = &merged;
        request.mission = &m_mission;
        const StrategyGoals goals = strategy_goals_for(*m_policy, m_mission, merged, m_map.snapshot(), m_run.floor);
        request.strategy_terminal_nodes = goals.terminal_nodes;
        request.binding_milestone_candidates = goals.binding_candidates;
        request.undemotable_binding_count = goals.undemotable_count;
        request.no_AP_is_terminal = require_physical_endpoint ? false : no_action_points_is_terminal();
        if (m_profile == "automation_collection") {
            request.forbidden_node_types = automation_collection_forbidden_landing_types(m_run.floor);
        }
        if (m_profile == "automation_collection" && m_run.floor >= 2) {
            // 当前动作仍避开现场标记；再枚举结算后一拍的“原地或沿确认连线一步”，要求每种
            // 居民结果各自存在安全应对。实际只执行首步并重新观测；严格检验无解才退回首步避让。
            request.root_forbidden_marker_types = { "savage" };
            request.robust_mobile_marker_lookahead = true;
        }
        const int reserved_full_map_charges =
            m_profile == "automation_collection"
                ? automation_collection_reserved_full_map_charges(m_run.floor)
                : 0;
        if (reserved_full_map_charges > 0) {
            request.reserved_movement_kinds = {
                MovementKind::M08,
                MovementKind::M09,
                MovementKind::M11,
            };
            request.reserved_movement_charges = reserved_full_map_charges;
        }
        request.forbidden_actions = &m_unreachable_actions;
        request.probe_target = m_pending_probe_target;
        if (m_last_plan.has_value()) {
            // planned_route_steps 已在事务 Applied 时消费过首步；未提交或取消的事务不会消费，
            // 因而这里无需再从 source/landing 猜测游标位置。
            for (const PlannedRouteStep& step : m_last_plan->decision.planned_route_steps) {
                request.route_hint_action_ids.emplace_back(step.move.action_id);
            }
        }
        result = BlackFlowPlanner {}.plan(request);
        // 自动化收集的 1--4 层通常都能证出安全出口路线；若当前观测下暂时证不出，
        // 本轮只取消出口安全证明，保留避战和全图移动预留，继续按揭示数探索。
        // 这个状态不写入事实：下一次地图/加工品观测后会重新优先尝试出口路线，因此是可逆的。
        if (allow_endpoint_fallback && !result && result.error == "no eligible safe candidate" &&
            m_profile == "automation_collection" &&
            m_run.floor >= 1 && m_run.floor <= 4 && !request.no_AP_is_terminal) {
            const std::string endpoint_failure = result.error;
            const bool no_AP_was_terminal = request.no_AP_is_terminal;
            request.no_AP_is_terminal = true;
            result = BlackFlowPlanner {}.plan(request);
            result.endpoint_fallback_active = true;
            result.endpoint_fallback_reason = endpoint_failure;
            if (result &&
                !endpoint_fallback_candidate_is_safe(
                    no_AP_was_terminal,
                    m_run.resources.action_points,
                    result.decision.selected->predicted_action_point_cost,
                    result.decision.selected->predicted_action_point_gain,
                    result.decision.selected->terminal_on_completion,
                    result.decision.selected->direct_exhaustion)) {
                result.decision.selected.reset();
                result.error = "endpoint fallback would exhaust action points outside a real terminal";
                result.no_AP_is_terminal = no_AP_was_terminal;
                request.no_AP_is_terminal = no_AP_was_terminal;
            }
            Log.info(
                "BlackFlow automation collection temporarily relaxed endpoint constraint",
                "floor",
                m_run.floor,
                "endpoint planning error",
                endpoint_failure,
                "fallback result",
                result.error.empty() ? "selected" : result.error);
        }
        if (!result && includes_full_routing_details(m_diagnostics.level)) {
            m_decision_id = "BF-D" + std::to_string(m_run_revision) + "-" + std::to_string(++m_decision_sequence);
            json::object diagnostic {
                { "decision_id", m_decision_id },
                { "profile", m_profile },
                { "floor", m_run.floor },
                { "planning_error", result.error },
                { "endpoint_required", !request.no_AP_is_terminal },
                { "no_AP_is_terminal", request.no_AP_is_terminal },
                { "endpoint_fallback_active", result.endpoint_fallback_active },
                { "endpoint_fallback_reason", result.endpoint_fallback_reason },
                { "mobile_marker_lookahead_active", result.mobile_marker_lookahead_active },
                { "mobile_marker_lookahead_fallback_active",
                  result.mobile_marker_lookahead_fallback_active },
                { "mobile_marker_lookahead_rejected_candidates",
                  static_cast<std::int64_t>(result.mobile_marker_lookahead_rejected_candidates) },
                { "mobile_marker_outcomes_checked",
                  static_cast<std::int64_t>(result.mobile_marker_outcomes_checked) },
                { "reserved_movement_charges", request.reserved_movement_charges },
                { "automation_collection_full_map_count",
                  m_resources.read("automation_collection_full_map_movement", m_run).value_or(0) },
                { "total_candidates", result.decision.total_candidates },
                { "eligible_candidates", result.decision.eligible_candidates },
            };
            std::vector<json::value> forbidden_node_types;
            for (const NodeType type : request.forbidden_node_types) {
                forbidden_node_types.emplace_back(std::string(to_string(type)));
            }
            diagnostic["forbidden_node_types"] = json::array(std::move(forbidden_node_types));
            std::vector<json::value> future_forbidden_node_types;
            for (const NodeType type : future_forbidden_landing_types(request.forbidden_node_types)) {
                future_forbidden_node_types.emplace_back(std::string(to_string(type)));
            }
            diagnostic["future_forbidden_node_types"] = json::array(std::move(future_forbidden_node_types));
            std::vector<json::value> root_forbidden_marker_types;
            for (const std::string& marker : request.root_forbidden_marker_types) {
                root_forbidden_marker_types.emplace_back(marker);
            }
            diagnostic["root_forbidden_marker_types"] = json::array(std::move(root_forbidden_marker_types));
            std::vector<json::value> rejected_candidates;
            rejected_candidates.reserve(result.decision.rejected.size());
            for (const std::string& rejected_candidate : result.decision.rejected) {
                rejected_candidates.emplace_back(rejected_candidate);
            }
            diagnostic["rejected_candidates"] = json::array(std::move(rejected_candidates));
            append_map_visualization(diagnostic);
            request_diagnostics(DiagnosticTrigger::RoutingDecision, std::move(diagnostic));
        }
        Log.info(
            "BlackFlow strategy goals",
            "profile",
            m_profile,
            "floor",
            m_run.floor,
            "binding candidates",
            goals.binding_candidates.size(),
            "locked",
            result.binding_milestone_ids.size(),
            "demoted",
            result.demoted_milestone_ids.size(),
            "strategy terminal nodes",
            goals.terminal_nodes.size());
        if (result && m_pending_probe_target.has_value() &&
            result.decision.selected->target != *m_pending_probe_target) {
            m_pending_probe_target.reset();
        }
    }
    if (!result.error.empty() && error != nullptr) {
        *error = result.error;
    }
    if (result) {
        m_last_plan = result;
    }
    return result;
}

bool BlackFlowSession::record_direct_exhaustion_decision(std::string* error)
{
    if (!m_last_plan.has_value() || !m_last_plan->decision.selected.has_value() ||
        !m_last_plan->decision.selected->direct_exhaustion) {
        if (error != nullptr) {
            *error = "direct exhaustion was requested without a matching plan";
        }
        return false;
    }
    m_pending_candidate.reset();
    m_verified_move_arc.reset();
    queue_decision(&*m_last_plan->decision.selected);
    return true;
}

bool BlackFlowSession::save_pending_candidate(const MoveCandidate& candidate, std::string* error)
{
    if (m_transaction.has_value()) {
        if (error != nullptr) {
            *error = "cannot save a pending candidate while a movement transaction exists";
        }
        return false;
    }
    if (candidate.source != m_run.current_node || m_map.snapshot().find_node(candidate.source) == nullptr) {
        if (error != nullptr) {
            *error = "pending candidate does not start at the current node";
        }
        return false;
    }
    if (find_movement_spec(candidate.movement) == nullptr) {
        if (error != nullptr) {
            *error = "pending candidate references an unknown movement";
        }
        return false;
    }
    if (candidate.movement != MovementKind::Walk) {
        const auto charges = m_run.resources.movement_charges.find(candidate.movement);
        if (charges == m_run.resources.movement_charges.end() || charges->second <= 0) {
            if (error != nullptr) {
                *error = "pending candidate movement has no remaining charges";
            }
            return false;
        }
    }
    if (candidate.target == InvalidNodeId ||
        !m_viewport.clickable_rect(candidate.target, m_map.snapshot().revision, m_viewport.viewport_revision())
             .has_value()) {
        if (error != nullptr) {
            *error = "pending candidate has no current viewport coordinate";
        }
        return false;
    }
    m_pending_candidate = PendingMoveCandidate {
        candidate,
        m_run_revision,
        m_map.snapshot().revision,
        m_run.costs.revision,
        m_run.resources_revision,
        m_viewport.viewport_revision(),
    };
    return true;
}

bool BlackFlowSession::validate_pending_candidate(std::string* error) const
{
    if (!m_pending_candidate.has_value()) {
        if (error != nullptr) {
            *error = "no pending movement candidate is saved";
        }
        return false;
    }
    const PendingMoveCandidate& pending = *m_pending_candidate;
    if (pending.run_revision != m_run_revision || pending.map_revision != m_map.snapshot().revision ||
        pending.cost_revision != m_run.costs.revision || pending.resources_revision != m_run.resources_revision ||
        pending.viewport_revision != m_viewport.viewport_revision()) {
        if (error != nullptr) {
            *error = "pending movement candidate revisions no longer match the session";
        }
        return false;
    }
    if (pending.candidate.source != m_run.current_node) {
        if (error != nullptr) {
            *error = "pending movement candidate source is no longer current";
        }
        return false;
    }
    if (!m_run.active_movement.has_value() || *m_run.active_movement != pending.candidate.movement) {
        if (error != nullptr) {
            *error = "active movement does not match the pending candidate";
        }
        return false;
    }
    if (pending.candidate.movement != MovementKind::Walk) {
        const auto charges = m_run.resources.movement_charges.find(pending.candidate.movement);
        if (charges == m_run.resources.movement_charges.end() || charges->second <= 0) {
            if (error != nullptr) {
                *error = "pending candidate movement has no remaining charges";
            }
            return false;
        }
    }
    if (pending.candidate.target == InvalidNodeId ||
        !m_viewport.clickable_rect(pending.candidate.target, pending.map_revision, pending.viewport_revision)
             .has_value()) {
        if (error != nullptr) {
            *error = "pending movement candidate viewport coordinate is stale";
        }
        return false;
    }
    return true;
}

bool BlackFlowSession::begin_pending_transaction(std::string* error)
{
    if (!validate_pending_candidate(error)) {
        return false;
    }
    const MoveCandidate candidate = m_pending_candidate->candidate;
    if (!begin_transaction(candidate, error)) {
        return false;
    }
    m_pending_candidate.reset();
    return true;
}

bool BlackFlowSession::begin_transaction(const MoveCandidate& candidate, std::string* error)
{
    auto proposed = MoveTransaction::propose(candidate, m_map.snapshot(), m_viewport, error);
    if (!proposed.has_value()) {
        queue_decision(
            &candidate,
            "transaction_proposal_failed",
            error == nullptr ? "move transaction proposal failed" : *error);
        return false;
    }
    m_verified_move_arc.reset();
    m_pending_candidate.reset();
    m_transaction = std::move(*proposed);
    m_transaction_id = "BF-T" + std::to_string(m_run_revision) + "-" + std::to_string(++m_transaction_sequence);
    m_pending_move_node_attributions.clear();
    return true;
}

PreviewDisposition BlackFlowSession::accept_preview(MovePreview preview, std::string* error)
{
    if (!m_transaction.has_value()) {
        if (error != nullptr) {
            *error = "move preview arrived without a proposed transaction";
        }
        return PreviewDisposition::Failed;
    }
    if (preview.reachability == PreviewReachability::Reachable &&
        preview.exact_action_point_cost > m_run.resources.action_points) {
        preview.reachability = PreviewReachability::InsufficientActionPoints;
    }
    const PreviewReachability reachability = preview.reachability;
    if (!m_transaction->record_preview(preview, error)) {
        return PreviewDisposition::Failed;
    }
    const MoveCandidate proposal = m_transaction->proposal();
    if (m_transaction->stage() == MoveTransactionStage::Cancelled) {
        m_unreachable_actions.emplace(proposal.action_id);
        m_verified_move_arc.reset();
        if (reachability == PreviewReachability::Blocked && proposal.first_unclassified.has_value() &&
            *proposal.first_unclassified != proposal.target) {
            m_pending_probe_target = proposal.first_unclassified;
        }
        else if (m_pending_probe_target == proposal.target) {
            m_pending_probe_target.reset();
        }
        if (reachability == PreviewReachability::Blocked) {
            queue_warning(
                "route_blocked",
                "move preview shows that a blocking node prevents this move",
                DiagnosticTrigger::RebuildConflict);
        }
        else if (reachability == PreviewReachability::InsufficientActionPoints) {
            queue_warning(
                "insufficient_action_points",
                "move preview cost exceeds the remaining action points",
                DiagnosticTrigger::PreviewCostMismatch);
        }
        else {
            queue_warning(
                "target_state_changed",
                "move preview no longer accepts the selected target",
                DiagnosticTrigger::RebuildConflict);
        }
        m_transaction.reset();
        m_transaction_id.clear();
        return PreviewDisposition::ReplanAfterDismiss;
    }

    bool changed = false;
    const Node* existing = proposal.target == InvalidNodeId ? nullptr : m_map.snapshot().find_node(proposal.target);
    if (proposal.controllable && existing == nullptr) {
        if (error != nullptr) {
            *error = "preview target disappeared from normalized map";
        }
        return PreviewDisposition::Failed;
    }
    if (move_preview_updates_target_identity(proposal) && existing != nullptr &&
        preview.displayed_type != NodeType::Unknown) {
        const bool resident_overlay = preview_confirms_roaming_resident(*existing, preview);
        const bool identity_conflict =
            !resident_overlay && existing->identity_revealed && preview.identity_revealed &&
            existing->type != preview.displayed_type;
        if (identity_conflict) {
            queue_warning(
                "identity_conflict",
                "preview node identity conflicts with the current normalized map",
                DiagnosticTrigger::IdentityConflict,
                json::object {
                    { "preview_action_id", proposal.action_id },
                    { "preview_target", proposal.target },
                    { "preview_target_row", existing->position.row },
                    { "preview_target_column", existing->position.column },
                    { "expected_node_type", std::string(to_string(existing->type)) },
                    { "expected_node_name", existing->name },
                    { "observed_node_type", std::string(to_string(preview.displayed_type)) },
                    { "observed_node_name", preview.displayed_name },
                });
        }
        const bool identity_unresolved = existing->type == NodeType::Unknown ||
                                         existing->type == NodeType::HideInvisible ||
                                         existing->type == NodeType::HideBattle;
        const bool revealed_preview_correction =
            !resident_overlay && should_apply_revealed_preview_identity(*existing, preview);
        if (resident_overlay && existing->marker_type != "savage") {
            Node updated = *existing;
            updated.marker_type = "savage";
            updated.marker_display_name = "流窜“居民”（移动预览确认）";
            updated.marker_score = 1.0;
            updated.marker_resident_overlap_possible = false;
            changed = m_map.snapshot().upsert_node(std::move(updated));
            if (changed) {
                std::vector<NodeObservation> observations;
                observations.reserve(m_viewport.nodes().size());
                for (const auto& [id, observation] : m_viewport.nodes()) {
                    (void)id;
                    observations.emplace_back(observation);
                }
                m_viewport.replace(std::move(observations), m_map.snapshot().revision, m_viewport.viewport_revision());
            }
        }
        else if ((identity_unresolved || revealed_preview_correction) &&
            (existing->type != preview.displayed_type || existing->name != preview.displayed_name ||
             existing->identity_revealed != preview.identity_revealed)) {
            Node updated = *existing;
            updated.type = preview.displayed_type;
            updated.name = preview.displayed_name;
            updated.identity_revealed = preview.identity_revealed;
            updated.identity_state =
                preview.identity_revealed ? NodeIdentityState::Classified : NodeIdentityState::Hidden;
            if (preview.identity_revealed) {
                updated.visually_hidden = false;
                updated.identity_from_topology = false;
                updated.identity_source = "move_preview_ocr";
                updated.detected_by_vision = true;
            }
            if (!updated.traversal.repeatable && updated.progress == NodeProgress::Active) {
                updated.traversal = default_traversal_for(updated.type);
            }
            const Node corrected = updated;
            changed = m_map.snapshot().upsert_node(std::move(updated));
            if (changed) {
                if (preview.identity_revealed && m_exploration_notebook.floor() == corrected.floor) {
                    Node noted = corrected;
                    if (const Node* existing_note = m_exploration_notebook.snapshot().find_node(corrected.id);
                        existing_note != nullptr) {
                        noted.existence_source = existing_note->existence_source;
                        noted.confirmed_by_topology = existing_note->confirmed_by_topology;
                    }
                    noted.marker_type.clear();
                    noted.marker_display_name.clear();
                    noted.marker_score = 0.0;
                    noted.marker_resident_overlap_possible = false;
                    m_exploration_notebook.snapshot().upsert_node(std::move(noted));
                }
                std::vector<NodeObservation> observations;
                observations.reserve(m_viewport.nodes().size());
                for (const auto& [id, observation] : m_viewport.nodes()) {
                    (void)id;
                    observations.emplace_back(observation);
                }
                m_viewport.replace(std::move(observations), m_map.snapshot().revision, m_viewport.viewport_revision());
            }
        }
    }

    if (preview.exact_action_point_cost != proposal.predicted_action_point_cost) {
        queue_warning(
            "preview_cost_changed",
            "preview action-point cost differs from the planner prediction",
            DiagnosticTrigger::PreviewCostMismatch);
        m_run.costs.action_cost_overrides.insert_or_assign(proposal.action_id, preview.exact_action_point_cost);

        ++m_run.costs.revision;
        m_verified_move_arc.reset();
        changed = true;
    }
    if (m_profile == "automation_collection") {
        const Node* mapped_landing = m_map.snapshot().find_node(proposal.landing);
        const NodeType landing_type = preview_landing_type_for_safety(
            mapped_landing,
            proposal.landing == proposal.target,
            preview.displayed_type);
        if (automation_collection_forbidden_landing_types(m_run.floor).contains(landing_type)) {
            m_unreachable_actions.emplace(proposal.action_id);
            if (m_pending_probe_target == proposal.target) {
                m_pending_probe_target.reset();
            }
            json::object evidence {
                { "preview_action_id", proposal.action_id },
                { "preview_target", proposal.target },
                { "preview_landing", proposal.landing },
                { "observed_node_type", std::string(to_string(preview.displayed_type)) },
                { "observed_node_name", preview.displayed_name },
                { "landing_node_type", std::string(to_string(landing_type)) },
            };
            if (const Node* landing = m_map.snapshot().find_node(proposal.landing); landing != nullptr) {
                evidence["preview_landing_row"] = landing->position.row;
                evidence["preview_landing_column"] = landing->position.column;
                evidence["map_landing_node_type"] = std::string(to_string(landing->type));
                evidence["map_landing_node_name"] = landing->name;
            }
            queue_warning(
                "forbidden_landing",
                landing_type == NodeType::HideBattle
                    ? "automation collection forbids unknown savage nodes as movement landings"
                    : "automation collection forbids this battle node as a movement landing on the current floor",
                DiagnosticTrigger::RebuildConflict,
                std::move(evidence));
            m_transaction->cancel();
            m_transaction.reset();
            m_transaction_id.clear();
            m_verified_move_arc.reset();
            return PreviewDisposition::ReplanAfterDismiss;
        }
    }
    if (changed) {
        m_transaction->invalidate();
        m_transaction.reset();
        m_transaction_id.clear();
        return PreviewDisposition::ReplanAfterDismiss;
    }

    if (proposal.requires_preview_verification) {
        const FactStore merged = m_facts.merged();
        BlackFlowPlanRequest request;
        request.map = &m_map.snapshot();
        request.run = &m_run;
        request.policy = &*m_policy;
        request.facts = &merged;
        request.mission = &m_mission;
        const StrategyGoals goals = strategy_goals_for(*m_policy, m_mission, merged, m_map.snapshot(), m_run.floor);
        request.strategy_terminal_nodes = goals.terminal_nodes;
        // 预览验证沿用上一次规划锁定的目标，锁定集合的变化留给下一次规划。
        if (m_last_plan.has_value()) {
            request.binding_milestone_candidates.assign(
                m_last_plan->binding_milestone_ids.begin(),
                m_last_plan->binding_milestone_ids.end());
            std::ranges::sort(request.binding_milestone_candidates);
        }
        request.undemotable_binding_count = request.binding_milestone_candidates.size();
        // 精确预览必须沿用产生这一步的实际规划语义。若本步来自临时探索回退，重新套用
        // “必须到出口”的默认约束会把刚选出的合法动作误判成不安全。
        request.no_AP_is_terminal =
            m_last_plan.has_value() ? m_last_plan->no_AP_is_terminal : no_action_points_is_terminal();
        if (m_profile == "automation_collection") {
            request.forbidden_node_types = automation_collection_forbidden_landing_types(m_run.floor);
        }
        const int reserved_full_map_charges =
            m_profile == "automation_collection"
                ? automation_collection_reserved_full_map_charges(m_run.floor)
                : 0;
        if (reserved_full_map_charges > 0) {
            request.reserved_movement_kinds = {
                MovementKind::M08,
                MovementKind::M09,
                MovementKind::M11,
            };
            request.reserved_movement_charges = reserved_full_map_charges;
        }
        request.forbidden_actions = &m_unreachable_actions;
        const PreviewSafetyVerification verification =
            BlackFlowPlanner {}.verify_previewed_move(request, proposal, preview.exact_action_point_cost);
        if (!verification.error.empty()) {
            if (error != nullptr) {
                *error = verification.error;
            }
            return PreviewDisposition::Failed;
        }
        if (!verification.safe) {
            m_unreachable_actions.emplace(proposal.action_id);
            queue_warning(
                "preview_has_no_confirmed_safe_route",
                "the previewed move has no confirmed safe route from its landing",
                DiagnosticTrigger::RebuildConflict);
            m_transaction->cancel();
            m_transaction.reset();
            m_transaction_id.clear();
            return PreviewDisposition::ReplanAfterDismiss;
        }
        m_verified_move_arc = VerifiedMoveArc {
            proposal.action_id,
            proposal.source,
            proposal.target,
            proposal.landing,
            proposal.movement,
            preview.exact_action_point_cost,
            verification.required_action_points_after,
            verification.proof_depth,
            m_map.snapshot().revision,
            m_run.costs.revision,
            m_run.resources_revision,
            m_viewport.viewport_revision(),
        };
    }
    if (!set_fact("page_kind", std::string(to_string(preview.displayed_type)), error)) {
        return PreviewDisposition::Failed;
    }
    return PreviewDisposition::ReadyToCommit;
}

bool BlackFlowSession::validate_commit(std::string* error) const
{
    if (!m_transaction.has_value()) {
        if (error != nullptr) {
            *error = "move commit has no active transaction";
        }
        return false;
    }
    if (m_transaction->stage() != MoveTransactionStage::Previewed || !m_transaction->preview().has_value() ||
        m_transaction->preview()->reachability != PreviewReachability::Reachable) {
        if (error != nullptr) {
            *error = "only a reachable previewed transaction can be committed";
        }
        return false;
    }
    if (m_transaction->map_revision() != m_map.snapshot().revision ||
        m_transaction->viewport_revision() != m_viewport.viewport_revision()) {
        if (error != nullptr) {
            *error = "map or viewport revision changed before commit";
        }
        return false;
    }

    const MoveCandidate& pending = m_transaction->proposal();
    if (!pending.requires_preview_verification) {
        return true;
    }
    const bool verified =
        m_verified_move_arc.has_value() && m_verified_move_arc->action_id == pending.action_id &&
        m_verified_move_arc->source == pending.source && m_verified_move_arc->target == pending.target &&
        m_verified_move_arc->landing == pending.landing && m_verified_move_arc->movement == pending.movement &&
        m_verified_move_arc->map_revision == m_map.snapshot().revision &&
        m_verified_move_arc->cost_revision == m_run.costs.revision &&
        m_verified_move_arc->resources_revision == m_run.resources_revision &&
        m_verified_move_arc->viewport_revision == m_viewport.viewport_revision() &&
        m_verified_move_arc->exact_action_point_cost == m_transaction->preview()->exact_action_point_cost;
    if (!verified && error != nullptr) {
        *error = "preview-verified move arc expired before commit";
    }
    return verified;
}

bool BlackFlowSession::commit(EnteredPageObservation entered_page, std::string* error)
{
    if (!m_transaction.has_value()) {
        if (error != nullptr) {
            *error = "move commit has no active transaction";
        }
        return false;
    }
    const MoveCandidate& pending = m_transaction->proposal();
    if (pending.requires_preview_verification) {
        const bool verified =
            m_verified_move_arc.has_value() && m_verified_move_arc->action_id == pending.action_id &&
            m_verified_move_arc->source == pending.source && m_verified_move_arc->target == pending.target &&
            m_verified_move_arc->landing == pending.landing && m_verified_move_arc->movement == pending.movement &&
            m_verified_move_arc->map_revision == m_map.snapshot().revision &&
            m_verified_move_arc->cost_revision == m_run.costs.revision &&
            m_verified_move_arc->resources_revision == m_run.resources_revision &&
            m_verified_move_arc->viewport_revision == m_viewport.viewport_revision() &&
            m_transaction->preview().has_value() &&
            m_verified_move_arc->exact_action_point_cost == m_transaction->preview()->exact_action_point_cost;
        if (!verified) {
            m_transaction->invalidate();
            if (error != nullptr) {
                *error = "preview-verified move arc expired before commit";
            }
            return false;
        }
    }

    const bool committed = m_transaction->commit(
        m_map.snapshot().revision,
        m_viewport.viewport_revision(),
        m_run.resources,
        error);
    if (!committed) {
        return false;
    }

    const MoveCandidate& proposal = m_transaction->proposal();
    if (proposal.movement != MovementKind::Walk) {
        // 实际消耗后不在内存中猜测扣哪一个同类实例；回图后先重新观察零件箱，再规划下一步。
        m_movement_inventory_refresh_required = true;
    }
    NodeId page_node = proposal.controllable ? proposal.target : InvalidNodeId;
    const Node* target = page_node == InvalidNodeId ? nullptr : m_map.snapshot().find_node(page_node);
    if (target != nullptr && (target->type == NodeType::Empty || is_transfer_node(target->type))) {
        if (!m_transaction->mark_page_resolved(error)) {
            return false;
        }
        m_page_context.reset();
        if (m_pending_probe_target == proposal.target) {
            m_pending_probe_target.reset();
        }
        queue_decision();
        return true;
    }
    if (!proposal.controllable && entered_page.map_visible) {
        // 小八界随机落到林间空地时确认后直接回到地图，没有节点页面需要派发；实际落点由
        // 下一次完整地图观测与 possible_landings 对账。
        if (!m_transaction->mark_page_resolved(error)) {
            return false;
        }
        m_page_context.reset();
        queue_decision();
        return true;
    }

    NodeType map_type = target == nullptr ? NodeType::Unknown : target->type;
    std::string map_name = target == nullptr ? std::string {} : target->name;
    int page_floor = target == nullptr ? m_run.floor : target->floor;
    const MovePreview* preview = m_transaction->preview().has_value() ? &*m_transaction->preview() : nullptr;
    const PageContentEffect event_effect = entered_page.event_name.has_value()
                                               ? classify_page_content_effect(
                                                     "RoguelikeEvent",
                                                     *entered_page.event_name)
                                               : PageContentEffect {};

    // 小八界的 target 只是激活预览的格子。进入页面后若已识别出真实类型，就用规划时
    // 逐落点保存的语义把页面绑定到唯一实际候选；已探明和仍显示“未知的诡秘”的尽头
    // 都会在这里恢复为 final，从而沿用规划给出的 advance/pass 意图。
    if (!proposal.controllable) {
        const std::optional<NodeType> entered_type =
            event_effect.resolved_type.has_value() ? event_effect.resolved_type : entered_page.classified_type;
        if (entered_type.has_value()) {
            NodeId semantic_landing = InvalidNodeId;
            bool ambiguous = false;
            for (const NodeId landing : proposal.possible_landings) {
                if (move_landing_type(proposal, landing) != *entered_type) {
                    continue;
                }
                if (semantic_landing != InvalidNodeId) {
                    ambiguous = true;
                    break;
                }
                semantic_landing = landing;
            }
            if (!ambiguous && semantic_landing != InvalidNodeId) {
                page_node = semantic_landing;
                target = m_map.snapshot().find_node(page_node);
                map_type = target == nullptr ? *entered_type : target->type;
                map_name = target == nullptr ? std::string {} : target->name;
                page_floor = target == nullptr ? m_run.floor : target->floor;
                Log.info(
                    "BlackFlow binds uncontrollable page to semantic landing",
                    "landing",
                    page_node,
                    "type",
                    to_string(*entered_type),
                    "visually hidden",
                    target != nullptr && target->visually_hidden);
            }
        }
    }
    const bool event_identity_conflict =
        proposal.controllable && target != nullptr && event_effect.resolved_type.has_value() &&
        event_node_type_is_known(target->type) && target->type != *event_effect.resolved_type;
    const bool entered_identity_conflict =
        entered_page.classification_conflict ||
        (proposal.controllable && target != nullptr && target->identity_revealed &&
         entered_page.classified_type.has_value() &&
         target->type != *entered_page.classified_type);
    if (entered_identity_conflict || event_identity_conflict) {
        json::object evidence {
            { "floor", page_floor },
            { "target", page_node },
            { "map_node_type", std::string(to_string(map_type)) },
            { "map_node_name", map_name },
            { "entered_node_type",
              entered_page.classified_type.has_value()
                  ? std::string(to_string(*entered_page.classified_type))
                  : std::string("unclassified") },
            { "event_name", entered_page.event_name.value_or(std::string {}) },
        };
        Log.warn("BlackFlow entered event type conflicts with map identity", "evidence", evidence);
        queue_warning(
            event_identity_conflict ? "event_node_type_conflict" : "entered_page_identity_conflict",
            event_identity_conflict
                ? "事件标题推导的节点类型与落点当前身份不一致；已采用事件标题"
                : "进入后的页面分类与当前地图节点身份不一致；已采用页面分类",
            DiagnosticTrigger::IdentityConflict,
            std::move(evidence));
    }
    PageIdentityResolution identity = resolve_page_identity(map_type, map_name, preview, entered_page);

    // 隐藏节点要进来才认得出身份。地图阶段算出的意图是按 hide_invisible 定的，沿用它会把
    // 秘境行商这类节点分流到通用页面。这里按真实身份重新解析一次。
    //
    // 顺序不能颠倒：意图来自里程碑，而里程碑的 active_if 可能依赖身份带来的派生事实，
    // 所以要先补事实、再刷新里程碑、最后解析意图。地图节点本身仍由 finalize_entered_node
    // 在页面结束后写回，避免在事务提交之后改动地图版本。
    if (identity.type != map_type) {
        std::string identity_error;
        if (identity.type == NodeType::ScrapShop) {
            (void)set_fact("scrap_shop_available", true, &identity_error);
        }
        else if (identity.type == NodeType::Portal) {
            (void)set_fact("portal_available", true, &identity_error);
        }
        refresh_mission();
    }
    std::string page_intent = resolve_page_intent(identity, page_node, page_floor);
    if (identity.type == NodeType::Final && proposal.bypass_final_on_completion) {
        page_intent = "final.pass";
    }

    m_page_context = PageExecutionContext {
        m_run_revision,
        ++m_page_revision,
        m_decision_id,
        m_transaction_id,
        page_floor,
        page_node,
        identity.type,
        std::move(identity.name),
        std::move(page_intent),
        std::move(entered_page.matched_texts),
        {},
        PageExecutionStage::PendingDispatch,
    };
    if (entered_page.event_name.has_value()) {
        m_page_context->observed_contents.emplace_back(*entered_page.event_name);
        m_page_context->identity_from_event_name = event_effect.resolved_type.has_value();
        m_page_context->has_landing = event_effect.has_landing;
        m_page_context->changes_floor = event_effect.changes_floor;
    }
    if (m_pending_probe_target == proposal.target) {
        m_pending_probe_target.reset();
    }
    queue_decision();
    return true;
}

void BlackFlowSession::cancel_transaction()
{
    if (m_transaction.has_value()) {
        m_transaction->cancel();
        m_transaction.reset();
    }
    m_verified_move_arc.reset();
    m_transaction_id.clear();
    m_pending_move_node_attributions.clear();
}

bool BlackFlowSession::set_current_floor(int floor, std::string* error)
{
    if (floor <= 0) {
        if (error != nullptr) {
            *error = "recognized floor must be positive";
        }
        m_current_floor.reset();
        return false;
    }
    const bool floor_entered = !m_current_floor.has_value() || *m_current_floor != floor;
    const bool new_map_generation = floor_entered || m_floor_recognition_pending;
    m_current_map_is_floor_four_remembrance =
        floor == 4 && m_run.floor == 4 && m_floor_recognition_pending;
    m_current_floor = floor;
    if (m_collection_popup_pursuit_floor.has_value() && *m_collection_popup_pursuit_floor != floor) {
        m_collection_popup_pursuit_floor.reset();
        m_collection_popup_pursuit_stage_name.clear();
        m_collection_popup_pursuit_total_kills.reset();
    }
    if (floor != 3) {
        m_floor_three_pursuit_battle_pending = false;
    }
    m_next_level_transition_confirmed = m_floor_recognition_pending;
    m_floor_recognition_pending = false;
    if (new_map_generation) {
        ++m_map_generation;
        m_map_preserved_after_inventory.reset();
        m_utopia_effect_expired = false;
        m_utopia_status.clear();
        m_utopia_reason.clear();
        m_utopia_ideology.clear();
        m_utopia_policy.clear();
        m_ideal_source.reset();
        m_ideal_source_generation.reset();
        m_ideal_domain.clear();
        m_observed_ideal_domain.clear();
        m_ideal_source_score_margin = 0.0;
        m_ideal_source_heads_agree = false;
    }
    if (floor_entered) {
        // NextLevel 比地图合并更早知道楼层变化。这里立即让零件箱观测失效，保证首轮规划
        // 不会沿用上一层或开局配置推定出来的加工品状态。
        m_movement_inventory_refresh_required = next_movement_inventory_refresh_state(
            m_movement_inventory_refresh_required,
            MovementInventoryRefreshEvent::FloorEntered);
        // 同一楼层打完战斗回图也可能再次识别到楼层标题，不能因此清空本层已经积累的事实。
        m_facts.begin_floor();
    }
    if (!set_fact("current_floor", static_cast<std::int64_t>(floor), error)) {
        return false;
    }
    // 有些终点只看楼层号，认出标题就该收工，不必等这一层的地图重建成功。
    // 这里不刷新里程碑：它按 m_run.floor 算窗口，而那还是上一层的值。
    evaluate_terminal_rules();
    return true;
}

void BlackFlowSession::clear_current_floor() noexcept
{
    m_current_floor.reset();
    m_floor_recognition_pending = true;
    m_next_level_transition_confirmed = false;
}

bool BlackFlowSession::completed_page_changes_floor() const noexcept
{
    if (!m_transaction.has_value() || !m_page_context.has_value() ||
        m_transaction->stage() != MoveTransactionStage::PageResolved ||
        m_page_context->stage != PageExecutionStage::Resolved) {
        return false;
    }

    const NodeType node_type = m_page_context->node_type;
    if (node_type == NodeType::Final && m_page_context->page_intent == "final.pass") {
        return false;
    }
    // Portal returns to the same main-map floor and does not require NextLevel.
    return m_page_context->changes_floor || node_type == NodeType::Final || node_type == NodeType::Evacuate ||
           node_type == NodeType::BattleBoss;
}

bool BlackFlowSession::mark_page_running(std::string* error)
{
    if (!m_page_context.has_value() || !m_transaction.has_value() ||
        m_transaction->stage() != MoveTransactionStage::Committed ||
        m_page_context->stage != PageExecutionStage::PendingDispatch) {
        if (error != nullptr) {
            *error = "node dispatch has no committed BlackFlow page";
        }
        return false;
    }
    m_page_context->stage = PageExecutionStage::Running;
    return true;
}

bool BlackFlowSession::apply_node_signal(
    const NodeStrategySignal& signal,
    const json::value& callback_details,
    std::string* error)
{
    if (signal.kind == NodeSignalKind::Set) {
        if (!signal.value.has_value()) {
            if (error != nullptr) {
                *error = "set signal has no value";
            }
            return false;
        }
        FactValue value = std::visit([](const auto& item) -> FactValue { return item; }, *signal.value);
        return set_fact(signal.fact, std::move(value), error);
    }
    if (signal.kind == NodeSignalKind::Add) {
        if (!signal.value.has_value() || !std::holds_alternative<std::int64_t>(*signal.value)) {
            if (error != nullptr) {
                *error = "add signal has no integer value";
            }
            return false;
        }
        const FactValue* current = m_facts.find(signal.fact);
        if (current == nullptr || !std::holds_alternative<std::int64_t>(*current)) {
            if (error != nullptr) {
                *error = "add signal target is not an initialized integer fact";
            }
            return false;
        }
        const std::int64_t delta = std::get<std::int64_t>(*signal.value);
        return set_fact(signal.fact, saturated_add(std::get<std::int64_t>(*current), delta), error);
    }

    const std::string text = callback_details.get("details", "result", "text", "");
    std::smatch match;
    if (!std::regex_search(text, match, std::regex(R"([-+]?\d+)"))) {
        if (error != nullptr) {
            *error = "capture_integer node result found no integer in details.result.text";
        }
        return false;
    }
    std::int64_t parsed = 0;
    try {
        parsed = std::stoll(match.str());
    }
    catch (const std::exception&) {
        if (error != nullptr) {
            *error = "captured integer is outside the supported range";
        }
        return false;
    }
    parsed = std::clamp<std::int64_t>(parsed, signal.minimum, signal.maximum);
    return set_fact(signal.fact, parsed, error);
}

void BlackFlowSession::queue_node_resolution(const PageExecutionContext& context)
{
    NodeType resolved_type = context.node_type;
    NodeProgress progress = NodeProgress::Completed;
    bool repeatable = false;
    bool becomes_empty = false;
    if (context.result.has_value()) {
        const NodeStateUpdate& result = *context.result;
        resolved_type = result.actual_type.value_or(resolved_type);
        progress = result.progress.value_or(progress);
        repeatable = result.repeatable.value_or(false);
        becomes_empty = result.becomes_empty.value_or(false);
    }

    std::vector<json::value> observed_contents;
    observed_contents.reserve(context.observed_contents.size());
    for (const std::string& content : context.observed_contents) {
        observed_contents.emplace_back(content);
    }
    json::object details {
        { "run_revision", context.run_revision },
        { "observation_id", m_observation_id },
        { "decision_id", context.decision_id },
        { "transaction_id", context.transaction_id },
        { "page_revision", context.page_revision },
        { "floor", context.floor },
        { "node", context.node },
        { "event_name", context.node_name },
        { "observed_contents", json::array(std::move(observed_contents)) },
        { "node_type", std::string(to_string(resolved_type)) },
        { "progress",
          progress == NodeProgress::Active ? "active"
                                           : (progress == NodeProgress::Completed ? "completed" : "removed") },
        { "repeatable", repeatable },
        { "becomes_empty", becomes_empty },
    };
    if (context.battle.has_value()) {
        details["battle"] = serialize_node_battle(*context.battle);
        if (context.battle->total_kills.has_value()) {
            details["battle_total_kills"] = *context.battle->total_kills;
        }
    }
    Log.info(
        "BlackFlow node resolution",
        "floor",
        context.floor,
        "node",
        context.node,
        "event",
        context.node_name,
        "type",
        to_string(resolved_type));
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowNodeResolution", std::move(details) });
}

bool BlackFlowSession::observe_page_content(std::string content, std::string source, std::string* error)
{
    if (!m_page_context.has_value() || m_page_context->stage != PageExecutionStage::Running) {
        if (error != nullptr) {
            *error = "page content observation has no running BlackFlow node";
        }
        return false;
    }
    if (content.empty() || source.empty()) {
        if (error != nullptr) {
            *error = "page content observation must contain content and source";
        }
        return false;
    }

    PageExecutionContext& context = *m_page_context;
    const bool first_content = context.observed_contents.empty();
    const Node* existing_noted =
        context.node == InvalidNodeId ? nullptr : m_exploration_notebook.snapshot().find_node(context.node);
    const bool page_is_fate_event =
        context.node_name == "命运所指" || (existing_noted != nullptr && existing_noted->fate_event);
    const PageContentEffect effect = classify_page_content_effect(source, content);
    if (effect.resolved_type.has_value()) {
        if (context.identity_from_event_name && context.node_type != *effect.resolved_type) {
            queue_warning(
                "event_page_type_conflict",
                "同一事件节点的后续页面名称推导出了不同节点类型；已采用最新事件页",
                DiagnosticTrigger::IdentityConflict,
                json::object {
                    { "floor", context.floor },
                    { "node", context.node },
                    { "previous_node_type", std::string(to_string(context.node_type)) },
                    { "event_node_type", std::string(to_string(*effect.resolved_type)) },
                    { "event_name", content },
                });
        }
        context.node_type = *effect.resolved_type;
        context.identity_from_event_name = true;
    }
    context.changes_floor = context.changes_floor || effect.changes_floor;
    context.has_landing = context.has_landing && effect.has_landing;
    if (std::ranges::find(context.observed_contents, content) == context.observed_contents.end()) {
        context.observed_contents.emplace_back(content);
    }
    if (first_content) {
        // 地图上显示的是节点类别；页面回调给出的第一项才是该节点的具体事件或战斗关卡。
        context.node_name = content;
    }

    bool notebook_identity_updated = false;
    if (context.has_landing && context.node != InvalidNodeId &&
        context.floor == m_exploration_notebook.floor()) {
        Node noted;
        if (existing_noted != nullptr) {
            noted = *existing_noted;
        }
        else {
            noted.id = context.node;
            noted.floor = context.floor;
            noted.type = context.node_type;
            noted.traversal = default_traversal_for(noted.type);
        }
        if (effect.resolved_type.has_value()) {
            noted.type = *effect.resolved_type;
            noted.traversal = default_traversal_for(noted.type);
            noted.identity_revealed = true;
            noted.identity_state = NodeIdentityState::Classified;
            noted.identity_from_topology = false;
            noted.identity_from_prediction = false;
            noted.prediction_rule.clear();
            noted.identity_source = "event_name";
            noted.detected_by_vision = true;
        }
        if (first_content || noted.name.empty()) {
            noted.name = content;
            noted.identity_source = source;
        }
        noted.fate_event = noted.fate_event || page_is_fate_event;
        notebook_identity_updated = m_exploration_notebook.snapshot().upsert_node(std::move(noted));
    }

    json::object details {
        { "run_revision", context.run_revision },
        { "observation_id", m_observation_id },
        { "decision_id", context.decision_id },
        { "transaction_id", context.transaction_id },
        { "page_revision", context.page_revision },
        { "floor", context.floor },
        { "node", context.node },
        { "node_type", std::string(to_string(context.node_type)) },
        { "content", content },
        { "source", source },
    };
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowNodeContentObserved", details });
    // 未知诡秘可能在页面结算后直接跨层。身份虽然已经写入当前层探索笔记，但如果
    // 不在切换笔记前持久化一次，最终 routing-history 仍只会留下进入前的未知节点。
    if (notebook_identity_updated && first_content && effect.resolved_type.has_value()) {
        details["reason_category"] = "node_identity_resolved";
        details["reason_detail"] = "事件标题已回写节点具体类型与名称";
        append_map_visualization(details);
        request_diagnostics(DiagnosticTrigger::NodeIdentityResolved, std::move(details));
    }
    return true;
}

bool BlackFlowSession::observe_battle_stage_name(std::string stage_name, std::string* error)
{
    if (stage_name.empty()) {
        if (error != nullptr) {
            *error = "battle stage observation has an empty normalized stage name";
        }
        return false;
    }

    const bool running_page =
        m_page_context.has_value() && m_page_context->stage == PageExecutionStage::Running;
    const bool running_combat_page = running_page && is_combat_node_type(m_page_context->node_type);
    const bool entered_floor_three_boss =
        running_combat_page && m_page_context->floor == 3 && m_page_context->node_type == NodeType::BattleBoss;
    const bool pursued_floor_three_boss =
        m_floor_three_pursuit_battle_pending && m_current_floor.value_or(m_run.floor) == 3;

    // 每个页面最多一场战斗。普通战斗会同时用关卡名细化节点名；事件内嵌战斗
    // 只附加 battle，绝不能覆盖事件节点自身的类型和名称。
    if (running_page) {
        if (!m_page_context->battle.has_value() || m_page_context->battle->stage_name != stage_name) {
            m_page_context->battle = NodeBattleRecord { stage_name, std::nullopt };
        }
    }
    if (running_combat_page && !observe_page_content(stage_name, "StageInfo", error)) {
        return false;
    }
    if (running_page && !running_combat_page) {
        m_telemetry_events.emplace_back(
            BlackFlowTelemetryEvent {
                "BlackFlowNestedBattleStageObserved",
                json::object {
                    { "floor", m_page_context->floor },
                    { "node", m_page_context->node },
                    { "node_type", std::string(to_string(m_page_context->node_type)) },
                    { "node_name", m_page_context->node_name },
                    { "stage_name", stage_name },
                    { "entry_kind", "event_nested_battle" },
                },
            });
    }
    if (!entered_floor_three_boss && !pursued_floor_three_boss) {
        return true;
    }

    m_floor_three_pursuit_battle_pending = false;
    if (pursued_floor_three_boss) {
        m_collection_popup_pursuit_stage_name = stage_name;
    }
    std::optional<Node> boss;
    if (entered_floor_three_boss && m_page_context->node != InvalidNodeId) {
        if (const Node* noted = m_exploration_notebook.snapshot().find_node(m_page_context->node);
            noted != nullptr && noted->type == NodeType::BattleBoss) {
            boss = *noted;
        }
        else if (const Node* current = m_map.snapshot().find_node(m_page_context->node);
                 current != nullptr && current->type == NodeType::BattleBoss) {
            boss = *current;
        }
    }
    if (!boss.has_value()) {
        boss = floor_boss_node(m_exploration_notebook.snapshot(), m_map.snapshot(), 3);
    }
    if (!boss.has_value()) {
        if (error != nullptr) {
            *error = "floor 3 boss stage was recognized but no unique battle boss node exists in the map notebook";
        }
        return false;
    }

    boss->name = stage_name;
    boss->type = NodeType::BattleBoss;
    boss->traversal = default_traversal_for(NodeType::BattleBoss);
    boss->identity_revealed = true;
    boss->identity_state = NodeIdentityState::Classified;
    boss->identity_source = "battle_stage_name";
    if (!m_exploration_notebook.snapshot().upsert_node(*boss)) {
        if (error != nullptr) {
            *error = "failed to write the floor 3 boss stage into the exploration notebook";
        }
        return false;
    }

    json::object details {
        { "floor", 3 },
        { "node", boss->id },
        { "node_type", std::string(to_string(NodeType::BattleBoss)) },
        { "stage_name", stage_name },
        { "source", "StageInfo" },
        { "entry_kind", pursued_floor_three_boss ? "pursuit" : "map_node" },
        { "action_points_before", m_run.resources.action_points },
        { "action_points_after", m_run.resources.action_points },
        { "total_candidates", 0 },
        { "eligible_candidates", 0 },
        { "reason_category", "battle_stage_observation" },
        { "reason_detail", "战斗插件识别为「" + stage_name + "」，已回写三层险路恶敌关卡名" },
    };
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowBossStageObserved", details });
    append_map_visualization(details);
    request_diagnostics(DiagnosticTrigger::BattleStageObservation, std::move(details));
    return true;
}

bool BlackFlowSession::observe_battle_total_kills(
    std::string stage_name,
    int total_kills,
    std::string* error)
{
    if (stage_name.empty() || total_kills <= 0) {
        if (error != nullptr) {
            *error = "battle total-kills observation requires a stage name and positive N";
        }
        return false;
    }

    const bool pursuit = m_collection_popup_pursuit_floor.has_value();
    const bool running_battle_page =
        m_page_context.has_value() && m_page_context->stage == PageExecutionStage::Running &&
        (m_page_context->battle.has_value() || is_combat_node_type(m_page_context->node_type));
    if (!pursuit && !running_battle_page) {
        return true;
    }

    NodeId node_id = InvalidNodeId;
    int floor = m_current_floor.value_or(m_run.floor);
    std::string entry_kind = "map_node";
    if (pursuit) {
        entry_kind = "pursuit";
        floor = *m_collection_popup_pursuit_floor;
        m_collection_popup_pursuit_total_kills = std::max(
            m_collection_popup_pursuit_total_kills.value_or(0),
            total_kills);
        if (auto boss = floor_boss_node(m_exploration_notebook.snapshot(), m_map.snapshot(), floor);
            boss.has_value()) {
            node_id = boss->id;
            Node updated = std::move(*boss);
            updated.battle = NodeBattleRecord { stage_name, total_kills };
            if (m_exploration_notebook.floor() == floor) {
                m_exploration_notebook.snapshot().upsert_node(updated);
            }
            if (m_map.floor() == floor && m_map.snapshot().find_node(updated.id) != nullptr) {
                m_map.snapshot().upsert_node(std::move(updated));
            }
        }
    }
    else {
        PageExecutionContext& context = *m_page_context;
        floor = context.floor;
        node_id = context.node;
        if (!is_combat_node_type(context.node_type)) {
            entry_kind = "event_nested_battle";
        }
        context.battle = NodeBattleRecord { stage_name, total_kills };
    }

    m_telemetry_events.emplace_back(
        BlackFlowTelemetryEvent {
            "BlackFlowBattleTotalKillsObserved",
            json::object {
                { "floor", floor },
                { "node", node_id },
                { "stage_name", stage_name },
                { "battle_total_kills", total_kills },
                { "aggregation", "mode_latest_tiebreak" },
                { "entry_kind", std::move(entry_kind) },
            },
        });
    return true;
}

void BlackFlowSession::record_virtual_auto_skill_activation(std::string_view device_name)
{
    if (m_profile != "automation_collection" || device_name != "丰饶树冢") {
        return;
    }

    if (m_page_context.has_value() && m_page_context->stage == PageExecutionStage::Running) {
        const PageExecutionContext& context = *m_page_context;
        if (context.has_landing && context.node != InvalidNodeId) {
            m_node_attribution_records.emplace_back(
                NodeAttributionRecord { context.floor, context.node, {}, "丰饶树冢" });
        }
        else if (!context.has_landing && !context.node_name.empty()) {
            m_node_attribution_records.emplace_back(
                NodeAttributionRecord { context.floor, InvalidNodeId, context.node_name, "丰饶树冢" });
        }
        else if (m_transaction.has_value()) {
            // 小八界进入同类型页面时，页面阶段可能还不能唯一绑定实际落点；
            // 等回图差分确定节点后再把每次成功释放逐行落盘。
            m_pending_move_node_attributions.emplace_back("丰饶树冢");
        }
        return;
    }

    if (m_collection_popup_pursuit_floor.has_value()) {
        m_node_attribution_records.emplace_back(
            NodeAttributionRecord {
                *m_collection_popup_pursuit_floor,
                InvalidNodeId,
                "追猎",
                "丰饶树冢",
            });
    }
}

std::optional<NodeId> BlackFlowSession::next_battle_intel_probe() const
{
    if (m_profile != "automation_collection" || m_map.floor() <= 0) {
        return std::nullopt;
    }

    const Node* selected = nullptr;
    for (const auto& [id, node] : m_map.snapshot().nodes()) {
        if ((node.type != NodeType::BattleNormal && node.type != NodeType::BattleElite) || !node.identity_revealed ||
            node.progress != NodeProgress::Active || m_battle_intel_probed.contains(id) ||
            battle_intel_probe_blocked_by_resident(node) || m_viewport.find(id) == nullptr) {
            continue;
        }
        if (const Node* noted = m_exploration_notebook.snapshot().find_node(id);
            noted != nullptr && !noted->name.empty() && noted->name != "作战" && noted->name != "紧急作战") {
            continue;
        }
        if (selected == nullptr || node.position.row < selected->position.row ||
            (node.position.row == selected->position.row && node.position.column < selected->position.column)) {
            selected = &node;
        }
    }
    return selected == nullptr ? std::nullopt : std::optional<NodeId>(selected->id);
}

bool BlackFlowSession::record_battle_intel_probe(
    NodeId node,
    std::optional<std::string> stage_name,
    std::string observation_error,
    std::string* error)
{
    const Node* current = m_map.snapshot().find_node(node);
    if (current == nullptr || (current->type != NodeType::BattleNormal && current->type != NodeType::BattleElite)) {
        if (error != nullptr) {
            *error = "battle intel observation no longer refers to a revealed normal or emergency battle";
        }
        return false;
    }
    const bool first_probe = m_battle_intel_probed.emplace(node).second;
    if (!first_probe) {
        return true;
    }

    if (stage_name.has_value() && !stage_name->empty() && m_exploration_notebook.floor() == current->floor) {
        Node noted = *current;
        if (const Node* existing = m_exploration_notebook.snapshot().find_node(node); existing != nullptr) {
            noted = *existing;
            noted.type = current->type;
            noted.traversal = default_traversal_for(current->type);
        }
        noted.name = *stage_name;
        noted.identity_revealed = true;
        noted.identity_state = NodeIdentityState::Classified;
        noted.identity_source = "move_preview_stage_name";
        m_exploration_notebook.snapshot().upsert_node(std::move(noted));
    }

    json::object details {
        { "run_revision", m_run_revision },
        { "observation_id", m_observation_id },
        { "floor", current->floor },
        { "node", node },
        { "node_type", std::string(to_string(current->type)) },
        { "succeeded", stage_name.has_value() && !stage_name->empty() },
        { "stage_name", stage_name.value_or("") },
        { "source", "move_preview_stage_name" },
    };
    if (!observation_error.empty()) {
        details["error"] = std::move(observation_error);
    }
    m_telemetry_events.emplace_back(BlackFlowTelemetryEvent { "BlackFlowBattleIntelObserved", std::move(details) });
    return true;
}

bool BlackFlowSession::apply_node_task_result(
    const NodeTaskResult& result,
    const json::value& callback_details,
    std::string* error)
{
    if (!m_page_context.has_value() || !m_transaction.has_value()) {
        if (error != nullptr) {
            *error = "node task result arrived without an active BlackFlow page";
        }
        return false;
    }

    for (const NodeStrategySignal& signal : result.signals) {
        if (!apply_node_signal(signal, callback_details, error)) {
            return false;
        }
    }

    NodeStateUpdate update = result.node;
    if (result.kind == NodeTaskResultKind::PageCompleted && m_page_context->node_type == NodeType::Final &&
        m_transaction->proposal().bypass_final_on_completion) {
        // 第三项“路过”会回到同层，险路尽头保持可再次进入；它不是节点完成，更不能
        // 触发终点里程碑或 NextLevel。
        update.progress = NodeProgress::Active;
        update.repeatable = true;
        update.becomes_empty = false;
    }
    if (!update.actual_name_source.empty()) {
        const std::string captured_name = callback_details.get("details", "result", "text", "");
        if (captured_name.empty()) {
            if (error != nullptr) {
                *error = "node result found no event name in details.result.text";
            }
            return false;
        }
        update.actual_name = captured_name;
    }
    const bool has_node_update = update.progress.has_value() || update.actual_type.has_value() ||
                                 update.actual_name.has_value() || update.identity_revealed.has_value() ||
                                 update.repeatable.has_value() || update.becomes_empty.has_value();
    if (result.redispatch && !update.actual_type.has_value() && !update.actual_name.has_value()) {
        if (error != nullptr) {
            *error = "node redispatch requires an updated event name or node type";
        }
        return false;
    }
    if (has_node_update) {
        NodeStateUpdate merged = m_page_context->result.value_or(NodeStateUpdate {});
        if (update.progress.has_value()) {
            merged.progress = update.progress;
        }
        if (update.actual_type.has_value()) {
            merged.actual_type = update.actual_type;
            m_page_context->node_type = *update.actual_type;
        }
        if (update.actual_name.has_value()) {
            merged.actual_name = update.actual_name;
            m_page_context->node_name = *update.actual_name;
        }
        if (update.identity_revealed.has_value()) {
            merged.identity_revealed = update.identity_revealed;
        }
        if (update.repeatable.has_value()) {
            merged.repeatable = update.repeatable;
        }
        if (update.becomes_empty.has_value()) {
            merged.becomes_empty = update.becomes_empty;
        }
        m_page_context->result = std::move(merged);
    }

    if (result.kind == NodeTaskResultKind::PageCompleted) {
        if (!m_transaction->mark_page_resolved(error)) {
            return false;
        }
        m_page_context->stage = PageExecutionStage::Resolved;
        m_movement_inventory_refresh_required = next_movement_inventory_refresh_state(
            m_movement_inventory_refresh_required,
            MovementInventoryRefreshEvent::NodeCompleted);

        const bool landing_still_unknown = m_page_context->has_landing &&
                                           m_page_context->node == InvalidNodeId &&
                                           !m_transaction->proposal().controllable;
        if (!landing_still_unknown) {
            queue_node_resolution(*m_page_context);
            m_page_context->resolution_reported = true;
        }
    }
    refresh_mission();
    evaluate_terminal_rules();

    if (result.terminate && !m_result.has_value()) {
        const int cultivated = static_cast<int>(integer_fact(m_facts.merged(), "cultivated_animals"));
        m_result = BlackFlowStrategyResult {
            m_profile, result.outcome_code, result.termination_reason, std::clamp(cultivated, 0, 3), result.succeeded,
        };
        if (!result.succeeded && m_policy.has_value()) {
            m_result->next_action = m_policy->failure_action;
        }
    }
    return true;
}

void BlackFlowSession::fail(std::string outcome, std::string reason, FailureDisposition disposition)
{
    if (m_result.has_value()) {
        // 终局规则可能已写入结果（如培育完成）；后到的失败路径不得改写已定局的本局结局，
        // 与 apply_node_task_result 成功路径的 !m_result.has_value() 保护保持一致
        Log.warn(__FUNCTION__, "ignore failure; strategy result already present", outcome, reason);
        return;
    }
    if (outcome == "map_rebuild_failed") {
        queue_warning("map_rebuild_failed", reason, DiagnosticTrigger::MapRebuildFailed);
    }
    else if (outcome == "page_recovery_failed") {
        queue_warning("page_recovery_failed", reason, DiagnosticTrigger::PageRecoveryFailed);
    }
    m_result = BlackFlowStrategyResult {
        m_profile,
        std::move(outcome),
        std::move(reason),
        static_cast<int>(std::clamp<std::int64_t>(integer_fact(m_facts.merged(), "cultivated_animals"), 0, 3)),
        false,
    };
    m_result->next_action = failure_next_action(disposition);
}

bool BlackFlowSession::claim_result_report() noexcept
{
    if (!m_result.has_value() || m_result_reported) {
        return false;
    }
    m_result_reported = true;
    return true;
}

std::vector<BlackFlowTelemetryEvent> BlackFlowSession::take_telemetry_events()
{
    std::vector<BlackFlowTelemetryEvent> result;
    result.swap(m_telemetry_events);
    return result;
}

std::vector<NodeAttributionRecord> BlackFlowSession::take_node_attribution_records()
{
    std::vector<NodeAttributionRecord> result;
    result.swap(m_node_attribution_records);
    return result;
}

json::object BlackFlowSession::run_log_state() const
{
    std::vector<json::value> movement_instances;
    movement_instances.reserve(m_run.resources.movement_instances.size());
    for (const RunResources::MovementInstance& instance : m_run.resources.movement_instances) {
        movement_instances.emplace_back(
            json::object {
                { "movement", std::string(to_string(instance.movement)) },
                { "remaining_charges", instance.remaining_charges },
                { "inventory_index", instance.inventory_index },
                { "loaded", instance.loaded },
            });
    }
    std::vector<json::value> visited;
    visited.reserve(m_run.visited_nodes.size());
    for (const NodeId node : m_run.visited_nodes) {
        visited.emplace_back(node);
    }
    std::vector<json::value> revealed;
    revealed.reserve(m_run.revealed_nodes.size());
    for (const NodeId node : m_run.revealed_nodes) {
        revealed.emplace_back(node);
    }

    json::object result {
        { "profile", m_profile },
        { "run_revision", m_run_revision },
        { "floor", m_current_floor.value_or(m_run.floor) },
        { "map_generation", m_map_generation },
        { "map_revision", m_map.snapshot().revision },
        { "viewport_revision", m_viewport.viewport_revision() },
        { "current_node", m_run.current_node },
        { "observed_current_node", m_observed_current_node },
        { "observation_id", m_observation_id },
        { "decision_id", m_decision_id },
        { "transaction_id", m_transaction_id },
        { "action_points", m_run.resources.action_points },
        { "hope", m_run.resources.hope },
        { "ingots", m_run.resources.ingots },
        { "seeds", m_run.resources.seeds },
        { "sellable_scraps", m_run.resources.sellable_scraps },
        { "white_model_birds", m_run.resources.white_model_birds },
        { "painted_liberi", m_run.resources.painted_liberi },
        { "resources_revision", m_run.resources_revision },
        { "active_movement",
          m_run.active_movement.has_value() ? std::string(to_string(*m_run.active_movement)) : std::string() },
        { "movement_instances", json::array(std::move(movement_instances)) },
        { "visited_nodes", json::array(std::move(visited)) },
        { "revealed_nodes", json::array(std::move(revealed)) },
        { "strategy_terminal", m_run.strategy_terminal },
        { "terminated", terminated() },
        { "movement_inventory_refresh_required", m_movement_inventory_refresh_required },
    };
    if (m_transaction.has_value()) {
        result["transaction"] = json::object {
            { "stage", static_cast<int>(m_transaction->stage()) },
            { "action_id", m_transaction->proposal().action_id },
            { "movement", std::string(to_string(m_transaction->proposal().movement)) },
            { "source", m_transaction->proposal().source },
            { "target", m_transaction->proposal().target },
            { "landing", m_transaction->proposal().landing },
            { "authoritative_cost", m_transaction->authoritative_cost() },
        };
    }
    if (m_page_context.has_value()) {
        result["page"] = json::object {
            { "page_revision", m_page_context->page_revision },
            { "floor", m_page_context->floor },
            { "node", m_page_context->node },
            { "node_type", std::string(to_string(m_page_context->node_type)) },
            { "node_name", m_page_context->node_name },
            { "stage", static_cast<int>(m_page_context->stage) },
            { "changes_floor", m_page_context->changes_floor },
            { "has_landing", m_page_context->has_landing },
        };
        if (m_page_context->battle.has_value()) {
            result["page"]["battle"] = serialize_node_battle(*m_page_context->battle);
            if (m_page_context->battle->total_kills.has_value()) {
                result["page"]["battle_total_kills"] = *m_page_context->battle->total_kills;
            }
        }
    }
    if (m_collection_popup_pursuit_floor.has_value()) {
        result["abstract_node"] = json::object {
            { "kind", "pursuit" },
            { "floor", *m_collection_popup_pursuit_floor },
            { "name", m_collection_popup_pursuit_stage_name },
            { "node_type", "battle_boss" },
            { "node_type_display", "险路恶敌" },
        };
        if (!m_collection_popup_pursuit_stage_name.empty()) {
            result["abstract_node"]["battle"] = serialize_node_battle(
                NodeBattleRecord {
                    m_collection_popup_pursuit_stage_name,
                    m_collection_popup_pursuit_total_kills,
                });
            if (m_collection_popup_pursuit_total_kills.has_value()) {
                result["abstract_node"]["battle_total_kills"] = *m_collection_popup_pursuit_total_kills;
            }
        }
    }
    return result;
}

std::vector<DiagnosticArtifactRequest> BlackFlowSession::take_diagnostic_requests()
{
    std::vector<DiagnosticArtifactRequest> result;
    result.swap(m_diagnostic_requests);
    return result;
}

} // namespace asst::blackflow
