#include "RoguelikeRoutingTaskPlugin.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/PixelAnalyzer.h"
#include "Vision/MultiMatcher.h"

bool asst::RoguelikeRoutingTaskPlugin::load_params([[maybe_unused]] const json::value& params)
{
    const std::string& theme = m_config->get_theme();

    // 本插件暂处于实验阶段，仅用于萨卡兹和界园肉鸽的第一层
    if (theme != RoguelikeTheme::Sarkaz && theme != RoguelikeTheme::JieGarden) {
        return false;
    }

    const TaskPtr config_task = Task.get("RoguelikeRoutingConfig");

    m_origin_x = config_task->special_params.at(0);
    m_middle_x = config_task->special_params.at(1);
    m_last_x = config_task->special_params.at(2);
    m_node_width = config_task->special_params.at(3);
    m_node_height = config_task->special_params.at(4);
    m_column_offset = config_task->special_params.at(5);
    m_nameplate_offset = config_task->special_params.at(6);
    m_roi_margin = config_task->special_params.at(7);
    m_direction_threshold = config_task->special_params.at(8);

    const RoguelikeMode& mode = m_config->get_mode();
    const std::string squad = params.get("squad", "");

    if (theme == RoguelikeTheme::Sarkaz && mode == RoguelikeMode::FastPass && squad == "蓝图测绘分队") {
        m_routing_strategy = RoutingStrategy::Sarkaz_FastPass;
        return true;
    }

    if (theme == RoguelikeTheme::Sarkaz && mode == RoguelikeMode::Investment && squad == "点刺成锭分队") {
        m_routing_strategy = RoutingStrategy::Sarkaz_FastInvestment;
        return true;
    }

    if (theme == RoguelikeTheme::JieGarden) {
        if (mode == RoguelikeMode::DataCollection) {
            m_routing_strategy = RoutingStrategy::JieGarden_DataCollection;
            return true;
        }

        if (((mode == RoguelikeMode::Investment && squad == "指挥分队") ||
             (mode == RoguelikeMode::Collectible && params.get("collectible_mode_squad", squad) == "指挥分队")) &&
            m_config->get_difficulty() >= 3) {
            m_routing_strategy = RoutingStrategy::JieGarden_FastPassWithBattle;
            return true;
        }
    }

    return false;
}

void asst::RoguelikeRoutingTaskPlugin::reset_in_run_variables()
{
    m_map.reset();
    m_need_generate_map = true;
    m_data_collection_floor = 0;
    m_data_collection_current_node = RoguelikeMap::INIT_INDEX;
    m_data_collection_vertical_edge_used = false;
    m_data_collection_full_map_image.release();
    m_data_collection_column_xs.clear();
    m_data_collection_edge_scores.clear();
    m_selected_column = 0;
    m_selected_x = 0;
}

bool asst::RoguelikeRoutingTaskPlugin::verify(const AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    std::string task_name = details.get("details", "task", "");

    // trigger 任务的名字可以为 "...@Roguelike@Routing-..." 的形式
    if (const size_t pos = task_name.find('-'); pos != std::string::npos) {
        task_name = task_name.substr(0, pos);
    }

    if (task_name == m_config->get_theme() + "@Roguelike@Routing") {
        return true;
    }

    return false;
}

bool asst::RoguelikeRoutingTaskPlugin::_run()
{
    LogTraceFunction;

    switch (m_routing_strategy) {
    case RoutingStrategy::Sarkaz_FastInvestment:
        if (m_need_generate_map) {
            // 随机点击一个第一列的节点，先随便写写，垃圾代码迟早要重构
            ProcessTask(*this, { "Sarkaz@RoguelikeRouting-CombatOps" }).run();
            // 刷新节点
            ProcessTask(*this, { "Sarkaz@RoguelikeRouting-RefreshNode" }).run();
            // 不识别了，进商店，Go!
            Task.set_task_base("RoguelikeRoutingAction", "Sarkaz@RoguelikeRoutingAction-StageTraderEnter");
            // 偷懒，直接用 m_need_generate_map 判断是否已进过商店
            m_need_generate_map = false;
        }
        else {
            RoguelikeDataCollector.set_pending_abandon_reason(
                "sarkaz_fast_investment_complete",
                json::object { { "strategy", "sarkaz_fast_investment" } });
            Task.set_task_base("RoguelikeRoutingAction", "Sarkaz@RoguelikeRoutingAction-ExitThenAbandon");
        }
        break;
    case RoutingStrategy::JieGarden_FastPassWithBattle:
        if (m_need_generate_map) {
            // 向左滑动以检视前三列节点
            ProcessTask(*this, { "RoguelikeRouting-MoveRightToPeek" }).run();
            cv::Mat image = ctrler()->get_image();
            cv::Mat image_draw = image.clone();
            update_map(image, RoguelikeMap::INIT_INDEX + 1, image_draw);
#ifdef ASST_DEBUG
            utils::save_debug_image(
                image_draw,
                utils::path("debug") / "roguelikeMap",
                /*auto_clean=*/true,
                /*description=*/"bosky map draw",
                /*suffix=*/"draw");
#endif
            m_need_generate_map = false;

            // 根据第三列节点类型更新导航策略
            const size_t sample_node_of_last_column = m_map.size() - 1;
            const RoguelikeNodeType sample_node_type = m_map.get_node_type(sample_node_of_last_column);
            Log.info("RoguelikeRouting | Type of last node:", type2name(sample_node_type));
            if (sample_node_type == RoguelikeNodeType::RogueTrader) {
                m_routing_strategy = RoutingStrategy::JieGarden_FastPassWithoutBattle;
                m_config->set_skip_recruit_in_fast_pass(true);
                return _run();
            }
        }
        if (m_map.get_curr_pos() == RoguelikeMap::INIT_INDEX) {
            // 规划路线
            m_map.set_cost_fun([&](const RoguelikeNodePtr& node) {
                if (node->type == RoguelikeNodeType::CombatOps) {
                    return 10;
                }
                if (node->type == RoguelikeNodeType::EmergencyOps || node->type == RoguelikeNodeType::DreadfulFoe) {
                    return 11;
                }
                return 0;
            });
            m_map.update_node_costs();
            const size_t next_node = m_map.get_next_node();

            // 若无法避免超过三场战斗则重开
            if (m_map.get_node_cost(next_node) >= 30) {
                RoguelikeDataCollector.set_pending_abandon_reason(
                    "too_many_battles_ahead",
                    json::object {
                        { "strategy", "fast_pass_with_battle" },
                        { "node_cost", m_map.get_node_cost(next_node) },
                    });
                callback(
                    AsstMsg::TaskChainExtraInfo,
                    json::object {
                        { "what", "RoutingRestart" },
                        { "why", "TooManyBattlesAhead" },
                        { "node_cost", m_map.get_node_cost(next_node) },
                    });

                Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-ExitThenAbandon");
            }
            else {
                const int next_node_x = m_left_most_column_x_in_view;
                const int next_node_y = m_map.get_node_y(next_node);
                Point next_node_center = Point(next_node_x + m_node_width / 2, next_node_y + m_node_height / 2);
                ctrler()->click(next_node_center);
                sleep(200);

                Task.set_task_base(
                    "RoguelikeRoutingAction",
                    "JieGarden@RoguelikeRoutingAction-StageCombatOpsEnterThenLeave");
                m_map.set_curr_pos(next_node);
            }
        }
        else {
            // 执行默认的避战策略
            Task.set_task_base("RoguelikeRoutingAction", "JieGarden@Roguelike@Stages_default");
        }
        break;

    case RoutingStrategy::JieGarden_FastPassWithoutBattle:
        if (m_need_generate_map) {
            cv::Mat image = ctrler()->get_image();
            cv::Mat image_draw = image.clone();
            update_map(image, RoguelikeMap::INIT_INDEX + 1, image_draw);
#ifdef ASST_DEBUG
            utils::save_debug_image(
                image_draw,
                utils::path("debug") / "roguelikeMap",
                /*auto_clean=*/true,
                /*description=*/"bosky map draw",
                /*suffix=*/"draw");
#endif
            m_need_generate_map = false;
        }
        if (m_map.get_curr_pos() == RoguelikeMap::INIT_INDEX) {
            m_map.set_cost_fun([&](const RoguelikeNodePtr& node) {
                if (node->type == RoguelikeNodeType::CombatOps || node->type == RoguelikeNodeType::EmergencyOps ||
                    node->type == RoguelikeNodeType::DreadfulFoe) {
                    return 1;
                }
                return 0;
            });
            m_map.update_node_costs();
            const size_t next_node = m_map.get_next_node();

            // 若无法避免超过两场战斗则重开
            if (m_map.get_node_cost(next_node) >= 2) {
                RoguelikeDataCollector.set_pending_abandon_reason(
                    "too_many_battles_ahead",
                    json::object {
                        { "strategy", "fast_pass_without_battle" },
                        { "node_cost", m_map.get_node_cost(next_node) },
                    });
                callback(
                    AsstMsg::TaskChainExtraInfo,
                    json::object {
                        { "what", "RoutingRestart" },
                        { "why", "TooManyBattlesAhead" },
                        { "node_cost", m_map.get_node_cost(next_node) },
                    });

                Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-ExitThenAbandon");
            }
            else {
                const int next_node_x = m_left_most_column_x_in_view;
                const int next_node_y = m_map.get_node_y(next_node);
                Point next_node_center = Point(next_node_x + m_node_width / 2, next_node_y + m_node_height / 2);
                ctrler()->click(next_node_center);
                sleep(200);

                Task.set_task_base(
                    "RoguelikeRoutingAction",
                    "JieGarden@RoguelikeRoutingAction-StageCombatOpsEnterThenLeave");
                m_map.set_curr_pos(next_node);
            }
        }
        else {
            // 执行默认的避战策略
            Task.set_task_base("RoguelikeRoutingAction", "JieGarden@Roguelike@Stages_default");
        }
        break;
    case RoutingStrategy::JieGarden_DataCollection:
        navigate_data_collection_route();
        break;
    case RoutingStrategy::Sarkaz_FastPass:
        if (m_need_generate_map) {
            generate_map();
            m_need_generate_map = false;
        }

        m_selected_column = m_map.get_node_column(m_map.get_curr_pos());
        update_selected_x();

        refresh_following_combat_nodes();
        navigate_route();
        break;

    default:
        break;
    }

    return true;
}

bool asst::RoguelikeRoutingTaskPlugin::update_map(
    const cv::Mat& image,
    const size_t leftmost_column,
    std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt,
    bool include_same_column_edges)
{
    LogTraceFunction;

    if (leftmost_column == 0) {
        Log.error(__FUNCTION__, "| leftmost_column must be greater than zero");
        return false;
    }

    const std::string& theme = m_config->get_theme();

    size_t curr_col = leftmost_column - 1;
    int curr_x = -m_node_width - 1; // 第一列节点将触发 rect.x >= curr_x + m_node_width 并更新 curr_col 与 curr_x

    MultiMatcher node_analyzer(image);
    node_analyzer.set_task_info(theme + "@RoguelikeRoutingNodeAnalyze");
    if (const auto task_ptr = Task.get<MatchTaskInfo>(theme + "@RoguelikeRoutingNodeAnalyze");
        task_ptr && task_ptr->roi.x + task_ptr->roi.width < image.cols) {
        Rect roi = task_ptr->roi;
        roi.width = image.cols - roi.x;
        node_analyzer.set_roi(roi);
    }
    if (!node_analyzer.analyze()) {
        Log.error(__FUNCTION__, "| no nodes are recognised");
        return false;
    }
    MultiMatcher::ResultsVec match_results = node_analyzer.get_result();
    sort_by_vertical_(match_results); // 按照水平方向从左到右排序各列节点，同一列节点按照垂直方向从上到下排序
    m_left_most_column_x_in_view = match_results.front().rect.x;

    struct RecognizedMapNode
    {
        Rect rect;
        std::string templ_name;
        RoguelikeNodeType type = RoguelikeNodeType::Unknown;
        size_t column = RoguelikeMap::INIT_INDEX;
        bool is_grey = false;
    };

    std::vector<RecognizedMapNode> recognized_nodes;
    recognized_nodes.reserve(match_results.size());
    size_t first_active_column = std::numeric_limits<size_t>::max();

    for (const auto& [rect, score, templ_name] : match_results) {
        if (rect.x >= curr_x + m_node_width) { // 识别到下一列的节点
            ++curr_col;
            curr_x = rect.x;
        }

        const bool is_grey = templ_name.find("Grey") != std::string::npos;
        if (!is_grey) {
            first_active_column = std::min(first_active_column, curr_col);
        }
        recognized_nodes.emplace_back(RecognizedMapNode {
            rect,
            templ_name,
            RoguelikeMapInfo.templ2type(theme, templ_name),
            curr_col,
            is_grey,
        });
    }

    const size_t old_num_columns = m_map.get_num_columns();
    for (const auto& node_info : recognized_nodes) {
#ifdef ASST_DEBUG
        if (image_draw_opt.has_value()) {
            cv::rectangle(
                image_draw_opt.value().get(),
                make_rect<cv::Rect>(node_info.rect),
                cv::Scalar(255, 255, 255),
                2);
            cv::putText(
                image_draw_opt.value().get(),
                node_info.templ_name,
                cv::Point(node_info.rect.x, node_info.rect.y - m_roi_margin),
                cv::FONT_HERSHEY_DUPLEX,
                0.5,
                cv::Scalar(255, 255, 255));
        }
#endif
        if (node_info.column >= old_num_columns) // 仅更新新列节点
        {
            const size_t node =
                m_map.create_and_insert_node(node_info.type, node_info.column, node_info.rect.y).value();
            const bool visited = node_info.is_grey && first_active_column != std::numeric_limits<size_t>::max() &&
                                 node_info.column < first_active_column;
            m_map.set_node_visited(node, visited);
            generate_edges(node, image, node_info.rect.x, include_same_column_edges, image_draw_opt);
        }
    }

    return true;
}

void asst::RoguelikeRoutingTaskPlugin::generate_map()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    m_map.reset();
    size_t curr_col = RoguelikeMap::INIT_INDEX + 1;
    Rect roi = Task.get<MatchTaskInfo>(theme + "@RoguelikeRoutingNodeAnalyze")->roi;

    // 第一列节点
    cv::Mat image = ctrler()->get_image();
    MultiMatcher node_analyzer(image);
    node_analyzer.set_task_info(theme + "@RoguelikeRoutingNodeAnalyze");
    if (!node_analyzer.analyze()) {
        Log.error(__FUNCTION__, "| no nodes found in the first column");
        return;
    }
    MultiMatcher::ResultsVec match_results = node_analyzer.get_result();
    sort_by_horizontal_(match_results); // 按照垂直方向排序（从上到下）
    for (const auto& [rect, score, templ_name] : match_results) {
        const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(theme, templ_name);
        const size_t node = m_map.create_and_insert_node(type, curr_col, rect.y).value();
        generate_edges(node, image, rect.x);
    }

    // 第二列及以后的节点
    roi.x += m_column_offset;
    node_analyzer.set_roi(roi);
    while (!need_exit() && node_analyzer.analyze()) {
        ++curr_col;
        match_results = node_analyzer.get_result();
        sort_by_horizontal_(match_results);
        for (const auto& [rect, score, templ_name] : match_results) {
            const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(theme, templ_name);
            const size_t node = m_map.create_and_insert_node(type, curr_col, rect.y).value();
            generate_edges(node, image, rect.x);
        }
        ProcessTask(*this, { "RoguelikeRouting-MoveRight" }).run();
        sleep(200);
        image = ctrler()->get_image();
        node_analyzer.set_image(image);
    }

    ProcessTask(*this, { theme + "@RoguelikeRouting-ExitThenContinue" }).run(); // 通过退出重进回到初始位置
}

asst::RoguelikeRoutingTaskPlugin::DataCollectionEdgeScore
    asst::RoguelikeRoutingTaskPlugin::score_data_collection_horizontal_edge(
        const cv::Mat& image,
        size_t source,
        int source_x,
        size_t target,
        int target_x) const
{
    DataCollectionEdgeScore result;
    result.source = source;
    result.target = target;

    constexpr int SampleStepX = 2;
    constexpr int MaxStepYPerSample = 8;
    constexpr double EdgeScoreThreshold = 0.97;
    constexpr int BrightGate = 245;
    constexpr double RoiLeftRatio = 0.15;
    constexpr double RoiRightRatio = 0.85;

    const int source_y = m_map.get_node_y(source);
    const int target_y = m_map.get_node_y(target);
    const int raw_roi_x = source_x + m_node_width;
    const int raw_roi_right = target_x;
    const int raw_roi_width = raw_roi_right - raw_roi_x;
    const int roi_x = raw_roi_x + static_cast<int>(std::round(raw_roi_width * RoiLeftRatio));
    const int roi_right = raw_roi_x + static_cast<int>(std::round(raw_roi_width * RoiRightRatio));
    const int roi_y = std::min(source_y, target_y);
    const int roi_bottom = std::max(source_y + m_node_height, target_y + m_node_height);

    Rect roi(roi_x, roi_y, roi_right - roi_x, roi_bottom - roi_y);
    if (roi.width <= SampleStepX || roi.height <= 2 || roi.x < 0 || roi.y < 0 ||
        roi.x + roi.width > image.cols || roi.y + roi.height > image.rows) {
        result.reject_reason = "invalid_roi";
        return result;
    }

    const int start_begin_y = std::clamp(source_y - roi.y, 0, roi.height - 1);
    const int start_end_y = std::clamp(source_y + m_node_height - 1 - roi.y, 0, roi.height - 1);
    const int target_begin_y = std::clamp(target_y - roi.y, 0, roi.height - 1);
    const int target_end_y = std::clamp(target_y + m_node_height - 1 - roi.y, 0, roi.height - 1);

    cv::Mat cropped = make_roi(image, roi);
    cv::Mat gray;
    cv::cvtColor(cropped, gray, cv::COLOR_BGR2GRAY);

    cv::Mat bright_mask;
    cv::threshold(gray, bright_mask, BrightGate, 255, cv::THRESH_BINARY);

    cv::Mat profile_mat;
    bright_mask.convertTo(profile_mat, CV_32F, 1.0 / 255.0);

    std::vector<int> sample_xs;
    for (int x = 0; x < roi.width; x += SampleStepX) {
        sample_xs.emplace_back(x);
    }
    if (sample_xs.empty() || sample_xs.back() != roi.width - 1) {
        sample_xs.emplace_back(roi.width - 1);
    }

    const int sample_count = static_cast<int>(sample_xs.size());
    result.path_length = sample_count;
    std::vector<std::vector<float>> profiles(sample_count, std::vector<float>(roi.height, 0.0F));
    for (int sample = 0; sample < sample_count; ++sample) {
        const int x_begin = std::max(0, sample_xs[sample] - SampleStepX / 2);
        const int x_end = std::min(roi.width - 1, sample_xs[sample] + SampleStepX / 2);
        for (int y = 0; y < roi.height; ++y) {
            float max_value = 0.0F;
            for (int x = x_begin; x <= x_end; ++x) {
                max_value = std::max(max_value, profile_mat.at<float>(y, x));
            }
            profiles[sample][y] = max_value;
        }
    }

    constexpr double NegInf = -1.0e9;
    std::vector<double> prev_dp(roi.height, NegInf);
    std::vector<double> curr_dp(roi.height, NegInf);
    std::vector<std::vector<int>> backtrace(sample_count, std::vector<int>(roi.height, -1));

    for (int y = start_begin_y; y <= start_end_y; ++y) {
        prev_dp[y] = profiles.front()[y];
    }

    for (int sample = 1; sample < sample_count; ++sample) {
        std::ranges::fill(curr_dp, NegInf);
        for (int y = 0; y < roi.height; ++y) {
            const int prev_begin = std::max(0, y - MaxStepYPerSample);
            const int prev_end = std::min(roi.height - 1, y + MaxStepYPerSample);
            for (int prev_y = prev_begin; prev_y <= prev_end; ++prev_y) {
                const double value = prev_dp[prev_y] + profiles[sample][y];
                if (value > curr_dp[y]) {
                    curr_dp[y] = value;
                    backtrace[sample][y] = prev_y;
                }
            }
        }
        prev_dp.swap(curr_dp);
    }

    int best_y = target_begin_y;
    double best_value = NegInf;
    for (int y = target_begin_y; y <= target_end_y; ++y) {
        if (prev_dp[y] > best_value) {
            best_value = prev_dp[y];
            best_y = y;
        }
    }
    if (best_value <= NegInf / 2) {
        result.reject_reason = "endpoint_unreachable";
        return result;
    }

    std::vector<int> path(sample_count, best_y);
    for (int sample = sample_count - 1; sample > 0; --sample) {
        const int prev_y = backtrace[sample][path[sample]];
        if (prev_y < 0) {
            break;
        }
        path[sample - 1] = prev_y;
    }

    int support_count = 0;
    double total_step = 0;
    for (int sample = 0; sample < sample_count; ++sample) {
        const double support = profiles[sample][path[sample]];
        if (support >= 0.25) {
            ++support_count;
        }
        if (sample > 0) {
            total_step += std::abs(path[sample] - path[sample - 1]);
        }
    }

    result.support_ratio = support_count / static_cast<double>(sample_count);
    result.endpoint_score = 1.0;
    result.continuity =
        1.0 - std::min(1.0, total_step / std::max(1, sample_count - 1) / MaxStepYPerSample);
    result.end_error = 0.0;
    result.score = result.support_ratio;
    result.score = std::clamp(result.score, 0.0, 1.0);

    if (result.score < EdgeScoreThreshold) {
        result.reject_reason = "below_threshold";
    }

    return result;
}

void asst::RoguelikeRoutingTaskPlugin::generate_data_collection_map()
{
    LogTraceFunction;

    m_map.reset();
    m_data_collection_current_node = RoguelikeMap::INIT_INDEX;

    cv::Mat image = ctrler()->get_image();
    cv::Mat stitched_image = image.clone();
    struct ViewColumn
    {
        int x = 0;
        struct Node
        {
            std::string templ_name;
            RoguelikeNodeType type = RoguelikeNodeType::Unknown;
            int y = 0;
            bool is_grey = false;
        };
        std::vector<Node> nodes;
    };

    auto get_view_columns = [&](const cv::Mat& view) -> std::vector<ViewColumn> {
        MultiMatcher node_analyzer(view);
        node_analyzer.set_task_info(m_config->get_theme() + "@RoguelikeRoutingNodeAnalyze");
        if (const auto task_ptr = Task.get<MatchTaskInfo>(m_config->get_theme() + "@RoguelikeRoutingNodeAnalyze");
            task_ptr && task_ptr->roi.x + task_ptr->roi.width < view.cols) {
            Rect roi = task_ptr->roi;
            roi.width = view.cols - roi.x;
            node_analyzer.set_roi(roi);
        }
        if (!node_analyzer.analyze()) {
            return {};
        }

        auto match_results = node_analyzer.get_result();
        sort_by_vertical_(match_results);

        std::vector<ViewColumn> columns;
        int curr_x = -m_node_width - 1;
        for (const auto& [rect, score, templ_name] : match_results) {
            if (rect.x >= curr_x + m_node_width) {
                curr_x = rect.x;
                columns.emplace_back(ViewColumn { rect.x, {} });
            }

            const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(m_config->get_theme(), templ_name);
            columns.back().nodes.emplace_back(ViewColumn::Node {
                templ_name,
                type,
                rect.y,
                templ_name.find("Grey") != std::string::npos,
            });
        }
        return columns;
    };

    auto column_matches = [](const ViewColumn& lhs, const ViewColumn& rhs) -> bool {
        if (lhs.nodes.size() != rhs.nodes.size()) {
            return false;
        }

        constexpr int NodeYTolerance = 12;
        for (size_t i = 0; i < lhs.nodes.size(); ++i) {
            if (lhs.nodes.at(i).type != rhs.nodes.at(i).type ||
                std::abs(lhs.nodes.at(i).y - rhs.nodes.at(i).y) > NodeYTolerance) {
                return false;
            }
        }
        return true;
    };

    auto has_equivalent_column = [&](const std::vector<ViewColumn>& base_columns, const ViewColumn& column) -> bool {
        return std::ranges::any_of(base_columns, [&](const ViewColumn& base_column) {
            return column_matches(base_column, column) && std::abs(base_column.x - column.x) <= m_node_width / 2;
        });
    };

    auto append_new_columns = [&](std::vector<ViewColumn>& base_columns,
                                  const std::vector<ViewColumn>& view_columns,
                                  int offset) -> bool {
        bool appended = false;
        for (ViewColumn column : view_columns) {
            column.x += offset;
            if (column.x <= base_columns.back().x + m_node_width / 2 || has_equivalent_column(base_columns, column)) {
                continue;
            }

            base_columns.emplace_back(std::move(column));
            appended = true;
        }
        return appended;
    };

    int last_view_offset = 0;
    auto find_stitch_offset = [&](const std::vector<ViewColumn>& base_columns,
                                  const std::vector<ViewColumn>& new_columns) -> std::optional<int> {
        struct OffsetCandidate
        {
            int offset = 0;
            int matched_count = 0;
        };

        std::vector<OffsetCandidate> candidates;
        for (const ViewColumn& base_column : base_columns) {
            for (const ViewColumn& new_column : new_columns) {
                if (!column_matches(base_column, new_column)) {
                    continue;
                }

                const int offset = base_column.x - new_column.x;
                if (offset <= last_view_offset + m_node_width / 2) {
                    continue;
                }

                int matched_count = 0;
                for (const ViewColumn& test_new_column : new_columns) {
                    const int test_global_x = test_new_column.x + offset;
                    const bool matched = std::ranges::any_of(base_columns, [&](const ViewColumn& test_base_column) {
                        return column_matches(test_base_column, test_new_column) &&
                               std::abs(test_base_column.x - test_global_x) <= m_node_width / 2;
                    });
                    if (matched) {
                        ++matched_count;
                    }
                }

                candidates.emplace_back(OffsetCandidate { offset, matched_count });
            }
        }

        auto best_it = std::ranges::max_element(candidates, [](const auto& lhs, const auto& rhs) {
            if (lhs.matched_count != rhs.matched_count) {
                return lhs.matched_count < rhs.matched_count;
            }
            return lhs.offset > rhs.offset;
        });
        if (best_it == candidates.end() || best_it->matched_count <= 0) {
            return std::nullopt;
        }
        return best_it->offset;
    };

    auto edge_crosses = [&](const DataCollectionEdgeScore& lhs, const DataCollectionEdgeScore& rhs) {
        const int lhs_source_y = m_map.get_node_y(lhs.source);
        const int rhs_source_y = m_map.get_node_y(rhs.source);
        const int lhs_target_y = m_map.get_node_y(lhs.target);
        const int rhs_target_y = m_map.get_node_y(rhs.target);
        return (lhs_source_y < rhs_source_y && lhs_target_y > rhs_target_y + m_direction_threshold) ||
               (lhs_source_y > rhs_source_y && lhs_target_y + m_direction_threshold < rhs_target_y);
    };

    auto select_column_edges = [&](std::vector<DataCollectionEdgeScore>& scores) {
        std::vector<size_t> candidate_indices;
        for (size_t i = 0; i < scores.size(); ++i) {
            if (scores[i].reject_reason.empty()) {
                candidate_indices.emplace_back(i);
            }
        }

        std::vector<size_t> selected_indices;
        if (candidate_indices.size() <= 20) {
            double best_score = -1.0;
            double best_endpoint_score = -1.0;
            int best_path_length = std::numeric_limits<int>::max();
            const size_t mask_count = size_t { 1 } << candidate_indices.size();
            for (size_t mask = 1; mask < mask_count; ++mask) {
                bool valid = true;
                double total_score = 0;
                double total_endpoint_score = 0;
                int total_path_length = 0;
                std::vector<size_t> selected;
                for (size_t bit = 0; bit < candidate_indices.size(); ++bit) {
                    if ((mask & (size_t { 1 } << bit)) == 0) {
                        continue;
                    }

                    const size_t score_index = candidate_indices[bit];
                    for (size_t selected_index : selected) {
                        if (edge_crosses(scores[score_index], scores[selected_index])) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        break;
                    }

                    selected.emplace_back(score_index);
                    total_score += scores[score_index].score;
                    total_endpoint_score += scores[score_index].endpoint_score;
                    total_path_length += scores[score_index].path_length;
                }

                if (!valid) {
                    continue;
                }

                if (total_score > best_score ||
                    (std::abs(total_score - best_score) < 1.0e-6 &&
                     (total_endpoint_score > best_endpoint_score ||
                      (std::abs(total_endpoint_score - best_endpoint_score) < 1.0e-6 &&
                       total_path_length < best_path_length)))) {
                    best_score = total_score;
                    best_endpoint_score = total_endpoint_score;
                    best_path_length = total_path_length;
                    selected_indices = std::move(selected);
                }
            }
        }
        else {
            std::ranges::sort(candidate_indices, [&](size_t lhs, size_t rhs) {
                if (std::abs(scores[lhs].score - scores[rhs].score) > 1.0e-6) {
                    return scores[lhs].score > scores[rhs].score;
                }
                if (std::abs(scores[lhs].endpoint_score - scores[rhs].endpoint_score) > 1.0e-6) {
                    return scores[lhs].endpoint_score > scores[rhs].endpoint_score;
                }
                return scores[lhs].path_length < scores[rhs].path_length;
            });

            for (size_t score_index : candidate_indices) {
                const bool crossed = std::ranges::any_of(selected_indices, [&](size_t selected_index) {
                    return edge_crosses(scores[score_index], scores[selected_index]);
                });
                if (!crossed) {
                    selected_indices.emplace_back(score_index);
                }
            }
        }

        for (size_t selected_index : selected_indices) {
            scores[selected_index].accepted = true;
        }

        for (DataCollectionEdgeScore& score : scores) {
            if (score.accepted) {
                score.reject_reason.clear();
                m_map.add_edge(score.source, score.target);
            }
            else if (score.reject_reason.empty()) {
                score.reject_reason = "crossing_rejected";
            }
            m_data_collection_edge_scores.emplace_back(score);
        }
    };

    auto add_horizontal_edges = [&](const std::vector<ViewColumn>& columns, const cv::Mat& map_image) {
        for (size_t i = 1; i < columns.size(); ++i) {
            const size_t source_column = RoguelikeMap::INIT_INDEX + i;
            const size_t target_column = source_column + 1;
            const size_t source_begin = m_map.get_column_begin(source_column);
            const size_t source_end = m_map.get_column_end(source_column);
            const size_t target_begin = m_map.get_column_begin(target_column);
            const size_t target_end = m_map.get_column_end(target_column);

            std::vector<DataCollectionEdgeScore> scores;
            for (size_t source = source_begin; source < source_end; ++source) {
                for (size_t target = target_begin; target < target_end; ++target) {
                    scores.emplace_back(score_data_collection_horizontal_edge(
                        map_image,
                        source,
                        columns[i - 1].x,
                        target,
                        columns[i].x));
                }
            }
            select_column_edges(scores);
        }
    };

    auto add_vertical_edges = [&](const std::vector<ViewColumn>& columns, const cv::Mat& map_image) {
        PixelAnalyzer analyzer(map_image);
        Rect roi(0, 0, m_roi_margin * 2, m_roi_margin * 2);
        for (size_t i = 0; i < columns.size(); ++i) {
            const size_t column = RoguelikeMap::INIT_INDEX + 1 + i;
            const size_t begin = m_map.get_column_begin(column);
            const size_t end = m_map.get_column_end(column);
            for (size_t node = begin + 1; node < end; ++node) {
                const size_t prev = node - 1;
                roi.x = columns[i].x + m_node_width / 2 - m_roi_margin;
                roi.y = (m_map.get_node_y(prev) + m_node_height + m_nameplate_offset + m_map.get_node_y(node)) / 2 -
                        m_roi_margin;
                analyzer.set_roi(roi);
                if (analyzer.analyze()) {
                    m_map.add_edge(prev, node);
                    m_map.add_edge(node, prev);
                }
            }
        }
    };

    auto build_map_from_columns = [&](const std::vector<ViewColumn>& columns, const cv::Mat& map_image) {
        m_data_collection_edge_scores.clear();

        size_t first_active_column = std::numeric_limits<size_t>::max();
        for (size_t i = 0; i < columns.size(); ++i) {
            const size_t column_index = RoguelikeMap::INIT_INDEX + 1 + i;
            const bool has_active_node = std::ranges::any_of(columns[i].nodes, [](const ViewColumn::Node& node) {
                return !node.is_grey;
            });
            if (has_active_node) {
                first_active_column = std::min(first_active_column, column_index);
            }
        }

        for (size_t i = 0; i < columns.size(); ++i) {
            const ViewColumn& column = columns[i];
            const size_t column_index = RoguelikeMap::INIT_INDEX + 1 + i;
            for (const auto& node_info : column.nodes) {
                const size_t node = m_map.create_and_insert_node(node_info.type, column_index, node_info.y).value();
                const bool visited = node_info.is_grey && first_active_column != std::numeric_limits<size_t>::max() &&
                                     column_index < first_active_column;
                m_map.set_node_visited(node, visited);
            }
        }

        if (!columns.empty()) {
            const size_t first_column = RoguelikeMap::INIT_INDEX + 1;
            for (size_t node = m_map.get_column_begin(first_column); node < m_map.get_column_end(first_column);
                 ++node) {
                m_map.add_edge(RoguelikeMap::INIT_INDEX, node);
            }
        }
        add_horizontal_edges(columns, map_image);
        add_vertical_edges(columns, map_image);
    };

    std::vector<ViewColumn> stitched_columns = get_view_columns(stitched_image);
    if (stitched_columns.empty()) {
        Log.error(__FUNCTION__, "| no nodes are recognised in first view");
        return;
    }

    size_t swipe_count = 0;
    constexpr size_t MaxMapSwipeTimes = 8;
    while (swipe_count < MaxMapSwipeTimes && !need_exit()) {
        ProcessTask(*this, { "RoguelikeRouting-MoveRight" }).run();
        ++swipe_count;
        sleep(200);
        image = ctrler()->get_image();

        std::vector<ViewColumn> view_columns = get_view_columns(image);
        if (view_columns.empty()) {
            break;
        }

        const std::optional<int> offset_opt = find_stitch_offset(stitched_columns, view_columns);
        if (!offset_opt) {
            Log.info(__FUNCTION__, "| no reliable map overlap, finish scan");
            break;
        }

        const int offset = *offset_opt;
        std::vector<ViewColumn> new_global_columns = view_columns;
        for (ViewColumn& column : new_global_columns) {
            column.x += offset;
        }
        const bool has_new_column = std::ranges::any_of(new_global_columns, [&](const ViewColumn& column) {
            return column.x > stitched_columns.back().x + m_node_width / 2 &&
                   !has_equivalent_column(stitched_columns, column);
        });
        if (!has_new_column) {
            Log.info(__FUNCTION__, "| no new map column after swipe, finish scan");
            break;
        }

        const int new_width = std::max(stitched_image.cols, offset + image.cols);
        if (new_width <= stitched_image.cols) {
            Log.info(__FUNCTION__, "| stitched map width not increased, finish scan");
            break;
        }

        cv::Mat merged_image(stitched_image.rows, new_width, stitched_image.type(), cv::Scalar(0));
        stitched_image.copyTo(merged_image(cv::Rect { 0, 0, stitched_image.cols, stitched_image.rows }));

        const int src_x = std::max(0, stitched_image.cols - offset);
        const int dst_x = offset + src_x;
        const int copy_width = std::min(image.cols - src_x, new_width - dst_x);
        if (copy_width <= 0) {
            Log.info(__FUNCTION__, "| no image area to append, finish scan");
            break;
        }

        image(cv::Rect { src_x, 0, copy_width, image.rows })
            .copyTo(merged_image(cv::Rect { dst_x, 0, copy_width, image.rows }));
        stitched_image = std::move(merged_image);
        append_new_columns(stitched_columns, view_columns, offset);
        last_view_offset = offset;
    }

    if (need_exit()) {
        Log.info(__FUNCTION__, "| map scan interrupted, skip incomplete route graph");
        return;
    }

    build_map_from_columns(stitched_columns, stitched_image);
    ++m_data_collection_floor;
    m_data_collection_full_map_image = stitched_image.clone();
    m_data_collection_column_xs.clear();
    for (const ViewColumn& column : stitched_columns) {
        m_data_collection_column_xs.emplace_back(column.x);
    }
    const std::string full_map_path = RoguelikeDataCollector.save_image(stitched_image, "full_map");
    const std::string full_map_graph_path =
        RoguelikeDataCollector.save_image(build_data_collection_graph_image(), "full_map_graph");
    RoguelikeDataCollector.log_event(
        "map_edges",
        json::object {
            { "floor", m_data_collection_floor },
            { "full_map", full_map_path },
            { "full_map_graph", full_map_graph_path },
            { "edge_score_matrix", build_data_collection_edge_score_json() },
        });

    for (size_t i = 0; i < swipe_count && !need_exit(); ++i) {
        ProcessTask(*this, { "RoguelikeRouting-MoveLeft" }).run();
        sleep(200);
    }

    m_need_generate_map = false;
}

cv::Mat asst::RoguelikeRoutingTaskPlugin::build_data_collection_graph_image(
    const std::vector<size_t>& selected_route,
    std::optional<size_t> selected_node) const
{
    if (m_data_collection_full_map_image.empty()) {
        return {};
    }

    cv::Mat graph_image = m_data_collection_full_map_image.clone();
    auto node_center = [&](size_t node) -> std::optional<cv::Point> {
        if (node == RoguelikeMap::INIT_INDEX || node >= m_map.size()) {
            return std::nullopt;
        }

        const size_t column = m_map.get_node_column(node);
        if (column == RoguelikeMap::INIT_INDEX || column > m_data_collection_column_xs.size()) {
            return std::nullopt;
        }

        const int x = m_data_collection_column_xs[column - 1] + m_node_width / 2;
        const int y = m_map.get_node_y(node) + m_node_height / 2;
        return cv::Point(x, y);
    };

    auto draw_edge = [&](size_t from_node, size_t to_node, const cv::Scalar& color, int thickness) {
        const auto from_opt = node_center(from_node);
        const auto to_opt = node_center(to_node);
        if (!from_opt || !to_opt) {
            return;
        }

        cv::Point from = *from_opt;
        cv::Point to = *to_opt;
        if (to.x == from.x) {
            if (to.y >= from.y) {
                from.y += m_node_height / 2 - 4;
                to.y -= m_node_height / 2 - 4;
            }
            else {
                from.y -= m_node_height / 2 - 4;
                to.y += m_node_height / 2 - 4;
            }
        }
        else if (to.x > from.x) {
            from.x += m_node_width / 2 - 4;
            to.x -= m_node_width / 2 - 4;
        }
        else {
            from.x -= m_node_width / 2 - 4;
            to.x += m_node_width / 2 - 4;
        }
        cv::arrowedLine(graph_image, from, to, color, thickness, cv::LINE_AA, 0, 0.08);
    };

    for (const DataCollectionEdgeScore& score : m_data_collection_edge_scores) {
        if (!score.accepted) {
            draw_edge(score.source, score.target, cv::Scalar(60, 60, 255), 1);
        }
    }

    for (size_t node = 1; node < m_map.size(); ++node) {
        for (size_t succ : m_map.get_node_succs(node)) {
            draw_edge(node, succ, cv::Scalar(0, 220, 255), 3);
        }
    }

    for (size_t i = 1; i < selected_route.size(); ++i) {
        draw_edge(selected_route[i - 1], selected_route[i], cv::Scalar(255, 80, 0), 6);
    }

    for (size_t node = 1; node < m_map.size(); ++node) {
        const auto center_opt = node_center(node);
        if (!center_opt) {
            continue;
        }

        const bool in_selected_route = std::ranges::find(selected_route, node) != selected_route.end();
        const bool is_selected_node = selected_node && *selected_node == node;
        const cv::Point center = *center_opt;
        const int radius = std::max(m_node_width, m_node_height) / 2;
        cv::Scalar color = m_map.get_node_visited(node) ? cv::Scalar(120, 120, 120) : cv::Scalar(80, 255, 80);
        int thickness = 3;
        if (in_selected_route) {
            color = cv::Scalar(255, 0, 255);
            thickness = 5;
        }
        if (is_selected_node) {
            color = cv::Scalar(0, 80, 255);
            thickness = 7;
        }
        cv::circle(graph_image, center, radius, color, thickness, cv::LINE_AA);
        cv::putText(
            graph_image,
            std::to_string(node),
            cv::Point(center.x - 12, center.y + 8),
            cv::FONT_HERSHEY_DUPLEX,
            0.8,
            cv::Scalar(255, 255, 255),
            2,
            cv::LINE_AA);
    }

    return graph_image;
}

void asst::RoguelikeRoutingTaskPlugin::generate_edges(
    const size_t& node,
    const cv::Mat& image,
    const int& node_x,
    bool include_same_column_edges,
    [[maybe_unused]] std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt)
{
    LogTraceFunction;

    const size_t node_column = m_map.get_node_column(node);

    if (node_column == RoguelikeMap::INIT_INDEX) {
        Log.error(__FUNCTION__, "| cannot generate edges for init node");
        return;
    }

    // 将 image转换为二值图像后计算亮点
    PixelAnalyzer analyzer(image);
    const int node_y = m_map.get_node_y(node);
    Rect roi(0, 0, m_roi_margin * 2, m_roi_margin * 2);

    if (node_column == RoguelikeMap::INIT_INDEX + 1) {
        m_map.add_edge(RoguelikeMap::INIT_INDEX, node); // 第一列节点直接与 init 连接
    }
    else {
        const int center_x = node_x - (m_column_offset - m_node_width) / 2; // node 与 前一列节点的中点横坐标

        auto edge_span = [&](size_t source, size_t target) {
            return std::abs(m_map.get_node_y(source) - m_map.get_node_y(target));
        };

        auto try_add_non_crossing_edge = [&](size_t prev) {
            const int prev_y = m_map.get_node_y(prev);
            const int candidate_span = edge_span(prev, node);

            std::vector<std::pair<size_t, size_t>> crossing_edges;
            const size_t curr_col_begin = m_map.get_column_begin(node_column);
            for (size_t target = curr_col_begin; target < node; ++target) {
                const int target_y = m_map.get_node_y(target);
                for (size_t source : m_map.get_node_preds(target)) {
                    if (m_map.get_node_column(source) != node_column - 1) {
                        continue;
                    }

                    const int source_y = m_map.get_node_y(source);
                    const bool crossed =
                        (prev_y < source_y && node_y > target_y + m_direction_threshold) ||
                        (prev_y > source_y && node_y + m_direction_threshold < target_y);
                    if (!crossed) {
                        continue;
                    }

                    const int existing_span = edge_span(source, target);
                    if (candidate_span + m_direction_threshold >= existing_span) {
                        Log.info(
                            __FUNCTION__,
                            "| edge rejected by non-crossing prior | Node",
                            prev,
                            "-> Node",
                            node,
                            "conflicts with Node",
                            source,
                            "-> Node",
                            target);
                        return false;
                    }
                    crossing_edges.emplace_back(source, target);
                }
            }

            for (const auto& [source, target] : crossing_edges) {
                Log.info(
                    __FUNCTION__,
                    "| crossing edge replaced | Node",
                    source,
                    "-> Node",
                    target,
                    "by Node",
                    prev,
                    "-> Node",
                    node);
                m_map.remove_edge(source, target);
            }
            m_map.add_edge(prev, node);
            return true;
        };

        // 遍历前一列节点
        const size_t pre_col_begin = m_map.get_column_begin(node_column - 1);
        const size_t pre_col_end = m_map.get_column_end(node_column - 1);
        for (size_t prev = pre_col_begin; prev < pre_col_end; ++prev) {
            const int prev_y = m_map.get_node_y(prev);
            const int center_y = (prev_y + node_y + m_node_height) / 2;
            roi.x = center_x - m_roi_margin;
            roi.y = center_y - m_roi_margin;
#ifdef ASST_DEBUG
            if (image_draw_opt.has_value()) {
                cv::rectangle(image_draw_opt.value().get(), make_rect<cv::Rect>(roi), cv::Scalar(255, 255, 255), 1);
            }
#endif
            analyzer.set_roi(roi);

            if (!analyzer.analyze()) { // 节点间没有连线
                continue;
            }

            // 按照水平方向排序（从左到右）
            std::vector<Point> brightPixels = analyzer.get_result();

            auto [x_min_p, x_max_p] =
                std::ranges::minmax(brightPixels, /*comp=*/ {}, [](const Point& p) { return p.x; });
            const int leftmost_x = x_min_p.x;
            const int rightmost_x = x_max_p.x;

            auto leftmostBrightPixels =
                brightPixels | std::views::filter([&](const Point& p) { return p.x == leftmost_x; });
            auto rightmostBrightPixels =
                brightPixels | std::views::filter([&](const Point& p) { return p.x == rightmost_x; });

            auto [leftmost_y_min_p, leftmost_y_max_p] =
                std::ranges::minmax(leftmostBrightPixels, /*comp=*/ {}, [](const Point& p) { return p.y; });
            const int leftmost_y = (leftmost_y_min_p.y + leftmost_y_max_p.y) / 2;

            auto [rightmost_y_min_p, rightmost_y_max_p] =
                std::ranges::minmax(rightmostBrightPixels, /*comp=*/ {}, [](const Point& p) { return p.y; });
            const int rightmost_y = (rightmost_y_min_p.y + rightmost_y_max_p.y) / 2;

            const bool direction_matched =
                (std::abs(prev_y - node_y) < m_direction_threshold &&
                 std::abs(leftmost_y - rightmost_y) < m_direction_threshold) ||
                (prev_y < node_y && leftmost_y < rightmost_y - m_direction_threshold) ||
                (prev_y > node_y && leftmost_y > rightmost_y + m_direction_threshold);
            if (direction_matched) {
#ifdef ASST_DEBUG
                const bool edge_added = try_add_non_crossing_edge(prev);
                if (edge_added && image_draw_opt.has_value()) {
                    cv::line(
                        image_draw_opt.value().get(),
                        cv::Point(node_x - m_column_offset + m_node_width / 2, prev_y + m_node_height / 2),
                        cv::Point(node_x + m_node_width / 2, node_y + m_node_height / 2),
                        cv::Scalar(255, 255, 255),
                        2);
                }
#else
                try_add_non_crossing_edge(prev);
#endif
            }
        }
    }

    if (!include_same_column_edges) {
        return;
    }

    // 同列前一个节点
    if (node > m_map.get_column_begin(node_column)) {
        size_t prev = node - 1;
        roi.x = node_x + m_node_width / 2 - m_roi_margin;
        roi.y = (m_map.get_node_y(prev) + m_node_height + m_nameplate_offset + node_y) / 2 - m_roi_margin;
#ifdef ASST_DEBUG
        if (image_draw_opt.has_value()) {
            cv::rectangle(image_draw_opt.value().get(), make_rect<cv::Rect>(roi), cv::Scalar(255, 255, 255), 1);
        }
#endif
        analyzer.set_roi(roi);
        if (analyzer.analyze()) {
            m_map.add_edge(prev, node);
            m_map.add_edge(node, prev);
#ifdef ASST_DEBUG
            if (image_draw_opt.has_value()) {
                cv::line(
                    image_draw_opt.value().get(),
                    cv::Point(node_x + m_node_width / 2, m_map.get_node_y(prev) + m_node_height / 2),
                    cv::Point(node_x + m_node_width / 2, node_y + m_node_height / 2),
                    cv::Scalar(255, 255, 255),
                    2);
            }
#endif
        }
    }
}

void asst::RoguelikeRoutingTaskPlugin::refresh_following_combat_nodes()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    const size_t curr_node = m_map.get_curr_pos();
    const size_t curr_node_column = m_map.get_node_column(curr_node);

    for (size_t next_node : m_map.get_node_succs(curr_node)) {
        // 不刷新同一列的节点
        const size_t next_node_column = m_map.get_node_column(next_node);
        if (next_node_column <= curr_node_column) {
            continue;
        }
        // 每个节点仅刷新一次
        if (m_map.get_node_refresh_times(next_node)) {
            continue;
        }
        // 不刷新非战斗节点
        RoguelikeNodeType next_node_type = m_map.get_node_type(next_node);
        if (next_node_type != RoguelikeNodeType::CombatOps && next_node_type != RoguelikeNodeType::EmergencyOps &&
            next_node_type != RoguelikeNodeType::DreadfulFoe) {
            continue;
        }

        int next_node_x = m_selected_x + (next_node_column == m_selected_column ? 0 : m_column_offset);
        int next_node_y = m_map.get_node_y(next_node);
        Rect next_node_rect = Rect(next_node_x, next_node_y, m_node_width, m_node_height);

        // 点击节点
        ctrler()->click(next_node_rect);
        m_selected_column = m_map.get_node_column(next_node);
        update_selected_x();
        next_node_rect.x = m_selected_x;
        sleep(200);

        // 刷新节点
        ProcessTask(*this, { m_config->get_theme() + "@RoguelikeRouting-RefreshNode" }).run();
        m_map.set_node_refresh_times(next_node, m_map.get_node_refresh_times(next_node) + 1);

        // 识别并更新节点类型
        Matcher node_analyzer(ctrler()->get_image());
        node_analyzer.set_task_info(theme + "@RoguelikeRoutingNodeAnalyze");
        node_analyzer.set_roi(next_node_rect);
        if (node_analyzer.analyze()) {
            const Matcher::Result& match_results = node_analyzer.get_result();
            m_map.set_node_type(next_node, RoguelikeMapInfo.templ2type(theme, match_results.templ_name));
        }
    }
}

void asst::RoguelikeRoutingTaskPlugin::navigate_route()
{
    LogTraceFunction;

    const size_t curr_col = m_map.get_node_column(m_map.get_curr_pos());

    m_map.set_cost_fun([&](const RoguelikeNodePtr& node) {
        if (node->visited) {
            return 1000;
        }

        if (node->column == curr_col) {
            return 1000;
        }

        if (node->type == RoguelikeNodeType::CombatOps || node->type == RoguelikeNodeType::EmergencyOps ||
            node->type == RoguelikeNodeType::DreadfulFoe) {
            return 1 + (node->refresh_times ? 999 : 0);
        }

        return 0;
    });

    m_map.update_node_costs();

    const size_t next_node = m_map.get_next_node();

    if (m_map.get_node_cost(next_node) >= 1000) {
        RoguelikeDataCollector.set_pending_abandon_reason(
            "no_viable_route",
            json::object {
                { "strategy", "sarkaz_fast_investment" },
                { "node_cost", m_map.get_node_cost(next_node) },
            });
        Task.set_task_base("RoguelikeRoutingAction", "Sarkaz@RoguelikeRoutingAction-ExitThenAbandon");
        reset_in_run_variables();
        return;
    }

    const size_t next_node_column = m_map.get_node_column(next_node);
    const int next_node_x = m_selected_x + (next_node_column == m_selected_column ? 0 : m_column_offset);
    const int next_node_y = m_map.get_node_y(next_node);
    Point next_node_center = Point(next_node_x + m_node_width / 2, next_node_y + m_node_height / 2);
    ctrler()->click(next_node_center);
    sleep(200);

    if (m_map.get_node_type(next_node) == RoguelikeNodeType::Encounter) {
        Task.set_task_base("RoguelikeRoutingAction", "Sarkaz@RoguelikeRoutingAction-StageEncounterEnter");
        m_map.set_curr_pos(next_node);
    }
    else if (m_map.get_node_type(next_node) == RoguelikeNodeType::RogueTrader) {
        Task.set_task_base("RoguelikeRoutingAction", "Sarkaz@RoguelikeRoutingAction-StageTraderEnter");
        reset_in_run_variables();
    }
    else {
        RoguelikeDataCollector.set_pending_abandon_reason(
            "unexpected_route_node",
            json::object {
                { "strategy", "sarkaz_fast_investment" },
                { "node_type", type2name(m_map.get_node_type(next_node)) },
            });
        Task.set_task_base("RoguelikeRoutingAction", "Sarkaz@RoguelikeRoutingAction-ExitThenAbandon");
        reset_in_run_variables();
    }
}

void asst::RoguelikeRoutingTaskPlugin::navigate_data_collection_route()
{
    LogTraceFunction;

    if (m_need_generate_map ||
        (m_data_collection_current_node != RoguelikeMap::INIT_INDEX &&
         m_data_collection_current_node < m_map.size() &&
         m_map.get_node_succs(m_data_collection_current_node).empty())) {
        generate_data_collection_map();
    }

    ProcessTask(*this, { "RoguelikeRouting-MoveRightToPeek" }).run();

    const cv::Mat image = ctrler()->get_image();
    MultiMatcher node_analyzer(image);
    node_analyzer.set_task_info(m_config->get_theme() + "@RoguelikeRoutingNodeAnalyze");
    if (node_analyzer.analyze()) {
        auto match_results = node_analyzer.get_result();
        sort_by_vertical_(match_results);
        m_left_most_column_x_in_view = match_results.front().rect.x;
    }

    const std::string image_path = RoguelikeDataCollector.save_image(image, "route_decision");
    const bool planned_new_floor = m_data_collection_current_node == RoguelikeMap::INIT_INDEX;

    std::vector<DataCollectionRouteScore> candidates;
    auto append_candidate = [&](size_t next_node) {
        const std::optional<size_t> previous_node =
            m_data_collection_current_node != RoguelikeMap::INIT_INDEX &&
                    m_data_collection_current_node < m_map.size()
                ? std::optional<size_t> { m_data_collection_current_node }
                : std::nullopt;
        auto score = score_data_collection_path(next_node, previous_node);
        score.first_node = next_node;
        score.first_type_priority = data_collection_type_priority(m_map.get_node_type(next_node));
        candidates.emplace_back(std::move(score));
    };

    if (m_data_collection_current_node != RoguelikeMap::INIT_INDEX &&
        m_data_collection_current_node < m_map.size()) {
        for (size_t next_node : m_map.get_node_succs(m_data_collection_current_node)) {
            append_candidate(next_node);
        }
    }
    else {
        for (size_t next_node : m_map.get_node_succs(RoguelikeMap::INIT_INDEX)) {
            append_candidate(next_node);
        }
    }

    auto best_it = std::ranges::max_element(candidates, data_collection_score_less);

    const bool has_remaining_encounter_route = std::ranges::any_of(candidates, [](const auto& candidate) {
        return candidate.valid && candidate.encounter_count > 0;
    });
    if (m_data_collection_floor == 2 && !has_remaining_encounter_route) {
        auto details =
            build_data_collection_route_details(candidates, std::nullopt, "second_floor_no_remaining_encounter");
        details["floor"] = m_data_collection_floor;
        details["planned_new_floor"] = planned_new_floor;
        if (!image_path.empty()) {
            details["image"] = image_path;
        }
        auto abandon_details = details;
        RoguelikeDataCollector.log_event("route_exit", std::move(details));
        RoguelikeDataCollector.set_pending_abandon_reason(
            "second_floor_no_remaining_encounter",
            std::move(abandon_details));
        callback(
            AsstMsg::TaskChainExtraInfo,
            json::object {
                { "what", "RoguelikeRouteExit" },
                { "message", "界园数据收集: 山水阁后续路线没有不期而遇，提前退出本局" },
            });
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-ExitThenAbandon");
        reset_in_run_variables();
        return;
    }

    if (best_it == candidates.end() || !best_it->valid) {
        auto details = build_data_collection_route_details(candidates, std::nullopt, "no_viable_route");
        if (!image_path.empty()) {
            details["image"] = image_path;
        }
        RoguelikeDataCollector.log_event("route_decision", std::move(details));
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@Roguelike@Stages_dataCollectionFallback");
        reset_in_run_variables();
        return;
    }

    const size_t current_node = m_data_collection_current_node;
    const size_t next_node = best_it->first_node;
    const bool selected_uses_vertical_edge = is_data_collection_vertical_edge(current_node, next_node);
    auto details = build_data_collection_route_details(candidates, next_node, "");
    details["selected_uses_vertical_edge"] = selected_uses_vertical_edge;
    details["vertical_edge_used_before"] = m_data_collection_vertical_edge_used;
    if (selected_uses_vertical_edge) {
        m_data_collection_vertical_edge_used = true;
    }
    details["vertical_edge_used_after"] = m_data_collection_vertical_edge_used;
    m_data_collection_current_node = next_node;
    if (!image_path.empty()) {
        details["image"] = image_path;
    }
    const cv::Mat route_graph = build_data_collection_graph_image(best_it->path, next_node);
    if (!route_graph.empty()) {
        const std::string route_graph_path = RoguelikeDataCollector.save_image(route_graph, "selected_route_graph");
        if (!route_graph_path.empty()) {
            details["route_graph"] = route_graph_path;
        }
    }
    RoguelikeDataCollector.log_event("route_decision", std::move(details));

    if (planned_new_floor) {
        auto graph_message = build_data_collection_adjacency_list();
        Log.info(graph_message);
        callback(
            AsstMsg::TaskChainExtraInfo,
            json::object {
                { "what", "RoguelikeRoutePlanned" },
                { "message", std::move(graph_message) },
            });

        std::string route_message = "界园数据收集路线: ";
        for (size_t i = 0; i < best_it->path.size(); ++i) {
            if (i != 0) {
                route_message += " -> ";
            }
            route_message += data_collection_display_name(m_map.get_node_type(best_it->path[i]));
        }
        callback(
            AsstMsg::TaskChainExtraInfo,
            json::object {
                { "what", "RoguelikeRoutePlanned" },
                { "message", std::move(route_message) },
            });
    }

    const size_t leftmost_visible_column =
        planned_new_floor ? RoguelikeMap::INIT_INDEX + 1 : m_map.get_node_column(current_node);
    const int next_node_x =
        m_left_most_column_x_in_view +
        static_cast<int>(m_map.get_node_column(next_node) - leftmost_visible_column) * m_column_offset;
    const int next_node_y = m_map.get_node_y(next_node);
    Point next_node_center = Point(next_node_x + m_node_width / 2, next_node_y + m_node_height / 2);
    ctrler()->click(next_node_center);
    sleep(200);

    const RoguelikeNodeType next_node_type = m_map.get_node_type(next_node);
    RoguelikeDataCollector.set_record_map_encounters(next_node_type == RoguelikeNodeType::Encounter);

    switch (next_node_type) {
    case RoguelikeNodeType::Encounter:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageEncounterEnter");
        break;
    case RoguelikeNodeType::Guidance:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageGuidanceEnter");
        break;
    case RoguelikeNodeType::Boons:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageBoonsEnter");
        break;
    case RoguelikeNodeType::SafeHouse:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageSafeHouseEnter");
        break;
    case RoguelikeNodeType::RogueTrader:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageTraderEnterThenLeave");
        break;
    case RoguelikeNodeType::BoskyPassage:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageBoskyPassageEnter");
        break;
    case RoguelikeNodeType::LostAndFound:
    case RoguelikeNodeType::Scout:
    case RoguelikeNodeType::Prophecy:
    case RoguelikeNodeType::FaceOff:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageEncounterEnter");
        break;
    case RoguelikeNodeType::CombatOps:
    case RoguelikeNodeType::EmergencyOps:
    case RoguelikeNodeType::DreadfulFoe:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@RoguelikeRoutingAction-StageCombatOpsEnter");
        break;
    default:
        Task.set_task_base("RoguelikeRoutingAction", "JieGarden@Roguelike@Stages_default");
        break;
    }

    m_selected_column = 0;
    m_selected_x = 0;
}

asst::RoguelikeRoutingTaskPlugin::DataCollectionRouteScore
    asst::RoguelikeRoutingTaskPlugin::score_data_collection_path(
        size_t node,
        std::optional<size_t> previous_node,
        std::vector<size_t> path,
        bool vertical_edge_used_in_path) const
{
    DataCollectionRouteScore score;
    if (previous_node && is_data_collection_vertical_edge(*previous_node, node)) {
        if (m_data_collection_vertical_edge_used || vertical_edge_used_in_path) {
            score.reject_reason = "vertical_edge_unavailable";
            return score;
        }
        vertical_edge_used_in_path = true;
        score.vertical_edge_count = 1;
    }
    if (m_data_collection_current_node != RoguelikeMap::INIT_INDEX && node == m_data_collection_current_node) {
        score.reject_reason = "cycle_to_current_node";
        return score;
    }
    if (std::ranges::find(path, node) != path.end()) {
        score.reject_reason = "cycle";
        return score;
    }
    if (m_map.get_node_visited(node)) {
        score.reject_reason = "visited_node";
        return score;
    }

    const RoguelikeNodeType type = m_map.get_node_type(node);
    path.emplace_back(node);
    score.path = path;
    score.path_length = static_cast<int>(path.size());

    if (is_data_collection_rejected_type(type)) {
        score.reject_reason = "rejected_node_type:" + type2name(type);
        return score;
    }

    score.valid = true;
    score.encounter_count = type == RoguelikeNodeType::Encounter ? 1 : 0;
    score.combat_count = is_data_collection_combat_type(type) ? 1 : 0;
    score.non_combat_count = score.combat_count ? 0 : 1;
    if (type == RoguelikeNodeType::Encounter) {
        score.first_encounter_step = 0;
    }

    const auto succs = m_map.get_node_succs(node);
    if (succs.empty()) {
        if (is_data_collection_combat_type(type)) {
            score.valid = false;
            score.reject_reason = "terminal_combat_node";
        }
        return score;
    }

    std::vector<DataCollectionRouteScore> child_scores;
    for (size_t succ : succs) {
        child_scores.emplace_back(score_data_collection_path(succ, node, path, vertical_edge_used_in_path));
    }

    auto best_child = std::ranges::max_element(child_scores, data_collection_score_less);

    if (best_child != child_scores.end() && best_child->valid) {
        score.encounter_count += best_child->encounter_count;
        score.combat_count += best_child->combat_count;
        score.non_combat_count += best_child->non_combat_count;
        score.vertical_edge_count += best_child->vertical_edge_count;
        score.path_length = best_child->path_length;
        if (type != RoguelikeNodeType::Encounter &&
            best_child->first_encounter_step != std::numeric_limits<int>::max()) {
            score.first_encounter_step = best_child->first_encounter_step + 1;
        }
        score.path = std::move(best_child->path);
    }

    return score;
}

bool asst::RoguelikeRoutingTaskPlugin::data_collection_score_less(
    const DataCollectionRouteScore& lhs,
    const DataCollectionRouteScore& rhs)
{
    if (lhs.valid != rhs.valid) {
        return !lhs.valid && rhs.valid;
    }
    if (lhs.encounter_count != rhs.encounter_count) {
        return lhs.encounter_count < rhs.encounter_count;
    }
    if (lhs.path_length != rhs.path_length) {
        return lhs.path_length > rhs.path_length;
    }
    if (lhs.non_combat_count != rhs.non_combat_count) {
        return lhs.non_combat_count < rhs.non_combat_count;
    }
    if (lhs.first_encounter_step != rhs.first_encounter_step) {
        return lhs.first_encounter_step > rhs.first_encounter_step;
    }
    if (lhs.first_type_priority != rhs.first_type_priority) {
        return lhs.first_type_priority < rhs.first_type_priority;
    }
    if (lhs.combat_count != rhs.combat_count) {
        return lhs.combat_count > rhs.combat_count;
    }
    return lhs.path > rhs.path;
}

json::array asst::RoguelikeRoutingTaskPlugin::build_data_collection_edge_score_json() const
{
    json::array edge_scores;
    for (const DataCollectionEdgeScore& edge_score : m_data_collection_edge_scores) {
        edge_scores.emplace_back(json::object {
            { "source", static_cast<int>(edge_score.source) },
            { "target", static_cast<int>(edge_score.target) },
            { "score", edge_score.score },
            { "support_ratio", edge_score.support_ratio },
            { "endpoint_score", edge_score.endpoint_score },
            { "continuity", edge_score.continuity },
            { "end_error", edge_score.end_error },
            { "path_length", edge_score.path_length },
            { "accepted", edge_score.accepted },
            { "reject_reason", edge_score.reject_reason },
        });
    }
    return edge_scores;
}

json::object asst::RoguelikeRoutingTaskPlugin::build_data_collection_route_details(
    const std::vector<DataCollectionRouteScore>& candidates,
    std::optional<size_t> chosen,
    std::string_view reject_reason) const
{
    json::array nodes;
    for (size_t i = 1; i < m_map.size(); ++i) {
        json::array succs;
        for (size_t succ : m_map.get_node_succs(i)) {
            succs.emplace_back(static_cast<int>(succ));
        }
        json::array vertical_succs;
        for (size_t succ : m_map.get_node_succs(i)) {
            if (is_data_collection_vertical_edge(i, succ)) {
                vertical_succs.emplace_back(static_cast<int>(succ));
            }
        }
        nodes.emplace_back(json::object {
            { "index", static_cast<int>(i) },
            { "type", type2name(m_map.get_node_type(i)) },
            { "column", static_cast<int>(m_map.get_node_column(i)) },
            { "y", m_map.get_node_y(i) },
            { "visited", m_map.get_node_visited(i) },
            { "successors", std::move(succs) },
            { "vertical_successors", std::move(vertical_succs) },
        });
    }

    json::array candidate_json;
    for (const auto& candidate : candidates) {
        json::array path;
        for (size_t node : candidate.path) {
            path.emplace_back(static_cast<int>(node));
        }
        candidate_json.emplace_back(json::object {
            { "first_node", static_cast<int>(candidate.first_node) },
            { "first_node_type", candidate.first_node < m_map.size() ? type2name(m_map.get_node_type(candidate.first_node)) : "Unknown" },
            { "valid", candidate.valid },
            { "encounter_count", candidate.encounter_count },
            { "combat_count", candidate.combat_count },
            { "non_combat_count", candidate.non_combat_count },
            { "path_length", candidate.path_length },
            { "vertical_edge_count", candidate.vertical_edge_count },
            { "first_encounter_step",
              candidate.first_encounter_step == std::numeric_limits<int>::max() ? -1 : candidate.first_encounter_step },
            { "first_type_priority", candidate.first_type_priority },
            { "path", std::move(path) },
            { "reject_reason", candidate.reject_reason },
        });
    }

    json::object details {
        { "reachable_nodes", std::move(nodes) },
        { "candidate_paths", std::move(candidate_json) },
        { "current_node", static_cast<int>(m_data_collection_current_node) },
        { "selected_node", chosen ? static_cast<int>(*chosen) : -1 },
        { "selected_type", chosen ? type2name(m_map.get_node_type(*chosen)) : "None" },
        { "vertical_edge_used", m_data_collection_vertical_edge_used },
        { "edge_score_matrix", build_data_collection_edge_score_json() },
        { "reject_reason", std::string(reject_reason) },
    };

    return details;
}

bool asst::RoguelikeRoutingTaskPlugin::is_data_collection_rejected_type(RoguelikeNodeType /*type*/)
{
    return false;
}

bool asst::RoguelikeRoutingTaskPlugin::is_data_collection_combat_type(RoguelikeNodeType type)
{
    return type == RoguelikeNodeType::CombatOps || type == RoguelikeNodeType::EmergencyOps ||
           type == RoguelikeNodeType::DreadfulFoe;
}

bool asst::RoguelikeRoutingTaskPlugin::is_data_collection_vertical_edge(size_t source, size_t target) const
{
    if (source == RoguelikeMap::INIT_INDEX || target == RoguelikeMap::INIT_INDEX || source >= m_map.size() ||
        target >= m_map.size()) {
        return false;
    }
    return m_map.get_node_column(source) == m_map.get_node_column(target);
}

int asst::RoguelikeRoutingTaskPlugin::data_collection_type_priority(RoguelikeNodeType type)
{
    switch (type) {
    case RoguelikeNodeType::Encounter:
        return 4;
    case RoguelikeNodeType::Boons:
        return 3;
    case RoguelikeNodeType::Guidance:
    case RoguelikeNodeType::SafeHouse:
    case RoguelikeNodeType::LostAndFound:
    case RoguelikeNodeType::Scout:
    case RoguelikeNodeType::Prophecy:
    case RoguelikeNodeType::FaceOff:
        return 3;
    case RoguelikeNodeType::RogueTrader:
        return 2;
    case RoguelikeNodeType::Unknown:
    case RoguelikeNodeType::BoskyPassage:
        return 1;
    default:
        return 0;
    }
}

std::string asst::RoguelikeRoutingTaskPlugin::data_collection_display_name(RoguelikeNodeType type)
{
    switch (type) {
    case RoguelikeNodeType::Init:
        return "起点";
    case RoguelikeNodeType::CombatOps:
        return "作战";
    case RoguelikeNodeType::EmergencyOps:
        return "紧急作战";
    case RoguelikeNodeType::DreadfulFoe:
        return "险路恶敌";
    case RoguelikeNodeType::Encounter:
        return "不期而遇";
    case RoguelikeNodeType::Guidance:
        return "指点迷津";
    case RoguelikeNodeType::Boons:
        return "得偿所愿";
    case RoguelikeNodeType::SafeHouse:
        return "安全的角落";
    case RoguelikeNodeType::RogueTrader:
        return "诡意行商";
    case RoguelikeNodeType::LostAndFound:
        return "失与得";
    case RoguelikeNodeType::Scout:
        return "先行一步";
    case RoguelikeNodeType::BoskyPassage:
        return "误入奇境";
    case RoguelikeNodeType::Prophecy:
        return "命运所指";
    case RoguelikeNodeType::FaceOff:
        return "狭路相逢";
    default:
        return type2name(type);
    }
}

std::string asst::RoguelikeRoutingTaskPlugin::build_data_collection_adjacency_list() const
{
    auto node_label = [&](size_t node) -> std::string {
        std::string label = std::to_string(node) + "(" + data_collection_display_name(m_map.get_node_type(node));
        if (node != RoguelikeMap::INIT_INDEX) {
            label += ",列" + std::to_string(m_map.get_node_column(node));
            label += ",y=" + std::to_string(m_map.get_node_y(node));
            if (m_map.get_node_visited(node)) {
                label += ",已访问";
            }
        }
        label += ")";
        return label;
    };

    std::string message =
        std::string("界园数据收集建图邻接表: 纵向通路") + (m_data_collection_vertical_edge_used ? "已使用" : "未使用");
    for (size_t node = 0; node < m_map.size(); ++node) {
        message += "\n";
        message += node_label(node);
        message += " -> ";

        const auto succs = m_map.get_node_succs(node);
        if (succs.empty()) {
            message += "[]";
            continue;
        }

        message += "[";
        for (size_t i = 0; i < succs.size(); ++i) {
            if (i != 0) {
                message += ", ";
            }
            message += node_label(succs[i]);
            if (is_data_collection_vertical_edge(node, succs[i])) {
                message += "(纵向)";
            }
        }
        message += "]";
    }
    return message;
}

void asst::RoguelikeRoutingTaskPlugin::update_selected_x()
{
    if (m_selected_column == RoguelikeMap::INIT_INDEX) {
        m_selected_x = m_origin_x - m_column_offset;
    }
    else if (m_selected_column == RoguelikeMap::INIT_INDEX + 1) {
        m_selected_x = m_origin_x;
    }
    else if (m_selected_column == m_map.get_num_columns() - 1) [[unlikely]] {
        m_selected_x = m_last_x;
    }
    else {
        m_selected_x = m_middle_x;
    }
}
