#include "Vision/Roguelike/BlackFlow/BlackFlowMapAnalyzer.h"

#include "Vision/Roguelike/BlackFlow/BlackFlowTopologyMatcher.h"
#include "Vision/Roguelike/BlackFlow/NodeOcrRules.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Utils/Logger.hpp"

namespace asst::blackflow::perception
{
namespace
{
nlohmann::json read_json(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open JSON: " + path.string());
    }
    nlohmann::json output;
    input >> output;
    return output;
}

NodeDetectorConfig parse_node_config(const nlohmann::json& json)
{
    NodeDetectorConfig config;
    const auto roi = json.at("map_roi");
    config.map_roi = cv::Rect(roi.at(0).get<int>(), roi.at(1).get<int>(), roi.at(2).get<int>(), roi.at(3).get<int>());
    config.grid.spacing_min = json.value("spacing_min", config.grid.spacing_min);
    config.grid.spacing_max = json.value("spacing_max", config.grid.spacing_max);
    config.grid.spacing_step = json.value("spacing_step", config.grid.spacing_step);
    config.grid.spacing_hint = json.value("spacing_hint", config.grid.spacing_hint);
    config.seed_empty_threshold = json.value("seed_empty_threshold", config.seed_empty_threshold);
    config.cell_empty_threshold = json.value("cell_empty_threshold", config.cell_empty_threshold);
    config.large_type_threshold = json.value("large_type_threshold", config.large_type_threshold);
    config.large_center_tolerance = json.value("large_center_tolerance", config.large_center_tolerance);
    config.cell_roi_size = json.value("cell_roi_size", config.cell_roi_size);
    config.large_roi_offset_x = json.value("large_roi_offset_x", config.large_roi_offset_x);
    config.large_roi_offset_y = json.value("large_roi_offset_y", config.large_roi_offset_y);
    config.large_roi_width = json.value("large_roi_width", config.large_roi_width);
    config.large_roi_height = json.value("large_roi_height", config.large_roi_height);
    config.bright_delta_threshold = json.value("bright_delta_threshold", config.bright_delta_threshold);
    config.current_marker_threshold = json.value("current_marker_threshold", config.current_marker_threshold);
    config.current_marker_grid_tolerance =
        json.value("current_marker_grid_tolerance", config.current_marker_grid_tolerance);
    config.marker_grid_tolerance = json.value("marker_grid_tolerance", config.marker_grid_tolerance);
    config.refinement_mode = GridRefinementMode::FixedGrid;
    config.guard_ring_shift_radius = json.value("guard_ring_shift_radius", config.guard_ring_shift_radius);
    config.guard_ring_outside_weight = json.value("guard_ring_outside_weight", config.guard_ring_outside_weight);
    config.guard_ring_min_score_gain = json.value("guard_ring_min_score_gain", config.guard_ring_min_score_gain);
    config.guard_ring_minimum_anchor_count =
        json.value("guard_ring_minimum_anchor_count", config.guard_ring_minimum_anchor_count);
    config.fixed_grid_translation_limit =
        json.value("fixed_grid_translation_limit", config.fixed_grid_translation_limit);
    config.fixed_grid_translation_tolerance =
        json.value("fixed_grid_translation_tolerance", config.fixed_grid_translation_tolerance);
    config.empty_multi_suppression_radius =
        json.value("empty_multi_suppression_radius", config.empty_multi_suppression_radius);
    config.fixed_grid_hit_tolerance = json.value("fixed_grid_hit_tolerance", config.fixed_grid_hit_tolerance);
    config.ocr_column_width = json.value("ocr_column_width", config.ocr_column_width);
    config.ocr_row_center_offset_y = json.value("ocr_row_center_offset_y", config.ocr_row_center_offset_y);
    config.ocr_row_height = json.value("ocr_row_height", config.ocr_row_height);
    config.ocr_grid_tolerance = json.value("ocr_grid_tolerance", config.ocr_grid_tolerance);
    config.ocr_merge_max_gap = json.value("ocr_merge_max_gap", config.ocr_merge_max_gap);
    config.ocr_merge_max_overlap = json.value("ocr_merge_max_overlap", config.ocr_merge_max_overlap);
    config.ocr_merge_min_vertical_overlap =
        json.value("ocr_merge_min_vertical_overlap", config.ocr_merge_min_vertical_overlap);
    config.ocr_merge_max_center_y_delta =
        json.value("ocr_merge_max_center_y_delta", config.ocr_merge_max_center_y_delta);
    config.ocr_similarity_threshold = json.value("ocr_similarity_threshold", config.ocr_similarity_threshold);
    config.ocr_similarity_margin = json.value("ocr_similarity_margin", config.ocr_similarity_margin);
    config.ocr_short_exact_length = json.value("ocr_short_exact_length", config.ocr_short_exact_length);
    return config;
}

TopologyCoordinate parse_topology_coordinate(const nlohmann::json& value, int columns, int rows)
{
    if (!value.is_array() || value.size() != 2) {
        throw std::runtime_error("topology coordinate must be [column, row]");
    }
    TopologyCoordinate result { value.at(0).get<int>(), value.at(1).get<int>() };
    if (result.column < 0 || result.column >= columns || result.row < 0 || result.row >= rows) {
        throw std::runtime_error("topology coordinate is outside grid_shape");
    }
    return result;
}

std::vector<MapTopologyTemplate>
    parse_topology_library(const nlohmann::json& json, std::string& source_digest)
{
    if (!json.is_object() || json.value("schema_version", 0) != 1 ||
        json.value("coordinate_order", std::string {}) != "column_row" || !json.contains("templates") ||
        !json.at("templates").is_array()) {
        throw std::runtime_error("unsupported BlackFlow topology library schema");
    }
    source_digest = json.value("source_digest", std::string {});
    if (source_digest.empty()) {
        throw std::runtime_error("BlackFlow topology library has no source digest");
    }
    std::vector<MapTopologyTemplate> result;
    std::set<std::string> ids;
    std::map<int, int> floor_counts;
    for (const auto& entry : json.at("templates")) {
        MapTopologyTemplate topology;
        topology.id = entry.at("id").get<std::string>();
        topology.floor = entry.at("floor").get<int>();
        const auto grid_shape = entry.at("grid_shape");
        if (topology.id.empty() || !ids.emplace(topology.id).second || topology.floor < 1 || topology.floor > 5 ||
            !grid_shape.is_array() || grid_shape.size() != 2) {
            throw std::runtime_error("invalid or duplicate BlackFlow topology template");
        }
        topology.columns = grid_shape.at(0).get<int>();
        topology.rows = grid_shape.at(1).get<int>();
        const auto profile = floor_profile(topology.floor);
        if (!profile.has_value() || topology.columns <= 0 || topology.columns > profile->columns ||
            topology.rows != profile->rows) {
            throw std::runtime_error("BlackFlow topology grid_shape exceeds its floor recognition grid");
        }
        topology.start = parse_topology_coordinate(entry.at("start_slot"), topology.columns, topology.rows);
        std::set<std::pair<int, int>> occupied;
        for (const auto& coordinate : entry.at("occupied_slots")) {
            const TopologyCoordinate parsed =
                parse_topology_coordinate(coordinate, topology.columns, topology.rows);
            if (!occupied.emplace(parsed.column, parsed.row).second) {
                throw std::runtime_error("BlackFlow topology contains a duplicate occupied slot");
            }
            topology.occupied.emplace_back(parsed);
        }
        if (!occupied.contains({ topology.start.column, topology.start.row })) {
            throw std::runtime_error("BlackFlow topology start_slot is not occupied");
        }
        std::set<std::pair<int, int>> terminals;
        for (const auto& coordinate : entry.at("terminal_slots")) {
            const TopologyCoordinate parsed =
                parse_topology_coordinate(coordinate, topology.columns, topology.rows);
            if (!occupied.contains({ parsed.column, parsed.row }) ||
                !terminals.emplace(parsed.column, parsed.row).second) {
                throw std::runtime_error("BlackFlow topology contains an invalid or duplicate terminal slot");
            }
            topology.terminals.emplace_back(parsed);
        }
        std::set<std::pair<int, int>> fixed_slots;
        for (const auto& fixed : entry.at("fixed_nodes")) {
            const std::string type = fixed.at("type").get<std::string>();
            if (type != "shop") {
                throw std::runtime_error("BlackFlow topology contains an unsupported fixed node type");
            }
            const TopologyCoordinate position =
                parse_topology_coordinate(fixed.at("slot"), topology.columns, topology.rows);
            if (!occupied.contains({ position.column, position.row }) ||
                !fixed_slots.emplace(position.column, position.row).second) {
                throw std::runtime_error("BlackFlow topology contains an invalid or duplicate fixed node slot");
            }
            topology.fixed_nodes.emplace_back(
                TopologyFixedNode {
                    position,
                    type,
                });
        }
        std::set<std::pair<std::pair<int, int>, std::pair<int, int>>> edges;
        for (const auto& edge : entry.at("edges")) {
            if (!edge.is_array() || edge.size() != 2) {
                throw std::runtime_error("BlackFlow topology edge must contain two coordinates");
            }
            TopologyEdge parsed {
                parse_topology_coordinate(edge.at(0), topology.columns, topology.rows),
                parse_topology_coordinate(edge.at(1), topology.columns, topology.rows),
            };
            if (!occupied.contains({ parsed.first.column, parsed.first.row }) ||
                !occupied.contains({ parsed.second.column, parsed.second.row }) || parsed.first == parsed.second) {
                throw std::runtime_error("BlackFlow topology edge references an invalid slot");
            }
            auto first = std::pair { parsed.first.column, parsed.first.row };
            auto second = std::pair { parsed.second.column, parsed.second.row };
            if (second < first) {
                std::swap(first, second);
                std::swap(parsed.first, parsed.second);
            }
            if (!edges.emplace(first, second).second) {
                throw std::runtime_error("BlackFlow topology contains a duplicate edge");
            }
            topology.edges.emplace_back(parsed);
        }
        ++floor_counts[topology.floor];
        result.emplace_back(std::move(topology));
    }
    const std::map<int, int> expected_counts { { 1, 3 }, { 2, 10 }, { 3, 10 }, { 4, 10 }, { 5, 10 } };
    if (result.size() != 43 || floor_counts != expected_counts) {
        throw std::runtime_error("BlackFlow topology library must contain 3/10/10/10/10 templates");
    }
    return result;
}

bool contains_coordinate(const std::vector<TopologyCoordinate>& values, int column, int row)
{
    return std::ranges::any_of(values, [&](const TopologyCoordinate& value) {
        return value.column == column && value.row == row;
    });
}

const Node* find_node(const NodeDetectionResult& detection, const TopologyCoordinate& position)
{
    const auto found = std::ranges::find_if(detection.nodes, [&](const Node& node) {
        return node.column == position.column && node.row == position.row;
    });
    return found == detection.nodes.end() ? nullptr : &*found;
}

const Node* find_node(const NodeDetectionResult& detection, int id)
{
    const auto found = std::ranges::find_if(detection.nodes, [&](const Node& node) { return node.id == id; });
    return found == detection.nodes.end() ? nullptr : &*found;
}

Node* find_node(NodeDetectionResult& detection, const TopologyCoordinate& position)
{
    const auto found = std::ranges::find_if(detection.nodes, [&](const Node& node) {
        return node.column == position.column && node.row == position.row;
    });
    return found == detection.nodes.end() ? nullptr : &*found;
}

std::pair<int, int> edge_key(int first, int second)
{
    return first < second ? std::pair { first, second } : std::pair { second, first };
}

std::set<std::pair<int, int>> topology_edge_keys(
    const MapTopologyTemplate& topology,
    const NodeDetectionResult& detection)
{
    std::set<std::pair<int, int>> result;
    for (const TopologyEdge& edge : topology.edges) {
        const Node* first = find_node(detection, edge.first);
        const Node* second = find_node(detection, edge.second);
        if (first != nullptr && second != nullptr) {
            result.emplace(edge_key(first->id, second->id));
        }
    }
    return result;
}

int topology_match_score(
    const MapTopologyTemplate& topology,
    const NodeDetectionResult& nodes,
    const EdgeDetectionResult& edges)
{
    int symmetric_difference = 0;
    for (const Node& node : nodes.nodes) {
        if (node.exists != contains_coordinate(topology.occupied, node.column, node.row)) {
            ++symmetric_difference;
        }
    }
    const auto template_edges = topology_edge_keys(topology, nodes);
    int overlap = 0;
    int extras = 0;
    for (const Edge& edge : edges.edges) {
        if (!edge.connected) {
            continue;
        }
        if (template_edges.contains(edge_key(edge.node_a, edge.node_b))) {
            ++overlap;
        }
        else {
            ++extras;
        }
    }
    return score_topology_match(
        TopologyMatchEvidence {
            symmetric_difference,
            overlap,
            extras,
        });
}

bool permits_observed_extra_edge(const Node& node)
{
    return node.type == "hide_invisible" || node.type == "evacuate";
}

void set_template_identity(Node& node, std::string type, std::string display_name)
{
    node.visually_hidden = node.type == "hide_invisible" || node.type == "hide_battle";
    node.identity_from_topology = true;
    if (node.type != type) {
        node.evidence.emplace_back("map_template_fixed_identity");
    }
    node.type = std::move(type);
    node.display_name = std::move(display_name);
    node.identity_source = "map_template_fixed_identity";
    node.kind = NodeKind::Large;
    node.confidence = std::max(node.confidence, 1.0);
}

void apply_topology(MapRecognitionResult& result, const MapTopologyTemplate& topology)
{
    result.topology_template_id = topology.id;
    result.topology_base_edge_count = static_cast<int>(topology.edges.size());

    for (Node& node : result.node_detection.nodes) {
        const bool occupied = contains_coordinate(topology.occupied, node.column, node.row);
        node.detected_by_vision = node.exists;
        node.confirmed_by_topology = occupied;
        node.exists = occupied;
        if (occupied) {
            node.existence_confidence = std::max(node.existence_confidence, 1.0);
            node.existence_source = "map_topology_template";
            node.evidence.emplace_back("map_template_occupied_slot");
        }
    }
    const TopologyTerminalIdentity terminal_identity = terminal_identity_for_floor(topology.floor);
    for (const TopologyCoordinate& terminal : topology.terminals) {
        if (Node* node = find_node(result.node_detection, terminal); node != nullptr) {
            set_template_identity(
                *node,
                std::string(terminal_identity.node_type),
                std::string(terminal_identity.display_name));
        }
    }
    for (const TopologyFixedNode& fixed : topology.fixed_nodes) {
        if (Node* node = find_node(result.node_detection, fixed.position); node != nullptr) {
            if (fixed.type == "shop") {
                set_template_identity(*node, "shop", "诡意行商");
            }
        }
    }
    if (topology.floor <= 2) {
        for (const TopologyEdge& edge : topology.edges) {
            std::optional<TopologyCoordinate> neighbor;
            if (edge.first == topology.start) {
                neighbor = edge.second;
            }
            else if (edge.second == topology.start) {
                neighbor = edge.first;
            }
            if (!neighbor.has_value()) {
                continue;
            }
            if (Node* node = find_node(result.node_detection, *neighbor); node != nullptr) {
                set_template_identity(*node, "battle_normal", "作战");
            }
        }
    }

    for (Node& node : result.node_detection.nodes) {
        if (!topology_occupied_without_identity_is_empty(node.exists, node.type)) {
            continue;
        }
        // This is deliberately weak evidence: topology proves that the slot exists, while the
        // absence of a large title makes empty the useful routing fallback. Downstream merge and
        // reveal code identify this source explicitly, so it cannot overwrite reliable OCR or be
        // counted as a visual reveal.
        node.type = "empty";
        node.display_name.clear();
        node.identity_source = "map_topology_no_ocr_empty";
        node.identity_from_topology = true;
        node.visually_hidden = false;
        node.kind = NodeKind::Small;
        node.confidence = std::max(node.confidence, 1.0);
        node.visual_center = node.center;
        node.visual_half_width = 9.0F;
        node.visual_half_height = 9.0F;
        node.evidence.emplace_back("map_topology_no_ocr_empty");
    }

    const auto base_edges = topology_edge_keys(topology, result.node_detection);
    int extra_edges = 0;
    for (Edge& edge : result.edge_detection.edges) {
        const Node* first = find_node(result.node_detection, edge.node_a);
        const Node* second = find_node(result.node_detection, edge.node_b);
        if (first == nullptr || second == nullptr || !first->exists || !second->exists) {
            edge.connected = false;
            continue;
        }
        if (edge.connected && !base_edges.contains(edge_key(edge.node_a, edge.node_b))) {
            if (!retain_topology_extra_edge(
                    edge.cnn_connected,
                    permits_observed_extra_edge(*first),
                    permits_observed_extra_edge(*second))) {
                edge.connected = false;
                continue;
            }
            ++extra_edges;
            edge.decision_source = "observed_extra_edge";
        }
    }
    for (const TopologyEdge& topology_edge : topology.edges) {
        Node* first = find_node(result.node_detection, topology_edge.first);
        Node* second = find_node(result.node_detection, topology_edge.second);
        if (first == nullptr || second == nullptr) {
            continue;
        }
        const auto key = edge_key(first->id, second->id);
        auto found = std::ranges::find_if(result.edge_detection.edges, [&](const Edge& edge) {
            return edge_key(edge.node_a, edge.node_b) == key;
        });
        if (found == result.edge_detection.edges.end()) {
            Edge edge;
            edge.id = static_cast<int>(result.edge_detection.edges.size());
            edge.node_a = first->id;
            edge.node_b = second->id;
            edge.path = { first->center, second->center };
            result.edge_detection.edges.emplace_back(std::move(edge));
            found = std::prev(result.edge_detection.edges.end());
        }
        found->connected = true;
        found->forced_by_connectivity_constraint = false;
        found->calibrated_probability = 1.0;
        found->confidence = 1.0;
        found->decision_source = "map_template_base_edge";
    }
    result.topology_extra_edge_count = extra_edges;
    result.edge_detection.existing_node_count = static_cast<int>(topology.occupied.size());
    result.edge_detection.connected_components_after_constraint = 1;
    result.edge_detection.graph_connected = true;
}
} // namespace

bool BlackFlowMapAnalyzer::load(
    const std::filesystem::path& template_manifest_path,
    const std::filesystem::path& edge_config_path,
    const std::filesystem::path& runtime_manifest_path,
    const std::filesystem::path& topology_path,
    const std::filesystem::path& ideal_model_path,
    std::string& error)
{
    try {
        if (!std::filesystem::is_regular_file(edge_config_path)) {
            error = "BlackFlow map node config does not exist: " + edge_config_path.string();
            return false;
        }
        if (!m_bridge.load(template_manifest_path, error)) {
            return false;
        }
        m_topologies = parse_topology_library(read_json(topology_path), m_topology_source_digest);
        m_cached_topology.reset();
        m_node_detector = std::make_unique<NodeDetector>(m_bridge, parse_node_config(read_json(edge_config_path)));
        if (!m_edge_detector.load(runtime_manifest_path, error)) {
            m_node_detector.reset();
            return false;
        }
        if (!m_ideal_recognizer.load(ideal_model_path, error)) {
            m_node_detector.reset();
            return false;
        }
        m_loaded = true;
        return true;
    }
    catch (const std::exception& exception) {
        error = "BlackFlow map perception initialization failed: " + std::string(exception.what());
        m_node_detector.reset();
        m_loaded = false;
        return false;
    }
    catch (...) {
        error = "BlackFlow map perception initialization failed: unknown exception";
        m_node_detector.reset();
        m_loaded = false;
        return false;
    }
}

MapRecognitionResult BlackFlowMapAnalyzer::recognize(
    const cv::Mat& image,
    int floor,
    int difficulty,
    std::string_view utopia_ideology,
    std::string_view utopia_policy,
    bool render_overlay) const
{
    MapRecognitionResult result;
    result.floor = floor;
    std::chrono::steady_clock::time_point normalization_start;
    std::chrono::steady_clock::time_point recognition_start;
    bool normalization_started = false;
    bool normalization_finished = false;
    bool recognition_started = false;
    try {
        if (!m_loaded || m_node_detector == nullptr) {
            throw std::runtime_error("BlackFlow map perception is not loaded");
        }
        if (image.empty() || image.type() != CV_8UC3) {
            throw std::runtime_error("BlackFlow map perception requires a non-empty BGR8 image");
        }
        const auto profile_candidates = floor_profile_candidates(floor);
        if (profile_candidates.empty()) {
            throw std::runtime_error("floor must be an integer from 1 to 5");
        }
        result.captured_bgr = image.clone();

        normalization_start = std::chrono::steady_clock::now();
        normalization_started = true;
        FrameNormalizationInfo normalization;
        std::string normalization_error;
        if (!m_normalizer.normalize(image, result.normalized_bgr, normalization, normalization_error)) {
            throw std::runtime_error("Image normalization failed: " + normalization_error);
        }
        if (result.normalized_bgr.data == image.data) {
            result.normalized_bgr = image.clone();
        }
        result.normalization_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - normalization_start)
                                      .count();
        normalization_finished = true;

        recognition_start = std::chrono::steady_clock::now();
        recognition_started = true;
        {
            std::lock_guard lock(m_topology_mutex);
            struct ProfileRecognition
            {
                FloorProfile profile;
                NodeDetectionResult nodes;
                EdgeDetectionResult edges;
            };

            std::vector<FloorProfile> profiles;
            const bool same_floor_template_locked =
                m_cached_topology.has_value() && m_topologies[*m_cached_topology].floor == floor;
            if (same_floor_template_locked) {
                const MapTopologyTemplate& topology = m_topologies[*m_cached_topology];
                profiles.emplace_back(FloorProfile { floor, topology.rows, topology.columns });
            }
            else {
                profiles.assign(profile_candidates.begin(), profile_candidates.end());
            }

            std::vector<ProfileRecognition> recognitions;
            recognitions.reserve(profiles.size());
            const auto recognize_profile = [&](const FloorProfile& profile,
                                               std::optional<cv::Point2d> forced_translation = std::nullopt) {
                ProfileRecognition recognition;
                recognition.profile = profile;
                recognition.nodes = m_node_detector->detect(
                    result.normalized_bgr,
                    profile.rows,
                    profile.columns,
                    forced_translation);
                recognition.edges = m_edge_detector.detect(
                    std::vector<cv::Mat> { result.normalized_bgr },
                    recognition.nodes.nodes,
                    profile.rows,
                    profile.columns);
                // CorridorNet connectivity is only observation evidence. A complete immutable topology template can
                // bridge a missed visual edge after the occupied slots identify that template unambiguously.
                if (!recognition.edges.error.empty() &&
                    !topology_can_recover_disconnected_edge_graph(
                        recognition.edges.model_ready,
                        recognition.edges.existing_node_count,
                        recognition.edges.connected_components_after_constraint)) {
                    throw std::runtime_error(recognition.edges.error);
                }
                return recognition;
            };
            for (const FloorProfile& profile : profiles) {
                recognitions.emplace_back(recognize_profile(profile));
            }

            // Auto-pan can move a five-row map by more than half a row after a movement. Empty-node anchors are
            // periodic, so the nearest fixed-grid alignment may then alias every observed row to the adjacent row.
            // Once a floor topology is locked, retry both adjacent row aliases and let that immutable topology select
            // the geometrically consistent grid.
            if (same_floor_template_locked) {
                const MapTopologyTemplate& topology = m_topologies[*m_cached_topology];
                const ProfileRecognition& primary = recognitions.front();
                if (topology_match_score(topology, primary.nodes, primary.edges) == InvalidTopologyMatchScore) {
                    const FixedGridAliasRetryPlan retry_plan = make_fixed_grid_alias_retry_plan(
                        primary.profile.floor,
                        primary.profile.rows,
                        primary.profile.columns,
                        primary.nodes.grid.origin_x - primary.nodes.seed_grid.origin_x,
                        primary.nodes.grid.origin_y - primary.nodes.seed_grid.origin_y,
                        primary.nodes.seed_grid.spacing_y);
                    // retry_plan 在追加 recognitions 之前复制了全部参数。第一次 emplace 可能扩容，
                    // 不能再读取上面的 primary 引用，否则第二次重试会拿到损坏的行列数。
                    for (std::size_t index = 1; index < retry_plan.translation_y.size(); ++index) {
                        recognitions.emplace_back(recognize_profile(
                            FloorProfile { retry_plan.floor, retry_plan.rows, retry_plan.columns },
                            cv::Point2d(retry_plan.translation_x, retry_plan.translation_y[index])));
                    }
                }
            }
            else {
                bool any_initial_match = false;
                for (const ProfileRecognition& recognition : recognitions) {
                    for (const MapTopologyTemplate& topology : m_topologies) {
                        if (topology.floor == floor && topology.rows == recognition.profile.rows &&
                            topology.columns == recognition.profile.columns &&
                            topology_match_score(topology, recognition.nodes, recognition.edges) !=
                                InvalidTopologyMatchScore) {
                            any_initial_match = true;
                            break;
                        }
                    }
                    if (any_initial_match) {
                        break;
                    }
                }
                if (!any_initial_match) {
                    const std::size_t primary_count = recognitions.size();
                    for (std::size_t primary_index = 0; primary_index < primary_count; ++primary_index) {
                        const FloorProfile profile = recognitions[primary_index].profile;
                        const double primary_x = recognitions[primary_index].nodes.grid.origin_x -
                                                 recognitions[primary_index].nodes.seed_grid.origin_x;
                        const double primary_y = recognitions[primary_index].nodes.grid.origin_y -
                                                 recognitions[primary_index].nodes.seed_grid.origin_y;
                        const double spacing_y = recognitions[primary_index].nodes.seed_grid.spacing_y;
                        const auto aliases = fixed_grid_row_alias_offsets(primary_y, spacing_y);
                        for (std::size_t index = 1; index < aliases.size(); ++index) {
                            recognitions.emplace_back(recognize_profile(
                                profile,
                                cv::Point2d(primary_x, aliases[index])));
                        }
                    }
                }
            }

            std::optional<std::size_t> matched_topology;
            std::optional<std::size_t> matched_recognition;
            std::vector<std::pair<std::size_t, std::size_t>> tied_matches;
            int matched_score = std::numeric_limits<int>::min();
            // A floor's topology is immutable. Once selected, later observations may reveal node types but must not
            // silently switch the floor to another template.
            if (same_floor_template_locked) {
                matched_topology = m_cached_topology;
                for (std::size_t recognition_index = 0; recognition_index < recognitions.size(); ++recognition_index) {
                    const int score = topology_match_score(
                        m_topologies[*matched_topology],
                        recognitions[recognition_index].nodes,
                        recognitions[recognition_index].edges);
                    if (score > matched_score) {
                        matched_score = score;
                        matched_recognition = recognition_index;
                    }
                }
                if (matched_score == std::numeric_limits<int>::min()) {
                    throw std::runtime_error(
                        "BlackFlow cached topology template no longer matches the current floor observation");
                }
            }
            else {
                for (std::size_t recognition_index = 0; recognition_index < recognitions.size(); ++recognition_index) {
                    const ProfileRecognition& recognition = recognitions[recognition_index];
                    for (std::size_t topology_index = 0; topology_index < m_topologies.size(); ++topology_index) {
                        const MapTopologyTemplate& topology = m_topologies[topology_index];
                        if (topology.floor != floor || topology.rows != recognition.profile.rows ||
                            topology.columns != recognition.profile.columns) {
                            continue;
                        }
                        const int score = topology_match_score(topology, recognition.nodes, recognition.edges);
                        if (score > matched_score) {
                            matched_score = score;
                            tied_matches = { { recognition_index, topology_index } };
                        }
                        else if (score == matched_score && score != std::numeric_limits<int>::min()) {
                            tied_matches.emplace_back(recognition_index, topology_index);
                        }
                    }
                }
                if (tied_matches.size() == 1) {
                    matched_recognition = tied_matches.front().first;
                    matched_topology = tied_matches.front().second;
                }
            }
            if (matched_topology.has_value() && matched_recognition.has_value() &&
                matched_score != std::numeric_limits<int>::min()) {
                ProfileRecognition& selected = recognitions[*matched_recognition];
                result.rows = selected.profile.rows;
                result.columns = selected.profile.columns;
                result.node_detection = std::move(selected.nodes);
                result.edge_detection = std::move(selected.edges);
                m_cached_topology = matched_topology;
                result.topology_match_score = matched_score;
                result.topology_source_digest = m_topology_source_digest;
                apply_topology(result, m_topologies[*matched_topology]);
                result.edge_detection.error.clear();
            }
            else if (tied_matches.empty()) {
                throw std::runtime_error("BlackFlow map did not match any topology template for this floor");
            }
            else {
                throw std::runtime_error(
                    "BlackFlow map topology match is ambiguous across " + std::to_string(tied_matches.size()) +
                    " templates");
            }

            IdealDomainRecognition ideal = m_ideal_recognizer.recognize(
                result.normalized_bgr,
                result.node_detection.nodes,
                difficulty,
                utopia_ideology,
                utopia_policy);
            m_same_map_ideal_domain.observe(ideal.status, ideal.source, ideal.domain);
            if (ideal.status == "recognized" && m_same_map_ideal_domain.source().has_value()) {
                // 识别器先确定中心、后续代码才会应用“非希望的沃土中心必为紧急”规则。
                // 因而必须在改写节点身份之前冻结同图几何，避免相邻空地被误写成紧急。
                ideal.source = m_same_map_ideal_domain.source();
                ideal.domain = m_same_map_ideal_domain.domain();
            }
            result.utopia_status = ideal.status;
            result.utopia_reason = ideal.reason;
            result.utopia_ideology = ideal.ideology;
            result.utopia_policy = ideal.policy;
            result.ideal_source = ideal.source;
            result.ideal_domain = ideal.domain;
            result.observed_ideal_domain = ideal.observed_domain;
            result.ideal_source_score_margin = ideal.score_margin;
            result.ideal_source_heads_agree = ideal.heads_agree;

            if (ideal.status == "recognized") {
                for (const IdealCoordinate& position : ideal.domain) {
                    if (Node* node = find_node(result.node_detection, TopologyCoordinate { position.column, position.row });
                        node != nullptr && ideal.ideology == "diffused-mist") {
                        // 标记弥散虚雾理想域内的节点：这些节点不能被连线自然揭示，
                        // 但直接进入和羽瞰点主动照亮仍会揭示；理念的适用范围不限制楼层。
                        node->natural_reveal_suppressed = true;
                    }
                }
                if (ideal.source.has_value() && ideal.ideology != "hopeful-soil") {
                    Node* source = find_node(
                        result.node_detection,
                        TopologyCoordinate { ideal.source->column, ideal.source->row });
                    const bool identity_is_unknown =
                        source != nullptr &&
                        (source->type == "null" || source->type == "unclassified" || source->type == "empty" ||
                         source->type == "hide_battle" || source->type == "hide_invisible" ||
                         weak_normal_battle_ocr_defers_to_ideal_source_prediction(
                             source->type,
                             source->identity_source,
                             source->ocr_exact_match));
                    if (source != nullptr && !source->current_marker && identity_is_unknown) {
                        source->type = "battle_elite";
                        source->display_name = "\u7d27\u6025\u4f5c\u6218";
                        source->identity_source = "ideal_source_emergency_prediction";
                        source->identity_from_topology = false;
                        source->identity_from_prediction = true;
                        // 这里只确定“紧急作战”这一语义类别。未被视野点亮前，游戏仍显示
                        // “未知的凶戾”，后续不能据此尝试读取具体关卡名。
                        source->visually_hidden = true;
                        source->prediction_rule = "non_hopeful_ideal_source_is_emergency_battle";
                        source->confidence = 1.0;
                    }
                    else if (source == nullptr || (!source->current_marker && source->type != "battle_elite")) {
                        // 规则只覆盖非“希望的沃土”：理想源中心应当是紧急作战。若同一帧已经
                        // 看到了别的明确身份，不覆盖实测结果，但必须留下冲突警告。
                        Log.warn(
                            "BlackFlow deterministic ideal-source rule conflicts with observation",
                            "row",
                            ideal.source->row,
                            "column",
                            ideal.source->column,
                            "ideology",
                            ideal.ideology,
                            "observed type",
                            source == nullptr ? "absent" : source->type);
                    }
                }
            }
        }
        result.ok = true;
        if (render_overlay) {
            result.overlay_bgr = draw_overlay(result);
        }
    }
    catch (const std::exception& exception) {
        result.error = exception.what();
    }
    catch (...) {
        result.error = "BlackFlow map recognition failed: unknown exception";
    }
    if (normalization_started && !normalization_finished) {
        result.normalization_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - normalization_start)
                                      .count();
    }
    if (recognition_started) {
        result.recognition_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - recognition_start)
                .count();
    }
    return result;
}

void BlackFlowMapAnalyzer::reset_topology_cache() const
{
    std::lock_guard lock(m_topology_mutex);
    m_cached_topology.reset();
    m_same_map_ideal_domain.reset();
}

cv::Mat BlackFlowMapAnalyzer::draw_overlay(const MapRecognitionResult& result) const
{
    if (result.normalized_bgr.empty() || m_node_detector == nullptr) {
        return result.captured_bgr.clone();
    }
    cv::Mat overlay = m_node_detector->draw_overlay(result.normalized_bgr, result.node_detection);
    return m_edge_detector.draw_overlay(overlay, result.node_detection.nodes, result.edge_detection);
}

bool BlackFlowMapAnalyzer::loaded() const noexcept
{
    return m_loaded;
}
} // namespace asst::blackflow::perception
