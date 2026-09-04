#include "RoguelikeStageEncounterTaskPlugin.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/Map/RoguelikeBoskyPassageMap.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/FuzzyTextMatcher.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/RegionOCRer.h"

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

    m_lake_fairy_plan.reset();
    m_lake_fairy_initial_choice_index = 0;
    m_lake_fairy_unique_choice_selected = false;

    const std::string& theme = m_config->get_theme();
    std::vector<std::string> event_names = RoguelikeStageEncounter.get_event_names(theme);

    if (theme == RoguelikeTheme::BlackFlow) {
        // 事件选项点击失败时画面通常仍是原事件页。继续走事件结果分发即可重新识别并续办；
        // 禁止点击左上角尝试“恢复地图”，那只会打开退出探索确认并回到原事件页。
        Task.set_task_base("BlackFlow@Roguelike@StageEncounterResult", "BlackFlow@Roguelike@StageEncounterReward");
    }

    const std::string themed_ocr_task = theme + "@Roguelike@StageEncounterOcr";
    const auto event_name_task_ptr =
        Task.get(themed_ocr_task) != nullptr ? Task.get(themed_ocr_task) : Task.get("Roguelike@StageEncounterOcr");
    sleep(event_name_task_ptr->pre_delay);

    if (need_exit()) {
        return false;
    }

    constexpr int EventNameOcrAttempts = 3;
    constexpr int EventNameOcrRetryDelay = 700;
    cv::Mat image;
    std::string current_event_name;
    for (int attempt = 1; attempt <= EventNameOcrAttempts; ++attempt) {
        image = ctrler()->get_image();
        OCRer name_analyzer(image);
        name_analyzer.set_task_info(event_name_task_ptr);
        name_analyzer.set_required(event_names);
        if (name_analyzer.analyze() && !name_analyzer.get_result().empty()) {
            current_event_name = name_analyzer.get_result().front().text;
            break;
        }
        if (attempt < EventNameOcrAttempts) {
            Log.warn("Unknown Event, retrying title OCR", attempt, "of", EventNameOcrAttempts);
            sleep(EventNameOcrRetryDelay);
            if (need_exit()) {
                return false;
            }
        }
    }

    if (current_event_name.empty()) {
        Log.error("Unknown Event after title OCR retries");
        save_img(image, "event title OCR failed");
        auto info = basic_info_with_what("EncounterOcrError");
        info["details"]["attempts"] = EventNameOcrAttempts;
        callback(AsstMsg::SubTaskExtraInfo, info);
        return true;
    }

    // 处理主事件及其链式 next_event
    while (!current_event_name.empty()) {
        auto next = handle_single_event(current_event_name);
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

    auto it = event_map.find(event_name);
    if (it == event_map.end()) {
        Log.error("Unknown event:", event_name);
        return std::nullopt;
    }

    const auto& event = it->second;

    cv::Mat image = ctrler()->get_image();

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
    std::optional<std::vector<std::string>> planned_choice_order;
    if (theme == RoguelikeTheme::BlackFlow && m_blackflow_encounter_choice_provider) {
        if (const auto planned_choice = m_blackflow_encounter_choice_provider(event.name);
            planned_choice.has_value()) {
            choose_option = *planned_choice;
        }
    }
    if (theme == RoguelikeTheme::BlackFlow && m_blackflow_encounter_choice_order_provider) {
        planned_choice_order = m_blackflow_encounter_choice_order_provider(event.name);
        if (planned_choice_order.has_value()) {
            Log.info("Event:", event.name, "planned configured choice order", json::array(*planned_choice_order));
        }
    }
    Log.info("Event:", event.name, "special_val", special_val, "choose option", choose_option);

    auto info = basic_info_with_what("RoguelikeEvent");
    info["details"]["name"] = event.name;
    info["details"]["default_choose"] = event.default_choose;
    info["details"]["choose_option"] = choose_option;
    if (planned_choice_order.has_value()) {
        info["details"]["planned_choice_order"] = json::array(*planned_choice_order);
    }
    callback(AsstMsg::SubTaskExtraInfo, info);
    // 插件 callback 只会上报给外部调用方，不会再广播给同一 ProcessTask 的其他插件。
    // 需要页面内容的主题专用逻辑必须通过显式观察器接收事件名。
    if (m_event_observer) {
        m_event_observer(event.name);
    }

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

    // 界园与黑流树海通过识别实际选项列表选择事件选项。
    if (theme == RoguelikeTheme::JieGarden || theme == RoguelikeTheme::BlackFlow) {
        reset_option_list_and_view_data();
        if (update_option_list(event.name)) {
            if (theme == RoguelikeTheme::BlackFlow && event.name == blackflow::LakeFairyEventName) {
                return handle_blackflow_lake_fairy(event);
            }
            if (!event.choice_groups.empty()) {
                std::unordered_set<size_t> grouped_choices;
                std::vector<std::string> grouped_ocr_candidates;
                for (const auto& candidate_group : event.choice_groups) {
                    for (const auto& candidate_choice : candidate_group.choices) {
                        if (const auto* candidate_text = std::get_if<std::string>(&candidate_choice)) {
                            grouped_ocr_candidates.emplace_back(*candidate_text);
                        }
                    }
                }
                const auto resolve_grouped_choice = [&](int configured_choice) -> std::optional<size_t> {
                    const auto option_count = static_cast<std::int64_t>(m_option_list.size());
                    const std::int64_t resolved_choice = configured_choice > 0
                        ? static_cast<std::int64_t>(configured_choice)
                        : option_count + static_cast<std::int64_t>(configured_choice) + 1;
                    if (resolved_choice <= 0 || resolved_choice > option_count) {
                        return std::nullopt;
                    }
                    return static_cast<size_t>(resolved_choice);
                };

                for (const auto& group : event.choice_groups) {
                    std::vector<size_t> resolved_group_choices;
                    std::unordered_map<std::string, size_t> resolved_group_choice_texts;
                    const auto append_grouped_choice = [&](size_t grouped_choice) {
                        if (!grouped_choices.emplace(grouped_choice).second) {
                            Log.warn(
                                "Event:",
                                event.name,
                                "Option",
                                grouped_choice,
                                "is already assigned to an earlier group");
                            return;
                        }
                        resolved_group_choices.emplace_back(grouped_choice);
                    };

                    for (const auto& configured_choice : group.choices) {
                        std::optional<size_t> grouped_choice_opt;
                        if (const auto* choice_index = std::get_if<int>(&configured_choice)) {
                            grouped_choice_opt = resolve_grouped_choice(*choice_index);
                        }
                        else if (const auto* choice_text = std::get_if<std::string>(&configured_choice)) {
                            const auto option_it =
                                std::ranges::find_if(m_option_list, [&](const OptionAnalyzer::Option& option) {
                                    return option.text == *choice_text;
                                });
                            if (option_it != m_option_list.end()) {
                                grouped_choice_opt = std::distance(m_option_list.begin(), option_it) + 1;
                            }
                            else {
                                std::optional<size_t> fuzzy_choice;
                                utils::FuzzyTextMatch fuzzy_match;
                                bool ambiguous = false;
                                for (size_t option_index = 0; option_index < m_option_list.size(); ++option_index) {
                                    const auto match = utils::fuzzy_match_ocr_text(
                                        m_option_list[option_index].text,
                                        grouped_ocr_candidates);
                                    if (!match.accepted || match.canonical != *choice_text) {
                                        continue;
                                    }
                                    if (fuzzy_choice.has_value()) {
                                        ambiguous = true;
                                        break;
                                    }
                                    fuzzy_choice = option_index + 1;
                                    fuzzy_match = match;
                                }
                                if (!ambiguous && fuzzy_choice.has_value()) {
                                    grouped_choice_opt = fuzzy_choice;
                                    Log.info(
                                        "Event:",
                                        event.name,
                                        "Grouped OCR text",
                                        *choice_text,
                                        "fuzzy matched",
                                        m_option_list[*fuzzy_choice - 1].text,
                                        "distance",
                                        fuzzy_match.edit_distance,
                                        "similarity",
                                        fuzzy_match.similarity);
                                }
                                else if (ambiguous) {
                                    Log.warn(
                                        "Event:",
                                        event.name,
                                        "Grouped OCR text",
                                        *choice_text,
                                        "fuzzy matched multiple options; rejecting ambiguous match");
                                }
                            }
                        }

                        if (!grouped_choice_opt) {
                            if (const auto* choice_index = std::get_if<int>(&configured_choice)) {
                                Log.warn(
                                    "Event:",
                                    event.name,
                                    "Grouped choice",
                                    *choice_index,
                                    "cannot be resolved for",
                                    m_option_list.size(),
                                    "option(s)");
                            }
                            else {
                                Log.warn(
                                    "Event:",
                                    event.name,
                                    "Grouped OCR text",
                                    std::get<std::string>(configured_choice),
                                    "does not match any option");
                            }
                            continue;
                        }
                        append_grouped_choice(grouped_choice_opt.value());
                        if (const auto* choice_text = std::get_if<std::string>(&configured_choice)) {
                            resolved_group_choice_texts.insert_or_assign(*choice_text, grouped_choice_opt.value());
                        }
                    }

                    if (group.choice_range) {
                        const auto range_begin_opt = resolve_grouped_choice(group.choice_range->first);
                        const auto range_end_opt = resolve_grouped_choice(group.choice_range->second);
                        if (!range_begin_opt || !range_end_opt) {
                            Log.warn(
                                "Event:",
                                event.name,
                                "Grouped range cannot be resolved for",
                                m_option_list.size(),
                                "option(s)");
                        }
                        else {
                            const size_t range_begin = range_begin_opt.value();
                            const size_t range_end = range_end_opt.value();
                            for (size_t grouped_choice = range_begin;;) {
                                append_grouped_choice(grouped_choice);
                                if (grouped_choice == range_end) {
                                    break;
                                }
                                if (range_begin < range_end) {
                                    ++grouped_choice;
                                }
                                else {
                                    --grouped_choice;
                                }
                            }
                        }
                    }

                    if (planned_choice_order.has_value()) {
                        std::vector<size_t> planned_group_choices;
                        std::unordered_set<size_t> appended_planned_choices;
                        for (const std::string& configured_text : *planned_choice_order) {
                            const auto resolved = resolved_group_choice_texts.find(configured_text);
                            if (resolved == resolved_group_choice_texts.end() ||
                                !appended_planned_choices.emplace(resolved->second).second) {
                                continue;
                            }
                            planned_group_choices.emplace_back(resolved->second);
                        }
                        // 名称计划是权威候选集：没有列出的配置项代表安全评估已明确排除。
                        // grouped_choices 仍保留完整显式组，避免被排除项又落入隐式最终回退。
                        resolved_group_choices = std::move(planned_group_choices);
                    }

                    if (group.random) {
                        static thread_local std::mt19937 random_engine(std::random_device {}());
                        std::shuffle(resolved_group_choices.begin(), resolved_group_choices.end(), random_engine);
                    }

                    if (theme == RoguelikeTheme::BlackFlow &&
                        event.name == blackflow::GoldStasisEventName) {
                        const auto context = m_blackflow_encounter_context_provider
                                                 ? m_blackflow_encounter_context_provider()
                                                 : std::nullopt;
                        const bool core_operator_elite_two =
                            context.has_value() && context->core_operator_elite_two;
                        std::ranges::stable_sort(
                            resolved_group_choices,
                            [&](size_t lhs, size_t rhs) {
                                return blackflow::gold_stasis_choice_priority_bucket(
                                           m_option_list[lhs - 1].text,
                                           core_operator_elite_two) <
                                       blackflow::gold_stasis_choice_priority_bucket(
                                           m_option_list[rhs - 1].text,
                                           core_operator_elite_two);
                            });
                        if (core_operator_elite_two) {
                            Log.info(
                                "Event: 金色凝滞 | defer 查看无人机 and 向泉水许愿 behind all other choices");
                        }
                    }

                    for (const size_t grouped_choice : resolved_group_choices) {
                        if (!m_option_list[grouped_choice - 1].enabled) {
                            Log.info("Event:", event.name, "Grouped choice", grouped_choice, "is disabled");
                            continue;
                        }
                        if (!select_analyzed_option(grouped_choice - 1)) {
                            continue;
                        }

                        if (theme == RoguelikeTheme::BlackFlow) {
                            Task.set_task_base(
                                "BlackFlow@Roguelike@StageEncounterResult",
                                "BlackFlow@Roguelike@StageEncounterReward");
                        }
                        return next_event(group.next_event.value_or(event.next_event));
                    }
                }

                if (planned_choice_order.has_value()) {
                    // 权威名称计划中的所有候选都未能选择时宁可让本轮失败/重试，也不能把
                    // OCR 未成功归组的危险代价项当成普通未覆盖选项再次捞回来。
                    return std::nullopt;
                }

                // 隐式最终回退组：从后向前尝试所有未被显式组覆盖的可用选项，继承事件级 next_event。
                for (size_t fallback_choice = m_option_list.size(); fallback_choice > 0; --fallback_choice) {
                    if (grouped_choices.contains(fallback_choice) || !m_option_list[fallback_choice - 1].enabled) {
                        continue;
                    }
                    if (!select_analyzed_option(fallback_choice - 1)) {
                        continue;
                    }
                    if (theme == RoguelikeTheme::BlackFlow) {
                        Task.set_task_base(
                            "BlackFlow@Roguelike@StageEncounterResult",
                            "BlackFlow@Roguelike@StageEncounterReward");
                    }
                    return next_event(event.next_event);
                }
                return std::nullopt;
            }

            size_t choice = 0; // 以 0 作为无效 index。
            if (theme == RoguelikeTheme::BlackFlow) {
                if (choose_option > 0 && choose_option <= m_option_list.size() &&
                    m_option_list[choose_option - 1].enabled) {
                    choice = choose_option;
                }
                else {
                    const auto enabled_it =
                        std::ranges::find_if(m_option_list.rbegin(), m_option_list.rend(), [](const auto& option) {
                            return option.enabled;
                        });
                    if (enabled_it != m_option_list.rend()) {
                        choice = static_cast<size_t>(std::distance(m_option_list.begin(), enabled_it.base()));
                    }
                }
            }
            else if (!event.option_text.empty()) {
                for (const std::string& event_text : event.option_text) {
                    const auto option_it =
                        std::ranges::find_if(m_option_list, [&event_text](const OptionAnalyzer::Option& option) {
                            return option.text == event_text;
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
                if (theme == RoguelikeTheme::BlackFlow) {
                    Task.set_task_base(
                        "BlackFlow@Roguelike@StageEncounterResult",
                        "BlackFlow@Roguelike@StageEncounterReward");
                }
                return next_event(event.next_event);
            }

            if (theme == RoguelikeTheme::BlackFlow) {
                for (choice = m_option_list.size(); choice > 0; --choice) {
                    if (m_option_list[choice - 1].enabled && select_analyzed_option(choice - 1)) {
                        Task.set_task_base(
                            "BlackFlow@Roguelike@StageEncounterResult",
                            "BlackFlow@Roguelike@StageEncounterReward");
                        return next_event(event.next_event);
                    }
                }
                return std::nullopt;
            }

            // 界园兜底：从下到上依次选择。
            for (choice = m_option_list.size(); choice > 0; --choice) {
                if (m_option_list[choice - 1].enabled && select_analyzed_option(choice - 1)) {
                    return next_event(event.next_event);
                }
            }
        }
        else if (theme == RoguelikeTheme::BlackFlow) {
            Log.error("BlackFlow encounter option analysis failed");
            return std::nullopt;
        }
    }

    const auto click_option_task_name = [&](size_t item, size_t total) {
        if (item > total) {
            Log.warn("Event:", event.name, "Total:", total, "Choice", item, "out of range, switch to choice", total);
            item = total;
        }
        return m_config->get_theme() + "@Roguelike@OptionChoose" + std::to_string(total) + "-" + std::to_string(item);
    };

    for (int j = 0; j < 2; ++j) {
        ProcessTask(*this, { click_option_task_name(choose_option, event.option_num) }).run();
        sleep(300);
    }

    sleep(1500);

    // 判断是否点击成功，成功进入对话后左上角的生命值会消失
    image = ctrler()->get_image();
    bool hp_disappeared = (hp(image) < 0);
    // fallback 可变选项，临时处理，之后还得改成更通用的方式
    if (!hp_disappeared) {
        for (const auto& [total, item] : event.fallback_choices) {
            Log.info("Trying fallback choice", total, "-", item);
            for (int j = 0; j < 2; ++j) {
                ProcessTask(*this, { click_option_task_name(item, total) }).run();
                sleep(300);
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
        return next_event(event.next_event);
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

            for (int j = 0; j < 2; ++j) {
                ProcessTask(*this, { click_option_task_name(i, max_time) }).run();
                sleep(300);
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

    return event.next_event.empty() ? std::nullopt : std::optional { event.next_event };
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::handle_blackflow_lake_fairy(
    const Config::RoguelikeEvent& event)
{
    if (!m_lake_fairy_plan.has_value()) {
        const std::optional<blackflow::LakeFairyContext> context =
            m_blackflow_encounter_context_provider ? m_blackflow_encounter_context_provider() : std::nullopt;
        if (!context.has_value()) {
            Log.warn("Event: 湖中仙女 | encounter context unavailable; use the conservative branch");
        }
        const blackflow::LakeFairyContext resolved_context = context.value_or(blackflow::LakeFairyContext {});
        m_lake_fairy_plan = blackflow::make_lake_fairy_choice_plan(resolved_context);
        Log.info(
            "Event: 湖中仙女 | locked branch",
            resolved_context.core_operator_elite_two && resolved_context.ingots >= 3
                ? "first option four times"
                : "first option, then second option",
            "core operator elite two",
            resolved_context.core_operator_elite_two,
            "ingots",
            resolved_context.ingots);
    }

    size_t choice = 0;
    bool selecting_unique_choice = false;
    if (m_lake_fairy_initial_choice_index < m_lake_fairy_plan->initial_choice_count) {
        choice = m_lake_fairy_plan->initial_choices[m_lake_fairy_initial_choice_index];
    }
    else {
        if (m_lake_fairy_unique_choice_selected) {
            Log.error("Event: 湖中仙女 | options remain after selecting the expected final unique option");
            return std::nullopt;
        }
        const auto enabled_count = std::ranges::count_if(m_option_list, [](const auto& option) {
            return option.enabled;
        });
        if (enabled_count != 1) {
            Log.error(
                "Event: 湖中仙女 | expected one enabled follow-up option, got",
                enabled_count,
                "from",
                m_option_list.size(),
                "recognized options");
            return std::nullopt;
        }
        const auto unique = std::ranges::find_if(m_option_list, [](const auto& option) { return option.enabled; });
        choice = static_cast<size_t>(std::distance(m_option_list.begin(), unique)) + 1;
        selecting_unique_choice = true;
    }

    if (choice == 0 || choice > m_option_list.size() || !m_option_list[choice - 1].enabled) {
        Log.error(
            "Event: 湖中仙女 | planned option",
            choice,
            "is unavailable among",
            m_option_list.size(),
            "recognized options");
        return std::nullopt;
    }
    if (!select_analyzed_option(choice - 1)) {
        return std::nullopt;
    }

    if (selecting_unique_choice) {
        m_lake_fairy_unique_choice_selected = true;
    }
    else {
        ++m_lake_fairy_initial_choice_index;
    }
    Task.set_task_base("BlackFlow@Roguelike@StageEncounterResult", "BlackFlow@Roguelike@StageEncounterReward");
    return next_event(event.next_event);
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

bool asst::RoguelikeStageEncounterTaskPlugin::update_option_list(std::string_view event_name)
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    // 不期而遇默认位置会在选项列表中央, 为了从上到下检视选项列表, 需要先向上滑动
    move_to_option_list_head();

    cv::Mat image = ctrler()->get_image();
    RoguelikeEncounterOptionAnalyzer analyzer(image);
    analyzer.set_theme(theme);
    const size_t max_swipe_times =
        theme == RoguelikeTheme::BlackFlow ? BLACKFLOW_MAX_SWIPE_TIMES : MAX_SWIPE_TIMES;
    for (size_t swipe_times = 0; swipe_times < max_swipe_times && !need_exit(); ++swipe_times) {
        move_forward();
        image = ctrler()->get_image();
        const std::optional<int> ret = analyzer.merge_image(image);
        if (!ret) {
            return false;
        }
        if (ret.value() <= 0) {
            break;
        }
    }

    if (!analyzer.analyze()) {
        return false;
    }

    if (m_event_capture_observer && !analyzer.get_stitched_image().empty()) {
        m_event_capture_observer(event_name, analyzer.get_stitched_image());
    }

    m_option_list = analyzer.get_result();
    report_analyzed_options();

    update_view(image);

    return true;
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

    if (m_config->get_theme() == RoguelikeTheme::BlackFlow) {
        constexpr int ClickAttempts = 3;
        constexpr int ConfirmationRecognitionRetries = 3;
        for (int attempt = 1; attempt <= ClickAttempts && !need_exit(); ++attempt) {
            if (!move_to_analyzed_option(index)) {
                Log.warn(
                    __FUNCTION__,
                    std::format("| Could not bring option {} back into view on click attempt {}", index + 1, attempt));
                continue;
            }
            const std::optional<Rect> stable_header = wait_for_analyzed_option_stable(index);
            if (!stable_header.has_value()) {
                Log.warn(
                    __FUNCTION__,
                    std::format(
                        "| Option {} did not settle before click attempt {}",
                        index + 1,
                        attempt));
                continue;
            }

            Log.info(
                __FUNCTION__,
                std::format(
                    "| Clicking stable option {}: {} (attempt {})",
                    index + 1,
                    m_option_list[index].text,
                    attempt));
            const Point click_point {
                stable_header->x + stable_header->width / 2 + 100,
                stable_header->y + stable_header->height / 2,
            };
            ctrler()->click(click_point);
            sleep(300);
            if (ProcessTask(*this, { "BlackFlow@Roguelike@StageEncounterLeaveConfirm" })
                    .set_retry_times(ConfirmationRecognitionRetries)
                    .run()) {
                return true;
            }

            Log.warn(
                __FUNCTION__,
                std::format(
                    "| Option {} did not respond; retry the same planned option before using a fallback",
                    index + 1));
        }
    }
    else {
        if (!move_to_analyzed_option(index)) {
            return false;
        }

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
    }

    Log.error(__FUNCTION__, "| The option doesn't respond to click");
    save_img(ctrler()->get_image(), "current screenshot");

    return false;
}

std::optional<asst::Rect>
    asst::RoguelikeStageEncounterTaskPlugin::wait_for_analyzed_option_stable(size_t index)
{
    constexpr int ObservationAttempts = 15;
    constexpr int ObservationDelay = 120;

    blackflow::EncounterOptionPositionStability stability;
    cv::Mat latest_image;
    for (int attempt = 0; attempt < ObservationAttempts && !need_exit(); ++attempt) {
        latest_image = ctrler()->get_image();
        const Matcher::ResultOpt match = RoguelikeEncounterOptionAnalyzer::match_option(
            m_config->get_theme(),
            latest_image,
            m_option_list[index].templ);
        if (match.has_value()) {
            m_option_y_in_view[index] = match->rect.y;
            m_option_rect_in_view[index] = match->rect;
            if (stability.observe(match->rect.y)) {
                return match->rect;
            }
        }
        else {
            (void)stability.observe(std::nullopt);
        }

        if (attempt + 1 < ObservationAttempts) {
            sleep(ObservationDelay);
        }
    }

    // Refresh the complete visible range so the next click attempt can scroll the target
    // back into view if inertia carried it outside the matcher ROI.
    if (!latest_image.empty()) {
        update_view(latest_image);
    }
    return std::nullopt;
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_option_list_and_view_data()
{
    m_option_list.clear();
    reset_view();
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

    reset_view();

    cv::Mat img = image.empty() ? ctrler()->get_image() : image;

    for (size_t i = 0; i < m_option_list.size(); ++i) {
        if (const Matcher::ResultOpt option_match_ret =
                RoguelikeEncounterOptionAnalyzer::match_option(m_config->get_theme(), img, m_option_list[i].templ);
            option_match_ret) {
            if (i < m_view_begin) {
                m_view_begin = i;
            }
            if (i >= m_view_end) {
                m_view_end = i + 1;
            }
            m_option_y_in_view[i] = option_match_ret.value().rect.y;
            m_option_rect_in_view[i] = option_match_ret.value().rect;
        }
    }

    Log.info(__FUNCTION__, std::format("| Current view is [{}, {}]", m_view_begin + 1, m_view_end));
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_view()
{
    m_view_begin = m_option_list.size();
    m_view_end = 0;
    m_option_y_in_view.assign(m_option_list.size(), UNDEFINED);
    m_option_rect_in_view.assign(m_option_list.size(), Rect { UNDEFINED, UNDEFINED, 0, 0 });
}

bool asst::RoguelikeStageEncounterTaskPlugin::move_to_analyzed_option(size_t index)
{
    LogTraceFunction;

    // sanity check
    if (index >= m_option_list.size()) [[unlikely]] {
        Log.error(
            __FUNCTION__,
            std::format("| Attempt to move to option {} out of {}", index + 1, m_option_list.size()));
        return false;
    }

    Log.info(__FUNCTION__, std::format("Moving to option {}: {}", index + 1, m_option_list[index].text));

    cv::Mat image;
    int completed_attempts = 0;
    while (!need_exit() && blackflow::encounter_option_navigation_should_continue(completed_attempts)) {
        if (index < m_view_begin) {
            move_backward();
            ++completed_attempts;
            image = ctrler()->get_image();
            update_view(image);
            continue;
        }
        if (index >= m_view_end) {
            move_forward();
            ++completed_attempts;
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
            ++completed_attempts;
            continue;
        }
        return true;
    }

    Log.error(
        __FUNCTION__,
        std::format(
            "| Failed to bring option {} into view after {} attempts",
            index + 1,
            completed_attempts));
    return false;
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

std::optional<std::string>
    asst::RoguelikeStageEncounterTaskPlugin::next_event(const std::string& next_event_name)
{
    LogTraceFunction;

    if (next_event_name.empty()) {
        return std::nullopt;
    }

    const auto& task = Task.get("Roguelike@StageEncounterJudgeClick");
    const bool is_blackflow = m_config->get_theme() == RoguelikeTheme::BlackFlow;
    const auto observe_page_state = [&]() {
        const cv::Mat image = ctrler()->get_image();
        bool map_visible = false;
        if (is_blackflow) {
            for (const std::string_view map_task : {
                     std::string_view { "BlackFlow@Roguelike@MapPrepare-Ready" },
                     std::string_view { "BlackFlow@Roguelike@MapPrepare-ZoomOut" },
                }) {
                Matcher map_matcher(image);
                map_matcher.set_task_info(std::string { map_task });
                if (map_matcher.analyze().has_value()) {
                    map_visible = true;
                    break;
                }
            }
        }
        return blackflow::classify_chained_encounter_page(is_blackflow, map_visible, hp(image) >= 0);
    };
    const auto stop_if_returned_to_map = [&]() {
        if (observe_page_state() != blackflow::ChainedEncounterPageState::MapReturned) {
            return false;
        }
        Log.info("BlackFlow chained encounter returned to map; finish event without another click");
        return true;
    };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            // 地图和事件页都有生命值 HUD，不能再用 hp() 单独判断链式事件是否已经出现。
            // 每次点击前后都判图；有些选项会在本次点击后直接退回地图。
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
            ctrler()->click(task->specific_rect);
            sleep(500);
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
        }
        if (observe_page_state() == blackflow::ChainedEncounterPageState::EventReady) {
            Log.debug("HP restored, going to next_event:", next_event_name);
            // 多点一次，确保选项恢复
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
            ctrler()->click(task->specific_rect);
            sleep(500);
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
            return next_event_name;
        }
    }

    return std::nullopt;
}

bool asst::RoguelikeStageEncounterTaskPlugin::save_img(const cv::Mat& image, const std::string_view description)
{
    return utils::save_debug_image(image, utils::path("debug") / "roguelike" / "encounter", true, description);
}
