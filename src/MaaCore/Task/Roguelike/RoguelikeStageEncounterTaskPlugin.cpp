#include "RoguelikeStageEncounterTaskPlugin.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>
#include <vector>

#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/Encoding.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Task/Roguelike/Map/RoguelikeBoskyPassageMap.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/RegionOCRer.h"

namespace
{
std::string save_map_encounter_image(const cv::Mat& image, std::string_view type)
{
    if (type == "Legend") {
        return asst::RoguelikeDataCollector.save_legend_image(image);
    }
    return asst::RoguelikeDataCollector.save_encounter_image(image);
}

std::wstring normalize_encounter_lookup_text(std::string_view text)
{
    std::wstring result;
    for (wchar_t ch : MAA_NS::to_u16(text)) {
        if (static_cast<unsigned long>(ch) <= 0x7F) {
            const unsigned char ascii = static_cast<unsigned char>(ch);
            if (std::isspace(ascii) || std::ispunct(ascii)) {
                continue;
            }
            if (std::isalpha(ascii)) {
                ch = static_cast<wchar_t>(std::tolower(ascii));
            }
        }

        constexpr std::wstring_view Separators = L"　，。、；：！？（）【】《》“”‘’「」『』〔〕·—…";
        if (Separators.find(ch) != std::wstring_view::npos) {
            continue;
        }

        result.push_back(ch);
    }
    return result;
}

size_t edit_distance(std::wstring_view lhs, std::wstring_view rhs)
{
    if (lhs.empty()) {
        return rhs.size();
    }
    if (rhs.empty()) {
        return lhs.size();
    }

    std::vector<size_t> prev(rhs.size() + 1);
    std::vector<size_t> curr(rhs.size() + 1);
    for (size_t i = 0; i <= rhs.size(); ++i) {
        prev[i] = i;
    }

    for (size_t i = 1; i <= lhs.size(); ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= rhs.size(); ++j) {
            const size_t cost = lhs[i - 1] == rhs[j - 1] ? 0 : 1;
            curr[j] = std::min({ prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, curr);
    }

    return prev.back();
}

size_t max_event_name_distance(size_t name_length)
{
    if (name_length >= 4) {
        return 2;
    }
    if (name_length == 3) {
        return 1;
    }
    return 0;
}

std::string build_event_title_text(const asst::OCRer::ResultsVec& results)
{
    if (results.empty()) {
        return {};
    }

    const auto top_it = std::ranges::min_element(results, [](const auto& lhs, const auto& rhs) {
        return lhs.rect.y < rhs.rect.y;
    });
    const int top_y = top_it->rect.y;
    const int line_limit = top_y + std::max(24, top_it->rect.height + 8);

    std::vector<const asst::OCRer::Result*> title_results;
    for (const auto& result : results) {
        if (result.rect.y <= line_limit) {
            title_results.emplace_back(&result);
        }
    }
    std::ranges::sort(title_results, [](const auto* lhs, const auto* rhs) {
        if (lhs->rect.x == rhs->rect.x) {
            return lhs->rect.y < rhs->rect.y;
        }
        return lhs->rect.x < rhs->rect.x;
    });

    std::string title;
    for (const auto* result : title_results) {
        title += result->text;
    }
    return title;
}

std::string build_ocr_text(asst::OCRer::ResultsVec results)
{
    if (results.empty()) {
        return {};
    }

    std::ranges::sort(results, [](const auto& lhs, const auto& rhs) {
        const int y_delta = lhs.rect.y > rhs.rect.y ? lhs.rect.y - rhs.rect.y : rhs.rect.y - lhs.rect.y;
        const int line_delta = std::max(lhs.rect.height, rhs.rect.height) / 2;
        if (y_delta <= line_delta) {
            return lhs.rect.x < rhs.rect.x;
        }
        return lhs.rect.y < rhs.rect.y;
    });

    std::string text;
    for (const auto& result : results) {
        text += result.text;
    }
    return text;
}

bool is_jiegarden_target_candle_body(std::string_view text)
{
    const std::wstring normalized = normalize_encounter_lookup_text(text);
    if (normalized.empty()) {
        return false;
    }

    const auto contains = [&](std::wstring_view part) {
        return normalized.find(part) != std::wstring::npos;
    };
    const bool has_intro = contains(L"席地而坐") || contains(L"超脱神识");
    const bool has_candle = contains(L"基础烛火") || contains(L"获得6") || contains(L"6基础");
    const bool has_bosky = contains(L"岁兽残识");
    const bool has_explored = contains(L"已探索") || contains(L"探索状态");
    return has_intro && has_candle && has_bosky && has_explored;
}

bool is_jiegarden_target_candle_title(std::string_view text)
{
    return normalize_encounter_lookup_text(text).find(L"烛火") != std::wstring::npos;
}

bool is_jiegarden_relieving_event_body(std::string_view text)
{
    const std::wstring normalized = normalize_encounter_lookup_text(text);
    if (normalized.empty()) {
        return false;
    }

    const auto contains = [&](std::wstring_view part) {
        return normalized.find(part) != std::wstring::npos;
    };
    return contains(L"扼腕") && (contains(L"缸中之物") || contains(L"扼腕叹息") || contains(L"背生莲梗"));
}

bool has_event_name(const std::vector<std::string>& event_names, std::string_view event_name)
{
    return std::ranges::find(event_names, event_name) != event_names.end();
}

struct EncounterNameMatch
{
    std::string name;
    std::string raw_title;
    std::string normalized_title;
    size_t distance = 0;
};

std::optional<EncounterNameMatch> match_event_name_by_title(
    const std::string& raw_title,
    const std::vector<std::string>& event_names)
{
    const std::wstring normalized_title = normalize_encounter_lookup_text(raw_title);
    if (normalized_title.empty()) {
        return std::nullopt;
    }

    EncounterNameMatch direct_match;
    size_t direct_match_length = 0;
    size_t direct_match_count = 0;
    for (const std::string& event_name : event_names) {
        const std::wstring normalized_event = normalize_encounter_lookup_text(event_name);
        if (normalized_event.empty()) {
            continue;
        }
        if (normalized_title.find(normalized_event) == std::wstring::npos) {
            continue;
        }

        if (normalized_event.size() > direct_match_length) {
            direct_match = { event_name, raw_title, MAA_NS::from_u16(normalized_title), 0 };
            direct_match_length = normalized_event.size();
            direct_match_count = 1;
        }
        else if (normalized_event.size() == direct_match_length && event_name != direct_match.name) {
            ++direct_match_count;
        }
    }
    if (direct_match_count == 1) {
        return direct_match;
    }

    EncounterNameMatch fuzzy_match;
    size_t best_distance = std::numeric_limits<size_t>::max();
    size_t fuzzy_match_count = 0;
    for (const std::string& event_name : event_names) {
        const std::wstring normalized_event = normalize_encounter_lookup_text(event_name);
        if (normalized_event.empty()) {
            continue;
        }

        const size_t distance = edit_distance(normalized_title, normalized_event);
        if (distance > max_event_name_distance(normalized_event.size())) {
            continue;
        }
        if (distance < best_distance) {
            fuzzy_match = { event_name, raw_title, MAA_NS::from_u16(normalized_title), distance };
            best_distance = distance;
            fuzzy_match_count = 1;
        }
        else if (distance == best_distance && event_name != fuzzy_match.name) {
            ++fuzzy_match_count;
        }
    }

    if (fuzzy_match_count == 1) {
        return fuzzy_match;
    }
    return std::nullopt;
}

std::optional<EncounterNameMatch> match_event_name_from_raw_ocr(
    const asst::OCRer::ResultsVec& results,
    const std::vector<std::string>& event_names)
{
    return match_event_name_by_title(build_event_title_text(results), event_names);
}

bool is_configured_option_match(std::string_view recognized_text, std::string_view configured_text)
{
    if (recognized_text == configured_text) {
        return true;
    }

    const std::wstring recognized = normalize_encounter_lookup_text(recognized_text);
    const std::wstring configured = normalize_encounter_lookup_text(configured_text);
    if (recognized.empty() || configured.empty()) {
        return false;
    }
    if (recognized == configured) {
        return true;
    }

    if (
        configured == L"岁兽代理人的援助尽在眼前" &&
        recognized.find(L"岁兽代理人的援助尽在") != std::wstring::npos) {
        return true;
    }

    constexpr size_t MinPartialMatchLength = 6;
    if (std::min(recognized.size(), configured.size()) < MinPartialMatchLength) {
        return false;
    }

    return recognized.find(configured) != std::wstring::npos || configured.find(recognized) != std::wstring::npos;
}
}

bool asst::RoguelikeStageEncounterTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    // 安全屋，掷骰子之类的带选项的也都是视为不期而遇了
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (!RoguelikeConfig::is_valid_theme(m_config->get_theme())) {
        Log.error("Roguelike name doesn't exist!");
        return false;
    }
    const std::string roguelike_name = m_config->get_theme() + "@";
    const std::string& task = details.get("details", "task", "");
    std::string_view task_view = task;
    if (task_view.starts_with(roguelike_name)) {
        task_view.remove_prefix(roguelike_name.length());
    }
    if (task_view == "Roguelike@StageEncounterJudgeOption") {
        return true;
    }
    else {
        return false;
    }
}

bool asst::RoguelikeStageEncounterTaskPlugin::_run()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();
    std::vector<std::string> event_names = RoguelikeStageEncounter.get_event_names(theme);

    const auto event_name_task_ptr = Task.get("Roguelike@StageEncounterOcr");
    sleep(event_name_task_ptr->pre_delay);

    if (need_exit()) {
        return false;
    }

    cv::Mat image = ctrler()->get_image();
    OCRer name_analyzer(image);
    name_analyzer.set_task_info(event_name_task_ptr);
    name_analyzer.set_required(event_names);

    std::optional<std::string> current_event_name;
    std::string raw_event_title;
    std::string normalized_event_title;

    if (name_analyzer.analyze()) {
        const auto& result_vec = name_analyzer.get_result();
        if (!result_vec.empty()) {
            current_event_name = result_vec.front().text;
        }
    }

    if (!current_event_name) {
        OCRer raw_name_analyzer(image);
        raw_name_analyzer.set_task_info(event_name_task_ptr);
        if (raw_name_analyzer.analyze()) {
            const auto& raw_results = raw_name_analyzer.get_result();
            raw_event_title = build_event_title_text(raw_results);
            normalized_event_title = MAA_NS::from_u16(normalize_encounter_lookup_text(raw_event_title));
            if (auto match = match_event_name_from_raw_ocr(raw_results, event_names)) {
                Log.info(
                    "Encounter name fallback matched:",
                    match->raw_title,
                    "->",
                    match->name,
                    "normalized:",
                    match->normalized_title,
                    "distance:",
                    match->distance);
                current_event_name = match->name;
            }
            else if (
                theme == RoguelikeTheme::JieGarden && RoguelikeDataCollector.should_record_map_encounters() &&
                RoguelikeDataCollector.map_encounter_type() == "Legend" && has_event_name(event_names, "禳解") &&
                is_jiegarden_relieving_event_body(build_ocr_text(raw_results))) {
                Log.info(
                    "Encounter body fallback matched:",
                    raw_event_title,
                    "->",
                    "禳解",
                    "normalized:",
                    normalized_event_title);
                current_event_name = "禳解";
            }
        }
    }

    if (!current_event_name) {
        Log.error("Unknown Event", "raw_title:", raw_event_title, "normalized:", normalized_event_title);
        const bool record_map_encounter = RoguelikeDataCollector.should_record_map_encounters();
        const std::string map_encounter_type =
            record_map_encounter ? RoguelikeDataCollector.map_encounter_type() : "Encounter";
        const std::string image_path =
            record_map_encounter ? save_map_encounter_image(image, map_encounter_type) : "";
        if (record_map_encounter) {
            RoguelikeDataCollector.record_encounter("unknown", image_path, map_encounter_type);
        }
        json::object event_details { { "event_name", "" },
                                     { "record_type", map_encounter_type },
                                     { "options", json::array {} },
                                     { "selected_option", "" },
                                     { "image", image_path },
                                     { "ocr_failed", true } };
        if (!raw_event_title.empty()) {
            event_details["raw_event_name"] = raw_event_title;
        }
        if (!normalized_event_title.empty()) {
            event_details["normalized_event_name"] = normalized_event_title;
        }
        RoguelikeDataCollector.log_event("encounter", std::move(event_details));
        if (record_map_encounter) {
            json::object abandon_details {
                { "record_type", map_encounter_type },
                { "ocr_failed", true },
            };
            if (!image_path.empty()) {
                abandon_details["encounter_image"] = image_path;
            }
            if (!raw_event_title.empty()) {
                abandon_details["raw_event_name"] = raw_event_title;
            }
            if (!normalized_event_title.empty()) {
                abandon_details["normalized_event_name"] = normalized_event_title;
            }
            RoguelikeDataCollector.finish_run_as_abandoned("encounter_ocr_error", image, std::move(abandon_details));
        }
        callback(AsstMsg::SubTaskExtraInfo, basic_info_with_what("EncounterOcrError"));
        return true;
    }

    // 处理主事件及其链式 next_event
    while (!current_event_name->empty()) {
        auto next = handle_single_event(*current_event_name);
        if (!next) {
            break;
        }
        current_event_name = next.value();
    }

    return true;
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::handle_single_event(const std::string& event_name)
{
    const std::string& theme = m_config->get_theme();
    const RoguelikeMode& mode = m_config->get_mode();
    const auto& event_map = RoguelikeStageEncounter.get_events(theme, mode);
    const bool record_as_encounter = !(theme == RoguelikeTheme::JieGarden && event_name == "相随");
    cv::Mat image = ctrler()->get_image();
    const bool record_map_encounter = record_as_encounter && RoguelikeDataCollector.should_record_map_encounters();
    const std::string map_encounter_type =
        record_map_encounter ? RoguelikeDataCollector.map_encounter_type() : "Encounter";
    const std::string image_path = record_map_encounter ? save_map_encounter_image(image, map_encounter_type) : "";
    const auto log_encounter_event = [&](json::object details) {
        if (record_as_encounter) {
            details["record_type"] = map_encounter_type;
            RoguelikeDataCollector.log_event("encounter", std::move(details));
        }
    };

    auto it = event_map.find(event_name);
    if (it == event_map.end()) {
        Log.error("Unknown event:", event_name);
        if (record_map_encounter) {
            RoguelikeDataCollector.record_encounter(event_name, image_path, map_encounter_type);
        }
        log_encounter_event(json::object {
            { "event_name", event_name },
            { "options", json::array {} },
            { "selected_option", "" },
            { "image", image_path },
            { "ocr_failed", false },
            { "unknown_event", true },
        });
        return std::nullopt;
    }

    const auto& event = it->second;
    const auto remember_agent_source = [&](std::optional<std::string> next) {
        if (next && *next == "相随") {
            RoguelikeDataCollector.note_agent_source(event.name, map_encounter_type);
        }
        return next;
    };
    const auto checked_next_event = [&]() {
        return remember_agent_source(next_event(event));
    };
    const auto configured_next_event = [&]() -> std::optional<std::string> {
        if (event.next_event.empty()) {
            return std::nullopt;
        }
        return remember_agent_source(std::optional<std::string> { event.next_event });
    };

    if (record_map_encounter) {
        RoguelikeDataCollector.record_encounter(event.name, image_path, map_encounter_type);
    }

    int special_val = 0;
    // 水月的不好识别，先试试萨米能不能用
    if (m_config->get_theme() == RoguelikeTheme::Sami) {
        OCRer analyzer(image);
        analyzer.set_task_info(m_config->get_theme() + "@Roguelike@SpecialValRecognition");
        analyzer.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);
        analyzer.set_use_char_model(true);
        if (!analyzer.analyze()) {
            // return std::nullopt;
            Log.error("Failed to recognize special value for event:", event.name);
        }
        else {
            utils::chars_to_number(analyzer.get_result().front().text, special_val);
        }
    }

    size_t choose_option = process_task(event, special_val);
    Log.info("Event:", event.name, "special_val", special_val, "choose option", choose_option);

    auto info = basic_info_with_what("RoguelikeEvent");
    info["details"]["name"] = event.name;
    info["details"]["default_choose"] = event.default_choose;
    info["details"]["choose_option"] = choose_option;
    callback(AsstMsg::SubTaskExtraInfo, info);

    // 萨卡兹内容拓展 II，#11861
    if (event.name.starts_with("魂灵见闻：")) {
        Matcher matcher(image);
        matcher.set_task_info("Sarkaz@Roguelike@CloseCollectionClose");
        if (matcher.analyze()) {
            Log.trace("Found extra 'Plans', click CloseCollectionClose and StageEncounterJudgeClick");
            ctrler()->click(matcher.get_result().rect);
            ProcessTask(*this, { "Roguelike@StageEncounterJudgeClick" }).run();
            ProcessTask(*this, { "Roguelike@StageEncounterJudgeClick2" }).run();
        }
    }

    // 界园树洞
    if (m_config->get_theme() == RoguelikeTheme::JieGarden) {
        auto& bosky_map = RoguelikeBoskyPassageMap::get_instance();
        if (event.name == "掷地有声") {
            bosky_map.set_node_subtype(bosky_map.get_curr_pos(), RoguelikeBoskySubNodeType::Ling);
        }
        else if (event.name == "种因得果") {
            bosky_map.set_node_subtype(bosky_map.get_curr_pos(), RoguelikeBoskySubNodeType::Shu);
        }
        else if (event.name == "三缺一") {
            bosky_map.set_node_subtype(bosky_map.get_curr_pos(), RoguelikeBoskySubNodeType::Nian);
        }

        if (bosky_map.get_target_subtype() != RoguelikeBoskySubNodeType::Unknown) {
            if (bosky_map.get_node_subtype(bosky_map.get_curr_pos()) == bosky_map.get_target_subtype()) {
                Log.info(__FUNCTION__, "| Found target playtime node, completing task and exiting");

                auto target_info = basic_info_with_what("RoguelikeJieGardenTargetFound");
                target_info["details"]["target_subtype"] = subtype2name(bosky_map.get_target_subtype());
                callback(AsstMsg::SubTaskExtraInfo, target_info);

                // 完成任务，退出
                m_control_ptr->exit_then_stop();
                m_task_ptr->set_enable(false);
                return std::nullopt;
            }
        }
    }

    // 界园肉鸽实验性功能 -- 识别选项数量后调整选项
    if (theme == RoguelikeTheme::JieGarden) {
        reset_option_list_and_view_data();
        if (update_option_list()) {
            json::array option_details;
            std::vector<std::string> option_names;
            for (const auto& option : m_option_list) {
                option_names.emplace_back(option.text);
                option_details.emplace_back(json::object {
                    { "text", option.text },
                    { "enabled", option.enabled },
                });
            }
            if (event.name == "相随") {
                const Config::RoguelikeEvent* treasure_event = nullptr;
                if (auto treasure_it = event_map.find("宝盒"); treasure_it != event_map.end()) {
                    treasure_event = &treasure_it->second;
                }
                return handle_jiegarden_agent_event(event, treasure_event);
            }
            if (event.name == "禳解") {
                return handle_jiegarden_legend_relieving_event(event);
            }
            RoguelikeDataCollector.record_bosky_passage_event_node(event.name, m_option_list_image, option_names);
            record_agent_event_if_needed(event);

            size_t choice = 0; // 以 0 作为 无效 index
            if (event.name == "天圆地方") {
                choice = m_option_list.size() >= 2 ? 2 : 0;
            }
            else if (event.name == "天圆地方-1") {
                choice = m_option_list.size();
            }
            else if (!event.option_text.empty()) {
                for (const std::string& event_text : event.option_text) {
                    const auto option_it =
                        std::ranges::find_if(m_option_list, [&event_text](const OptionAnalyzer::Option& option) {
                            return is_configured_option_match(option.text, event_text);
                        });
                    if (option_it != m_option_list.end()) {
                        choice = std::distance(m_option_list.begin(), option_it) + 1;
                        break;
                    }
                }
            }
            else if (event.option_num == m_option_list.size()) {
                choice = choose_option;
            }
            else {
                for (const auto& [total, item] : event.fallback_choices) {
                    if (total == m_option_list.size()) {
                        choice = item;
                        break;
                    }
                }
            }
            if (choice == 0) {
                Log.error(
                    std::format(
                        "RoguelikeEncounter | Failed to find choice for scenario with {} option(s)",
                        m_option_list.size()));
            }
            else if (select_analyzed_option(choice - 1)) {
                log_encounter_event(json::object {
                    { "event_name", event.name },
                    { "options", option_details },
                    { "selected_option", m_option_list[choice - 1].text },
                    { "image", image_path },
                    { "ocr_failed", false },
                });
                return checked_next_event();
            }

            // 兜底：从下到上依次选择
            for (choice = m_option_list.size(); choice > 0; --choice) {
                if (m_option_list[choice - 1].enabled && select_analyzed_option(choice - 1)) {
                    log_encounter_event(json::object {
                        { "event_name", event.name },
                        { "options", option_details },
                        { "selected_option", m_option_list[choice - 1].text },
                        { "image", image_path },
                        { "ocr_failed", false },
                    });
                    return checked_next_event();
                }
            }

            log_encounter_event(json::object {
                { "event_name", event.name },
                { "options", std::move(option_details) },
                { "selected_option", "" },
                { "image", image_path },
                { "ocr_failed", false },
            });
        }
    }

    if (event.option_num == 0 || choose_option == 0) {
        Log.error(
            __FUNCTION__,
            "| invalid configured choice for event:",
            event.name,
            "option_num:",
            event.option_num,
            "choose_option:",
            choose_option);
        log_encounter_event(json::object {
            { "event_name", event.name },
            { "options", json::array {} },
            { "selected_option", "" },
            { "image", image_path },
            { "ocr_failed", false },
            { "invalid_choice", true },
        });
        return std::nullopt;
    }

    const auto click_option_task_name = [&](size_t item, size_t total) -> std::optional<std::string> {
        if (total == 0 || item == 0) {
            Log.error("Event:", event.name, "invalid option task", total, "-", item);
            return std::nullopt;
        }
        if (item > total) {
            Log.warn("Event:", event.name, "Total:", total, "Choice", item, "out of range, switch to choice", total);
            item = total;
        }
        if (total > 4) {
            Log.error("Event:", event.name, "fixed option task only supports up to 4 options, got", total);
            return std::nullopt;
        }
        return m_config->get_theme() + "@Roguelike@OptionChoose" + std::to_string(total) + "-" + std::to_string(item);
    };

    const auto click_configured_option = [&](size_t item, size_t total) -> bool {
        const std::optional<std::string> task_name = click_option_task_name(item, total);
        if (!task_name) {
            return false;
        }
        for (int j = 0; j < 2; ++j) {
            ProcessTask(*this, { *task_name }).run();
            sleep(300);
        }
        return true;
    };

    if (!click_configured_option(choose_option, event.option_num)) {
        log_encounter_event(json::object {
            { "event_name", event.name },
            { "options", json::array {} },
            { "selected_option", "" },
            { "image", image_path },
            { "ocr_failed", false },
            { "invalid_choice", true },
        });
        return std::nullopt;
    }

    log_encounter_event(json::object {
        { "event_name", event.name },
        { "options", json::array {} },
        { "selected_option", std::to_string(choose_option) },
        { "image", image_path },
        { "ocr_failed", false },
    });

    sleep(1500);

    // 判断是否点击成功，成功进入对话后左上角的生命值会消失
    image = ctrler()->get_image();
    bool hp_disappeared = (hp(image) < 0);
    // fallback 可变选项，临时处理，之后还得改成更通用的方式
    if (!hp_disappeared) {
        for (const auto& [total, item] : event.fallback_choices) {
            if (total == 0 || item == 0) {
                Log.warn("Event:", event.name, "skip invalid fallback choice", total, "-", item);
                continue;
            }
            Log.info("Trying fallback choice", total, "-", item);
            if (!click_configured_option(item, total)) {
                continue;
            }
            sleep(500);
            image = ctrler()->get_image();
            if (hp(image) < 0) {
                Log.info("Fallback choice success");
                hp_disappeared = true;
                break;
            }
        }
    }

    if (hp_disappeared) {
        return checked_next_event();
    }

    // 兜底处理，从 option_num-option_num 点到 1-1
    size_t max_time = event.option_num;
    while (max_time > 0) {
        for (size_t i = max_time; i > 0; --i) {
            bool ret =
                ProcessTask(*this, { "Roguelike@TutorialButton" }).set_reusable_image(image).set_retry_times(3).run();
            if (ret) {
                return std::nullopt;
            }

            if (!click_configured_option(i, max_time)) {
                continue;
            }

            if (need_exit()) {
                return std::nullopt;
            }

            sleep(500);
            image = ctrler()->get_image();
            if (hp(image) < 0) {
                return std::nullopt;
            }
        }
        --max_time;
    }

    return configured_next_event();
}

bool asst::RoguelikeStageEncounterTaskPlugin::satisfies_condition(
    const Config::ChoiceRequire& requirement,
    const int special_val)
{
    int value = 0;
    bool ret = utils::chars_to_number(requirement.vision.value, value);
    Log.trace("special_val: ", special_val, "value: ", value);
    switch (requirement.vision.type) {
    case Config::ComparisonType::GreaterThan:
        ret &= special_val > value;
        Log.trace("special_val > value: ", special_val > value ? "true" : "false");
        break;
    case Config::ComparisonType::LessThan:
        ret &= special_val < value;
        Log.trace("special_val < value: ", special_val < value ? "true" : "false");
        break;
    case Config::ComparisonType::Equal:
        ret &= special_val == value;
        Log.trace("special_val == value: ", special_val == value ? "true" : "false");
        break;
    case Config::ComparisonType::None:
        Log.warn("no vision type");
        break;
    case Config::ComparisonType::Unsupported:
        Log.warn("unsupported vision type");
        return false;
    }
    /*
    switch (requirement.hp.type) {
        // ...
    }
    */
    if (!ret) {
        return false;
    }
    return true;
}

size_t asst::RoguelikeStageEncounterTaskPlugin::process_task(const Config::RoguelikeEvent& event, const int special_val)
{
    for (const auto& requirement : event.choice_require) {
        if (requirement.choose == -1) {
            continue;
        }
        if (satisfies_condition(requirement, special_val)) {
            return requirement.choose;
        }
    }
    return event.default_choose;
}

int asst::RoguelikeStageEncounterTaskPlugin::hp(const cv::Mat& image) const
{
    LogTraceFunction;

    if (!ProcessTask(*this, { "Roguelike@HpFlag" }).run()) {
        Log.info("Not found HpFlag");
        return -1;
    }

    auto task = Task.get<OcrTaskInfo>("Roguelike@HpRecognition");
    std::vector<std::pair<std::string, std::string>> merged_map;
    merged_map.insert(merged_map.end(), task->replace_map.begin(), task->replace_map.end());
    merged_map.emplace_back("(.*)/.*", "$1");

    auto roi_image = make_roi(image, task->roi).clone();
    cv::Mat r_channel;
    cv::extractChannel(roi_image, r_channel, 2);
    cv::Mat mask;
    cv::threshold(r_channel, mask, 50, 255, cv::THRESH_BINARY);
    cv::Mat inv_mask;
    cv::bitwise_not(mask, inv_mask);
    roi_image.setTo(cv::Scalar(0, 0, 0), mask);

    RegionOCRer analyzer(roi_image);
    analyzer.set_replace(merged_map);
    analyzer.set_use_char_model(true);
    analyzer.set_bin_threshold(60); // 血量没有红色通道，虽然它看着很明显，但实际上在灰度中只有 2/3

    auto res_vec_opt = analyzer.analyze();
    if (!res_vec_opt) {
        return 0;
    }

    int hp_val;
    return utils::chars_to_number(res_vec_opt->text, hp_val) ? hp_val : 0;
}

bool asst::RoguelikeStageEncounterTaskPlugin::update_option_list()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    // 不期而遇默认位置会在选项列表中央, 为了从上到下检视选项列表, 需要先向上滑动
    move_to_option_list_head();

    cv::Mat image = ctrler()->get_image();
    const cv::Mat initial_image = image.clone();
    RoguelikeEncounterOptionAnalyzer analyzer(image);
    analyzer.set_theme(theme);
    for (size_t swipe_times = 0; swipe_times < MAX_SWIPE_TIMES && !need_exit(); ++swipe_times) {
        move_forward();
        image = ctrler()->get_image();
        const std::optional<int> ret = analyzer.merge_image(image);
        if (!ret) {
            Log.warn(__FUNCTION__, "| Failed to merge option screenshots; fall back to the latest viewport");
            analyzer.set_image(image);
            break;
        }
        if (ret.value() <= 0) {
            break;
        }
    }

    if (!analyzer.analyze()) {
        if (!initial_image.empty()) {
            Log.warn(__FUNCTION__, "| Failed to analyze latest viewport; retry initial option viewport");
            analyzer.set_image(initial_image);
            if (!analyzer.analyze()) {
                return false;
            }
        }
        else {
            return false;
        }
    }

    m_option_list = analyzer.get_result();
    m_option_list_image = analyzer.get_image().clone();
    report_analyzed_options();

    update_view(image);

    return true;
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::read_jiegarden_first_agent_option_body() const
{
    if (m_option_list.empty() || m_option_list_image.empty()) {
        return std::nullopt;
    }

    const auto first_match =
        OptionAnalyzer::match_option(m_config->get_theme(), m_option_list_image, m_option_list.front().templ);
    if (!first_match) {
        Log.warn(__FUNCTION__, "| Failed to locate first agent option body");
        return std::nullopt;
    }

    const Rect& first_rect = first_match->rect;
    int option_bottom = std::min(first_rect.y + 155, m_option_list_image.rows);
    if (m_option_list.size() > 1) {
        const auto second_match =
            OptionAnalyzer::match_option(m_config->get_theme(), m_option_list_image, m_option_list[1].templ);
        if (second_match) {
            option_bottom = std::min(option_bottom, second_match->rect.y - 5);
        }
    }

    Rect body_roi { first_rect.x + 60,
                    first_rect.y + first_rect.height + 5,
                    m_option_list_image.cols - first_rect.x - 65,
                    option_bottom - first_rect.y - first_rect.height - 5 };
    if (body_roi.empty()) {
        Log.warn(__FUNCTION__, "| Empty first agent option body ROI");
        return std::nullopt;
    }

    OCRer body_ocr(m_option_list_image, body_roi);
    if (!body_ocr.analyze()) {
        Log.warn(__FUNCTION__, "| Failed to recognize first agent option body");
        return std::nullopt;
    }

    const std::string body_text = build_ocr_text(body_ocr.get_result());
    Log.info(__FUNCTION__, "| First agent option body:", body_text);
    return body_text;
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::handle_jiegarden_agent_event(
    const Config::RoguelikeEvent& event,
    const Config::RoguelikeEvent* treasure_event)
{
    if (event.name != "相随" || m_option_list.empty() || m_option_list_image.empty()) {
        return std::nullopt;
    }

    const std::string agent_name = m_option_list.front().text;
    if (is_jiegarden_target_candle_title(agent_name)) {
        const std::optional<std::string> body_text = read_jiegarden_first_agent_option_body();
        if (body_text && is_jiegarden_target_candle_body(*body_text)) {
            const cv::Mat agent_image = m_option_list_image.clone();
            json::object extra_details {
                { "target_candle", true },
                { "option_text", *body_text },
            };

            auto target_info = basic_info_with_what("RoguelikeJieGardenTargetCandleSelected");
            target_info["details"]["agent_name"] = agent_name;
            target_info["details"]["option_text"] = *body_text;
            callback(AsstMsg::SubTaskExtraInfo, target_info);

            Log.info(__FUNCTION__, "| Found target candle option, selecting candle option");
            if (!select_analyzed_option(0)) {
                record_agent_event(agent_name, agent_image, std::move(extra_details));
                return std::nullopt;
            }

            reset_option_list_and_view_data();
            if (!update_option_list() || m_option_list.empty()) {
                Log.error(__FUNCTION__, "| Failed to recognize derived candle event option");
                record_agent_event(agent_name, agent_image, std::move(extra_details));
                return std::nullopt;
            }

            extra_details["derived_selected_option"] = m_option_list.front().text;
            if (!select_analyzed_option(0)) {
                record_agent_event(agent_name, agent_image, std::move(extra_details));
                return std::nullopt;
            }

            RoguelikeBoskyPassageMap::get_instance().request_abandon_on_exit();
            Log.info(__FUNCTION__, "| Target candle path entered bosky passage; will abandon when leaving bosky passage");
            record_agent_event(agent_name, agent_image, std::move(extra_details));
            return std::nullopt;
        }
    }

    const cv::Mat agent_image = m_option_list_image.clone();
    json::object extra_details;

    const bool is_treasure = is_configured_option_match(agent_name, "宝盒");
    const size_t choice = is_treasure ? 1 : 2;
    if (choice > m_option_list.size()) {
        Log.error(__FUNCTION__, "| Not enough options for agent event:", agent_name, "option count:", m_option_list.size());
        record_agent_event(agent_name, agent_image, std::move(extra_details));
        return std::nullopt;
    }

    Log.info(
        __FUNCTION__,
        "| Handling agent event:",
        agent_name,
        "branch:",
        is_treasure ? "treasure" : "collectible",
        "choice:",
        choice);
    if (!select_analyzed_option(choice - 1)) {
        record_agent_event(agent_name, agent_image, std::move(extra_details));
        return std::nullopt;
    }

    if (is_treasure) {
        if (treasure_event == nullptr) {
            Log.error(__FUNCTION__, "| Missing treasure event config for agent event");
        }
        else if (advance_to_next_event(treasure_event->name)) {
            const std::string treasure_image_path = capture_and_handle_jiegarden_agent_treasure(*treasure_event);
            if (!treasure_image_path.empty()) {
                extra_details["treasure_image"] = treasure_image_path;
            }
        }
    }
    else {
        const std::string collectible_image_path =
            RoguelikeDataCollector.save_agent_collectible_image(ctrler()->get_image());
        if (!collectible_image_path.empty()) {
            extra_details["collectible_image"] = collectible_image_path;
        }
    }

    record_agent_event(agent_name, agent_image, std::move(extra_details));
    return std::nullopt;
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::handle_jiegarden_legend_relieving_event(
    const Config::RoguelikeEvent& event)
{
    if (event.name != "禳解") {
        return std::nullopt;
    }

    std::vector<std::string> image_paths;
    std::vector<std::vector<std::string>> option_groups;

    size_t loop_count = 0;
    while (!need_exit()) {
        if (m_option_list.empty() || m_option_list_image.empty()) {
            break;
        }

        std::vector<std::string> option_names;
        option_names.reserve(m_option_list.size());
        for (const auto& option : m_option_list) {
            option_names.emplace_back(option.text);
        }
        option_groups.emplace_back(std::move(option_names));
        image_paths.emplace_back(RoguelikeDataCollector.save_bosky_passage_image(
            m_option_list_image,
            "Legend_relieving_options"));

        ++loop_count;
        Log.info(__FUNCTION__, "| selecting first option in relieving event, loop:", loop_count);
        if (!select_analyzed_option(0)) {
            break;
        }

        if (!advance_to_next_event(event.name)) {
            break;
        }

        reset_option_list_and_view_data();
        if (!update_option_list()) {
            break;
        }
    }

    RoguelikeDataCollector.record_bosky_passage_legend_relieving(image_paths, option_groups);
    return std::nullopt;
}

std::string asst::RoguelikeStageEncounterTaskPlugin::capture_and_handle_jiegarden_agent_treasure(
    const Config::RoguelikeEvent& event)
{
    reset_option_list_and_view_data();
    if (!update_option_list()) {
        Log.info(__FUNCTION__, "| Failed to recognize unique treasure option, skip treasure image");
        return {};
    }

    if (m_option_list.size() != 1) {
        Log.info(
            __FUNCTION__,
            "| Treasure branch did not show exactly one option, skip treasure image. option count:",
            m_option_list.size());
        return {};
    }

    const std::string image_path = RoguelikeDataCollector.save_agent_treasure_image(m_option_list_image);

    size_t choice = 0;
    if (!event.option_text.empty()) {
        for (const std::string& event_text : event.option_text) {
            const auto option_it =
                std::ranges::find_if(m_option_list, [&event_text](const OptionAnalyzer::Option& option) {
                    return is_configured_option_match(option.text, event_text);
                });
            if (option_it != m_option_list.end()) {
                choice = std::distance(m_option_list.begin(), option_it) + 1;
                break;
            }
        }
    }
    else if (event.option_num == m_option_list.size()) {
        choice = process_task(event, 0);
    }
    else if (m_option_list.size() == 1) {
        choice = 1;
    }
    else {
        for (const auto& [total, item] : event.fallback_choices) {
            if (total == m_option_list.size()) {
                choice = item;
                break;
            }
        }
    }

    if (choice == 0 || choice > m_option_list.size()) {
        Log.error(
            __FUNCTION__,
            "| Failed to find treasure choice for",
            event.name,
            "option count:",
            m_option_list.size());
        return image_path;
    }

    select_analyzed_option(choice - 1);
    return image_path;
}

bool asst::RoguelikeStageEncounterTaskPlugin::select_analyzed_option(size_t index)
{
    LogTraceFunction;

    // sanity check
    if (index >= m_option_list.size()) [[unlikely]] {
        Log.error(
            __FUNCTION__,
            std::format("| Attempt to select option {} out of {}", index + 1, m_option_list.size()));
        return false;
    }
    if (!m_option_list[index].enabled) {
        Log.info(__FUNCTION__, std::format("| Attempt to select disabled option {}", index + 1));
        return false;
    }

    move_to_analyzed_option(index);

    // click option
    Log.info(__FUNCTION__, std::format("| Clicking option {}: {}", index + 1, m_option_list[index].text));
    Rect click_rect = Task.get("JieGarden@RoguelikeEncounter-ClickOption")->specific_rect;
    click_rect.y = m_option_y_in_view[index];
    for (int j = 0; j < 2; ++j) {
        ctrler()->click(click_rect);
        sleep(300);
    }
    sleep(1500);

    if (hp(ctrler()->get_image()) < 0) {
        return true;
    }

    Log.error(__FUNCTION__, "| The option doesn't respond to click");
    save_img(ctrler()->get_image(), "current screenshot");

    return false;
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_option_list_and_view_data()
{
    m_option_list.clear();
    m_option_list_image.release();
    reset_view();
}

void asst::RoguelikeStageEncounterTaskPlugin::record_agent_event_if_needed(const Config::RoguelikeEvent& event)
{
    if (event.name != "相随" || m_option_list.empty() || m_option_list_image.empty()) {
        return;
    }

    record_agent_event(m_option_list.front().text, m_option_list_image);
}

void asst::RoguelikeStageEncounterTaskPlugin::record_agent_event(
    std::string_view agent_name,
    const cv::Mat& agent_image,
    json::object extra_details)
{
    if (agent_name.empty() || agent_image.empty()) {
        return;
    }

    const std::string image_path = RoguelikeDataCollector.save_agent_image(agent_image);
    json::object source_details =
        RoguelikeDataCollector.record_agent(agent_name, image_path, std::move(extra_details));
    json::object details {
        { "name", std::string(agent_name) },
        { "image", image_path },
    };
    details |= std::move(source_details);
    RoguelikeDataCollector.log_event("agents", std::move(details));
}

void asst::RoguelikeStageEncounterTaskPlugin::report_analyzed_options()
{
    std::vector<json::value> options;

    Log.info("Analyzed Options");
    Log.info(std::string(40, '-'));
    Log.info(std::format("{:^9} | {}", "Enabled", "Text"));
    Log.info(std::string(40, '-'));
    for (const auto& [enabled, templ, text] : m_option_list) {
        json::value option = json::object {
            { "enabled", enabled },
            { "text", text },
        };
        options.emplace_back(std::move(option));
        Log.info(std::format("{:^9} | {}", enabled ? "Y" : "N", text));
    }
    Log.info(std::string(40, '-'));

    json::value info = basic_info_with_what("RoguelikeEncounterOptions");
    info["details"]["options"] = std::move(options);
    callback(AsstMsg::SubTaskExtraInfo, info);
}

void asst::RoguelikeStageEncounterTaskPlugin::update_view(const cv::Mat& image)
{
    LogTraceFunction;

    constexpr double OptionInViewThreshold = 0.90;

    reset_view();

    cv::Mat img = image.empty() ? ctrler()->get_image() : image;

    for (size_t i = 0; i < m_option_list.size(); ++i) {
        if (const Matcher::ResultOpt option_match_ret =
                RoguelikeEncounterOptionAnalyzer::match_option(m_config->get_theme(), img, m_option_list[i].templ);
            option_match_ret) {
            if (option_match_ret->score < OptionInViewThreshold) {
                Log.debug(
                    __FUNCTION__,
                    "| Ignoring weak option-in-view match",
                    i + 1,
                    m_option_list[i].text,
                    "score:",
                    option_match_ret->score,
                    "rect:",
                    option_match_ret->rect);
                continue;
            }
            if (i < m_view_begin) {
                m_view_begin = i;
            }
            if (i >= m_view_end) {
                m_view_end = i + 1;
            }
            m_option_y_in_view[i] = option_match_ret.value().rect.y;
        }
    }

    Log.info(__FUNCTION__, std::format("| Current view is [{}, {}]", m_view_begin + 1, m_view_end));
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_view()
{
    m_view_begin = m_option_list.size();
    m_view_end = 0;
    m_option_y_in_view.assign(m_option_list.size(), UNDEFINED);
}

void asst::RoguelikeStageEncounterTaskPlugin::move_to_analyzed_option(size_t index)
{
    LogTraceFunction;

    // sanity check
    if (index >= m_option_list.size()) [[unlikely]] {
        Log.error(
            __FUNCTION__,
            std::format("| Attempt to move to option {} out of {}", index + 1, m_option_list.size()));
        return;
    }

    Log.info(__FUNCTION__, std::format("Moving to option {}: {}", index + 1, m_option_list[index].text));

    cv::Mat image;
    while (!need_exit()) {
        if (index < m_view_begin) {
            move_backward();
            image = ctrler()->get_image();
            update_view(image);
            continue;
        }
        if (index >= m_view_end) {
            move_forward();
            image = ctrler()->get_image();
            update_view(image);
            continue;
        }
        if (m_option_y_in_view[index] == UNDEFINED) {
            Log.error(__FUNCTION__, "| y for option {} in view is not updated", index + 1);
            save_img(image, "lastly used screenshot");
            save_img(m_option_list[index].templ, "option template");
            image = ctrler()->get_image();
            update_view(image);
            continue;
        }
        break;
    }
}

void asst::RoguelikeStageEncounterTaskPlugin::move_to_option_list_head()
{
    LogTraceFunction;

    ProcessTask(*this, { m_config->get_theme() + "@RoguelikeEncounter-InitialMoveUp" }).run();
}

void asst::RoguelikeStageEncounterTaskPlugin::move_forward()
{
    LogTraceFunction;

    ProcessTask(*this, { m_config->get_theme() + "@RoguelikeEncounter-MoveDown" }).run();
}

void asst::RoguelikeStageEncounterTaskPlugin::move_backward()
{
    LogTraceFunction;

    ProcessTask(*this, { m_config->get_theme() + "@RoguelikeEncounter-MoveUp" }).run();
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::next_event(const Config::RoguelikeEvent& event)
{
    LogTraceFunction;

    if (event.next_event.empty()) {
        return std::nullopt;
    }

    if (advance_to_next_event(event.next_event)) {
        return event.next_event;
    }
    return std::nullopt;
}

bool asst::RoguelikeStageEncounterTaskPlugin::advance_to_next_event(std::string_view next_event_name)
{
    const auto& task = Task.get("Roguelike@StageEncounterJudgeClick");
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            ctrler()->click(task->specific_rect);
            sleep(500);
        }
        if (hp(ctrler()->get_image()) >= 0) {
            Log.debug("HP restored, going to next_event:", next_event_name);
            // 多点一次，确保选项恢复
            ctrler()->click(task->specific_rect);
            sleep(500);
            return true;
        }
    }

    return false;
}

bool asst::RoguelikeStageEncounterTaskPlugin::save_img(const cv::Mat& image, const std::string_view description)
{
    return utils::save_debug_image(image, utils::path("debug") / "roguelike" / "encounter", true, description);
}
