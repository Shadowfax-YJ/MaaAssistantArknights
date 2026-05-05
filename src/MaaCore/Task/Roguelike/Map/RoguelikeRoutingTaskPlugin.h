#pragma once

#include <limits>
#include <string>

#include "Common/AsstTypes.h"
#include "MaaUtils/NoWarningCVMat.hpp"
#include "RoguelikeMap.h"
#include "Task/Roguelike/AbstractRoguelikeTaskPlugin.h"

namespace asst
{
class RoguelikeRoutingTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeRoutingTaskPlugin() override = default;
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual bool load_params(const json::value& params) override;
    virtual void reset_in_run_variables() override;

    enum class RoutingStrategy
    {
        None,
        Sarkaz_FastPass,                 // 实验模式，暂未开放给用户
        Sarkaz_FastInvestment,           // 点刺成锭分队快速投资
        JieGarden_FastPassWithBattle,    // 指挥分队一战快速投资/烧水
        JieGarden_FastPassWithoutBattle, // 指挥分队无战快速投资/烧水
        JieGarden_DataCollection,        // 界园数据收集
    };

protected:
    virtual bool _run() override;

private:
    /// <summary>
    /// 识别画面中节点并更新地图信息。
    /// </summary>
    /// <param name="image">截图。</param>
    /// <param name="leftmost_column">
    ///     画面最左侧节点 (忽视 init node) 所在列的 index。
    ///     按照定义，要求 leftmost_column >= 1。
    /// </param>
    /// <param name="image_draw_opt">ASST_DEBUG 模式下用于标注识别结果的截图。若为 std::nullopt
    /// 则不标注识别结果。</param> <returns> 若有新地图信息，则返回 true, 反之则返回 false。
    /// </returns>
    /// <remarks>
    /// 画面中最多同时存在三列节点。
    /// </remarks>
    bool update_map(
        const cv::Mat& image,
        size_t leftmost_column = RoguelikeMap::INIT_INDEX + 1,
        std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt = std::nullopt,
        bool include_same_column_edges = true);

    void generate_map();
    void generate_data_collection_map();
    void generate_edges(
        const size_t& node,
        const cv::Mat& image,
        const int& node_x,
        bool include_same_column_edges = true,
        std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt = std::nullopt);
    void refresh_following_combat_nodes();
    void navigate_route();
    void navigate_data_collection_route();
    void update_selected_x();

    struct DataCollectionRouteScore
    {
        bool valid = false;
        int encounter_count = 0;
        int combat_count = 0;
        int non_combat_count = 0;
        int path_length = 0;
        int vertical_edge_count = 0;
        int first_encounter_step = std::numeric_limits<int>::max();
        int first_type_priority = 0;
        size_t first_node = RoguelikeMap::INIT_INDEX;
        std::vector<size_t> path;
        std::string reject_reason;
    };

    DataCollectionRouteScore score_data_collection_path(
        size_t node,
        std::optional<size_t> previous_node = std::nullopt,
        std::vector<size_t> path = {},
        bool vertical_edge_used_in_path = false) const;
    cv::Mat build_data_collection_graph_image(
        const std::vector<size_t>& selected_route = {},
        std::optional<size_t> selected_node = std::nullopt) const;
    json::object build_data_collection_route_details(
        const std::vector<DataCollectionRouteScore>& candidates,
        std::optional<size_t> chosen,
        std::string_view reject_reason) const;
    static bool is_data_collection_rejected_type(RoguelikeNodeType type);
    static bool is_data_collection_combat_type(RoguelikeNodeType type);
    static int data_collection_type_priority(RoguelikeNodeType type);
    static std::string data_collection_display_name(RoguelikeNodeType type);
    static bool data_collection_score_less(const DataCollectionRouteScore& lhs, const DataCollectionRouteScore& rhs);
    bool is_data_collection_vertical_edge(size_t source, size_t target) const;
    std::string build_data_collection_adjacency_list() const;

    inline static std::function<std::string(RoguelikeNodeType)> type2name = &RoguelikeMapConfig::type2name;

    // ———————— constants and variables ———————————————————————————————————————————————
    RoutingStrategy m_routing_strategy = RoutingStrategy::None;
    RoguelikeMap m_map;
    bool m_need_generate_map = true;
    int m_data_collection_floor = 0;
    size_t m_data_collection_current_node = RoguelikeMap::INIT_INDEX;
    bool m_data_collection_vertical_edge_used = false;
    cv::Mat m_data_collection_full_map_image;
    std::vector<int> m_data_collection_column_xs;
    size_t m_selected_column = 0;  // 当前选中节点所在列
    int m_selected_x = 0;          // 当前选中节点的横坐标 (Rect.x)

    int m_origin_x = 0;            // 第一列节点的默认横坐标 (Rect.x)
    int m_middle_x = 0;            // 中间列节点的默认横坐标 (Rect.x)
    int m_last_x = 0;              // 最后列节点的默认横坐标 (Rect.x)
    int m_node_width = 0;          // 节点 Rect.width
    int m_node_height = 0;         // 节点 Rect.height
    int m_column_offset = 0;       // 两列节点之间的距离
    int m_nameplate_offset = 0;    // 节点 Rect 下边缘到节点铭牌下边缘的距离
    int m_roi_margin = 0;          // roi 的 margin offset
    int m_direction_threshold = 0; // 节点间连线方向判定的阈值

    // view-related
    int m_left_most_column_x_in_view = 0;
};
}
