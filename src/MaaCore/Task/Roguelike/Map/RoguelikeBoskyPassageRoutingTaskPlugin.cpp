#include "RoguelikeBoskyPassageRoutingTaskPlugin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <string_view>

#include "Common/AsstTypes.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/MultiMatcher.h"

namespace
{
struct BoskyEdgeScore
{
    int length = 0;
    int hits = 0;
    int longest_run = 0;
    double coverage = 0.0;
    double longest_ratio = 0.0;
    double density = 0.0;
    double head_coverage = 0.0;
    double tail_coverage = 0.0;
};

cv::Mat build_bosky_edge_mask(const cv::Mat& image)
{
    cv::Mat hsv;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);

    cv::Mat pink_mask;
    cv::inRange(hsv, cv::Scalar(135, 55, 200), cv::Scalar(179, 255, 255), pink_mask);

    cv::Mat red_mask;
    cv::inRange(hsv, cv::Scalar(0, 70, 200), cv::Scalar(8, 255, 255), red_mask);
    cv::bitwise_or(pink_mask, red_mask, pink_mask);

    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(pink_mask, pink_mask, cv::MORPH_CLOSE, kernel);
    cv::dilate(pink_mask, pink_mask, kernel);
    return pink_mask;
}

cv::Rect clipped_rect(const cv::Mat& image, int x, int y, int width, int height)
{
    const cv::Rect bounds(0, 0, image.cols, image.rows);
    return cv::Rect(x, y, width, height) & bounds;
}

BoskyEdgeScore score_bosky_edge_roi(const cv::Mat& mask, const cv::Rect& roi, bool horizontal)
{
    BoskyEdgeScore score;
    if (roi.empty()) {
        return score;
    }

    const cv::Mat edge_roi = mask(roi);
    score.length = horizontal ? edge_roi.cols : edge_roi.rows;
    if (score.length <= 0) {
        return score;
    }

    int run = 0;
    int head_hits = 0;
    int tail_hits = 0;
    const int segment_length = std::max(1, score.length / 3);
    for (int i = 0; i < score.length; ++i) {
        const cv::Mat slice = horizontal ? edge_roi.col(i) : edge_roi.row(i);
        if (cv::countNonZero(slice) > 0) {
            ++score.hits;
            ++run;
            score.longest_run = std::max(score.longest_run, run);
            if (i < segment_length) {
                ++head_hits;
            }
            if (i >= score.length - segment_length) {
                ++tail_hits;
            }
        }
        else {
            run = 0;
        }
    }

    score.coverage = static_cast<double>(score.hits) / score.length;
    score.longest_ratio = static_cast<double>(score.longest_run) / score.length;
    score.density = static_cast<double>(cv::countNonZero(edge_roi)) / edge_roi.total();
    score.head_coverage = static_cast<double>(head_hits) / segment_length;
    score.tail_coverage = static_cast<double>(tail_hits) / segment_length;
    return score;
}

std::pair<int, int> bosky_node_pixel(
    size_t index,
    const asst::RoguelikeBoskyPassageMap::BoskyPassageMapConfig& config)
{
    const int x = static_cast<int>(index % asst::RoguelikeBoskyPassageMap::width());
    const int y = static_cast<int>(index / asst::RoguelikeBoskyPassageMap::width());
    return { config.origin_x + x * config.column_offset, config.origin_y + y * config.row_offset };
}

bool is_bosky_passage_detail_only_node(asst::RoguelikeNodeType type)
{
    return type == asst::RoguelikeNodeType::Disaster || type == asst::RoguelikeNodeType::Omissions;
}

asst::Point bosky_passage_detail_close_point()
{
    return { 13, 626 };
}

size_t choose_random_bosky_node(const std::vector<size_t>& nodes)
{
    static std::mt19937 engine(std::random_device {}());
    std::uniform_int_distribution<size_t> dist(0, nodes.size() - 1);
    return nodes[dist(engine)];
}

std::string bosky_passage_node_display_name(std::string_view node_type)
{
    if (node_type == "Legend") {
        return "传说";
    }
    if (node_type == "Disaster") {
        return "祸乱";
    }
    if (node_type == "Scheme") {
        return "筹谋";
    }
    if (node_type == "Omissions") {
        return "拾遗";
    }
    if (node_type == "Boons") {
        return "抉择";
    }
    if (node_type == "Doubts") {
        return "杂疑";
    }
    if (node_type == "Playtime") {
        return "常乐";
    }
    if (node_type == "YiTrader") {
        return "易与";
    }
    if (node_type == "OldShop") {
        return "故肆";
    }
    return node_type.empty() ? "Unknown" : std::string(node_type);
}

int estimate_bosky_axis_origin(
    const asst::MultiMatcher::ResultsVec& match_results,
    bool horizontal,
    int default_origin,
    int offset,
    int max_grid)
{
    if (match_results.empty() || offset <= 0 || max_grid <= 0) {
        return default_origin;
    }

    std::vector<int> positions;
    positions.reserve(match_results.size());
    for (const auto& [rect, score, templ_name] : match_results) {
        positions.emplace_back(horizontal ? rect.x : rect.y);
    }

    int best_origin = default_origin;
    double best_score = std::numeric_limits<double>::infinity();
    const int search_begin = default_origin - offset;
    const int search_end = default_origin + offset;
    for (int candidate = search_begin; candidate <= search_end; ++candidate) {
        std::vector<int> errors;
        errors.reserve(positions.size());
        int total_error = 0;
        for (const int pos : positions) {
            const int grid = std::clamp(
                static_cast<int>(std::lround(static_cast<double>(pos - candidate) / offset)),
                0,
                max_grid);
            const int expected = candidate + grid * offset;
            const int error = std::abs(pos - expected);
            total_error += error;
            errors.emplace_back(error);
        }

        std::sort(errors.begin(), errors.end());
        const double median_error =
            static_cast<double>(errors.at(errors.size() / 2));
        const double average_error = static_cast<double>(total_error) / errors.size();
        const double score = median_error * 2.0 + average_error;
        if (score < best_score ||
            (std::abs(score - best_score) < 1e-6 && std::abs(candidate - default_origin) < std::abs(best_origin - default_origin))) {
            best_score = score;
            best_origin = candidate;
        }
    }

    return best_origin;
}

asst::RoguelikeBoskyPassageMap::BoskyPassageMapConfig estimate_bosky_view_config(
    const asst::MultiMatcher::ResultsVec& match_results,
    const asst::RoguelikeBoskyPassageMap::BoskyPassageMapConfig& base_config)
{
    auto view_config = base_config;
    view_config.origin_x = estimate_bosky_axis_origin(
        match_results,
        true,
        base_config.origin_x,
        base_config.column_offset,
        asst::RoguelikeBoskyPassageMap::width() - 1);
    view_config.origin_y = estimate_bosky_axis_origin(
        match_results,
        false,
        base_config.origin_y,
        base_config.row_offset,
        asst::RoguelikeBoskyPassageMap::height() - 1);
    return view_config;
}
}

bool asst::RoguelikeBoskyPassageRoutingTaskPlugin::load_params([[maybe_unused]] const json::value& params)
{
    if (m_config->get_theme() == RoguelikeTheme::JieGarden) {
        // ———————— 加载 BoskyPassage 配置 ————————
        const TaskPtr bosky_config = Task.get("JieGarden@RoguelikeRoutingConfig_BoskyPassage");
        m_bosky_config = bosky_config->special_params;
        m_bosky_view_config = m_bosky_config;

        // ———————— 选择导航策略 ————————
        if (m_config->get_mode() == RoguelikeMode::FindPlaytime) {
            m_bosky_routing_strategy = RoutingStrategy::FindPlaytime_JieGarden;
            int target = m_config->get_find_playTime_target();
            RoguelikeBoskyPassageMap::get_instance().set_target_subtype(static_cast<RoguelikeBoskySubNodeType>(target));
            Log.info(__FUNCTION__, "| FindPlaytime mode enabled with target:", target);
            return true;
        }

        m_bosky_routing_strategy = RoutingStrategy::BoskyPassage_JieGarden;
        return true;
    }

    return false;
}

void asst::RoguelikeBoskyPassageRoutingTaskPlugin::reset_in_run_variables()
{
    RoguelikeBoskyPassageMap::get_instance().reset();
    m_bosky_view_config = m_bosky_config;
    m_saved_entry_map = false;
    m_entry_map_image.clear();
    m_entry_map_overlay.clear();
}

bool asst::RoguelikeBoskyPassageRoutingTaskPlugin::verify(const AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    std::string task_name = details.get("details", "task", "");
    // Log.debug(__FUNCTION__, "| Checking task:", task_name); // 不太建议加这个，会在日志中大量出现

    // trigger 任务的名字可以为 "...@Roguelike@Routing_BoskyPassage-..." 的形式
    if (const size_t pos = task_name.find('-'); pos != std::string::npos) {
        task_name = task_name.substr(0, pos);
    }

    if (task_name == m_config->get_theme() + "@Roguelike@Routing_BoskyPassage") {
        return true;
    }

    return false;
}

bool asst::RoguelikeBoskyPassageRoutingTaskPlugin::_run()
{
    LogTraceFunction;

    Log.info(__FUNCTION__, "| Running with bosky_routing_strategy:", static_cast<int>(m_bosky_routing_strategy));
    RoguelikeDataCollector.note_strategy_change("_boskyPassageDefault");

    switch (m_bosky_routing_strategy) {
    case RoutingStrategy::BoskyPassage_JieGarden: {
        if (bosky_update_map()) {
            const std::vector<RoguelikeNodeType> priority_order = get_bosky_passage_priority("Default");
            bosky_decide_and_click(priority_order);
        }
        else {
            Task.set_task_base("RoguelikeRoutingAction", m_config->get_theme() + "@RoguelikeRoutingAction-ContinueBoskyPassage");
        }
        break;
    }
    case RoutingStrategy::FindPlaytime_JieGarden: {
        // 更新地图
        if (!bosky_update_map()) {
            Task.set_task_base("RoguelikeRoutingAction", m_config->get_theme() + "@RoguelikeRoutingAction-ContinueBoskyPassage");
            break;
        }
        const std::vector<RoguelikeNodeType> priority_order = get_bosky_passage_priority("FindPlaytime");

        // 获取目标常乐节点子类型
        Log.info(
            __FUNCTION__,
            "| Looking for playtime subtype:",
            subtype2name(RoguelikeBoskyPassageMap::get_instance().get_target_subtype()));

        // 尝试找到目标节点，使用常乐节点优先的策略
        bosky_decide_and_click(priority_order);
        break;
    }
    default:
        break;
    }

    return true;
}

// ==================== JieGarden BoskyPassage 平面地图逻辑 ====================
bool asst::RoguelikeBoskyPassageRoutingTaskPlugin::close_bosky_passage_detail_if_open()
{
    LogTraceFunction;

    const cv::Mat image = ctrler()->get_image();
    if (image.empty()) {
        Log.warn(__FUNCTION__, "| Failed to get image from controller");
        return false;
    }

    const std::array<std::string_view, 3> detail_enter_tasks {
        "JieGarden@Roguelike@StageEmergencyOpsEnter",
        "JieGarden@Roguelike@StageCombatOpsEnter",
        "JieGarden@Roguelike@StageEncounterEnter",
    };

    for (const auto task_name : detail_enter_tasks) {
        Matcher matcher(image);
        matcher.set_task_info(std::string(task_name));
        if (!matcher.analyze()) {
            continue;
        }

        Log.info(__FUNCTION__, "| Closing bosky passage detail panel matched by", task_name);
        const Point close_point = bosky_passage_detail_close_point();
        for (int i = 0; i < 3; ++i) {
            ctrler()->click(close_point);
            sleep(200);
        }
        sleep(500);
        return true;
    }

    return false;
}

bool asst::RoguelikeBoskyPassageRoutingTaskPlugin::bosky_update_map()
{
    LogTraceFunction;

    Log.info(__FUNCTION__, "| updating bosky map");

    // 有时候从不期而遇出来可能会多点一下，点到剩余烛火，导致接下来的一次点击只会把窗口关掉，无法进入节点。而且还遮挡了部分节点
    // 能检测到意识回归的话就点一下边缘，把退出树洞的弹窗关掉
    ProcessTask(*this, { "JieGarden@Roguelike@LeaveBoskyPassageCheck" }).set_retry_times(0).run();
    close_bosky_passage_detail_if_open();

    cv::Mat image;
    MultiMatcher::ResultsVec match_results;
    constexpr int MaxStableMapRetries = 10;
    constexpr int StableMapRetryDelayMs = 500;
    const int max_origin_drift = std::max(1, m_bosky_config.column_offset / 2);
    bool stable_map = false;
    for (int attempt = 0; attempt < MaxStableMapRetries; ++attempt) {
        image = ctrler()->get_image();
        if (image.empty()) {
            Log.error(__FUNCTION__, "| Failed to get image from controller");
            return false;
        }

        MultiMatcher node_analyzer(image);
        node_analyzer.set_task_info("JieGarden@RoguelikeRoutingNodeAnalyze_BoskyPassage");
        if (!node_analyzer.analyze()) {
            Log.warn(__FUNCTION__, "| no nodes are recognised, retry:", attempt + 1, "/", MaxStableMapRetries);
            sleep(StableMapRetryDelayMs);
            continue;
        }

        match_results = node_analyzer.get_result();
        Log.info(__FUNCTION__, "| found", match_results.size(), "nodes");
        m_bosky_view_config = estimate_bosky_view_config(match_results, m_bosky_config);
        Log.info(
            __FUNCTION__,
            "| bosky view origin estimated: (",
            m_bosky_view_config.origin_x,
            ",",
            m_bosky_view_config.origin_y,
            "), base: (",
            m_bosky_config.origin_x,
            ",",
            m_bosky_config.origin_y,
            ")");

        const int origin_drift = std::abs(m_bosky_view_config.origin_x - m_bosky_config.origin_x);
        if (origin_drift <= max_origin_drift) {
            stable_map = true;
            break;
        }

        Log.warn(
            __FUNCTION__,
            "| bosky map origin is unstable, drift:",
            origin_drift,
            "max:",
            max_origin_drift,
            "retry:",
            attempt + 1,
            "/",
            MaxStableMapRetries);
        sleep(StableMapRetryDelayMs);
    }

    if (!stable_map) {
        Log.error(__FUNCTION__, "| failed to get stable bosky map, skip this routing tick");
        return false;
    }

    // 排序 靠左上优先
    sort_by_vertical_(match_results);

    const std::string& theme = m_config->get_theme();
    const bool should_save_entry_map = !m_saved_entry_map;

#ifdef ASST_DEBUG
    constexpr bool should_draw_overlay = true;
#else
    const bool should_draw_overlay = should_save_entry_map;
#endif
    std::optional<cv::Mat> image_draw;
    if (should_draw_overlay) {
        image_draw.emplace(image.clone());
    }

    const auto draw_node_result = [&](cv::Mat& draw_target, const Rect& rect, RoguelikeNodeType type, size_t idx) {
        cv::rectangle(draw_target, make_rect<cv::Rect>(rect), cv::Scalar(0, 0, 255), 2);
        cv::putText(
            draw_target,
            std::to_string(static_cast<int>(type)) + " (" +
                std::to_string(RoguelikeBoskyPassageMap::get_instance().get_node_x(idx)) + ", " +
                std::to_string(RoguelikeBoskyPassageMap::get_instance().get_node_y(idx)) + ")",
            cv::Point(rect.x, rect.y - 5),
            cv::FONT_HERSHEY_SIMPLEX,
            0.5,
            cv::Scalar(0, 0, 255),
            1);
    };

    // 处理每个识别到的节点
    std::vector<size_t> recognized_nodes;
    recognized_nodes.reserve(match_results.size());
    for (const auto& [rect, score, templ_name] : match_results) {
        Log.debug(__FUNCTION__, "| analyzing node", templ_name, "at (", rect.x, ",", rect.y, ")");

        const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(theme, templ_name);
        if (type == RoguelikeNodeType::Unknown) {
            Log.warn(__FUNCTION__, "| unknown template:", templ_name);
            continue;
        }

        // 检查是否为灰色节点
        const bool is_open = templ_name.find("Grey") == std::string::npos;

        auto idx = RoguelikeBoskyPassageMap::get_instance()
                       .ensure_node_from_pixel(rect.x, rect.y, m_bosky_view_config, is_open, type);

        if (idx.has_value()) {
            // 更新节点类型（防止类型不一致）
            RoguelikeBoskyPassageMap::get_instance().set_node_type(idx.value(), type);
            Log.debug(__FUNCTION__, "| updated node (", idx.value(), ") type: (", type2name(type), ")");
            recognized_nodes.emplace_back(idx.value());
        }
        else {
            Log.warn(__FUNCTION__, "| failed to create/update node from pixel (", rect.x, ",", rect.y, ")");
        }

        if (idx.has_value() && image_draw.has_value()) {
            draw_node_result(image_draw.value(), rect, type, idx.value());
        }
    }

    RoguelikeBoskyPassageMap::get_instance().retain_nodes(recognized_nodes);

    if (image_draw.has_value()) {
        bosky_update_edges(image, m_bosky_view_config, image_draw.value());
    }
    else {
        bosky_update_edges(image, m_bosky_view_config);
    }

    if (should_save_entry_map && image_draw.has_value()) {
        m_entry_map_image = RoguelikeDataCollector.save_bosky_passage_image(image, "map_raw");
        m_entry_map_overlay = RoguelikeDataCollector.save_bosky_passage_image(image_draw.value(), "map_overlay");
        m_saved_entry_map = true;
    }
    record_bosky_map_snapshot(m_entry_map_image, m_entry_map_overlay);

#ifdef ASST_DEBUG
    if (image_draw.has_value()) {
        utils::save_debug_image(
            image_draw.value(),
            utils::path("debug") / "roguelikeMap",
            /*auto_clean=*/true,
            /*description=*/"bosky map draw",
            /*suffix=*/"draw");
    }
#endif

    Log.info(__FUNCTION__, "| map updated with", RoguelikeBoskyPassageMap::get_instance().size(), "nodes");
    return true;
}

void asst::RoguelikeBoskyPassageRoutingTaskPlugin::record_bosky_map_snapshot(
    std::string_view entry_map_image,
    std::string_view entry_map_overlay) const
{
    auto& bosky_map = RoguelikeBoskyPassageMap::get_instance();

    json::array nodes;
    for (size_t index = 0; index < static_cast<size_t>(RoguelikeBoskyPassageMap::width() * RoguelikeBoskyPassageMap::height());
         ++index) {
        if (!bosky_map.get_node_exists(index)) {
            continue;
        }

        const std::string node_type = type2name(bosky_map.get_node_type(index));
        json::object node {
            { "index", static_cast<int>(index) },
            { "grid", json::array { bosky_map.get_node_x(index), bosky_map.get_node_y(index) } },
            { "node_type", node_type },
            { "node_name", bosky_passage_node_display_name(node_type) },
            { "is_open", bosky_map.get_node_open(index) },
            { "visited", bosky_map.get_node_visited(index) },
        };
        const RoguelikeBoskySubNodeType sub_type = bosky_map.get_node_subtype(index);
        if (sub_type != RoguelikeBoskySubNodeType::Unknown) {
            node["sub_type"] = subtype2name(sub_type);
        }
        nodes.emplace_back(std::move(node));
    }

    json::array edges;
    for (size_t from = 0; from < static_cast<size_t>(RoguelikeBoskyPassageMap::width() * RoguelikeBoskyPassageMap::height());
         ++from) {
        for (const size_t to : bosky_map.get_neighbors(from)) {
            if (to <= from) {
                continue;
            }
            edges.emplace_back(json::object {
                { "from", static_cast<int>(from) },
                { "to", static_cast<int>(to) },
                { "from_grid", json::array { bosky_map.get_node_x(from), bosky_map.get_node_y(from) } },
                { "to_grid", json::array { bosky_map.get_node_x(to), bosky_map.get_node_y(to) } },
            });
        }
    }

    const size_t center_index = bosky_map.center_index();
    json::object snapshot {
        { "width", RoguelikeBoskyPassageMap::width() },
        { "height", RoguelikeBoskyPassageMap::height() },
        { "center_index", static_cast<int>(center_index) },
        { "center_grid", json::array { bosky_map.get_node_x(center_index), bosky_map.get_node_y(center_index) } },
        { "node_count", static_cast<int>(nodes.size()) },
        { "edge_count", static_cast<int>(bosky_map.get_edge_count()) },
        { "nodes", std::move(nodes) },
        { "edges", std::move(edges) },
        { "view_config", json::object {
            { "origin_x", m_bosky_view_config.origin_x },
            { "origin_y", m_bosky_view_config.origin_y },
            { "middle_x", m_bosky_view_config.middle_x },
            { "middle_y", m_bosky_view_config.middle_y },
            { "last_x", m_bosky_view_config.last_x },
            { "last_y", m_bosky_view_config.last_y },
            { "node_width", m_bosky_view_config.node_width },
            { "node_height", m_bosky_view_config.node_height },
            { "column_offset", m_bosky_view_config.column_offset },
            { "row_offset", m_bosky_view_config.row_offset },
            { "roi_margin", m_bosky_view_config.roi_margin },
        } },
    };
    if (!entry_map_image.empty()) {
        snapshot["entry_map_image"] = std::string(entry_map_image);
    }
    if (!entry_map_overlay.empty()) {
        snapshot["entry_map_overlay"] = std::string(entry_map_overlay);
    }

    RoguelikeDataCollector.record_bosky_passage_map(std::move(snapshot));
}

void asst::RoguelikeBoskyPassageRoutingTaskPlugin::bosky_update_edges(
    const cv::Mat& image,
    const RoguelikeBoskyPassageMap::BoskyPassageMapConfig& view_config,
    std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt)
{
    LogTraceFunction;

    auto& bosky_map = RoguelikeBoskyPassageMap::get_instance();
    bosky_map.reset_edges();

    if (image.empty() || view_config.node_width <= 0 || view_config.node_height <= 0 ||
        view_config.column_offset <= 0 || view_config.row_offset <= 0) {
        Log.warn(__FUNCTION__, "| skip bosky edge update because image or map config is invalid");
        return;
    }

    const cv::Mat edge_mask = build_bosky_edge_mask(image);
    constexpr int LineBandHalfWidth = 6;
    constexpr int EndpointPadding = 3;
    constexpr int NameplateClearance = 22;
    constexpr double CoverageThreshold = 0.40;
    constexpr double LongestRunThreshold = 0.32;
    constexpr double ShortCoverageThreshold = 0.34;
    constexpr double ShortLongestRunThreshold = 0.24;

    const auto graph_vertex_exists = [&](size_t index) {
        return index == bosky_map.center_index() || bosky_map.get_node_exists(index);
    };

    const auto edge_score_between = [&](size_t first, size_t second) -> std::optional<BoskyEdgeScore> {
        const int first_x = bosky_map.get_node_x(first);
        const int first_y = bosky_map.get_node_y(first);
        const int second_x = bosky_map.get_node_x(second);
        const int second_y = bosky_map.get_node_y(second);
        const bool horizontal = first_y == second_y && std::abs(first_x - second_x) == 1;
        const bool vertical = first_x == second_x && std::abs(first_y - second_y) == 1;
        if (!horizontal && !vertical) {
            return std::nullopt;
        }

        const auto [first_px, first_py] = bosky_node_pixel(first, view_config);
        const auto [second_px, second_py] = bosky_node_pixel(second, view_config);
        cv::Rect roi;
        if (horizontal) {
            const int left_px = std::min(first_px, second_px);
            const int right_px = std::max(first_px, second_px);
            const int center_y = (first_py + second_py + view_config.node_height) / 2;
            const int x1 = left_px + view_config.node_width + EndpointPadding;
            const int x2 = right_px - EndpointPadding;
            roi = clipped_rect(
                edge_mask,
                x1,
                center_y - LineBandHalfWidth,
                x2 - x1,
                LineBandHalfWidth * 2 + 1);
        }
        else {
            const int top_py = std::min(first_py, second_py);
            const int bottom_py = std::max(first_py, second_py);
            const int center_x = (first_px + second_px + view_config.node_width) / 2;
            const int y1 = top_py + view_config.node_height + NameplateClearance;
            const int y2 = bottom_py - EndpointPadding;
            roi = clipped_rect(
                edge_mask,
                center_x - LineBandHalfWidth,
                y1,
                LineBandHalfWidth * 2 + 1,
                y2 - y1);
        }

        return score_bosky_edge_roi(edge_mask, roi, horizontal);
    };

    const auto is_edge_accepted = [&](const BoskyEdgeScore& score, bool horizontal) {
        if (score.length <= 0) {
            return false;
        }
        const bool short_edge = score.length < 40;
        const double coverage_threshold = short_edge ? ShortCoverageThreshold : CoverageThreshold;
        const double longest_run_threshold = short_edge ? ShortLongestRunThreshold : LongestRunThreshold;
        if (score.coverage < coverage_threshold || score.longest_ratio < longest_run_threshold) {
            return false;
        }
        if (!horizontal && (score.longest_ratio < 0.60 || score.head_coverage < 0.25 || score.tail_coverage < 0.25)) {
            return false;
        }
        return true;
    };

    const auto draw_candidate = [&](size_t first, size_t second, bool accepted) {
        if (!image_draw_opt.has_value()) {
            return;
        }
        auto& image_draw = image_draw_opt.value().get();
        const auto [first_px, first_py] = bosky_node_pixel(first, view_config);
        const auto [second_px, second_py] = bosky_node_pixel(second, view_config);
        const cv::Point first_center(
            first_px + view_config.node_width / 2,
            first_py + view_config.node_height / 2);
        const cv::Point second_center(
            second_px + view_config.node_width / 2,
            second_py + view_config.node_height / 2);
        cv::line(
            image_draw,
            first_center,
            second_center,
            accepted ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255),
            accepted ? 3 : 1);
    };

    const size_t width = RoguelikeBoskyPassageMap::width();
    const size_t height = RoguelikeBoskyPassageMap::height();
    size_t candidates = 0;
    size_t accepted_edges = 0;
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const size_t first = y * width + x;
            if (!graph_vertex_exists(first)) {
                continue;
            }

            const std::array<std::pair<size_t, size_t>, 2> neighbors {
                std::pair<size_t, size_t> { x + 1, y },
                std::pair<size_t, size_t> { x, y + 1 },
            };
            for (const auto& [nx, ny] : neighbors) {
                if (nx >= width || ny >= height) {
                    continue;
                }
                const size_t second = ny * width + nx;
                if (!graph_vertex_exists(second)) {
                    continue;
                }

                ++candidates;
                const bool horizontal = bosky_map.get_node_y(first) == bosky_map.get_node_y(second);
                const std::optional<BoskyEdgeScore> score = edge_score_between(first, second);
                const bool accepted = score.has_value() && is_edge_accepted(*score, horizontal);
                if (accepted) {
                    bosky_map.add_undirected_edge(first, second);
                    ++accepted_edges;
                }
                draw_candidate(first, second, accepted);
                if (score) {
                    Log.trace(
                        __FUNCTION__,
                        "| edge",
                        first,
                        "<->",
                        second,
                        "score length:",
                        score->length,
                        "coverage:",
                        score->coverage,
                        "longest:",
                        score->longest_ratio,
                        "density:",
                        score->density,
                        "head:",
                        score->head_coverage,
                        "tail:",
                        score->tail_coverage,
                        "accepted:",
                        accepted);
                }
            }
        }
    }

    if (image_draw_opt.has_value()) {
        auto& image_draw = image_draw_opt.value().get();
        const auto [center_px, center_py] = bosky_node_pixel(bosky_map.center_index(), view_config);
        cv::circle(
            image_draw,
            cv::Point(center_px + view_config.node_width / 2, center_py + view_config.node_height / 2),
            8,
            cv::Scalar(0, 255, 255),
            -1);
    }

    Log.info(
        __FUNCTION__,
        "| bosky edges updated, candidates:",
        candidates,
        "accepted:",
        accepted_edges,
        "stored:",
        bosky_map.get_edge_count());
}

void asst::RoguelikeBoskyPassageRoutingTaskPlugin::bosky_decide_and_click(
    const std::vector<RoguelikeNodeType>& priority_order)
{
    LogTraceFunction;

    Log.info(__FUNCTION__, "| deciding and clicking a bosky passage node");

    size_t chosen = 0;
    bool found = false;

    // 按优先级顺序查找可用的节点
    for (const auto& node_type : priority_order) {
        auto nodes_of_type = RoguelikeBoskyPassageMap::get_instance().get_open_unvisited_nodes(node_type);
        if (!nodes_of_type.empty()) {
            chosen = choose_random_bosky_node(nodes_of_type);
            found = true;
            Log.debug(
                __FUNCTION__,
                "| found",
                nodes_of_type.size(),
                "node(s) of type (",
                type2name(node_type),
                "), random index (",
                chosen,
                ")");
            break;
        }
    }

    if (!found) {
        Log.info(__FUNCTION__, "| no open unvisited nodes available");
        RoguelikeDataCollector.set_record_map_encounters(false);
        if (RoguelikeBoskyPassageMap::get_instance().consume_abandon_on_exit()) {
            json::object details {
                { "floor", "是非境" },
                { "reason", "target_candle_bosky_complete" },
            };
            RoguelikeDataCollector.set_pending_abandon_reason("target_candle_bosky_complete", details);
            RoguelikeDataCollector.log_event("bosky_exit_abandon", std::move(details));
            Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-ExitThenAbandon");
            return;
        }
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-LeaveBoskyPassage");
        return;
    }

    int gx = RoguelikeBoskyPassageMap::get_instance().get_node_x(chosen);
    int gy = RoguelikeBoskyPassageMap::get_instance().get_node_y(chosen);
    RoguelikeNodeType node_type = RoguelikeBoskyPassageMap::get_instance().get_node_type(chosen);

    Log.info(__FUNCTION__, "| chosen node:", chosen, "(", gx, ",", gy, ") type:", type2name(node_type));

    // 点击节点中心
    auto [px, py] = RoguelikeBoskyPassageMap::get_instance().get_node_pixel(
        chosen,
        m_bosky_view_config.origin_x,
        m_bosky_view_config.origin_y,
        m_bosky_view_config.column_offset,
        m_bosky_view_config.row_offset);

    if (px == -1 || py == -1) {
        Log.error(__FUNCTION__, "| Invalid pixel coordinates for node", chosen, ": (", px, ",", py, ")");
        RoguelikeDataCollector.set_record_map_encounters(false);
        return;
    }

    Point click_point(px + m_bosky_view_config.node_width / 2, py + m_bosky_view_config.node_height / 2);
    sleep(1000);
    ctrler()->click(click_point);
    RoguelikeBoskyPassageMap::get_instance().set_visited(chosen);
    RoguelikeBoskyPassageMap::get_instance().set_curr_pos(chosen);
    std::string node_type_name = type2name(node_type);
    RoguelikeDataCollector.note_selected_node_type(node_type_name);
    RoguelikeDataCollector.note_bosky_passage_route(node_type_name, gx, gy, chosen);
    RoguelikeDataCollector.set_record_map_encounters(node_type == RoguelikeNodeType::Legend, "Legend");

    // 发送节点类型到 WPF
    auto node_info = basic_info_with_what("BoskyPassageNode");
    node_info["details"]["node_type"] = node_type_name;
    callback(AsstMsg::SubTaskExtraInfo, node_info);

    if (is_bosky_passage_detail_only_node(node_type)) {
        sleep(600);
        const cv::Mat detail_image = ctrler()->get_image();
        const std::string image_path =
            RoguelikeDataCollector.save_bosky_passage_image(detail_image, node_type_name + "_detail");
        RoguelikeDataCollector.record_bosky_passage_node(
            node_type_name,
            image_path,
            json::object {
                { "detail_only", true },
            });

        const Point close_point = bosky_passage_detail_close_point();
        for (int i = 0; i < 3; ++i) {
            ctrler()->click(close_point);
            sleep(200);
        }
        Task.set_task_base("RoguelikeRoutingAction", m_config->get_theme() + "@RoguelikeRoutingAction-ContinueBoskyPassage");
        return;
    }

    // 执行节点类型对应的任务
    const std::string& theme = m_config->get_theme();
    std::string node_name = node_type_name;

    const std::string node_task_name = theme + "@RoguelikeRoutingAction-Stage" + node_name + "Enter";
    // 设置 next
    Task.set_task_base("RoguelikeRoutingAction", node_task_name);
}

std::vector<asst::RoguelikeNodeType>
    asst::RoguelikeBoskyPassageRoutingTaskPlugin::get_bosky_passage_priority(const std::string& strategy)
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();
    const std::string config_name = theme + "@RoguelikeRouting-BoskyPassagePriority_" + strategy;

    const auto& task_info = Task.get<MatchTaskInfo>(config_name);
    if (!task_info) {
        Log.error(__FUNCTION__, "| priority config not found:", config_name);
        return {};
    }

    // 从 next 字段中读取优先级配置
    const auto& template_list = task_info->templ_names;
    if (template_list.empty()) {
        Log.warn(__FUNCTION__, "| Priority config is empty in:", config_name);
        return {};
    }

    // 从任务名称中解析节点类型
    auto priority_order_view = template_list | std::views::transform([&](const std::string& templ_name) {
                                   return RoguelikeMapInfo.templ2type(theme, templ_name);
                               });
    std::vector<RoguelikeNodeType> priority_order(priority_order_view.begin(), priority_order_view.end());

    Log.info(__FUNCTION__, "| Loaded", priority_order.size(), "node types from priority config");
    return priority_order;
}
