#include "RoguelikeSettlementTaskPlugin.h"

#include <map>
#include <optional>
#include <vector>

#include "Config/GeneralConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "Utils/DebugImageHelper.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

bool asst::RoguelikeSettlementTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    const auto task_name = details.get("details", "task", "");
    if (m_config->get_theme() == RoguelikeTheme::BlackFlow) {
        if (task_name == "BlackFlow@Roguelike@ClickToStartPoint" ||
            task_name == "BlackFlow@Roguelike@ClickToStartPointAfterFailed" ||
            task_name == "BlackFlow@Roguelike@SettlementRecoveryFailed") {
            m_blackflow_goal = task_name.ends_with("SettlementRecoveryFailed") ? BlackFlowGoal::Failed
                              : task_name.ends_with("ClickToStartPointAfterFailed") ? BlackFlowGoal::Entrance
                                                                                   : BlackFlowGoal::BeforeFinalPage;
            return true;
        }
    }
    m_blackflow_goal = BlackFlowGoal::Statistics;
    const RoguelikeMode mode = m_config->get_mode();
    const bool report_settlement = mode == RoguelikeMode::Exp || mode == RoguelikeMode::BlackFlowBabyAnimal ||
                                   mode == RoguelikeMode::BlackFlowAutomationCollection;
    if (task_name.ends_with("Roguelike@GamePass")) {
        m_game_pass = true;
        return report_settlement;
    }
    else if (task_name.ends_with("Roguelike@MissionFailedFlag2")) {
        m_game_pass = false;
        return report_settlement;
    }
    else {
        return false;
    }
}

bool asst::RoguelikeSettlementTaskPlugin::_run()
{
    LogTraceFunction;
    if (m_config->get_theme() == RoguelikeTheme::BlackFlow) {
        if (m_blackflow_goal == BlackFlowGoal::Failed || ++m_return_invocations > 32) {
            return fail_blackflow_settlement("settlement return handoff was exhausted", ctrler()->get_image());
        }
        cv::Mat statistics;
        if (!prepare_blackflow_settlement(m_blackflow_goal, statistics)) {
            return false;
        }
        return statistics.empty() || report_blackflow_statistics(statistics);
    }
    const auto& task = Task.get("RoguelikeSettlementConfirm");
    auto json_msg = basic_info_with_what("RoguelikeSettlement");
    json_msg["details"]["game_pass"] = m_game_pass;

    sleep(task->special_params[0]);
    if (m_game_pass) {
        save_img(ctrler()->get_image(), utils::path("achievement") / utils::path("roguelike"), "Page1");
    }

    const auto& rect = Task.get("Roguelike@ClickToStartPoint")->specific_rect;
    ctrler()->click(rect);
    sleep(task->pre_delay);

    if (!wait_for_whole_page()) {
        Log.error(__FUNCTION__, "wait for whole page failed");
        save_img(ctrler()->get_image(), utils::path("debug") / utils::path("roguelike"), "Page2_Error");
        return true;
    }
    sleep(task->post_delay);
    auto image = ctrler()->get_image();

    if (m_game_pass) {
        save_img(image, utils::path("achievement") / utils::path("roguelike"), "Page2");
    }
    get_settlement_info(json_msg, image);
    callback(AsstMsg::SubTaskExtraInfo, json_msg);
    return true;
}

void asst::RoguelikeSettlementTaskPlugin::reset_in_run_variables()
{
    m_statistics_reported = false;
    m_return_invocations = 0;
}

bool asst::RoguelikeSettlementTaskPlugin::record_blackflow_settlement(
    std::string_view phase,
    const cv::Mat& image,
    json::object details)
{
    if (need_exit() || (image.empty() && phase != "failed")) {
        return false;
    }
    details["scope"] = "run_settlement";
    details["acquisition_status"] = "not_an_acquisition";
    if (m_settlement_observer) {
        return m_settlement_observer(phase, std::move(details), std::make_shared<cv::Mat>(image.clone()));
    }
    if (!image.empty()) {
        save_img(image, utils::path("debug") / utils::path("roguelike"), "Settlement-" + std::string(phase));
    }
    return true;
}

bool asst::RoguelikeSettlementTaskPlugin::fail_blackflow_settlement(std::string reason, const cv::Mat& image)
{
    if (need_exit()) {
        return false;
    }
    Log.error("BlackFlow settlement recovery failed", reason);
    (void)record_blackflow_settlement("failed", image, { { "failure_reason", std::move(reason) } });
    if (m_task_ptr != nullptr) {
        m_task_ptr->set_enable(false);
    }
    return false;
}

bool asst::RoguelikeSettlementTaskPlugin::report_blackflow_statistics(const cv::Mat& image)
{
    if (m_statistics_reported) {
        return true;
    }
    const auto mode = m_config->get_mode();
    if (mode != RoguelikeMode::Exp && mode != RoguelikeMode::BlackFlowBabyAnimal &&
        mode != RoguelikeMode::BlackFlowAutomationCollection) {
        m_statistics_reported = true;
        return true;
    }
    auto info = basic_info_with_what("RoguelikeSettlement");
    info["details"]["game_pass"] = m_game_pass;
    get_settlement_info(info, image);
    if (!record_blackflow_settlement(
            "statistics-ready",
            image,
            { { "statistics", info["details"] }, { "recognition_scope", "settlement_statistics_rois" } })) {
        return fail_blackflow_settlement("settlement statistics evidence could not be saved", image);
    }
    callback(AsstMsg::SubTaskExtraInfo, info);
    m_statistics_reported = true;
    return true;
}

bool asst::RoguelikeSettlementTaskPlugin::prepare_blackflow_settlement(BlackFlowGoal goal, cv::Mat& statistics)
{
    enum class Page
    {
        Unknown,
        Loading,
        Detail,
        Entrance,
        Statistics,
        Summary,
        Failure,
        Handoff,
    };
    struct Observation
    {
        Page page = Page::Unknown;
        Rect rect;
        std::string task;
    };
    const auto match = [](const cv::Mat& image, const std::string& task) -> std::optional<Rect> {
        Matcher matcher(image);
        matcher.set_task_info(task);
        if (const auto result = matcher.analyze()) {
            return result->rect;
        }
        return std::nullopt;
    };
    const auto classify = [&](const cv::Mat& image) -> Observation {
        if (image.empty()) {
            return {};
        }
        OCRer loading(image);
        loading.set_task_info("LoadingText");
        if (loading.analyze()) {
            return { Page::Loading, {}, {} };
        }
        OCRer syncing(image);
        syncing.set_task_info("BlackFlow@Roguelike@DataSyncConfirm");
        if (syncing.analyze()) {
            return { Page::Loading, {}, {} };
        }
        const bool ending_header = match(image, "BlackFlow@Roguelike@SettlementPage").has_value() ||
                                   match(image, "BlackFlow@Roguelike@MissionFailedFlag2").has_value() ||
                                   match(image, "BlackFlow@Roguelike@GamePass").has_value();
        for (const auto& [task, page] : std::vector<std::pair<std::string, Page>> {
                 { "BlackFlow@Roguelike@SettlementDetail", Page::Detail },
                 { "BlackFlow@Roguelike@StartExplore", Page::Entrance } }) {
            if (auto rect = match(image, task)) {
                if (page == Page::Detail && !ending_header) {
                    return {};
                }
                return { page, *rect, task };
            }
        }
        if (goal == BlackFlowGoal::BeforeFinalPage) {
            std::vector<std::string> known;
            for (const auto& name : Task.get("BlackFlow@Roguelike@ClickToStartPoint")->next) {
                const auto task = Task.get(name);
                if (task && task->algorithm != AlgorithmType::JustReturn &&
                    name != "BlackFlow@Roguelike@MissionFailedFlag") {
                    known.push_back(name);
                }
            }
            PipelineAnalyzer next(image);
            next.set_tasks(known);
            if (next.analyze()) {
                return { Page::Handoff, {}, {} };
            }
            if (auto rect = match(image, "BlackFlow@Roguelike@MissionFailedFlag")) {
                return { Page::Failure, *rect, "BlackFlow@Roguelike@MissionFailedFlag" };
            }
            return {};
        }
        if (goal == BlackFlowGoal::Entrance) {
            std::vector<std::string> startup;
            for (const auto& name : Task.get("BlackFlow@Roguelike@ClickToStartPointAfterFailed")->next) {
                const auto task = Task.get(name);
                if (task && task->algorithm != AlgorithmType::JustReturn &&
                    name.find("Abandon") == std::string::npos &&
                    name.find("ChooseDifficulty") == std::string::npos && name != "BlackFlow@Roguelike") {
                    startup.push_back(name);
                }
            }
            PipelineAnalyzer next(image);
            next.set_tasks(startup);
            if (next.analyze()) {
                return { Page::Handoff, {}, {} };
            }
        }
        if (!ending_header) {
            return {};
        }
        if (auto rect = match(image, "BlackFlow@RoguelikeSettlementConfirm")) {
            return { Page::Statistics, *rect, "BlackFlow@RoguelikeSettlementConfirm" };
        }
        if (auto rect = match(image, "BlackFlow@Roguelike@SettlementNext")) {
            return { Page::Summary, *rect, "BlackFlow@Roguelike@SettlementNext" };
        }
        return {};
    };
    std::map<Page, int> inputs;
    Page last_input = Page::Unknown;
    int since_input = 8;
    cv::Mat last_image;
    for (int sample = 0; sample < 240 && !need_exit(); ++sample) {
        if (sample != 0 && !sleep(250)) {
            return false;
        }
        ++since_input;
        cv::Mat image = ctrler()->get_image();
        if (!image.empty()) {
            last_image = image.clone();
        }
        const auto observed = classify(image);
        if (observed.page == Page::Entrance || observed.page == Page::Handoff) {
            return record_blackflow_settlement("navigation-ready", image,
                { { "destination", observed.page == Page::Entrance ? "entrance"
                                     : goal == BlackFlowGoal::BeforeFinalPage ? "recognized_continuation"
                                                                             : "startup_transition" } });
        }
        if (observed.page == Page::Statistics) {
            if (goal == BlackFlowGoal::Statistics) {
                statistics = image.clone();
                return true;
            }
            if (!report_blackflow_statistics(image)) {
                return false;
            }
        }
        if (observed.page == Page::Unknown || observed.page == Page::Loading) {
            continue;
        }
        if (last_input == observed.page && since_input < 8) {
            continue;
        }
        if (inputs[observed.page] >= 3) {
            return fail_blackflow_settlement(
                "recognized settlement action did not change the page after three inputs",
                image);
        }
        const std::string phase = observed.page == Page::Detail ? "detail-before-close"
                                  : observed.page == Page::Statistics ? "statistics-before-confirm"
                                  : observed.page == Page::Summary ? "summary-before-advance"
                                                                  : "failure-before-advance";
        if (!record_blackflow_settlement(phase, image,
                { { "input_task", observed.task }, { "attempt", inputs[observed.page] + 1 } })) {
            return fail_blackflow_settlement("settlement input evidence could not be saved", image);
        }
        // The observer may perform disk I/O while the user or game changes pages.
        // Reclassify after recording; never apply a stale close/advance coordinate.
        const auto fresh = classify(ctrler()->get_image());
        if (need_exit() || fresh.page != observed.page || fresh.task != observed.task) {
            continue;
        }
        ++inputs[observed.page];
        last_input = observed.page;
        since_input = 0;
        (void)ctrler()->click(fresh.rect);
    }
    return need_exit() ? false
                       : fail_blackflow_settlement(
                             "settlement navigation did not reach a recognized destination",
                             last_image);
}

bool asst::RoguelikeSettlementTaskPlugin::get_settlement_info(json::value& info, const cv::Mat& image)
{
    const auto& number_replace = Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map;
    const auto& append_data = [&](std::string task_name, const std::string& ocr_result) {
        int num = -1;
        if (!utils::chars_to_number(ocr_result, num)) {
            Log.error(__FUNCTION__, "convert str to int failed, task:", task_name, ", string:", ocr_result);
            return;
        }

        utils::tolowers(task_name);
        info["details"][task_name] = num;
    };

    auto analyze_battle_data = [&](const std::string& task_name) {
        RegionOCRer ocr(image);
        ocr.set_task_info("RoguelikeSettlementOcr-" + task_name);
        ocr.set_bin_threshold(0, 160);
        auto merge_map = number_replace;
        merge_map.emplace_back("[^\\d]+", "");
        ocr.set_replace(merge_map);
        if (!ocr.analyze()) {
            Log.error(__FUNCTION__, "analyze battle data failed, task:", task_name);
            return;
        }
        append_data(task_name, ocr.get_result().text);
    };

    auto analyze_text_data = [&](const std::string& task_name, bool is_number) {
        RegionOCRer ocr(image);
        ocr.set_task_info(m_config->get_theme() + "@RoguelikeSettlementOcr-" + task_name);
        ocr.set_bin_threshold(50, 255);
        const auto& task_replace = Task.get<OcrTaskInfo>("RoguelikeSettlementOcr-" + task_name)->replace_map;
        auto merge_map = number_replace;
        std::ranges::copy(task_replace, std::back_inserter(merge_map));
        ocr.set_replace(merge_map);
        if (!ocr.analyze()) {
            Log.error(__FUNCTION__, "analyze battle data failed, task:", task_name);
            return;
        }

        if (is_number) {
            append_data(task_name, ocr.get_result().text);
        }
        else {
            std::string tag = task_name;
            utils::tolowers(tag);
            info["details"][tag] = ocr.get_result().text;
        }
    };

    static const auto& battle_tasks =
        std::vector<std::string> { "Floor", "Step", "Combat", "Recruit", "Collection", "BOSS", "Emergency" };
    static const auto& text_tasks = std::vector<std::pair<std::string, bool>> { { "Difficulty", true },
                                                                                { "Score", true },
                                                                                { "Exp", false },
                                                                                { "Skill", false } };

    std::ranges::for_each(battle_tasks, analyze_battle_data);

    using Theme = RoguelikeTheme;
    for (const auto& [task, is_number] :
         text_tasks | std::views::take(m_config->get_theme() == Theme::Phantom ? 3 : 4)) {
        analyze_text_data(task, is_number);
    }

    return true;
}

bool asst::RoguelikeSettlementTaskPlugin::wait_for_whole_page()
{
    int retry = 0;
    cv::Mat image;
    do {
        image = ctrler()->get_image();
        Matcher matcher(image);
        matcher.set_task_info(m_config->get_theme() + "@RoguelikeSettlementConfirm");
        if (matcher.analyze()) {
            return true;
        }
        Log.error(__FUNCTION__, "RoguelikeSettlementConfirm match failed, retry:", retry);
        ++retry;
        sleep(Config.get_options().task_delay);
    } while (!need_exit() && retry < 20);
    return false;
}

void asst::RoguelikeSettlementTaskPlugin::save_img(
    const cv::Mat& image,
    const std::filesystem::path& relative_dir,
    std::string suffix)
{
    utils::save_debug_image(image, relative_dir, true, "", suffix);
}
