#include "RoguelikeCoppersTaskPlugin.h"

#include <fstream>
#include <format>
#include <map>
#include <mutex>

#include <meojson/json.hpp>

#include "Common/AsstTypes.h"
#include "Config/Roguelike/JieGarden/RoguelikeCoppersConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/Time.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/Matcher.h"
#include "Vision/Roguelike/JieGarden/RoguelikeCoppersAnalyzer.h"

namespace
{
constexpr int CollectiblePoolTestSwipeToLeftmostTimes = 5;
constexpr int CollectiblePoolTestSwipeToRightmostTimes = 5;

const std::vector<std::string> CollectiblePoolTestPopupTasks = {
    "JieGarden@Roguelike@CoppersCollectiblePoolTestCloseCollectionContinue",
    "JieGarden@Roguelike@CoppersCollectiblePoolTestCloseCollectionClose",
    "JieGarden@Roguelike@CoppersCollectiblePoolTestAllCollectiblesOwned",
};

std::string_view debug_image_category_name(asst::CoppersDebugImageCategory category)
{
    switch (category) {
    case asst::CoppersDebugImageCategory::CoppersView:
        return "coppers_view";
    case asst::CoppersDebugImageCategory::Popup:
        return "popups";
    case asst::CoppersDebugImageCategory::General:
    default:
        return "general";
    }
}

std::filesystem::path debug_image_category_dir(asst::CoppersDebugImageCategory category)
{
    return asst::utils::path("debug") / "roguelike" / "coppers" / std::string(debug_image_category_name(category));
}

std::filesystem::path debug_image_log_path()
{
    return asst::UserDir.get() / asst::utils::path("debug") / "roguelike" / "coppers" / "screenshots.jsonl";
}
}

bool asst::RoguelikeCoppersTaskPlugin::load_params([[maybe_unused]] const json::value& params)
{
    const std::string& theme = m_config->get_theme();

    // 本插件仅用于界园肉鸽
    if (theme != RoguelikeTheme::JieGarden) {
        return false;
    }

    return true;
}

bool asst::RoguelikeCoppersTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    // 只处理子任务开始消息且为ProcessTask类型
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    const std::string& theme = m_config->get_theme();
    if (theme != RoguelikeTheme::JieGarden) {
        return false;
    }

    const std::string task_name = details.get("details", "task", "");

    // 根据任务名称确定运行模式
    if (task_name.ends_with("Roguelike@CoppersTakeFlag")) {
        m_run_mode = CoppersTaskRunMode::EXCHANGE;
        Log.info(__FUNCTION__, "| plugin activated for EXCHANGE mode");
    }
    else if (task_name.ends_with("Roguelike@GetDropSwitch")) {
        m_run_mode = CoppersTaskRunMode::PICKUP;
        Log.info(__FUNCTION__, "| plugin activated for PICKUP mode");
    }
    else if (
        task_name.ends_with("Roguelike@CollectiblePoolTest") &&
        m_config->get_mode() == RoguelikeMode::DeepExplorationCollectiblePoolTest) {
        m_run_mode = CoppersTaskRunMode::COLLECTIBLE_POOL_TEST;
        Log.info(__FUNCTION__, "| plugin activated for COLLECTIBLE_POOL_TEST mode");
    }
    else {
        return false; // 不支持的任务类型
    }

    // 投资模式下需要额外检查是否启用购物功能
    const auto mode = m_config->get_mode();
    if (mode == RoguelikeMode::Investment) {
        return m_config->get_invest_with_more_score();
    }

    return true;
}

// 重置运行时变量，为新一轮执行做准备
void asst::RoguelikeCoppersTaskPlugin::reset_in_run_variables()
{
    m_copper_list.clear();
    m_new_copper = RoguelikeCopper();
    m_pending_copper.clear();

    // 重置坐标计算相关变量
    m_col = 0;
    m_origin_x = 0;
    m_x = 0;
    m_last_x = 0;
    m_y = 0;
    m_row_offset = 0;
}

// 执行插件主要逻辑，根据当前运行模式处理通宝拾取或交换
bool asst::RoguelikeCoppersTaskPlugin::_run()
{
    LogTraceFunction;

    CopperTaskResult result = CopperTaskResult::FAILED;
    // 根据运行模式调用相应的处理函数
    switch (m_run_mode) {
    case CoppersTaskRunMode::PICKUP:
        result = handle_pickup_mode();
        break;
    case CoppersTaskRunMode::EXCHANGE:
        result = handle_exchange_mode();
        break;
    case CoppersTaskRunMode::COLLECTIBLE_POOL_TEST:
        result = handle_collectible_pool_test_mode();
        break;
    }

    // 执行完成后重置变量
    reset_in_run_variables();

    // 将结果转换为bool：SUCCESS和SKIPPED都返回true，FAILED返回false
    return (result == CopperTaskResult::SUCCESS || result == CopperTaskResult::SKIPPED);
}

// 处理掉落通宝的拾取：识别交换按钮，ROI偏移来识别通宝名称
asst::CopperTaskResult asst::RoguelikeCoppersTaskPlugin::handle_pickup_mode()
{
    LogTraceFunction;

    const auto& image = ctrler()->get_image();

#ifdef ASST_DEBUG
    cv::Mat image_draw = image.clone();
#endif

    // 使用Analyzer识别拾取界面中的通宝
    RoguelikeCoppersAnalyzer analyzer(image);
    if (!analyzer.analyze_pickup()) {
        LogError << __FUNCTION__ << "| no coppers recognized for pickup";
        return CopperTaskResult::FAILED;
    }

    const auto& detections = analyzer.get_detections();
    if (detections.empty()) {
        LogError << __FUNCTION__ << "| no detections returned for pickup mode";
        return CopperTaskResult::FAILED;
    }

    asst::Point click_point_fallback(0, 0);

    // 遍历每个检测到的通宝，创建通宝对象
    for (size_t i = 0; i < detections.size(); ++i) {
        const auto& detection = detections[i];

        LogInfo << __FUNCTION__ << "| found copper:" << detection.name << "at position" << i;

        // 根据识别到的名称创建通宝对象
        auto copper_opt = create_copper_from_name(detection.name, 1, static_cast<int>(i), false, detection.name_roi);
        if (!copper_opt) {
            LogError << __FUNCTION__ << "| failed to create copper at position" << i << " name:" << detection.name;

            click_point_fallback = detection.click_point;
            continue;
        }

        // 将通宝及其点击坐标保存到待选列表
        m_pending_copper.emplace_back(std::move(*copper_opt), detection.click_point);

#ifdef ASST_DEBUG
        // 调试模式下在图像上绘制检测结果（绿色表示拾取模式）
        draw_detection_debug(image_draw, detection, cv::Scalar(0, 255, 0));
#endif
    }

    if (m_pending_copper.empty()) {
        LogError << __FUNCTION__ << "| no valid coppers found for pickup";
        // 如果没有有效通宝，尝试点击最后一个检测到的名称错误的通宝位置作为回退
        if (click_point_fallback.x != 0 && click_point_fallback.y != 0) {
            LogWarn << __FUNCTION__ << "| clicking fallback point at (" << click_point_fallback.x << ","
                    << click_point_fallback.y << ")";
            ctrler()->click(click_point_fallback);
            return CopperTaskResult::SKIPPED;
        }
        LogError << __FUNCTION__ << "| no coppers recognized to fallback";
        return CopperTaskResult::FAILED;
    }

    // 从待选通宝中选择拾取优先级最高的
    auto max_pickup_it =
        std::max_element(m_pending_copper.begin(), m_pending_copper.end(), [](const auto& a, const auto& b) {
            return a.first.pickup_priority < b.first.pickup_priority;
        });

    LogInfo << __FUNCTION__ << "| selecting copper: " << max_pickup_it->first.name
            << " with priority:" << max_pickup_it->first.pickup_priority;

    // 点击选择的最优通宝
    ctrler()->click(max_pickup_it->second);

#ifdef ASST_DEBUG
    // 保存调试图像
    save_debug_image(image_draw, "pickup");
#endif

    return CopperTaskResult::SUCCESS;
}

// 交换通宝：先识别通宝类型，然后ROI偏移来OCR通宝名称和是否已投出
// 图片示意请看 文档(docs\zh-cn\protocol\integrated-strategy-schema.md) 或 #13835
asst::CopperTaskResult asst::RoguelikeCoppersTaskPlugin::handle_exchange_mode()
{
    if (m_config->get_mode() == RoguelikeMode::DataCollection) {
        Log.info(__FUNCTION__, "| data collection mode, abandoning copper exchange");
        RoguelikeDataCollector.log_event("copper_exchange_skipped", json::object { { "reason", "data_collection" } });
        return CopperTaskResult::SKIPPED;
    }

    // 确保列表滑动到最左边（有时候进入界面不在最左边）
    bool ret = swipe_copper_list_to_leftmost(2);

    // =================================================
    // 第一步：识别左侧新拾取的通宝（col == 0）
    // =================================================
    auto image = ctrler()->get_image();

#ifdef ASST_DEBUG
    cv::Mat image_draw = image.clone();
#endif

    // 分析左侧新拾取通宝列（不检测已投出状态）
    RoguelikeCoppersAnalyzer left_analyzer(image);
    if (!left_analyzer.analyze_column(RoguelikeCoppersAnalyzer::ColumnRole::Leftmost, false)) {
        LogInfo << __FUNCTION__ << "| left column copper recognition failed, skipping exchange";
        // 直接进入next，放弃交换
        return CopperTaskResult::SKIPPED;
    }

    const auto& left_detections = left_analyzer.get_detections();
    if (left_detections.empty()) {
        LogInfo << __FUNCTION__ << "| left column is empty, skipping exchange";
        // 直接进入next，放弃交换
        return CopperTaskResult::SKIPPED;
    }

    // 处理左侧列的检测结果
    if (left_detections.size() != 1) {
        LogWarn << __FUNCTION__ << "| expected exactly one copper in left column, got" << left_detections.size()
                << ", skipping exchange";

#ifdef ASST_DEBUG
        for (const auto& detection : left_detections) {
            draw_detection_debug(image_draw, detection, cv::Scalar(0, 0, 255));
        }
        save_debug_image(image_draw, "left_column_unexpected_count");
#endif
        // 直接进入next，放弃交换
        return CopperTaskResult::SKIPPED;
    }

    {
        const auto& detection = left_detections[0];

        LogInfo << __FUNCTION__ << "| found copper:" << detection.name << "at (0,0)";

#ifdef ASST_DEBUG
        // 调试模式下绘制检测结果（红色表示新拾取的通宝）
        draw_detection_debug(image_draw, detection, cv::Scalar(0, 0, 255));
#endif

        // 创建新拾取的通宝对象
        auto copper_opt = create_copper_from_name(detection.name, 0, 0, false, detection.name_roi);
        if (!copper_opt) {
            LogError << __FUNCTION__ << "| failed to create copper at (0,0)";
            // 直接进入next，放弃交换
            return CopperTaskResult::SKIPPED;
        }

        auto copper = std::move(*copper_opt);
        // 根据模板确定通宝类型
        copper.type = RoguelikeCoppersConfig::get_type_from_template(detection.templ_name);

        m_new_copper = std::move(copper);

#ifdef ASST_DEBUG
        save_debug_image(image_draw, "new_copper");
#endif
    }

    // =================================================
    // 第二步：扫描所有现有通宝列
    // =================================================
    auto image_last = ctrler()->get_image();

    // 预分配10个通宝的空间
    m_copper_list.reserve(10);

    // 总不可能超过20列(60个)通宝吧
    for (int col = 1; col <= 20; ++col) {
        // 检查是否是最后一列：尝试识别右侧通宝是否滑动到中间位置
        bool is_last_col =
            col == 1
                ? false
                : !ProcessTask(*this, { "JieGarden@Roguelike@CoppersAnalyzer-TypeSelected" }).set_retry_times(2).run();

        // 如果不是最后一列，点击右侧上方的通宝作为滑动成功的标志
        if (!is_last_col) {
            ret &= ProcessTask(*this, { "JieGarden@Roguelike@CoppersListSwipeFlagClick" }).run();
        }
        else {
            // 最后一列时先滑动到最右侧再识别
            swipe_copper_list_to_rightmost(2);
            // 点击中间的通宝避免干扰右侧识别
            ret &= ProcessTask(*this, { "JieGarden@Roguelike@CoppersListSwipeRightMostClick" }).run();
        }

        // 获取新图像并检查是否滑动成功
        image = ctrler()->get_image();
        if (image_last.data == image.data) {
            LogError << __FUNCTION__ << "| image not updated after swipe at column" << col;
            break;
        }
        image_last = image;

#ifdef ASST_DEBUG
        image_draw = image.clone();
#endif

        // 根据是否是最后一列选择识别中间列或右侧列
        RoguelikeCoppersAnalyzer::ColumnRole role = is_last_col ? RoguelikeCoppersAnalyzer::ColumnRole::Rightmost
                                                                : RoguelikeCoppersAnalyzer::ColumnRole::Middle;
        RoguelikeCoppersAnalyzer column_analyzer(image);
        // 分析当前列，检测已投出状态
        if (!column_analyzer.analyze_column(role, true)) {
            LogError << __FUNCTION__ << "| no coppers recognized in column" << col;
            continue;
        }

        const auto& detections = column_analyzer.get_detections();
        if (detections.empty()) {
            LogError << __FUNCTION__ << "| no coppers recognized in column" << col;
            continue;
        }

        // 获取列的度量信息用于坐标计算
        const auto& metrics = column_analyzer.get_column_metrics();
        update_column_coordinates(metrics, col, is_last_col);

        // 处理当前列的所有通宝
        for (size_t row = 0; row < detections.size(); ++row) {
            const auto& detection = detections[row];

            LogInfo << __FUNCTION__ << "| found copper:" << detection.name << "at (" << col << "," << row
                    << ") is_cast:" << detection.is_cast;

#ifdef ASST_DEBUG
            draw_detection_debug(image_draw, detection, cv::Scalar(0, 0, 255));
#endif

            // 从OCR结果创建通宝对象
            auto copper_opt = create_copper_from_name(
                detection.name,
                col,
                static_cast<int>(row + 1),
                detection.is_cast,
                detection.name_roi);
            if (!copper_opt) {
                LogError << __FUNCTION__ << "| failed to create copper at (" << col << "," << row << ")";
                continue;
            }

            auto copper = std::move(*copper_opt);
            copper.type = RoguelikeCoppersConfig::get_type_from_template(detection.templ_name);

            // 添加到现有通宝列表
            m_copper_list.emplace_back(std::move(copper));
        }

        // 如果不是最后一列，向右滑动一列继续扫描
        if (!is_last_col) {
            swipe_copper_list_right(1);
        }

#ifdef ASST_DEBUG
        save_debug_image(image_draw, "exchange");
#endif

        // 如果是最后一列，记录总列数并结束扫描
        if (is_last_col) {
            m_col = col;
            LogInfo << __FUNCTION__ << "| total columns detected:" << m_col;
            break;
        }
    }

    // 检查是否找到任何现有通宝
    if (m_copper_list.empty()) {
        LogError << __FUNCTION__ << "| no coppers found in list for comparison";
        // 直接进入next，放弃交换
        return CopperTaskResult::SKIPPED;
    }

    // =================================================
    // 第三步：决定是否交换并执行
    // =================================================

    // 找到现有通宝中丢弃优先级最高的（最不重要的）
    auto worst_it = std::max_element(m_copper_list.begin(), m_copper_list.end(), [](const auto& a, const auto& b) {
        return a.get_copper_discard_priority() < b.get_copper_discard_priority();
    });

    // 如果新通宝的丢弃优先级低于最差现有通宝，则放弃交换
    if (worst_it->get_copper_discard_priority() < m_new_copper.get_copper_discard_priority()) {
        LogInfo << __FUNCTION__ << "new copper (" << m_new_copper.name
                << ") is worse than all existing coppers, abandoning exchange";
        return CopperTaskResult::SKIPPED;
    }

    // 执行交换
    LogInfo << __FUNCTION__ << "| exchanging copper:" << worst_it->name << "(" << worst_it->col << "," << worst_it->row
            << ") -> " << m_new_copper.name;

    // 发送通宝替换信息到 WPF
    auto copper_info = basic_info_with_what("RoguelikeCoppersExchangeInfo");
    copper_info["details"]["to_discard"] = std::format("{} ({},{})", worst_it->name, worst_it->col, worst_it->row);
    copper_info["details"]["to_pickup"] = m_new_copper.name;
    callback(AsstMsg::SubTaskExtraInfo, copper_info);

    // 点击要替换的通宝
    click_copper_at_position(worst_it->col, worst_it->row);

    // 执行确认交换任务
    ret &= ProcessTask(*this, { "JieGarden@Roguelike@CoppersTakeConfirm" }).run();

    return ret ? CopperTaskResult::SUCCESS : CopperTaskResult::FAILED;
}

asst::CopperTaskResult asst::RoguelikeCoppersTaskPlugin::handle_collectible_pool_test_mode()
{
    LogTraceFunction;

    bool ret = ProcessTask(*this, { "JieGarden@Roguelike@CoppersOpenBox" }).set_retry_times(3).run();
    if (!ret) {
        Log.error(__FUNCTION__, "| failed to open coppers box for collectible pool test");
        return CopperTaskResult::FAILED;
    }

    int recast_times = 0;
    while (true) {
        sleep(500);
        ret &= swipe_copper_list_to_leftmost(CollectiblePoolTestSwipeToLeftmostTimes);
        sleep(300);
        save_debug_image(
            ctrler()->get_image(),
            std::format("collectible_pool_test_{}_leftmost", recast_times),
            false,
            CoppersDebugImageCategory::CoppersView);

        ret &= swipe_copper_list_to_rightmost(CollectiblePoolTestSwipeToRightmostTimes);
        sleep(300);
        save_debug_image(
            ctrler()->get_image(),
            std::format("collectible_pool_test_{}_rightmost", recast_times),
            false,
            CoppersDebugImageCategory::CoppersView);

        if (!run_first_matched_task({ "JieGarden@Roguelike@CoppersRecastCollectiblePoolTest" })) {
            Log.info(__FUNCTION__, "| recast unavailable, collectible pool test finished");
            ret &= ProcessTask(*this, { "JieGarden@Roguelike@CoppersCloseBox" }).set_retry_times(3).run();
            if (!ret) {
                Log.error(__FUNCTION__, "| failed to close coppers box after collectible pool test");
                return CopperTaskResult::FAILED;
            }
            return CopperTaskResult::SUCCESS;
        }

        ret &= ProcessTask(*this, { "JieGarden@Roguelike@CoppersRecastSecondConfirmCollectiblePoolTest" })
                   .set_retry_times(3)
                   .run();

        ret &= close_collectible_pool_test_recast_popups();
        ret &= run_first_matched_task_with_pre_click_snapshot(
            { "JieGarden@Roguelike@CoppersRecastResultCollectiblePoolTest" },
            std::format("collectible_pool_test_before_close_result_{}", recast_times),
            20,
            CoppersDebugImageCategory::Popup);

        if (!ret) {
            Log.error(__FUNCTION__, "| failed to finish recast flow for collectible pool test");
            return CopperTaskResult::FAILED;
        }

        ++recast_times;
    }
}

bool asst::RoguelikeCoppersTaskPlugin::run_first_matched_task(const std::vector<std::string>& task_names) const
{
    const cv::Mat image = ctrler()->get_image();
    PipelineAnalyzer analyzer(image, Rect(), m_inst);
    analyzer.set_tasks(task_names);
    auto result = analyzer.analyze();
    if (!result || !result->task_ptr) {
        return false;
    }

    return ProcessTask(*this, { result->task_ptr->name }).set_reusable_image(image).set_retry_times(0).run();
}

bool asst::RoguelikeCoppersTaskPlugin::run_first_matched_task_with_pre_click_snapshot(
    const std::vector<std::string>& task_names,
    const std::string& snapshot_suffix,
    int retry_times,
    CoppersDebugImageCategory category) const
{
    for (int retry = 0; retry <= retry_times; ++retry) {
        const cv::Mat image = ctrler()->get_image();
        PipelineAnalyzer analyzer(image, Rect(), m_inst);
        analyzer.set_tasks(task_names);
        auto result = analyzer.analyze();
        if (!result || !result->task_ptr) {
            sleep(500);
            continue;
        }

        save_debug_image(image, std::format("{}_{}", snapshot_suffix, result->task_ptr->name), false, category);
        return ProcessTask(*this, { result->task_ptr->name }).set_reusable_image(image).set_retry_times(0).run();
    }

    return false;
}

bool asst::RoguelikeCoppersTaskPlugin::close_collectible_pool_test_recast_popups() const
{
    bool closed = false;

    int close_times = 0;
    while (true) {
        if (!run_first_matched_task_with_pre_click_snapshot(
                CollectiblePoolTestPopupTasks,
                std::format("collectible_pool_test_before_close_popup_{}", close_times))) {
            break;
        }

        closed = true;
        ++close_times;
        sleep(500);
    }

    Log.info(__FUNCTION__, "| collectible pool test recast popup close:", closed);
    return closed;
}

// 滑动通宝列表指定次数
bool asst::RoguelikeCoppersTaskPlugin::swipe_copper_list(int times, bool to_left) const
{
    const int ERROR_THRESHOLD = 50; // 误差阈值，超过则进行校正滑动
    bool ret = true;
    for (int i = 0; i < times; ++i) {
        std::string task_name = to_left ? "JieGarden@Roguelike@CoppersListSlowlySwipeToTheLeft"
                                        : "JieGarden@Roguelike@CoppersListSlowlySwipeToTheRight";

        ret &= ProcessTask(*this, { task_name }).run();

        // 识别滑动效果，误差较大时额外滑动一次进行校准
        Matcher matcher(ctrler()->get_image());
        matcher.set_task_info("JieGarden@Roguelike@CoppersListSwipeErrorRecognition");
        if (matcher.analyze()) {
            int cur_x = matcher.get_result().rect.x;
            // m_origin_x 经典值: 572
            // cur_x 硬限制: [400,631]
            if (abs(m_origin_x - cur_x) >= ERROR_THRESHOLD) {
                Point origin_point = Point(m_origin_x, m_y);
                Point cur_point = Point(cur_x, m_y);
                auto swipe_task = Task.get("SlowlySwipeToTheRight");
                ret &= ctrler()->swipe(
                    cur_point,
                    origin_point,
                    swipe_task->special_params.empty() ? 0 : swipe_task->special_params.at(0),
                    (swipe_task->special_params.size() < 2) ? false : swipe_task->special_params.at(1),
                    (swipe_task->special_params.size() < 3) ? 1 : swipe_task->special_params.at(2),
                    (swipe_task->special_params.size() < 4) ? 1 : swipe_task->special_params.at(3));
                Log.debug(
                    __FUNCTION__,
                    std::format(
                        "| correcting swipe error: origin_x = {}, cur_x = {}, diff = {}",
                        m_origin_x,
                        cur_x,
                        abs(m_origin_x - cur_x)));
                ret &= sleep(100);
            }
        }
    }
    return ret;
}

// 向左滑动通宝列表指定次数
bool asst::RoguelikeCoppersTaskPlugin::swipe_copper_list_left(int times) const
{
    return swipe_copper_list(times, true);
}

// 向右滑动通宝列表指定次数
bool asst::RoguelikeCoppersTaskPlugin::swipe_copper_list_right(int times) const
{
    return swipe_copper_list(times, false);
}

bool asst::RoguelikeCoppersTaskPlugin::swipe_copper_list_to_leftmost(int times) const
{
    bool ret = true;
    for (int i = 0; i < times; ++i) {
        std::string task_name = "JieGarden@Roguelike@CoppersListSwipeToTheLeft";
        ret &= ProcessTask(*this, { task_name }).run();
    }
    return ret;
}

bool asst::RoguelikeCoppersTaskPlugin::swipe_copper_list_to_rightmost(int times) const
{
    bool ret = true;
    for (int i = 0; i < times; ++i) {
        std::string task_name = "JieGarden@Roguelike@CoppersListSwipeToTheRight";
        ret &= ProcessTask(*this, { task_name }).run();
    }
    return ret;
}

// 根据行列位置计算并点击指定位置的通宝
void asst::RoguelikeCoppersTaskPlugin::click_copper_at_position(int col, int row) const
{
    // 根据列数选择X坐标：最后一列使用last_x，其他列使用m_x
    int x = col == m_col ? m_last_x : m_x;
    // 计算Y坐标：基于行偏移量
    Point click_point(x, m_y + (row - 1) * m_row_offset);

    Log.debug(
        __FUNCTION__,
        std::format(
            "| clicking copper at ({},{}) -> point ({},{},{},{})",
            col,
            row,
            click_point.x,
            m_y,
            (row - 1),
            m_row_offset));

    // 先滑动回最左边
    swipe_copper_list_to_leftmost(m_col + 1);
    sleep(300);

    // 再滑动到目标列
    swipe_copper_list_right(col - 1);

    // 执行点击
    ctrler()->click(click_point);
    sleep(300);
}

// 辅助函数：根据列类型更新坐标基准点
void asst::RoguelikeCoppersTaskPlugin::update_column_coordinates(
    const RoguelikeCoppersAnalyzer::ColumnMetrics& metrics,
    int col,
    bool is_last_col)
{
    // 根据列类型更新X坐标基准点：非最后一列使用m_x，最后一列记录last_x用于点击计算
    m_x = !is_last_col ? metrics.m_x : m_x;
    m_last_x = is_last_col ? metrics.m_x : m_last_x;

    // 更新统一的Y坐标基准点和行偏移量
    m_y = metrics.m_y;
    if (metrics.row_offset != 0) {
        m_row_offset = metrics.row_offset;
    }

    if (col == 1) {
        m_origin_x = m_x;
    }
}

// 根据识别到的名称创建通宝对象
std::optional<asst::RoguelikeCopper> asst::RoguelikeCoppersTaskPlugin::create_copper_from_name(
    const std::string& name,
    int col,
    int row,
    bool is_cast,
    const Rect& pos)
{
    // 从配置中查找通宝信息
    if (auto found_copper = RoguelikeCoppers.find_copper(m_config->get_theme(), name)) {
        auto copper = *found_copper;
        copper.col = col;
        copper.row = row;
        copper.is_cast = is_cast;
        Log.info(
            __FUNCTION__,
            std::format(
                "| created copper: {} priority: {}/{}/{}",
                name,
                copper.pickup_priority,
                copper.discard_priority,
                copper.cast_discard_priority));
        return copper;
    }

    Log.error(__FUNCTION__, std::format("| copper not found in config: {}", name));

    // 将识别到的错误的名称发送到 WPF 进行反馈
    auto copper_info = basic_info_with_what("RoguelikeCoppersRecognitionError");
    copper_info["details"]["recognized_name"] = name;
    callback(AsstMsg::SubTaskExtraInfo, copper_info);

    // 如果通宝不在配置中，保存调试图像
    try {
        cv::Mat screen = ctrler()->get_image();
        if (!screen.empty()) {
            cv::Mat screen_draw = screen.clone();
            cv::rectangle(screen_draw, cv::Rect(pos.x, pos.y, pos.width, pos.height), cv::Scalar(0, 0, 255), 2);

            save_debug_image(screen_draw, "unknown_draw", false);
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, std::format("| failed to save unknown copper debug image: {}", e.what()));
    }

    return std::nullopt;
}

// 调试绘制辅助函数：在图像上绘制检测结果
void asst::RoguelikeCoppersTaskPlugin::draw_detection_debug(
    cv::Mat& image,
    const RoguelikeCoppersAnalyzer::CopperDetection& detection,
    const cv::Scalar& color) const
{
    // 绘制名称识别区域的矩形框
    cv::rectangle(
        image,
        cv::Rect(detection.name_roi.x, detection.name_roi.y, detection.name_roi.width, detection.name_roi.height),
        color,
        2);
    // 在矩形框上方显示名称识别置信度
    cv::putText(
        image,
        std::format("score: {:.6f}", detection.name_score),
        cv::Point(detection.name_roi.x, std::max(0, detection.name_roi.y - 6)),
        cv::FONT_HERSHEY_SIMPLEX,
        0.45,
        color,
        1);

    // 如果有已投出状态识别，也绘制相应的区域和置信度
    if (detection.is_cast) {
        cv::rectangle(
            image,
            cv::Rect(detection.cast_roi.x, detection.cast_roi.y, detection.cast_roi.width, detection.cast_roi.height),
            color,
            2);
        cv::putText(
            image,
            std::format("cast_score: {:.6f}", detection.cast_score),
            cv::Point(detection.cast_roi.x, std::max(0, detection.cast_roi.y + detection.cast_roi.height + 16)),
            cv::FONT_HERSHEY_SIMPLEX,
            0.45,
            color,
            1);
    }
}

// 保存调试图像到文件
void asst::RoguelikeCoppersTaskPlugin::save_debug_image(
    const cv::Mat& image,
    const std::string& suffix,
    bool auto_clean,
    CoppersDebugImageCategory category) const
{
    try {
        if (image.empty()) {
            return;
        }

        const static std::vector<int> jpeg_params = { cv::IMWRITE_JPEG_QUALITY, 80, cv::IMWRITE_JPEG_OPTIMIZE, 1 };
        static std::map<std::filesystem::path, size_t> s_save_cnt;
        static std::mutex s_mutex;

        std::filesystem::path image_dir = debug_image_category_dir(category).lexically_normal();
        if (image_dir.is_relative()) {
            image_dir = UserDir.get() / image_dir;
        }

        if (auto_clean) {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto& cnt = s_save_cnt[image_dir];
            if (cnt == 0) {
                utils::filenum_ctrl(image_dir, Config.get_options().debug.max_debug_file_num);
            }
            cnt = (cnt + 1) % Config.get_options().debug.clean_files_freq;
        }

        const std::string stem = MAA_NS::format_now_for_filename();
        const std::filesystem::path image_path = image_dir / std::format("{}_{}.jpeg", stem, suffix);

        Log.info("Save roguelikeCoppers debug to", image_path);
        if (!MAA_NS::imwrite(image_path, image, jpeg_params)) {
            Log.error(__FUNCTION__, "| failed to write debug image:", image_path);
            return;
        }

        append_debug_image_record(image_path, suffix, category, image);
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save debug image:", e.what());
    }
}

void asst::RoguelikeCoppersTaskPlugin::append_debug_image_record(
    const std::filesystem::path& image_path,
    const std::string& suffix,
    CoppersDebugImageCategory category,
    const cv::Mat& image) const
{
    try {
        static std::mutex s_jsonl_mutex;

        const std::filesystem::path log_path = debug_image_log_path();
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);
        if (ec) {
            Log.error(__FUNCTION__, "| failed to create debug image log dir:", ec.message());
            return;
        }

        json::value record = json::object {
            { "timestamp", MAA_NS::format_now() },
            { "category", std::string(debug_image_category_name(category)) },
            { "suffix", suffix },
            { "path", MAA_NS::path_to_utf8_string(image_path) },
            { "width", image.cols },
            { "height", image.rows },
            { "format", "jpeg" },
            { "jpeg_quality", 80 },
        };

        std::lock_guard<std::mutex> lock(s_jsonl_mutex);
        std::ofstream ofs(log_path, std::ios::out | std::ios::app);
        if (!ofs.is_open()) {
            Log.error(__FUNCTION__, "| failed to open debug image jsonl:", log_path);
            return;
        }
        ofs << record.to_string() << '\n';
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to append debug image record:", e.what());
    }
}
