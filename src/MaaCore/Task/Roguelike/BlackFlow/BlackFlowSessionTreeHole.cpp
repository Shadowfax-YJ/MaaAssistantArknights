#include "BlackFlowSession.h"

#include <algorithm>
#include <limits>
#include <map>

#include "BlackFlowAutomationStoreRules.h"
#include "BlackFlowInventoryRules.h"
#include "Config/TaskData.h"
#include "Utils/Logger.hpp"

namespace asst::blackflow
{
bool BlackFlowSession::tree_hole_nodes_shuffle() const noexcept
{
    return in_tree_hole() && m_tree_effect.find("全知者盲区") != std::string::npos;
}

bool BlackFlowSession::tree_hole_duel_used() const noexcept
{
    return in_tree_hole() && m_tree_effect.find("消耗螺旋") != std::string::npos && m_tree_duel_page != 0;
}

std::optional<std::size_t> BlackFlowSession::portal_choice(const std::vector<std::string>& prices) const
{
    if (m_profile != "automation_collection" || !m_page_context.has_value() ||
        m_page_context->node_type != NodeType::Portal || in_tree_hole()) {
        return std::nullopt;
    }
    const auto priority = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@MovementInventoryDiscardPriority");
    if (priority == nullptr) {
        return std::nullopt;
    }
    std::optional<std::size_t> selected;
    InventoryDiscardRank best;
    for (std::size_t index = 0; index < prices.size(); ++index) {
        for (std::size_t rank = 0; rank < priority->text.size(); ++rank) {
            const auto& name = priority->text[rank];
            if (prices[index].find(name) == std::string::npos) {
                continue;
            }
            const auto spec = std::ranges::find_if(movement_specs(), [&](const MovementSpec& value) {
                return value.kind != MovementKind::Walk && value.name == name;
            });
            if (spec == movement_specs().end()) {
                continue;
            }
            if (m_portal_page == m_page_context->page_revision && m_portal_item == prices[index]) {
                return index;
            }
            int charges = std::numeric_limits<int>::max();
            std::size_t pieces = 0;
            for (const auto& instance : m_run.resources.movement_instances) {
                if (instance.movement == spec->kind) {
                    ++pieces;
                    charges = std::min(charges, instance.remaining_charges);
                }
            }
            const auto quota = automation_store_purchase_quota(name);
            const InventoryDiscardRank candidate {
                quota.has_value() && pieces > *quota ? InventoryDiscardBand::QuotaExcess : InventoryDiscardBand::Normal,
                static_cast<int>(rank),
                charges,
            };
            if (!selected.has_value() || candidate < best) {
                selected = index;
                best = candidate;
            }
        }
    }
    return selected;
}

void BlackFlowSession::record_portal_choice(std::string item)
{
    if (!m_page_context.has_value() || m_page_context->node_type != NodeType::Portal || in_tree_hole()) {
        return;
    }
    m_portal_page = m_page_context->page_revision;
    m_portal_item = std::move(item);
    Log.info("BlackFlow portal entry selected", "item", m_portal_item, "page", m_portal_page);
}

bool BlackFlowSession::enter_tree_hole(std::string* error)
{
    if (!m_page_context.has_value() || m_page_context->node_type != NodeType::Portal ||
        m_portal_page != m_page_context->page_revision || !m_transaction.has_value() ||
        m_transaction->stage() != MoveTransactionStage::PageResolved || m_run.floor < 1 || m_run.floor > 5) {
        if (error != nullptr) {
            *error = "tree-hole entry has no resolved portal transaction";
        }
        return false;
    }
    m_tree_outer = std::make_shared<BlackFlowSession>(*this);
    m_tree_effect.clear();
    m_tree_effect_description.clear();
    m_tree_duel_page = 0;
    m_map.reset();
    m_exploration_notebook.reset();
    m_viewport.clear(0, 0);
    m_run.floor = 6;
    m_run.current_node = InvalidNodeId;
    m_run.resources.action_points = 0; // Replaced by the independent tree-hole HUD observation.
    m_run.visited_nodes.clear();
    m_run.revealed_nodes.clear();
    m_run.consumed_one_time_nodes.clear();
    m_run.node_progress.clear();
    m_run.costs.clear_action_cost_overrides();
    m_current_floor = 6;
    ++m_map_generation;
    m_map_section_generation = m_map_generation;
    m_initial_prediction_generation = 0;
    m_initial_reveal_checked_generation.reset();
    m_resident_settlement_prediction = {};
    m_floor_recognition_pending = false;
    m_next_level_transition_confirmed = false;
    m_current_map_is_floor_four_remembrance = false;
    m_unreachable_actions.clear();
    m_temporarily_unavailable_movements.clear();
    m_battle_intel_probed.clear();
    m_pending_probe_target.reset();
    m_verified_move_arc.reset();
    m_pending_candidate.reset();
    m_transaction.reset();
    m_page_context.reset();
    m_last_plan.reset();
    m_last_reveal_consistency.reset();
    m_map_preserved_after_inventory.reset();
    m_movement_inventory_refresh_required = true;
    m_utopia_status.clear();
    m_utopia_reason.clear();
    m_utopia_ideology.clear();
    m_utopia_policy.clear();
    m_ideal_source.reset();
    m_ideal_source_generation.reset();
    m_ideal_domain.clear();
    m_observed_ideal_domain.clear();
    m_utopia_effect_expired = false;
    Log.info(
        "BlackFlow suspended outer map for tree hole",
        "outer floor",
        outer_floor(),
        "portal",
        m_tree_outer->m_page_context->node,
        "item",
        m_portal_item);
    return true;
}

RouteContinuationEvaluator BlackFlowSession::tree_hole_continuation() const
{
    using Counts = std::array<int, ProcessingMovementSlotCount>;
    if (!in_tree_hole()) {
        return {};
    }
    auto projection = std::make_shared<BlackFlowSession>(*m_tree_outer);
    const MoveCandidate entry = projection->m_transaction->proposal();
    const int cost = projection->m_transaction->authoritative_cost();
    int gain = entry.predicted_action_point_gain;
    if (const auto found = entry.landing_action_point_gains.find(projection->m_page_context->node);
        found != entry.landing_action_point_gains.end()) {
        gain = found->second;
    }
    const int outer_ap = action_points_after(projection->m_run.resources.action_points, cost, gain);
    projection->m_run.resources = m_run.resources;          // Already rescanned after the entry sacrifice.
    projection->m_run.resources.action_points = outer_ap;
    projection->m_run.resources.movement_instances.clear(); // This copy carries projected aggregate charges only.
    projection->m_tree_continuation_projection = true;
    projection->m_transaction.reset();
    projection->m_pending_candidate.reset();
    projection->m_last_plan.reset();
    projection->m_movement_inventory_refresh_required = false;
    std::vector<NodeId> portals;
    if (projection->m_page_context->node != InvalidNodeId) {
        portals.emplace_back(projection->m_page_context->node);
    }
    else {
        for (NodeId id : entry.possible_landings) {
            const auto* node = projection->m_map.snapshot().find_node(id);
            if (node != nullptr && (node->type == NodeType::Portal || node->type == NodeType::HideInvisible ||
                                    node->type == NodeType::Unknown)) {
                portals.emplace_back(id);
            }
        }
    }
    projection->m_page_context.reset();
    const auto evaluate = [projection, portals](const Counts& used) {
        RouteContinuationValue worst { true, std::numeric_limits<int>::max(), std::numeric_limits<int>::max() };
        if (portals.empty()) {
            // Unknown return coordinates stay unknown; retain every processing charge until they are observed.
            return RouteContinuationValue { std::ranges::all_of(used, [](int n) { return n == 0; }), 0, 0 };
        }
        for (NodeId portal : portals) {
            BlackFlowSession candidate = *projection;
            candidate.m_run.current_node = portal;
            candidate.m_run.visited_nodes.insert(portal);
            candidate.m_run.node_progress[portal] = NodeProgress::Completed;
            if (const auto* old = candidate.m_map.snapshot().find_node(portal)) {
                Node completed = *old;
                completed.type = NodeType::Empty;
                completed.name = "林间空地";
                completed.progress = NodeProgress::Completed;
                completed.traversal = default_traversal_for(NodeType::Empty);
                candidate.m_map.snapshot().upsert_node(std::move(completed));
            }
            for (std::size_t i = 1; i < used.size(); ++i) {
                auto& available = candidate.m_run.resources.movement_charges[static_cast<MovementKind>(i)];
                if (used[i] > available) {
                    return RouteContinuationValue { false, 0, 0 };
                }
                available -= used[i];
            }
            std::string error;
            if (!candidate.synchronize_resource_facts(&error)) {
                return RouteContinuationValue { false, 0, 0 };
            }
            const BlackFlowPlan plan = candidate.plan_internal(false, false, &error);
            if (!plan || plan.endpoint_fallback_active) {
                return RouteContinuationValue { false, 0, 0 };
            }
            const auto summary = std::ranges::find_if(plan.decision.candidate_summaries, [&](const auto& value) {
                return value.move.action_id == plan.decision.selected->action_id;
            });
            if (summary == plan.decision.candidate_summaries.end()) {
                return RouteContinuationValue { false, 0, 0 };
            }
            worst.revealed_delta = std::min(worst.revealed_delta, summary->revealed_node_count);
            if (summary->effective_node_count < worst.effective_delta) {
                worst.effective_delta = summary->effective_node_count;
                worst.income_delta = summary->effective_node_income;
            }
        }
        return worst;
    };
    const RouteContinuationValue baseline = evaluate({});
    auto cache = std::make_shared<std::map<Counts, RouteContinuationValue>>();
    cache->emplace(Counts {}, RouteContinuationValue { true, 0, 0 });
    return [evaluate, baseline, cache](const Counts& used) {
        if (const auto found = cache->find(used); found != cache->end()) {
            return found->second;
        }
        // Bound work across root actions; unknown combinations preserve the outer inventory conservatively.
        if (cache->size() >= 48) {
            return RouteContinuationValue { false, 0, 0 };
        }
        RouteContinuationValue value = evaluate(used);
        if (baseline.safe) {
            value.revealed_delta -= baseline.revealed_delta;
            value.effective_delta -= baseline.effective_delta;
            value.income_delta.exploration -= baseline.income_delta.exploration;
            value.income_delta.development -= baseline.income_delta.development;
        }
        else {
            value.safe = false;
        }
        cache->emplace(used, value);
        return value;
    };
}

bool BlackFlowSession::restore_outer_map(int floor, std::string* error)
{
    if (!in_tree_hole() || floor != outer_floor()) {
        if (error != nullptr) {
            *error = "tree-hole return does not match its saved outer floor";
        }
        return false;
    }
    const auto outer = m_tree_outer;
    RunResources inventory = m_run.resources;
    // The last child move may exhaust AP and return without another child-map observation.
    // Settle its resource effect without inventing a child landing from the outer return marker.
    if (m_transaction.has_value() && (m_transaction->stage() == MoveTransactionStage::Committed ||
                                      m_transaction->stage() == MoveTransactionStage::PageResolved)) {
        if (const auto* movement = find_movement_spec(m_transaction->proposal().movement)) {
            inventory.hope += movement->effect.hope_gain;
            inventory.ingots += movement->effect.ingot_gain;
            if (movement->kind != MovementKind::Walk && m_page_context.has_value() &&
                is_route_battle_node_type(m_page_context->node_type) && inventory.white_model_birds > 0) {
                --inventory.white_model_birds;
            }
        }
    }
    const FactStore updated_facts = m_facts.merged();
    // Restore only map-local state. Diagnostic sequences and newly collected inventory remain current.
    m_map = outer->m_map;
    m_exploration_notebook = outer->m_exploration_notebook;
    m_run = outer->m_run;
    m_current_floor = outer->m_current_floor;
    m_facts = outer->m_facts;
    m_mission = outer->m_mission;
    m_transaction = outer->m_transaction;
    m_transaction_id = outer->m_transaction_id;
    m_decision_id = outer->m_decision_id;
    m_pending_move_node_attributions = outer->m_pending_move_node_attributions;
    m_page_context = outer->m_page_context;
    m_expedition_core_away = outer->m_expedition_core_away;
    m_resident_settlement_prediction = outer->m_resident_settlement_prediction;
    m_current_map_is_floor_four_remembrance = outer->m_current_map_is_floor_four_remembrance;
    m_topology_template_id = outer->m_topology_template_id;
    m_topology_source_digest = outer->m_topology_source_digest;
    m_topology_base_edge_count = outer->m_topology_base_edge_count;
    m_topology_extra_edge_count = outer->m_topology_extra_edge_count;
    m_topology_match_score = outer->m_topology_match_score;
    m_utopia_status = outer->m_utopia_status;
    m_utopia_reason = outer->m_utopia_reason;
    m_utopia_ideology = outer->m_utopia_ideology;
    m_utopia_policy = outer->m_utopia_policy;
    m_ideal_source = outer->m_ideal_source;
    m_ideal_source_generation = outer->m_ideal_source_generation;
    m_ideal_domain = outer->m_ideal_domain;
    m_observed_ideal_domain = outer->m_observed_ideal_domain;
    m_utopia_effect_expired = outer->m_utopia_effect_expired;
    m_ideal_source_score_margin = outer->m_ideal_source_score_margin;
    m_ideal_source_heads_agree = outer->m_ideal_source_heads_agree;
    m_battle_intel_probed = outer->m_battle_intel_probed;
    const int before_entry_ap = m_run.resources.action_points;
    m_run.resources = inventory;
    m_run.resources.action_points = before_entry_ap;
    ++m_run.resources_revision;
    for (const auto& [name, definition] : BlackFlowStrategy.facts()) {
        if (definition.scope == FactScope::Run) {
            if (const auto* value = updated_facts.find(name)) {
                (void)m_facts.set(FactScope::Run, name, *value);
            }
        }
    }
    m_tree_outer.reset();
    m_tree_effect.clear();
    m_tree_effect_description.clear();
    m_tree_duel_page = 0;
    m_portal_page = 0;
    m_current_floor = floor;
    ++m_map_generation;
    m_map_section_generation = outer->m_map_section_generation;
    if (m_ideal_source_generation.has_value()) {
        m_ideal_source_generation = m_map_generation;
    }
    m_initial_prediction_generation = m_map_generation;
    m_initial_reveal_checked_generation = m_map_generation;
    m_floor_recognition_pending = false;
    m_next_level_transition_confirmed = false;
    m_unreachable_actions.clear();
    m_temporarily_unavailable_movements.clear();
    m_pending_probe_target.reset();
    m_verified_move_arc.reset();
    m_pending_candidate.reset();
    m_last_plan.reset();
    m_last_reveal_consistency.reset();
    m_map_preserved_after_inventory.reset();
    m_movement_inventory_refresh_required = true;
    Log.info("BlackFlow restored outer map; resolve portal with actual return marker and preserved outer AP", floor);
    return true;
}
} // namespace asst::blackflow
