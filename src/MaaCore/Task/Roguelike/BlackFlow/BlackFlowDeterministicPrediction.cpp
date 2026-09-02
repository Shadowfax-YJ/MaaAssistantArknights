#include "BlackFlowDeterministicPrediction.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace asst::blackflow
{
namespace
{
using DistanceMap = std::unordered_map<NodeId, int>;

DistanceMap shortest_distances(const MapObservationBatch& map, NodeId source)
{
    std::unordered_map<NodeId, std::vector<NodeId>> adjacency;
    for (const ObservedEdge& edge : map.edges) {
        if (edge.knowledge != EdgeKnowledge::Confirmed) {
            continue;
        }
        const auto first = make_stable_node_id(map.floor, edge.first);
        const auto second = make_stable_node_id(map.floor, edge.second);
        if (!first.has_value() || !second.has_value()) {
            continue;
        }
        adjacency[*first].emplace_back(*second);
        adjacency[*second].emplace_back(*first);
    }
    DistanceMap distances;
    std::deque<NodeId> queue;
    distances.emplace(source, 0);
    queue.emplace_back(source);
    while (!queue.empty()) {
        const NodeId current = queue.front();
        queue.pop_front();
        const int next_distance = distances[current] + 1;
        for (const NodeId neighbor : adjacency[current]) {
            if (distances.emplace(neighbor, next_distance).second) {
                queue.emplace_back(neighbor);
            }
        }
    }
    return distances;
}

bool distance_from_start_is_allowed(int floor, int distance) noexcept
{
    if (floor == 2) {
        return distance >= 4 && distance <= 7;
    }
    return (floor == 4 || floor == 5) && distance >= 4;
}

std::vector<GridPosition> settlement_candidates_for_residents(
    const MapObservationBatch& map,
    NodeId original_start,
    bool floor_four_remembrance,
    const std::vector<GridPosition>& residents,
    const DistanceMap& from_start)
{
    std::vector<GridPosition> candidates;
    for (const ObservedNode& candidate : map.nodes) {
        if (candidate.type != NodeType::HideBattle) {
            continue;
        }
        const auto candidate_id = make_stable_node_id(map.floor, candidate.position);
        if (!candidate_id.has_value()) {
            continue;
        }
        const auto start_distance = from_start.find(*candidate_id);
        if (start_distance == from_start.end() || !distance_from_start_is_allowed(map.floor, start_distance->second)) {
            continue;
        }

        const DistanceMap from_candidate = shortest_distances(map, *candidate_id);
        const bool every_resident_is_nearby = std::ranges::all_of(residents, [&](GridPosition position) {
            const auto id = make_stable_node_id(map.floor, position);
            if (!id.has_value()) {
                return false;
            }
            const auto distance = from_candidate.find(*id);
            return distance != from_candidate.end() && distance->second >= 1 && distance->second <= 5;
        });
        if (!every_resident_is_nearby) {
            continue;
        }

        // 标准 2/4/5 层且该解释下居民不超过两个时，候选周围五步内的林间空地
        // 数量必须与居民数量一致。追忆四层规则不同；三个居民的解释也不使用该约束。
        if (!(map.floor == 4 && floor_four_remembrance) && residents.size() <= 2) {
            std::size_t nearby_forest_count = 0;
            for (const ObservedNode& node : map.nodes) {
                if (node.type != NodeType::Empty) {
                    continue;
                }
                const auto id = make_stable_node_id(map.floor, node.position);
                if (!id.has_value() || *id == original_start) {
                    continue;
                }
                const auto distance = from_candidate.find(*id);
                if (distance != from_candidate.end() && distance->second <= 5) {
                    ++nearby_forest_count;
                }
            }
            if (nearby_forest_count != residents.size()) {
                continue;
            }
        }
        candidates.emplace_back(candidate.position);
    }
    std::ranges::sort(candidates, {}, [](GridPosition position) {
        return std::pair { position.row, position.column };
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}
} // namespace

std::vector<std::vector<GridPosition>> resident_marker_hypotheses(const MapObservationBatch& map)
{
    std::vector<GridPosition> definite;
    std::vector<GridPosition> possible_overlaps;
    for (const ObservedNode& node : map.nodes) {
        // 这里推断的是“进入本层时”的初始居民位置；初始居民必定位于林间空地。
        // 非空地上的相似标记既不能成为确定居民，也不能成为重合候选。
        if (node.type != NodeType::Empty) {
            continue;
        }
        if (node.marker_type == "savage") {
            definite.emplace_back(node.position);
        }
        else if (node.marker_resident_overlap_possible.value_or(false)) {
            possible_overlaps.emplace_back(node.position);
        }
    }
    if (definite.empty()) {
        return {};
    }

    std::vector<std::vector<GridPosition>> hypotheses { definite };
    const std::size_t available_slots = definite.size() < 3 ? 3 - definite.size() : 0;
    const auto append_combinations = [&](auto&& self,
                                         std::size_t start,
                                         std::size_t remaining,
                                         std::vector<GridPosition>& current) -> void {
        if (remaining == 0) {
            hypotheses.emplace_back(current);
            return;
        }
        for (std::size_t index = start; index + remaining <= possible_overlaps.size(); ++index) {
            current.emplace_back(possible_overlaps[index]);
            self(self, index + 1, remaining - 1, current);
            current.pop_back();
        }
    };
    for (std::size_t count = 1; count <= std::min(available_slots, possible_overlaps.size()); ++count) {
        std::vector<GridPosition> current = definite;
        append_combinations(append_combinations, 0, count, current);
    }
    return hypotheses;
}

ResidentSettlementPrediction predict_resident_settlement(
    const MapObservationBatch& map,
    NodeId original_start,
    bool floor_four_remembrance)
{
    ResidentSettlementPrediction result;
    if (map.floor != 2 && map.floor != 4 && map.floor != 5) {
        return result;
    }
    if (std::ranges::any_of(map.nodes, [](const ObservedNode& node) {
            return node.type == NodeType::BattleSavage && node.identity_revealed.value_or(false);
        })) {
        return result;
    }

    const auto hypotheses = resident_marker_hypotheses(map);
    if (hypotheses.empty()) {
        return result;
    }
    result.initial_residents = hypotheses.front();
    for (const ObservedNode& node : map.nodes) {
        if (node.type == NodeType::Empty && node.marker_resident_overlap_possible.value_or(false)) {
            result.possible_overlap_residents.emplace_back(node.position);
        }
    }
    result.hypothesis_count = hypotheses.size();

    const DistanceMap from_start = shortest_distances(map, original_start);
    std::vector<std::vector<GridPosition>> candidates_by_hypothesis;
    candidates_by_hypothesis.reserve(hypotheses.size());
    for (const auto& residents : hypotheses) {
        candidates_by_hypothesis.emplace_back(
            settlement_candidates_for_residents(map, original_start, floor_four_remembrance, residents, from_start));
        result.candidates.insert(
            result.candidates.end(),
            candidates_by_hypothesis.back().begin(),
            candidates_by_hypothesis.back().end());
    }
    std::ranges::sort(result.candidates, {}, [](GridPosition position) {
        return std::pair { position.row, position.column };
    });
    result.candidates.erase(std::unique(result.candidates.begin(), result.candidates.end()), result.candidates.end());

    if (resident_settlement_hypotheses_have_exact_consensus(candidates_by_hypothesis)) {
        result.exact = candidates_by_hypothesis.front().front();
    }
    return result;
}

void apply_exact_resident_settlement_prediction(
    MapObservationBatch& map,
    const ResidentSettlementPrediction& prediction)
{
    if (!prediction.exact.has_value()) {
        return;
    }
    const auto found = std::ranges::find(map.nodes, *prediction.exact, &ObservedNode::position);
    if (found == map.nodes.end()) {
        return;
    }
    // 确定性规则只补充视觉仍未揭示的身份。后续真实观测一旦给出具体节点，必须保留
    // 实测结果，让调用方记录 prediction conflict，而不能再用旧预测覆盖它。
    if (found->identity_revealed.value_or(false)) {
        return;
    }
    found->type = NodeType::BattleSavage;
    found->name = "\u201c\u5c45\u6c11\u201d\u636e\u70b9";
    found->traversal = default_traversal_for(NodeType::BattleSavage);
    // 规则确定的是语义身份和位置，不是游戏视野。保留视觉未揭示状态，同时由
    // identity_from_prediction 让路线评分知道它不再属于待探明节点。
    found->identity_state = NodeIdentityState::Hidden;
    found->identity_revealed = false;
    found->visually_hidden = true;
    found->identity_from_topology = false;
    found->identity_from_prediction = true;
    found->prediction_rule = "initial_roaming_resident_settlement";
    found->identity_source = "initial_roaming_resident_prediction";
}

bool reject_resident_settlement_prediction(
    ResidentSettlementPrediction& prediction,
    GridPosition contradicted_position) noexcept
{
    if (!prediction.exact.has_value() || *prediction.exact != contradicted_position) {
        return false;
    }
    // candidates/initial_residents 仍作为这次初始推断的诊断证据保留；exact 是会在每一拍
    // 重新写入地图的结论，权威观测否定后必须立即撤销，不能在后续重建中复活。
    prediction.exact.reset();
    return true;
}
} // namespace asst::blackflow
