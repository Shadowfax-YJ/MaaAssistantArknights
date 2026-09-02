#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

#include "Vision/Roguelike/BlackFlow/BlackFlowFloor.h"
#include "Vision/Roguelike/BlackFlow/EdgeDetector.h"
#include "Vision/Roguelike/BlackFlow/FrameNormalizer.h"
#include "Vision/Roguelike/BlackFlow/IdealDomainRecognizer.h"
#include "Vision/Roguelike/BlackFlow/IdealDomainStability.h"
#include "Vision/Roguelike/BlackFlow/NodeDetector.h"
#include "Vision/Roguelike/BlackFlow/RecognitionBridge.h"

namespace asst::blackflow::perception
{
struct TopologyCoordinate
{
    int column = -1;
    int row = -1;

    bool operator==(const TopologyCoordinate&) const noexcept = default;
};

struct TopologyEdge
{
    TopologyCoordinate first;
    TopologyCoordinate second;
};

struct TopologyFixedNode
{
    TopologyCoordinate position;
    std::string type;
};

struct MapTopologyTemplate
{
    std::string id;
    int floor = 0;
    int columns = 0;
    int rows = 0;
    TopologyCoordinate start;
    std::vector<TopologyCoordinate> occupied;
    std::vector<TopologyCoordinate> terminals;
    std::vector<TopologyFixedNode> fixed_nodes;
    std::vector<TopologyEdge> edges;
};

struct MapRecognitionResult
{
    bool ok = false;
    std::string error;
    int floor = 0;
    int rows = 0;
    int columns = 0;
    cv::Mat captured_bgr;
    cv::Mat normalized_bgr;
    cv::Mat overlay_bgr;
    NodeDetectionResult node_detection;
    EdgeDetectionResult edge_detection;
    std::string topology_template_id;
    std::string topology_source_digest;
    int topology_base_edge_count = 0;
    int topology_extra_edge_count = 0;
    int topology_match_score = 0;
    std::string utopia_status;
    std::string utopia_reason;
    std::string utopia_ideology;
    std::string utopia_policy;
    std::optional<IdealCoordinate> ideal_source;
    std::vector<IdealCoordinate> ideal_domain;
    std::vector<IdealCoordinate> observed_ideal_domain;
    double ideal_source_score_margin = 0.0;
    bool ideal_source_heads_agree = false;
    std::int64_t normalization_us = 0;
    std::int64_t recognition_us = 0;
};

class BlackFlowMapAnalyzer
{
public:
    bool load(
        const std::filesystem::path& template_manifest_path,
        const std::filesystem::path& edge_config_path,
        const std::filesystem::path& runtime_manifest_path,
        const std::filesystem::path& topology_path,
        const std::filesystem::path& ideal_model_path,
        std::string& error);
    [[nodiscard]] MapRecognitionResult recognize(
        const cv::Mat& image,
        int floor,
        int difficulty,
        std::string_view utopia_ideology,
        std::string_view utopia_policy,
        bool render_overlay) const;
    void reset_topology_cache() const;
    [[nodiscard]] cv::Mat draw_overlay(const MapRecognitionResult& result) const;
    [[nodiscard]] bool loaded() const noexcept;

private:
    RecognitionBridge m_bridge;
    std::unique_ptr<NodeDetector> m_node_detector;
    EdgeDetector m_edge_detector;
    FrameNormalizer m_normalizer;
    IdealDomainRecognizer m_ideal_recognizer;
    std::vector<MapTopologyTemplate> m_topologies;
    std::string m_topology_source_digest;
    mutable std::mutex m_topology_mutex;
    mutable std::optional<std::size_t> m_cached_topology;
    // 与 m_cached_topology 同寿命：同一地图的理想源不会移动，识别抖动不能把
    // “中心必为紧急作战”的确定性身份写到相邻节点上。
    mutable SameMapIdealDomainState<IdealCoordinate> m_same_map_ideal_domain;
    bool m_loaded = false;
};

} // namespace asst::blackflow::perception
