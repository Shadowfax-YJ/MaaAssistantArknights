#include "RoguelikeBoskyPassageMap.h"

#include <array>

#include "Utils/Logger.hpp"

namespace asst
{
std::string subtype2name(RoguelikeBoskySubNodeType node_subtype)
{
    // 界园树洞子节点类型映射表
    static const std::unordered_map<RoguelikeBoskySubNodeType, std::string> NODE_SUBTYPE_MAPPING = {
        { RoguelikeBoskySubNodeType::Unknown, "Unknown" },
        { RoguelikeBoskySubNodeType::Ling, "Ling" }, // 令 - 常乐 掷地有声
        { RoguelikeBoskySubNodeType::Shu, "Shu" },   // 黍 - 常乐 种因得果
        { RoguelikeBoskySubNodeType::Nian, "Nian" }  // 年 - 常乐 三缺一
    };

    if (auto it = NODE_SUBTYPE_MAPPING.find(node_subtype); it != NODE_SUBTYPE_MAPPING.end()) {
        return it->second;
    }
    return "Unnamed";
}

std::optional<size_t>
    RoguelikeBoskyPassageMap::create_and_insert_node(int x, int y, RoguelikeNodeType type, bool is_open)
{
    if (!in_bounds(x, y)) {
        Log.warn(__FUNCTION__, "| Coordinates (", x, ",", y, ") out of bounds");
        return std::nullopt;
    }

    const auto idx = static_cast<size_t>(y * WIDTH + x);
    Node& n = m_nodes[idx];

    if (!n.exists) {
        n.exists = true;
        n.is_open = is_open;
        n.visited = false;
        n.type = type;
        ++m_existing_count;
        return idx;
    }
    else {
        Log.warn(__FUNCTION__, "| Node already exists at (", x, ",", y, ")");
        return std::nullopt;
    }
}

RoguelikeBoskyPassageMap::Node* RoguelikeBoskyPassageMap::get_valid_node(size_t index)
{
    if (index >= m_nodes.size()) {
        return nullptr;
    }
    Node& n = m_nodes[index];
    return &n;
}

void RoguelikeBoskyPassageMap::set_visited(size_t index)
{
    Node* n = get_valid_node(index);
    if (!n || !n->exists) {
        return;
    }
    n->visited = true;
}

void RoguelikeBoskyPassageMap::set_node_type(size_t index, RoguelikeNodeType type)
{
    Node* n = get_valid_node(index);
    if (!n || !n->exists) {
        return;
    }
    n->type = type;
}

void RoguelikeBoskyPassageMap::set_node_subtype(size_t index, RoguelikeBoskySubNodeType sub_type)
{
    Node* n = get_valid_node(index);
    if (!n || !n->exists) {
        Log.warn(__FUNCTION__, "| Invalid index or node doesn't exist:", index);
        return;
    }
    n->sub_type = sub_type;
    Log.info(__FUNCTION__, "| Set node", index, "subtype to", subtype2name(sub_type));
}

RoguelikeBoskySubNodeType RoguelikeBoskyPassageMap::get_node_subtype(size_t index) const
{
    if (index >= m_nodes.size()) {
        return RoguelikeBoskySubNodeType::Unknown;
    }
    const Node& n = m_nodes[index];
    if (!n.exists) {
        return RoguelikeBoskySubNodeType::Unknown;
    }
    return n.sub_type;
}

void RoguelikeBoskyPassageMap::reset()
{
    for (auto& n : m_nodes) {
        n = Node {}; // reset
    }
    reset_edges();
    m_curr_pos = 0;
    m_existing_count = 0;
    m_abandon_on_exit = false;
}

bool RoguelikeBoskyPassageMap::consume_abandon_on_exit()
{
    const bool result = m_abandon_on_exit;
    m_abandon_on_exit = false;
    return result;
}

std::vector<size_t>
    RoguelikeBoskyPassageMap::get_open_unvisited_nodes(std::optional<RoguelikeNodeType> type_filter) const
{
    std::vector<size_t> res;
    res.reserve(m_existing_count);
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const Node& n = m_nodes[i];
        if (n.exists && n.is_open && !n.visited) {
            if (!type_filter.has_value() || n.type == type_filter.value()) {
                res.push_back(i);
            }
        }
    }
    return res;
}

std::vector<size_t> RoguelikeBoskyPassageMap::get_open_nodes(std::optional<RoguelikeNodeType> type_filter) const
{
    std::vector<size_t> res;
    res.reserve(m_existing_count);
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const Node& n = m_nodes[i];
        if (n.exists && n.is_open) {
            if (!type_filter.has_value() || n.type == type_filter.value()) {
                res.push_back(i);
            }
        }
    }
    return res;
}

bool RoguelikeBoskyPassageMap::is_graph_vertex(size_t index) const
{
    return index < m_nodes.size() && (m_nodes[index].exists || index == center_index());
}

void RoguelikeBoskyPassageMap::reset_edges()
{
    for (auto& edge_set : m_edges) {
        edge_set.reset();
    }
}

void RoguelikeBoskyPassageMap::add_undirected_edge(size_t first, size_t second)
{
    if (first >= m_edges.size() || second >= m_edges.size() || first == second) {
        return;
    }
    if (!is_graph_vertex(first) || !is_graph_vertex(second)) {
        return;
    }
    m_edges[first].set(second);
    m_edges[second].set(first);
}

bool RoguelikeBoskyPassageMap::has_edge(size_t first, size_t second) const
{
    if (first >= m_edges.size() || second >= m_edges.size()) {
        return false;
    }
    return m_edges[first].test(second);
}

std::vector<size_t> RoguelikeBoskyPassageMap::get_neighbors(size_t index) const
{
    std::vector<size_t> res;
    if (index >= m_edges.size()) {
        return res;
    }

    res.reserve(m_edges[index].count());
    for (size_t i = 0; i < m_edges[index].size(); ++i) {
        if (m_edges[index].test(i) && is_graph_vertex(i)) {
            res.push_back(i);
        }
    }
    return res;
}

std::vector<size_t> RoguelikeBoskyPassageMap::get_open_unvisited_neighbors(
    size_t index,
    std::optional<RoguelikeNodeType> type_filter) const
{
    std::vector<size_t> res;
    if (index >= m_edges.size()) {
        return res;
    }

    for (size_t node_index = 0; node_index < m_edges[index].size(); ++node_index) {
        if (!m_edges[index].test(node_index) || node_index == center_index()) {
            continue;
        }
        const Node& n = m_nodes[node_index];
        if (n.exists && n.is_open && !n.visited) {
            if (!type_filter.has_value() || n.type == type_filter.value()) {
                res.push_back(node_index);
            }
        }
    }
    return res;
}

size_t RoguelikeBoskyPassageMap::get_edge_count() const
{
    size_t count = 0;
    for (const auto& edge_set : m_edges) {
        count += edge_set.count();
    }
    return count / 2;
}

RoguelikeNodeType RoguelikeBoskyPassageMap::get_node_type(size_t index) const
{
    if (index >= m_nodes.size()) {
        return RoguelikeNodeType::Unknown;
    }
    return m_nodes[index].type;
}

bool RoguelikeBoskyPassageMap::get_node_exists(size_t index) const
{
    return index < m_nodes.size() && m_nodes[index].exists;
}

bool RoguelikeBoskyPassageMap::get_node_open(size_t index) const
{
    return index < m_nodes.size() && m_nodes[index].exists && m_nodes[index].is_open;
}

bool RoguelikeBoskyPassageMap::get_node_visited(size_t index) const
{
    return index < m_nodes.size() && m_nodes[index].exists && m_nodes[index].visited;
}

int RoguelikeBoskyPassageMap::get_node_x(size_t index) const
{
    return static_cast<int>(index % WIDTH);
}

int RoguelikeBoskyPassageMap::get_node_y(size_t index) const
{
    return static_cast<int>(index / WIDTH);
}

std::pair<int, int> RoguelikeBoskyPassageMap::get_node_pixel(
    size_t index,
    int origin_x,
    int origin_y,
    int column_offset,
    int row_offset) const
{
    if (index >= m_nodes.size() || !m_nodes[index].exists) {
        Log.warn(__FUNCTION__, "| Invalid node index: (", index, ")");
        return { -1, -1 };
    }
    const int x = get_node_x(index);
    const int y = get_node_y(index);
    return { origin_x + x * column_offset, origin_y + y * row_offset };
}

std::optional<size_t> RoguelikeBoskyPassageMap::coord_to_index(int x, int y) const
{
    if (!in_bounds(x, y)) {
        return std::nullopt;
    }
    const auto idx = static_cast<size_t>(y * WIDTH + x);
    if (!m_nodes[idx].exists) {
        return std::nullopt;
    }
    return idx;
}

// 像素坐标到网格坐标的转换辅助函数
std::pair<int, int>
    RoguelikeBoskyPassageMap::pixel_to_grid_coords(int px, int py, const BoskyPassageMapConfig& config) const
{
    const double dx = static_cast<double>(px - config.origin_x) / config.column_offset;
    const double dy = static_cast<double>(py - config.origin_y) / config.row_offset;
    const auto gx = static_cast<int>(dx + (dx >= 0 ? 0.5 : -0.5));
    const auto gy = static_cast<int>(dy + (dy >= 0 ? 0.5 : -0.5));
    return { gx, gy };
}

std::optional<size_t>
    RoguelikeBoskyPassageMap::pixel_to_index(int px, int py, const BoskyPassageMapConfig& config) const
{
    if (config.node_width <= 0 || config.node_height <= 0 || config.column_offset <= 0 || config.row_offset <= 0) {
        return std::nullopt;
    }

    const auto [gx, gy] = pixel_to_grid_coords(px, py, config);
    return coord_to_index(gx, gy);
}

std::optional<size_t> RoguelikeBoskyPassageMap::ensure_node_from_pixel(
    int px,
    int py,
    const BoskyPassageMapConfig& config,
    bool is_open,
    RoguelikeNodeType type)
{
    if (config.node_width <= 0 || config.node_height <= 0 || config.column_offset <= 0 || config.row_offset <= 0) {
        Log.warn(
            __FUNCTION__,
            "| Invalid parameters: node_width=(",
            config.node_width,
            "), node_height=(",
            config.node_height,
            "), column_offset=(",
            config.column_offset,
            "), row_offset=(",
            config.row_offset,
            ")");
        return std::nullopt;
    }

    auto [gx, gy] = pixel_to_grid_coords(px, py, config);
    Log.info(__FUNCTION__, "| analyzing node (", px, ",", py, ") -> (", gx, ",", gy, ")");

    if (!in_bounds(gx, gy)) {
        Log.warn(__FUNCTION__, "| Grid coordinates (", gx, ",", gy, ") out of bounds");
        return std::nullopt;
    }

    if (auto idx = coord_to_index(gx, gy); idx.has_value()) {
        // 节点已存在，只更新状态
        if (Node* n = get_valid_node(idx.value())) {
            n->is_open = is_open;
            n->type = type;
        }
        return idx;
    }

    // 节点不存在，创建新节点
    return create_and_insert_node(gx, gy, type, is_open);
}

void RoguelikeBoskyPassageMap::retain_nodes(const std::vector<size_t>& indices)
{
    std::bitset<NODE_COUNT> retained;
    for (const size_t index : indices) {
        if (index < NODE_COUNT) {
            retained.set(index);
        }
    }

    size_t count = 0;
    for (size_t index = 0; index < m_nodes.size(); ++index) {
        if (!m_nodes[index].exists) {
            continue;
        }
        if (!retained.test(index)) {
            m_nodes[index] = Node {};
            continue;
        }
        ++count;
    }
    m_existing_count = count;
}
} // namespace asst
